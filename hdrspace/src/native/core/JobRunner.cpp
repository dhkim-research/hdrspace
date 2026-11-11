#include "native/core/JobRunner.h"

namespace hdrspace {

JobRunner::~JobRunner() {
    std::unique_lock<std::mutex> lock{mutex_};
    finishing_ = true;
    jobDone_.wait(lock, [this] {
        for (const Job& job : jobs_) {
            if (!job.done) {
                return false;
            }
        }
        return true;
    });
    joinFinishedLocked();
}

void JobRunner::markDone(std::list<Job>::iterator job) {
    std::lock_guard<std::mutex> lock{mutex_};
    job->done = true;
    jobDone_.notify_all();
}

void JobRunner::joinFinishedLocked() {
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (it->done) {
            // The thread only returns after markDone(), so this join does not wait for long.
            if (it->thread.joinable()) {
                it->thread.join();
            }
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
}

std::size_t JobRunner::finish(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock{mutex_};
    finishing_ = true;
    jobDone_.wait_for(lock, timeout, [this] {
        for (const Job& job : jobs_) {
            if (!job.done) {
                return false;
            }
        }
        return true;
    });
    joinFinishedLocked();
    const std::size_t stillRunning = jobs_.size();
    for (Job& job : jobs_) {
        if (job.thread.joinable()) {
            job.thread.detach();
        }
    }
    // A detached job still calls markDone() on its list entry, so the entries stay.
    return stillRunning;
}

std::size_t JobRunner::runningCount() const {
    std::lock_guard<std::mutex> lock{mutex_};
    std::size_t count = 0;
    for (const Job& job : jobs_) {
        count += job.done ? 0 : 1;
    }
    return count;
}

} // namespace hdrspace
