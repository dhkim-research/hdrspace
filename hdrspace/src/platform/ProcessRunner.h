#ifndef MERGEHDR_DESKTOP_PLATFORM_PROCESSRUNNER_H
#define MERGEHDR_DESKTOP_PLATFORM_PROCESSRUNNER_H

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>

#if defined(_WIN32)
#include "platform/WinPosix.h"
#else
#include <sys/types.h>
#endif

struct ProcessResult {
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

// Runs a shell command line (/bin/sh -c on macOS, busybox sh on Windows) and returns its exit code
// and output. The shell is a registered child (below), so the app can stop it when it closes.
ProcessResult runShellCommandWithCapturedOutput(const std::string &command);

// The child processes started for background work: merge and render jobs, shell commands, the AI
// helper. An entry is a process group (a shell and everything it starts) or a single process.
// Whoever starts a child also waits for it with waitChildProcess(), which forgets it once reaped.
void registerChildProcess(pid_t pid, bool processGroup);
pid_t waitChildProcess(pid_t pid, int *status, int options);
std::size_t childProcessCount();

// When the app closes: SIGTERM to every registered child, then SIGKILL to the ones not reaped by
// their waiters within `grace`. Returns how many are still registered afterwards.
std::size_t terminateChildProcesses(std::chrono::milliseconds grace);

// Stops a child that no other thread waits for: SIGTERM, SIGKILL after `grace`, then reaps it.
void stopChildProcess(pid_t pid, bool processGroup, std::chrono::milliseconds grace);

#if !defined(_WIN32)
// Held while a child's pipes are made (and marked close-on-exec) and while children are forked,
// so no child is forked in between and keeps another child's pipe ends open.
std::mutex &childSpawnMutex();

// In a forked child before exec (async-signal-safe): SIGPIPE back to its default and no blocked
// signals. The app ignores SIGPIPE; the commands it starts expect the default, as in a terminal.
void resetSignalsInChild();
#endif

#endif
