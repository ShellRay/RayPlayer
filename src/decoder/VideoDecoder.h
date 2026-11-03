#ifndef RAYPLAYER_VIDEO_DECODER_H
#define RAYPLAYER_VIDEO_DECODER_H

#include <string>
#include <functional>
#include <vector>

// ✅ 1. 包裹所有 FFmpeg 的头文件
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
// swscale 如果现在还没用到，可以先不引入，保持简洁
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h> // 新增这一行
}
// #include <libavcodec/avcodec.h>
// #include <libavformat/avformat.h>
// #include <libswscale/swscale.h>

namespace player_sdk {

struct VideoFrame {
    int width = 0;
    int height = 0;
    double pts = 0.0;
    std::string pixelFormat;
    std::vector<uint8_t> yuv;
    int yStride = 0;
    int uStride = 0;
    int vStride = 0;
};

class VideoDecoder {
public:
    using FrameCallback = std::function<void(const VideoFrame&)>;

    ~VideoDecoder();

    bool open(AVFormatContext* fmtCtx, int streamIndex, FrameCallback cb);
    bool decode(AVPacket* packet);
    void flush();
    void close();

private:
    void convertAndNotify(AVFrame* frame);

    AVCodecContext* m_codecCtx = nullptr;
    SwsContext* m_swsCtx = nullptr;
    AVFrame* m_frame = nullptr;
    AVStream* m_stream = nullptr;
    FrameCallback m_callback;
    AVRational m_timeBase{};
};

} // namespace player_sdk

#endif // RAYPLAYER_VIDEO_DECODER_H
