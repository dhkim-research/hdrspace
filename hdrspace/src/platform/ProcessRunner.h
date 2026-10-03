#ifndef MERGEHDR_DESKTOP_PLATFORM_PROCESSRUNNER_H
#define MERGEHDR_DESKTOP_PLATFORM_PROCESSRUNNER_H

#include <string>

struct ProcessResult {
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

ProcessResult runShellCommandWithCapturedOutput(const std::string &command);

#endif
