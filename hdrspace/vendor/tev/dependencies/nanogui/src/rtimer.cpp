#include "rtimer.h"

NAMESPACE_BEGIN(nanogui)

RestartableTimer::RestartableTimer(Callback callback, std::chrono::milliseconds delay) :
    m_callback(std::move(callback)),
    m_delay(delay),
    m_should_exit(false),
    m_timer_active(false) {
    m_worker = std::thread(&RestartableTimer::run, this);
}

RestartableTimer::~RestartableTimer() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_should_exit = true;
        m_timer_active = false;
        m_cv.notify_one();
    }
    m_worker.join();
}

void RestartableTimer::restart() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_deadline = std::chrono::steady_clock::now() + m_delay;

    if (!m_timer_active) {
        m_timer_active = true;
        m_cv.notify_one();
    }
}

void RestartableTimer::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_timer_active = false;
}

void RestartableTimer::run() {
    std::unique_lock<std::mutex> lock(m_mutex);

    while (!m_should_exit) {
        // Wait for timer activation
        if (!m_timer_active)
            m_cv.wait(lock);

        if (m_should_exit)
            break;

        while (m_timer_active) {
            m_cv.wait_until(lock, m_deadline);

            // Check if we should exit or timer was cleared
            if (m_should_exit || !m_timer_active)
                break;

            // Check if we've actually reached the deadline (it may have been extended)
            if (std::chrono::steady_clock::now() >= m_deadline) {
                m_timer_active = false;
                lock.unlock();
                m_callback();
                lock.lock();
                break;
            }
            // Otherwise the deadline was extended, loop back to wait until new deadline
        }
    }
}

NAMESPACE_END(nanogui)
