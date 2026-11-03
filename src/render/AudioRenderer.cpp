
//
// Created by 谢稳 on 2026/10/21.
// Android 音频渲染器（基于 AAudio API，需要 Android 8.0/API 26+）
// 使用 write() 推数据模式，与播放器解码线程架构匹配
//
#include "AudioRenderer.h"
#include <android/log.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PlayerSDK", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PlayerSDK", __VA_ARGS__)

namespace player_sdk {

AudioRenderer::AudioRenderer() = default;

AudioRenderer::~AudioRenderer() {
    release();
}

bool AudioRenderer::init(int sampleRate, int channels) {
    if (sampleRate <= 0 || channels <= 0 || channels > 8) {
        LOGE("Invalid audio parameters: %dHz, %dch", sampleRate, channels);
        return false;
    }

    m_sampleRate = sampleRate;
    m_channels = channels;

    LOGI("AudioRenderer::init: starting init (sampleRate=%d, channels=%d)", sampleRate, channels);

    AAudioStreamBuilder *builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
        LOGE("AAudio_createStreamBuilder failed: %d", result);
        return false;
    }
    LOGI("AAudio_createStreamBuilder OK");

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, sampleRate);
    AAudioStreamBuilder_setChannelCount(builder, channels);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    LOGI("AAudio stream builder configured: %dHz, %dch, PCM_I16, LOW_LATENCY", sampleRate, channels);

    result = AAudioStreamBuilder_openStream(builder, &m_stream);
    if (result != AAUDIO_OK) {
        LOGE("AAudioStreamBuilder_openStream failed: %d", result);
        AAudioStreamBuilder_delete(builder);
        return false;
    }
    LOGI("AAudioStreamBuilder_openStream OK");

    AAudioStreamBuilder_delete(builder);

    LOGI("AAudio stream opened: %dHz, %dch, format=%d",
         AAudioStream_getSampleRate(m_stream),
         AAudioStream_getChannelCount(m_stream),
         AAudioStream_getFormat(m_stream));

    // 启动音频流
    result = AAudioStream_requestStart(m_stream);
    if (result != AAUDIO_OK) {
        LOGE("AAudioStream_requestStart failed: %d", result);
        AAudioStream_close(m_stream);
        m_stream = nullptr;
        return false;
    }
    LOGI("AAudioStream_requestStart OK - audio playback started");

    m_started.store(true);
    return true;
}

int AudioRenderer::play(const uint8_t* data, int size) {
    if (!m_stream || !data || size <= 0) {
        if (size > 0) {
            LOGE("AudioRenderer::play: invalid params (stream=%p, data=%p, size=%d)",
                 m_stream, data, size);
        }
        return -1;
    }

    // 计算需要写入的帧数
    // 每帧 = channels * 2字节(S16)
    int32_t frames = size / (m_channels * sizeof(int16_t));
    if (frames <= 0) {
        LOGI("AudioRenderer::play: frames <= 0 (size=%d, channels=%d)", size, m_channels);
        return 0;
    }

    // 使用阻塞写入，timeout=1秒，防止缓冲区临时满时音频中断
    // 非阻塞(timeout=0)在缓冲区满时会立即返回AAUDIO_WOULD_BLOCK导致音频中断
    aaudio_result_t result = AAudioStream_write(m_stream, data, frames, 1000000000LL);
    if (result < 0) {
        LOGE("AudioRenderer::play: AAudioStream_write failed: %d", result);
        return -1;
    }

    // 返回实际写入的字节数
    int bytesWritten = result * m_channels * sizeof(int16_t);
    LOGI("AudioRenderer::play: wrote %d frames (%d bytes)", result, bytesWritten);

    return bytesWritten;
}

void AudioRenderer::pause() {
    if (!m_stream) return;
    if (!m_started.load()) return;

    aaudio_result_t result = AAudioStream_requestPause(m_stream);
    if (result != AAUDIO_OK) {
        LOGE("AudioRenderer::pause: AAudioStream_requestPause failed: %d", result);
        return;
    }

    m_started.store(false);
    LOGI("AAudio stream paused");
}

void AudioRenderer::resume() {
    if (!m_stream) return;
    if (m_started.load()) return;

    aaudio_result_t result = AAudioStream_requestStart(m_stream);
    if (result != AAUDIO_OK) {
        LOGE("AudioRenderer::resume: AAudioStream_requestStart failed: %d", result);
        return;
    }

    m_started.store(true);
    LOGI("AAudio stream resumed");
}

void AudioRenderer::stop() {
    if (!m_stream) return;

    aaudio_result_t result = AAudioStream_requestStop(m_stream);
    if (result != AAUDIO_OK) {
        LOGE("AudioRenderer::stop: AAudioStream_requestStop failed: %d", result);
        return;
    }

    m_started.store(false);
    LOGI("AAudio stream stopped");
}

void AudioRenderer::release() {
    if (m_stream) {
        // 先停止
        AAudioStream_requestStop(m_stream);
        // 关闭流
        AAudioStream_close(m_stream);
        m_stream = nullptr;
        m_started.store(false);
        LOGI("AAudio stream released");
    }
}

} // namespace player_sdk