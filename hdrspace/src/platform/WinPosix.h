// Windows replacements for the few POSIX process calls hdrspace uses (fork/exec of a shell,
// waitpid, kill, glob, the executable path). Only included on _WIN32; macOS and Linux use the
// system calls directly.
#pragma once
#if defined(_WIN32)

#include <csignal>
#include <filesystem>
#include <string>
#include <vector>

#include <io.h>
#include <process.h>

typedef int pid_t;
#if !defined(_SSIZE_T_DEFINED) && !defined(__MINGW32__)
typedef long long ssize_t;
#define _SSIZE_T_DEFINED
#endif

#ifndef WNOHANG
#define WNOHANG 1
#endif
#ifndef WIFEXITED
#define WIFEXITED(status) (true)
#define WEXITSTATUS(status) (status)
#endif
#ifndef SIGTERM
#define SIGTERM 15
#endif

// Runs `command` with the POSIX shell shipped next to hdrspace.exe (busybox sh), with stdout and
// stderr written to logPath. The shell and everything it starts belong to one job object, so
// kill() stops the whole command line, like killing the process group on macOS.
pid_t hdrspaceSpawnShell(const std::string& command, const std::filesystem::path& logPath);

// waitpid(): status is the exit code. Returns pid when finished, 0 (WNOHANG) while running.
pid_t waitpid(pid_t pid, int* status, int options);

// kill(): signal 0 tests whether the process is still running; any other signal terminates the
// job of a process started by hdrspaceSpawnShell (a negative pid means the same process).
int kill(pid_t pid, int signal);

// Full path of hdrspace.exe.
std::filesystem::path hdrspaceExecutablePathWin();

// Paths matching a pattern with * and ? in its last component, sorted (glob() on POSIX).
std::vector<std::filesystem::path> hdrspaceWildcardPaths(const std::filesystem::path& pattern);

// The per-user data folder (%LOCALAPPDATA%), used where macOS uses ~/Library.
std::filesystem::path hdrspaceLocalAppData();

#endif
