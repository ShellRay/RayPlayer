#ifndef RAYPLAYER_CLOCK_H
#define RAYPLAYER_CLOCK_H

#include <atomic>
#include <mutex>

namespace player_sdk {

    class Clock {
    public:
        void setStreamTime(double seconds);
        double getStreamTime() const;

        void setPaused(bool paused);
        bool isPaused() const { return m_paused.load(); }

        // 基于系统时间推进，用于无音频流时驱动视频
        void updateBySystemClock();
        void reset();

    private:
        mutable std::mutex m_mutex;
        double m_streamTime = 0.0;
        double m_lastSystemMs = 0.0;
        std::atomic<bool> m_paused{false};
    };

} // namespace player_sdk

#endif // RAYPLAYER_CLOCK_H