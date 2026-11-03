
#ifndef RAYPLAYER_AUDIO_RENDERER_H
#define RAYPLAYER_AUDIO_RENDERER_H

#include <cstdint>
#include <atomic>

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#endif

namespace player_sdk {

    class AudioRenderer {
    public:
        AudioRenderer();
        ~AudioRenderer();

        // 初始化 AAudio 流并启动播放
        // sampleRate: 采样率
        // channels:   声道数
        // 返回值: true 成功, false 失败
        bool init(int sampleRate, int channels);

        // 写入 PCM 数据到音频流（等价于 write）
        // data: PCM 数据指针
        // size: 数据字节数
        // 返回值: 写入的字节数, <0 失败
        int play(const uint8_t* data, int size);

        // 暂停播放
        void pause();

        // 恢复播放
        void resume();

        // 停止播放
        void stop();

        // 释放资源
        void release();

    private:
        AAudioStream* m_stream = nullptr;
        int m_sampleRate = 0;
        int m_channels = 0;
        std::atomic<bool> m_started{false};
    };

} // namespace player_sdk

#endif // RAYPLAYER_AUDIO_RENDERER_H