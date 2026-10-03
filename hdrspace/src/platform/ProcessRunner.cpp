#include "platform/ProcessRunner.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

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
