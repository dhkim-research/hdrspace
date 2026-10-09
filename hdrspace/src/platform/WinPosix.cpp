#include "platform/WinPosix.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

struct ChildJob {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
};

std::mutex& jobsMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<pid_t, ChildJob>& jobs() {
    static std::map<pid_t, ChildJob> table;
    return table;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

// One argument for CommandLineToArgvW / the C runtime's argv parser.
std::wstring quoteArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result += L'"';
            backslashes = 0;
            continue;
        } else {
            backslashes = 0;
        }
        result += ch;
    }
    result.append(backslashes, L'\\');
    result += L'"';
    return result;
}

bool wildcardMatch(const std::wstring& pattern, const std::wstring& name) {
    size_t p = 0, n = 0, star = std::wstring::npos, mark = 0;
    const auto same = [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); };
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == L'?' || same(pattern[p], name[n]))) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == L'*') {
            star = p++;
            mark = n;
        } else if (star != std::wstring::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == L'*') {
        ++p;
    }
    return p == pattern.size();
}

}  // namespace

fs::path hdrspaceExecutablePathWin() {
    std::wstring buffer(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count == 0 || count >= buffer.size()) {
        return {};
    }
    buffer.resize(count);
    return fs::path(buffer);
}

fs::path hdrspaceLocalAppData() {
    if (const wchar_t* value = _wgetenv(L"LOCALAPPDATA"); value && *value) {
        return fs::path(value);
    }
    if (const wchar_t* value = _wgetenv(L"USERPROFILE"); value && *value) {
        return fs::path(value) / "AppData" / "Local";
    }
    std::error_code ec;
    return fs::temp_directory_path(ec);
}

pid_t hdrspaceSpawnShell(const std::string& command, const fs::path& logPath) {
    const fs::path shell = hdrspaceExecutablePathWin().parent_path() / "busybox.exe";
    if (!fs::exists(shell)) {
        throw std::runtime_error("The shell helper busybox.exe is missing next to hdrspace.exe.");
    }
    std::wstring commandLine = quoteArgument(shell.wstring()) + L" sh -c " + quoteArgument(widen(command));

    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE log = CreateFileW(logPath.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Could not create the job log file.");
    }
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION info{};
    const BOOL started = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                        nullptr, nullptr, &startup, &info);
    CloseHandle(log);
    if (input != INVALID_HANDLE_VALUE) {
        CloseHandle(input);
    }
    if (!started) {
        if (job) {
            CloseHandle(job);
        }
        throw std::runtime_error("Could not start the job shell.");
    }
    if (job) {
        AssignProcessToJobObject(job, info.hProcess);
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);

    const pid_t pid = static_cast<pid_t>(info.dwProcessId);
    std::lock_guard<std::mutex> lock{jobsMutex()};
    jobs()[pid] = ChildJob{info.hProcess, job};
    return pid;
}

pid_t waitpid(pid_t pid, int* status, int options) {
    HANDLE process = nullptr;
    {
        std::lock_guard<std::mutex> lock{jobsMutex()};
        const auto it = jobs().find(pid);
        if (it == jobs().end()) {
            return -1;
        }
        process = it->second.process;
    }
    const DWORD waited = WaitForSingleObject(process, (options & WNOHANG) ? 0 : INFINITE);
    if (waited == WAIT_TIMEOUT) {
        return 0;
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process, &exitCode);
    if (status) {
        *status = static_cast<int>(exitCode);
    }
    std::lock_guard<std::mutex> lock{jobsMutex()};
    const auto it = jobs().find(pid);
    if (it != jobs().end()) {
        CloseHandle(it->second.process);
        if (it->second.job) {
            CloseHandle(it->second.job);
        }
        jobs().erase(it);
    }
    return pid;
}

int kill(pid_t pid, int signal) {
    const pid_t key = pid < 0 ? -pid : pid;
    std::lock_guard<std::mutex> lock{jobsMutex()};
    const auto it = jobs().find(key);
    if (it == jobs().end()) {
        return -1;
    }
    if (signal == 0) {
        return WaitForSingleObject(it->second.process, 0) == WAIT_TIMEOUT ? 0 : -1;
    }
    if (it->second.job) {
        TerminateJobObject(it->second.job, 1);
    } else {
        TerminateProcess(it->second.process, 1);
    }
    return 0;
}

std::vector<fs::path> hdrspaceWildcardPaths(const fs::path& pattern) {
    std::vector<fs::path> out;
    const fs::path directory = pattern.has_parent_path() ? pattern.parent_path() : fs::path(".");
    const std::wstring namePattern = pattern.filename().wstring();
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, ec)) {
        if (wildcardMatch(namePattern, entry.path().filename().wstring())) {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

#endif
