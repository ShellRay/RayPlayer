
//
// Created by 谢稳 on 2026/10/21.
// 适配 FFmpeg 5.0+ API（ch_layout 结构体、swr_alloc_set_opts 新签名等）
// 修复：convertAndCallback 中交错/计划格式不匹配导致的 SIGSEGV 崩溃
//
#include "AudioDecoder.h"
#include <android/log.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PlayerSDK", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PlayerSDK", __VA_ARGS__)

namespace player_sdk {

AudioDecoder::AudioDecoder() = default;

AudioDecoder::~AudioDecoder() {
    release();
}

int AudioDecoder::init(AVCodecParameters *codecpar) {
    // 1. 查找解码器
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        LOGE("Cannot find decoder for codec_id=%d", codecpar->codec_id);
        return -1;
    }

    // 2. 创建并配置解码上下文
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        LOGE("Cannot allocate codec context");
        return -1;
    }

    if (avcodec_parameters_to_context(m_codecCtx, codecpar) < 0) {
        LOGE("Cannot copy codec parameters to context");
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
        return -1;
    }

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        LOGE("Cannot open decoder");
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
        return -1;
    }

    // 3. 保存原始音频参数
    m_sampleRate = m_codecCtx->sample_rate;
    m_channels = m_codecCtx->ch_layout.nb_channels;
    m_sampleFmt = m_codecCtx->sample_fmt;

    // 4. 拷贝输入端的声道布局到成员变量中（使用新 API）
    if (av_channel_layout_copy(&m_inChLayout, &m_codecCtx->ch_layout) < 0) {
        LOGE("Cannot copy input channel layout");
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
        return -1;
    }

    // 5. 设定输出端的声道布局（固定输出 2 个声道的默认立体声布局）
    av_channel_layout_default(&m_outChLayout, 2);

    // 6. 创建重采样上下文
    // 使用 swr_alloc_set_opts（经典 API，兼容性最好）
    // 注意：avcodec_parameters_to_context 已经同时填充了 ch_layout(新) 和 channel_layout(旧)
    // 所以 m_codecCtx->channel_layout 在这里是有效的
    m_swrCtx = swr_alloc_set_opts(
        nullptr,
        av_get_default_channel_layout(OUT_CHANNELS),       // 输出声道布局(int64_t)
        OUT_SAMPLE_FMT,                                    // 输出采样格式
        OUT_SAMPLE_RATE,                                   // 输出采样率
        m_codecCtx->channel_layout,                        // 输入声道布局(int64_t)
        static_cast<enum AVSampleFormat>(m_sampleFmt),     // 输入采样格式
        m_sampleRate,                                      // 输入采样率
        0,                                                 // 日志偏移
        nullptr                                            // 日志上下文
    );

    if (!m_swrCtx) {
        LOGE("Cannot allocate resampler context");
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
        av_channel_layout_uninit(&m_inChLayout);
        av_channel_layout_uninit(&m_outChLayout);
        return -1;
    }

    if (swr_init(m_swrCtx) < 0) {
        LOGE("Cannot initialize resampler");
        swr_free(&m_swrCtx);
        m_swrCtx = nullptr;
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
        av_channel_layout_uninit(&m_inChLayout);
        av_channel_layout_uninit(&m_outChLayout);
        return -1;
    }

    LOGI("AudioDecoder initialized: %dHz, %dch, fmt=%d", m_sampleRate, m_channels, m_sampleFmt);
    return 0;
}

int AudioDecoder::decode(AVPacket *packet) {
    if (!m_codecCtx || !packet) return -1;

    int ret = avcodec_send_packet(m_codecCtx, packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        LOGE("avcodec_send_packet failed: %d", ret);
        return ret;
    }

    // FFmpeg 5.0+: unique_ptr 配合 av_frame_free 需要自定义 deleter
    auto frameDeleter = [](AVFrame *f) { av_frame_free(&f); };
    std::unique_ptr<AVFrame, decltype(frameDeleter)> frame(av_frame_alloc(), frameDeleter);

    while (true) {
        ret = avcodec_receive_frame(m_codecCtx, frame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            LOGE("avcodec_receive_frame failed: %d", ret);
            return ret;
        }

        convertAndCallback(frame.get());
    }

    return 0;
}

