#pragma once

// Runs the app's background work (analyses, helper requests, the threads that watch merge and
// render jobs) on threads it keeps track of. When the app closes it waits for them instead of
// leaving detached threads running while the viewer and GLFW shut down.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <list>
#include <mutex>
#include <thread>
#include <utility>

namespace hdrspace {

class JobRunner {
public:
    JobRunner() = default;
    JobRunner(const JobRunner&) = delete;
    JobRunner& operator=(const JobRunner&) = delete;
    // Waits for the jobs that are still running (the app calls finish() instead).
    ~JobRunner();

    // Starts `work` on a new thread. Returns false, without running it, once finish() has begun.
    template <typename Work>
    bool start(Work&& work) {
        std::lock_guard<std::mutex> lock{mutex_};
        if (finishing_) {
            return false;
        }
        joinFinishedLocked();
        const auto job = jobs_.emplace(jobs_.end());
        try {
            // The thread marks itself done under the lock, so it cannot do so before this
            // assignment has happened.
            job->thread = std::thread([this, job, work = std::forward<Work>(work)]() mutable {
                work();
                markDone(job);
            });
        } catch (...) {
            jobs_.erase(job);
            throw;
        }
        return true;
    }

    // Refuses new jobs, waits up to `timeout` for the running ones and joins them. Jobs still
    // running afterwards are detached; returns how many there were.
    std::size_t finish(std::chrono::milliseconds timeout);

    std::size_t runningCount() const;

private:
    struct Job {
        std::thread thread;
        bool done = false;
    };

    void markDone(std::list<Job>::iterator job);
    void joinFinishedLocked();

    mutable std::mutex mutex_;
    std::condition_variable jobDone_;
    std::list<Job> jobs_;
    bool finishing_ = false;
};

} // namespace hdrspace
