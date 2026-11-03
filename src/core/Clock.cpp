//
// Created by 谢稳 on 2026/10/21.
//
#include "Clock.h"
#include <chrono>

namespace player_sdk {

    static double nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void Clock::setStreamTime(double seconds) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_streamTime = seconds;
        m_lastSystemMs = nowMs();
    }

    double Clock::getStreamTime() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_streamTime;
    }

    void Clock::setPaused(bool paused) {
        if (paused == m_paused.load()) return;
        m_paused.store(paused);
        if (!paused) {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_lastSystemMs = nowMs();
        }
    }

    void Clock::updateBySystemClock() {
        if (m_paused.load()) return;
        std::lock_guard<std::mutex> lk(m_mutex);
        double cur = nowMs();
        m_streamTime += (cur - m_lastSystemMs) / 1000.0;
        m_lastSystemMs = cur;
    }

    void Clock::reset() {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_streamTime = 0.0;
        m_lastSystemMs = nowMs();
    }

} // namespace player_sdk