int AudioDecoder::convertAndCallback(AVFrame *frame) {
    if (!frame || !m_swrCtx) return -1;

    // 【修复】检查 frame 数据指针有效性
    if (!frame->data[0]) {
        LOGE("convertAndCallback: frame->data[0] is NULL");
        return -1;
    }

    // 【修复】检查样本数有效性
    if (frame->nb_samples <= 0) {
        LOGE("convertAndCallback: invalid nb_samples=%d", frame->nb_samples);
        return -1;
    }

    // 获取输出样本数
    int outSampleCount = swr_get_out_samples(m_swrCtx, frame->nb_samples);
    if (outSampleCount <= 0) {
        outSampleCount = frame->nb_samples;
    }

    // 【关键修复】OUT_SAMPLE_FMT 是 AV_SAMPLE_FMT_S16（交错/packed 格式）
    // 之前错误地使用 av_samples_alloc_array_and_samples 按 planar 分配，
    // 导致 outBuffers[1] 未初始化，后续循环访问时拿到垃圾指针崩溃。
    // 现在直接分配一个连续的交错缓冲区，避免格式不匹配问题。
    int bytesPerSample = av_get_bytes_per_sample(OUT_SAMPLE_FMT);
    if (bytesPerSample <= 0) {
        LOGE("convertAndCallback: av_get_bytes_per_sample failed");
        return -1;
    }

    int outBufferSize = outSampleCount * OUT_CHANNELS * bytesPerSample;

    // 直接分配一个交错的输出缓冲区
    uint8_t *outBuffer = (uint8_t *)av_malloc(outBufferSize);
    if (!outBuffer) {
        LOGE("convertAndCallback: av_malloc failed");
        return -1;
    }

    // 交错格式只需要 outBuffers[0] 指向缓冲区起点
    uint8_t *outBuffers[8] = { outBuffer };
    const uint8_t **inBuffers = (const uint8_t **)frame->data;

    // 重采样（swr_convert 会直接写入交错的 outBuffer）
    int samplesConverted = swr_convert(
        m_swrCtx,
        outBuffers,              // 输出缓冲区 (uint8_t**)
        outSampleCount,          // 输出样本数
        inBuffers,               // 输入缓冲区
        frame->nb_samples        // 输入样本数
    );

    // 【修复】处理 swr_convert 返回值
    if (samplesConverted <= 0) {
        av_free(outBuffer);
        return samplesConverted < 0 ? samplesConverted : 0;
    }

    // 防御性 clamp：样本数不能超过分配范围
    if (samplesConverted > outSampleCount) {
        LOGE("convertAndCallback: samplesConverted(%d) > outSampleCount(%d), clamping",
             samplesConverted, outSampleCount);
        samplesConverted = outSampleCount;
    }

    int outSize = samplesConverted * OUT_CHANNELS * bytesPerSample;

    // 回调输出（数据已经是交错格式，无需额外转换）
    if (m_callback) {
        m_callback(outBuffer, outSize, m_outSampleRate, m_outChannels);
    }

    av_free(outBuffer);
    return 0;
}

void AudioDecoder::flush() {
    if (!m_codecCtx) return;
    avcodec_send_packet(m_codecCtx, nullptr);
}

void AudioDecoder::release() {
    if (m_swrCtx) {
        swr_free(&m_swrCtx);
        m_swrCtx = nullptr;
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    // 无条件释放声道布局（av_channel_layout_uninit 对未初始化的结构体是安全的）
    av_channel_layout_uninit(&m_inChLayout);
    av_channel_layout_uninit(&m_outChLayout);
}

} // namespace player_sdk