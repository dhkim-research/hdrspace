#include "platform/ProcessRunner.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#if defined(_WIN32)
#include <filesystem>
#include <random>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::string readFile(const std::string &path) {
    std::ifstream input(path.c_str());
    if (!input)
        return std::string();
    std::ostringstream oss;
    oss << input.rdbuf();
    return oss.str();
}

std::mutex &childrenMutex() {
    static std::mutex mutex;
    return mutex;
}

// pid -> true when the entry is a process group
std::map<pid_t, bool> &children() {
    static std::map<pid_t, bool> table;
    return table;
}

void unregisterChildProcess(pid_t pid) {
    std::lock_guard<std::mutex> lock{childrenMutex()};
    children().erase(pid);
}

void signalChild(pid_t pid, bool processGroup, int signal) {
    kill(processGroup ? -pid : pid, signal);
}

}  // namespace

void registerChildProcess(pid_t pid, bool processGroup) {
    if (pid <= 0)
        return;
    std::lock_guard<std::mutex> lock{childrenMutex()};
    children()[pid] = processGroup;
}

pid_t waitChildProcess(pid_t pid, int *status, int options) {
    const pid_t result = waitpid(pid, status, options);
    if (result == pid || (result < 0 && errno != EINTR))
        unregisterChildProcess(pid);
    return result;
}

std::size_t childProcessCount() {
    std::lock_guard<std::mutex> lock{childrenMutex()};
    return children().size();
}

std::size_t terminateChildProcesses(std::chrono::milliseconds grace) {
    const auto signalAll = [](int signal) {
        std::lock_guard<std::mutex> lock{childrenMutex()};
        for (const auto &[pid, processGroup] : children())
            signalChild(pid, processGroup, signal);
    };
    const auto waitForWaiters = [](std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (childProcessCount() > 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };
    if (childProcessCount() == 0)
        return 0;
    signalAll(SIGTERM);
    waitForWaiters(grace);
#if !defined(_WIN32)
    if (childProcessCount() > 0) {
        signalAll(SIGKILL);
        waitForWaiters(std::chrono::milliseconds(500));
    }
#endif
    return childProcessCount();
}

void stopChildProcess(pid_t pid, bool processGroup, std::chrono::milliseconds grace) {
    if (pid <= 0)
        return;
    signalChild(pid, processGroup, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + grace;
    while (true) {
        const pid_t result = waitChildProcess(pid, nullptr, WNOHANG);
        if (result == pid || (result < 0 && errno != EINTR))
            return;  // reaped, or not a child of this process any more
        if (std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#if !defined(_WIN32)
    signalChild(pid, processGroup, SIGKILL);
#endif
    pid_t result = -1;
    do {
        result = waitChildProcess(pid, nullptr, 0);
    } while (result < 0 && errno == EINTR);
}

#if defined(_WIN32)
// Windows: the command is a POSIX shell command line; it runs in the bundled busybox sh.
ProcessResult runShellCommandWithCapturedOutput(const std::string &command) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec);
    std::random_device random;
    const std::string id = std::to_string(random()) + std::to_string(random());
    const fs::path stdoutPath = dir / ("hdrspace-stdout-" + id + ".txt");
    const fs::path stderrPath = dir / ("hdrspace-stderr-" + id + ".txt");
    const fs::path logPath = dir / ("hdrspace-shell-" + id + ".txt");
    const auto quote = [](const std::string &value) {
        std::string result = "'";
        for (char ch : value) {
            if (ch == '\'')
                result += "'\\''";
            else
                result += ch;
        }
        return result + "'";
    };
    // Command lines written for macOS start with "zsh -lc '<script>'"; busybox sh has no zsh,
    // so it runs the quoted script itself.
    std::string script = command;
    if (script.rfind("zsh -lc ", 0) == 0)
        script = "eval " + script.substr(8);
    ProcessResult result;
    try {
        const pid_t pid = hdrspaceSpawnShell(
            script + " 1>" + quote(stdoutPath.string()) + " 2>" + quote(stderrPath.string()), logPath);
        registerChildProcess(pid, true);
        int status = 0;
        waitChildProcess(pid, &status, 0);
        result.exitCode = status;
    } catch (const std::exception &e) {
        result.exitCode = 127;
        result.stderrText = e.what();
    }
    result.stdoutText = readFile(stdoutPath.string());
    if (result.stderrText.empty())
        result.stderrText = readFile(stderrPath.string());
    fs::remove(stdoutPath, ec);
    fs::remove(stderrPath, ec);
    fs::remove(logPath, ec);
    return result;
}
#else
std::mutex &childSpawnMutex() {
    static std::mutex mutex;
    return mutex;
}

void resetSignalsInChild() {
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPIPE, &action, nullptr);
    // A mask blocked in the forking thread would stay blocked in the command (SIGTERM included).
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
}

ProcessResult runShellCommandWithCapturedOutput(const std::string &command) {
    char stdoutTemplate[] = "/tmp/hdrspace-stdout-XXXXXX";
    char stderrTemplate[] = "/tmp/hdrspace-stderr-XXXXXX";
    int stdoutFd = mkstemp(stdoutTemplate);
    int stderrFd = mkstemp(stderrTemplate);
    if (stdoutFd < 0 || stderrFd < 0) {
        if (stdoutFd >= 0) {
            close(stdoutFd);
            std::remove(stdoutTemplate);
        }
        if (stderrFd >= 0) {
            close(stderrFd);
            std::remove(stderrTemplate);
        }
        throw std::runtime_error("Could not create temporary log files.");
    }
    close(stdoutFd);
    close(stderrFd);

    std::string shellCommand =
        command + " 1>\"" + std::string(stdoutTemplate) + "\" 2>\"" + std::string(stderrTemplate) + "\"";
    // /bin/sh -c as std::system() did, but in a process group of its own and registered as a
    // child, so the app can stop the command and everything it started when it closes.
    pid_t pid = -1;
    {
        std::lock_guard<std::mutex> lock{childSpawnMutex()};
        pid = fork();
        if (pid == 0) {
            setpgid(0, 0);
            resetSignalsInChild();
            execl("/bin/sh", "sh", "-c", shellCommand.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        if (pid > 0) {
            setpgid(pid, pid);
            registerChildProcess(pid, true);
        }
    }
    int rc = -1;  // as std::system() when the shell could not be started
    if (pid > 0) {
        int status = 0;
        pid_t waited = -1;
        do {
            waited = waitChildProcess(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited == pid)
            rc = status;
    }

    ProcessResult result;
    if (WIFEXITED(rc))
        result.exitCode = WEXITSTATUS(rc);
    else
        result.exitCode = rc;
    result.stdoutText = readFile(stdoutTemplate);
    result.stderrText = readFile(stderrTemplate);
    std::remove(stdoutTemplate);
    std::remove(stderrTemplate);
    return result;
}
#endif
