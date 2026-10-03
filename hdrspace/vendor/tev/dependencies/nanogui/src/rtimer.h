#pragma once

#include <nanogui/object.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

NAMESPACE_BEGIN(nanogui)

/**
 * A thread-safe timer that executes a callback after a fixed delay.
 *
 * The timer can be restarted at any time: calling restart() while the timer
 * is already running cancels the pending callback and starts a new countdown.
 * Only the final callback executes after the delay period expires without
 * further restarts.
 *
 * The callback is executed asynchronously on a dedicated worker thread.
 * All public methods are thread-safe and can be called from any thread.
 */
class RestartableTimer : public Object {
public:
    using Callback = std::function<void()>;

    /**
     * Constructs a timer with a callback and fixed delay.
     *
     * \param callback Function to call when the timer expires
     * \param delay Time to wait before executing the callback
     */
    RestartableTimer(Callback callback, std::chrono::milliseconds delay);

    /// Destructor. Stops the timer and waits for the worker thread to finish.
    ~RestartableTimer();

    /**
     * Starts or restarts the timer countdown.
     * If the timer is already running, cancels the pending callback and starts
     * a new countdown from the current time.
     */
    void restart();

    /**
     * Cancels any pending callback execution.
     * If the timer is running, it will be stopped without executing the callback.
     */
    void clear();

private:
    /**
     * Worker thread main loop.
     * Waits for timer activation and executes callbacks after the specified delay.
     */
    void run();

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_worker;
    const Callback m_callback;
    const std::chrono::milliseconds m_delay;
    std::chrono::steady_clock::time_point m_deadline;
    bool m_should_exit;
    bool m_timer_active;
};
NAMESPACE_END(nanogui)
