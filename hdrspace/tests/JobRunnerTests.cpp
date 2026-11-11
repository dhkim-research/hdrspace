// Tests for the background-work helpers: the JobRunner that runs the tools' threads, and the
// registry of child processes that the app stops when it closes (platform/ProcessRunner.h).

#include "TestHarness.h"

#include "native/core/JobRunner.h"
#include "platform/ProcessRunner.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#if !defined(_WIN32)
#include <csignal>
#endif

using namespace std::chrono_literals;

TEST_CASE("jobs: finish waits for the running jobs") {
    hdrspace::JobRunner runner;
    auto count = std::make_shared<std::atomic<int>>(0);
    for (int i = 0; i < 3; ++i) {
        CHECK(runner.start([count]() {
            std::this_thread::sleep_for(50ms);
            ++*count;
        }));
    }
    CHECK(runner.finish(5000ms) == 0);
    CHECK(count->load() == 3);
    CHECK(runner.runningCount() == 0);
}

TEST_CASE("jobs: move-only work runs, and no new job starts after finish") {
    hdrspace::JobRunner runner;
    auto value = std::make_unique<int>(7);
    auto seen = std::make_shared<std::atomic<int>>(0);
    CHECK(runner.start([seen, value = std::move(value)]() mutable { *seen = *value; }));
    CHECK(runner.finish(5000ms) == 0);
    CHECK(seen->load() == 7);
    CHECK(!runner.start([seen]() { *seen = 1; }));
    CHECK(seen->load() == 7);
}

TEST_CASE("jobs: a job still running after the timeout is counted and left running") {
    // As in the app, the runner is never destroyed: the detached job still reports to it.
    auto* runner = new hdrspace::JobRunner;
    auto release = std::make_shared<std::atomic<bool>>(false);
    auto ended = std::make_shared<std::atomic<bool>>(false);
    CHECK(runner->start([release, ended]() {
        while (!release->load()) {
            std::this_thread::sleep_for(5ms);
        }
        ended->store(true);
    }));
    const auto before = std::chrono::steady_clock::now();
    CHECK(runner->finish(100ms) == 1);
    CHECK(std::chrono::steady_clock::now() - before < 2s);
    CHECK(!ended->load());
    release->store(true);
    for (int i = 0; i < 400 && runner->runningCount() > 0; ++i) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(runner->runningCount() == 0);
    CHECK(ended->load());
}

#if !defined(_WIN32)
namespace {

bool waitForChildren(std::size_t count) {
    for (int i = 0; i < 500; ++i) {
        if (childProcessCount() >= count) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

} // namespace

TEST_CASE("children: a shell command gives its exit code and output") {
    const ProcessResult ok = runShellCommandWithCapturedOutput("{ echo hello; echo oops 1>&2; }");
    CHECK(ok.exitCode == 0);
    CHECK(ok.stdoutText == "hello\n");
    CHECK(ok.stderrText == "oops\n");
    CHECK(runShellCommandWithCapturedOutput("exit 3").exitCode == 3);
    CHECK(childProcessCount() == 0);
}

TEST_CASE("children: the commands get the default SIGPIPE although the app ignores it") {
    // With SIGPIPE ignored, `yes` would report "Broken pipe" when `head` stops reading.
    const auto previous = std::signal(SIGPIPE, SIG_IGN);
    const ProcessResult result = runShellCommandWithCapturedOutput("{ yes | head -n 1; }");
    std::signal(SIGPIPE, previous);
    CHECK(result.exitCode == 0);
    CHECK(result.stdoutText == "y\n");
    CHECK(result.stderrText.empty());
}

TEST_CASE("children: terminateChildProcesses stops a running command") {
    hdrspace::JobRunner runner;
    auto result = std::make_shared<ProcessResult>();
    CHECK(runner.start([result]() { *result = runShellCommandWithCapturedOutput("sleep 30"); }));
    CHECK(waitForChildren(1));
    const auto before = std::chrono::steady_clock::now();
    CHECK(terminateChildProcesses(2000ms) == 0);
    CHECK(runner.finish(5000ms) == 0);
    CHECK(std::chrono::steady_clock::now() - before < 3s);
    CHECK(result->exitCode != 0);
}

TEST_CASE("children: a command that ignores SIGTERM is killed after the grace period") {
    hdrspace::JobRunner runner;
    auto result = std::make_shared<ProcessResult>();
    CHECK(runner.start([result]() { *result = runShellCommandWithCapturedOutput("trap '' TERM; sleep 30"); }));
    CHECK(waitForChildren(1));
    std::this_thread::sleep_for(200ms);  // the shell sets its trap first
    const auto before = std::chrono::steady_clock::now();
    CHECK(terminateChildProcesses(300ms) == 0);
    const auto elapsed = std::chrono::steady_clock::now() - before;
    CHECK(elapsed >= 300ms);
    CHECK(elapsed < 3s);
    CHECK(runner.finish(5000ms) == 0);
    CHECK(result->exitCode != 0);
}
#endif
