#include "platform/ProcessRunner.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#if defined(_WIN32)
#include "platform/WinPosix.h"
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

}  // namespace

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
    ProcessResult result;
    try {
        const pid_t pid = hdrspaceSpawnShell(
            command + " 1>" + quote(stdoutPath.string()) + " 2>" + quote(stderrPath.string()), logPath);
        int status = 0;
        waitpid(pid, &status, 0);
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
    int rc = std::system(shellCommand.c_str());

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
