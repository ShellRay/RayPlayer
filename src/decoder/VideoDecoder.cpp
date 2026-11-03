#include "VideoDecoder.h"
#include <cstring>
#include <android/log.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PlayerSDK", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PlayerSDK", __VA_ARGS__)

namespace player_sdk {

VideoDecoder::~VideoDecoder() {
    close();
}

bool VideoDecoder::open(AVFormatContext* fmtCtx, int streamIndex, FrameCallback cb) {
    if (!fmtCtx || streamIndex < 0 || streamIndex >= (int)fmtCtx->nb_streams) return false;
    m_stream = fmtCtx->streams[streamIndex];
    m_timeBase = m_stream->time_base;
    m_callback = std::move(cb);

    const AVCodec* codec = avcodec_find_decoder(m_stream->codecpar->codec_id);
    if (!codec) {
        LOGE("VideoDecoder: cannot find decoder for codec_id=%d", m_stream->codecpar->codec_id);
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        LOGE("VideoDecoder: cannot allocate codec context");
        return false;
    }

    // 【修复】avcodec_parameters_to_context 返回 0 表示成功，负数表示失败
    // 原代码用 ! 判断导致成功时反而进入错误分支
    if (avcodec_parameters_to_context(m_codecCtx, m_stream->codecpar) < 0) {
        LOGE("VideoDecoder: cannot copy codec parameters to context");
        close();
        return false;
    }

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        LOGE("VideoDecoder: cannot open decoder");
        close();
        return false;
    }

    m_frame = av_frame_alloc();
    LOGI("VideoDecoder opened: codec=%s, width=%d, height=%d, pixel_fmt=%d",
         codec->name, m_codecCtx->width, m_codecCtx->height, m_codecCtx->pix_fmt);
    return true;
}

bool VideoDecoder::decode(AVPacket* packet) {
    if (!m_codecCtx || !m_frame) return false;

    int ret = avcodec_send_packet(m_codecCtx, packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) return false;

    while (true) {
        ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return false;

        convertAndNotify(m_frame);
        av_frame_unref(m_frame);
    }
    return true;
}

void VideoDecoder::convertAndNotify(AVFrame* frame) {
    if (!frame || !frame->data || !frame->data[0]) {
        LOGE("VideoDecoder: invalid frame in convertAndNotify");
        return;
    }

    if (frame->format != AV_PIX_FMT_YUV420P) {
        LOGI("VideoDecoder: unsupported pixel format=%d, skipping", frame->format);
        return;
    }

    VideoFrame out;
    out.width = frame->width;
    out.height = frame->height;
    const char* fmtName = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
    out.pixelFormat = fmtName ? fmtName : "unknown";

    if (frame->pts != AV_NOPTS_VALUE) {
        out.pts = frame->pts * av_q2d(m_timeBase);
    } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
        out.pts = frame->best_effort_timestamp * av_q2d(m_timeBase);
    } else {
        out.pts = 0.0;
    }

    int ySize = frame->width * frame->height;
    int uvSize = ySize / 4;
    out.yuv.resize(ySize + uvSize * 2);
    out.yStride = frame->width;
    out.uStride = frame->width / 2;
    out.vStride = frame->width / 2;

    for (int i = 0; i < frame->height; ++i) {
        memcpy(out.yuv.data() + i * out.yStride, frame->data[0] + i * frame->linesize[0], frame->width);
    }
    for (int i = 0; i < frame->height / 2; ++i) {
        memcpy(out.yuv.data() + ySize + i * out.uStride, frame->data[1] + i * frame->linesize[1], frame->width / 2);
    }
    for (int i = 0; i < frame->height / 2; ++i) {
        memcpy(out.yuv.data() + ySize + uvSize + i * out.vStride, frame->data[2] + i * frame->linesize[2], frame->width / 2);
    }

    if (m_callback) m_callback(out);
}

void VideoDecoder::flush() {
    if (!m_codecCtx) return;
    avcodec_send_packet(m_codecCtx, nullptr);
    while (avcodec_receive_frame(m_codecCtx, m_frame) == 0) {
        convertAndNotify(m_frame);
        av_frame_unref(m_frame);
    }
}

void VideoDecoder::close() {
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_frame) { av_frame_free(&m_frame); m_frame = nullptr; }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); m_codecCtx = nullptr; }
    m_stream = nullptr;
}

} // namespace player_sdk
