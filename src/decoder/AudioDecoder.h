
#ifndef RAYPLAYER_AUDIO_DECODER_H
#define RAYPLAYER_AUDIO_DECODER_H

#include <cstdint>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <vector>

// FFmpeg 头文件
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

namespace player_sdk {

// 前向声明
class PlayerImpl;

// 音频帧回调：解码器解码出一帧 PCM 数据时通过回调交给渲染层
// data:     PCM 原始数据指针（统一转换为 AV_SAMPLE_FMT_S16，小端）
// size:     数据字节数
// sample_rate: 采样率（输出统一为 48000）
// channels:  声道数（输出统一为 2 声道立体声）
using AudioFrameCallback = std::function<void(uint8_t *data, int size,
                                               int sample_rate, int channels)>;

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    // 初始化：传入音频流的编解码器参数（从 AVFormatContext 的 stream->codecpar 取）
    // 返回值: 0 成功, -1 失败
    int init(AVCodecParameters *codecpar);

    // 解码：传入一个压缩数据包，解码后通过 callback 输出 PCM 帧
    // packet:  解封装线程送来的 AVPacket（内部会 free）
    // 返回值: 0 成功, <0 FFmpeg 错误码
    int decode(AVPacket *packet);

    // 刷新解码器缓冲区（seek 后必须调用，否则会有旧数据残留）
    void flush();

    // 释放所有 FFmpeg 资源
    void release();

    // 设置 PCM 数据回调（由 AudioRenderer 注册）
    void setCallback(AudioFrameCallback callback) { m_callback = callback; }

    // 获取解码后的音频参数（供渲染层使用）
    int getSampleRate() const { return m_outSampleRate; }
    int getChannels() const { return m_outChannels; }

private:
    // 将解码出的 AVFrame 转换为统一格式（48kHz / 立体声 / S16）并回调
    int convertAndCallback(AVFrame *frame);

    // FFmpeg 解码上下文
    AVCodecContext *m_codecCtx = nullptr;
    // 重采样上下文（格式/采样率/声道数转换）
    SwrContext *m_swrCtx = nullptr;
    // 原始音频参数
    int m_sampleRate = 0;
    int m_channels = 0;

    // 输入源的声道布局（新 API）
    AVChannelLayout m_inChLayout{};
    // 输出重采样的声道布局（例如立体声）
    AVChannelLayout m_outChLayout{};

    int m_sampleFmt = AV_SAMPLE_FMT_NONE;
    // 统一输出参数（SDK 内部统一格式，方便渲染层处理）
    static constexpr int OUT_SAMPLE_RATE = 48000;
    static constexpr int OUT_CHANNELS = 2;
    // 输出采样格式：交错（packed）S16，Android AudioTrack 标准格式
    static constexpr enum AVSampleFormat OUT_SAMPLE_FMT = AV_SAMPLE_FMT_S16;
    int m_outSampleRate = OUT_SAMPLE_RATE;
    int m_outChannels = OUT_CHANNELS;
    // 回调函数
    AudioFrameCallback m_callback;
};

} // namespace player_sdk

#endif // RAYPLAYER_AUDIO_DECODER_H