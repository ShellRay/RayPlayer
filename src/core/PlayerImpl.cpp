

//
// Created by 谢稳 on 2026/10/21.
//
#include "PlayerImpl.h"
#include <android/log.h>
#include <android/native_window.h>
#include "../src/render/AudioRenderer.h"  // [修改1] 新增：引入音频渲染器头文件

#include "player/Player.h"
#include <functional>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PlayerSDK", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PlayerSDK", __VA_ARGS__)

namespace player_sdk {


PlayerImpl::PlayerImpl() = default;

PlayerImpl::~PlayerImpl() {
    release();
}

int PlayerImpl::setDataSource(const std::string &url) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state.load() != PlayerState::IDLE && m_state.load() != PlayerState::STOPPED) {
        LOGE("setDataSource illegal state");
        return -1;
    }
    if (url.empty()) {
        postError(ErrorCode::INVALID_SOURCE, "url is empty");
        return -2;
    }
    m_url = url;
    transitTo(PlayerState::INITIALIZED);
    return 0;
}

int PlayerImpl::prepareAsync() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state.load() != PlayerState::INITIALIZED) {
            LOGE("prepareAsync illegal state");
            return -1;
        }
        transitTo(PlayerState::PREPARING);
        m_abortFlag.store(false);
    }
    // 先把旧线程收干净，避免重复 prepare 导致线程泄漏
    if (m_prepareThread.joinable()) m_prepareThread.join();
    m_prepareThread = std::thread(&PlayerImpl::doPrepare, this);
    return 0;
}

int PlayerImpl::prepare() {
    int ret = prepareAsync();
    if (ret != 0) return ret;
    if (m_prepareThread.joinable()) m_prepareThread.join();
    return m_state.load() == PlayerState::PREPARED ? 0 : -3;
}

void PlayerImpl::doPrepare() {
    AVFormatContext *ctx = nullptr;
    int ret = avformat_open_input(&ctx, m_url.c_str(), nullptr, nullptr);
    if (ret < 0) {
        LOGE("avformat_open_input failed %d", ret);
        transitTo(PlayerState::ERROR);
        postError(ErrorCode::IO_ERROR, "open input failed");
        return;
    }

    ret = avformat_find_stream_info(ctx, nullptr);
    if (ret < 0) {
        avformat_close_input(&ctx);
        transitTo(PlayerState::ERROR);
        postError(ErrorCode::DECODE_ERROR, "find stream info failed");
        return;
    }

    if (m_abortFlag.load()) {
        avformat_close_input(&ctx);
        return;
    }

    int audioIdx = -1, videoIdx = -1;
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        AVMediaType type = ctx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_AUDIO && audioIdx < 0) audioIdx = static_cast<int>(i);
        if (type == AVMEDIA_TYPE_VIDEO && videoIdx < 0) videoIdx = static_cast<int>(i);
    }

    LOGI("doPrepare: audioIdx=%d, videoIdx=%d, nb_streams=%u", audioIdx, videoIdx, ctx->nb_streams);

    if (audioIdx < 0 && videoIdx < 0) {
        avformat_close_input(&ctx);
        transitTo(PlayerState::ERROR);
        postError(ErrorCode::DECODE_ERROR, "no valid stream");
        return;
    }

    // 初始化音频解码器
    std::unique_ptr<AudioDecoder> audioDecoder;
    if (audioIdx >= 0) {
        audioDecoder = std::make_unique<AudioDecoder>();
        if (audioDecoder->init(ctx->streams[audioIdx]->codecpar) < 0) {
            LOGE("AudioDecoder init failed");
            audioDecoder = nullptr;
        }
    }

    // 初始化视频解码器
    std::unique_ptr<VideoDecoder> videoDecoder;
    if (videoIdx >= 0) {
        videoDecoder = std::make_unique<VideoDecoder>();
        VideoDecoder::FrameCallback videoCb = std::bind(&PlayerImpl::onVideoFrame, this, std::placeholders::_1);
        if (!videoDecoder->open(ctx, videoIdx, videoCb)) {
            LOGE("VideoDecoder open failed");
            videoDecoder = nullptr;
        }
    }

    if (m_abortFlag.load()) {
        avformat_close_input(&ctx);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_fmtCtx = ctx;
        m_audioStreamIndex = audioIdx;
        m_videoStreamIndex = videoIdx;
        m_audioDecoder = std::move(audioDecoder);
        m_videoDecoder = std::move(videoDecoder);

        m_info.duration_ms = (ctx->duration > 0)
            ? static_cast<long>(ctx->duration / (AV_TIME_BASE / 1000)) : 0;
        if (videoIdx >= 0) {
            m_info.width = ctx->streams[videoIdx]->codecpar->width;
            m_info.height = ctx->streams[videoIdx]->codecpar->height;
        }
        if (audioIdx >= 0) {
            m_info.audio_sample_rate = ctx->streams[audioIdx]->codecpar->sample_rate;
            m_info.audio_channel_count = ctx->streams[audioIdx]->codecpar->ch_layout.nb_channels;
        }
        transitTo(PlayerState::PREPARED);
    }

    // [修改2] 新增：在 prepare 阶段初始化 AudioRenderer
    // 注意：此时 m_info 中已经包含了音频参数（sample_rate, channel_count）
    // [修复] 增加 m_audioDecoder 非空检查，确保解码器已创建成功
    if (audioIdx >= 0 && m_audioDecoder) {
        // [修复] 使用 AudioDecoder 的输出参数（48kHz/立体声）初始化 AudioRenderer，
        // 而不是原始流参数，确保 AAudio 流格式与解码输出的 PCM 数据格式完全一致
        LOGI("doPrepare: initializing AudioRenderer (decoder output: rate=%d, ch=%d)",
             m_audioDecoder->getSampleRate(), m_audioDecoder->getChannels());
        if (m_audioRenderer == nullptr) {
            m_audioRenderer = std::make_unique<AudioRenderer>();
        }
        bool audioInitResult = m_audioRenderer->init(m_audioDecoder->getSampleRate(), m_audioDecoder->getChannels());
        LOGI("doPrepare: AudioRenderer::init returned %d", audioInitResult);
        if (!audioInitResult) {
            LOGE("doPrepare: AudioRenderer init FAILED! No audio output.");
            m_audioRenderer.reset();
        } else {
            LOGI("doPrepare: AudioRenderer initialized successfully");

            // [核心修复] 注册音频解码回调！
            // AudioDecoder 解码出 PCM 帧后，会自动调用此回调函数，将数据传递给 AudioRenderer 播放
            // 对比视频端：VideoDecoder::open() 在构造时直接传入了回调
            // 音频端 AudioDecoder::init() 不接收回调参数，必须单独调用 setCallback() 注册
            m_audioDecoder->setCallback([this](uint8_t *data, int size, int sample_rate, int channels) {
                onAudioFrame(data, size, sample_rate, channels);
            });
            LOGI("doPrepare: AudioDecoder callback registered successfully");
        }
    } else {
        if (audioIdx >= 0 && !m_audioDecoder) {
            LOGE("doPrepare: AudioDecoder not created, skipping AudioRenderer init");
        }
    }

    LOGI("doPrepare done: audioDecoder=%p, videoDecoder=%p, audioRenderer=%p",
         m_audioDecoder.get(), m_videoDecoder.get(),
         m_audioRenderer ? m_audioRenderer.get() : nullptr);

    if (m_listener) {
        m_listener->onPrepared(m_info);
        m_listener->onEvent(PlayerEvent::PREPARED);
    }
}

int PlayerImpl::start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state.load() != PlayerState::PREPARED && m_state.load() != PlayerState::PAUSED) {
        LOGE("start illegal state: %d", static_cast<int>(m_state.load()));
        return -1;
    }

    // 确保窗口指针有效
    if (m_nativeWindow == nullptr) {
        LOGE("Cannot start: window is null");
        return -1;
    }

    // 首次启动时启动解封装线程和音频解码线程
    if (m_state.load() == PlayerState::PREPARED) {
        LOGI("start: creating threads (PREPARED)");
        // 启动音频解码线程
        if (m_audioStreamIndex >= 0 && m_audioDecoder) {
            m_audioDecoderThread = std::thread(&PlayerImpl::doAudioDecode, this);
            LOGI("start: audio decode thread created");
        } else {
            LOGE("start: skipping audio thread (audioIdx=%d, decoder=%p)",
                 m_audioStreamIndex, m_audioDecoder.get());
        }
        // 启动视频解码线程
        if (m_videoStreamIndex >= 0 && m_videoDecoder) {
            m_videoDecoderThread = std::thread(&PlayerImpl::doVideoDecode, this);
            LOGI("start: video decode thread created");
        } else {
            LOGE("start: skipping video thread (videoIdx=%d, decoder=%p)",
                 m_videoStreamIndex, m_videoDecoder.get());
        }
        // 启动解封装线程
        m_demuxThread = std::thread(&PlayerImpl::doDemux, this);
        LOGI("start: demux thread created");
    } else {
        LOGE("start: not PREPARED state, state=%d", static_cast<int>(m_state.load()));
    }

    transitTo(PlayerState::STARTED);
    if (m_listener) m_listener->onEvent(PlayerEvent::STARTED);
    return 0;
}

int PlayerImpl::pause() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state.load() != PlayerState::STARTED) return -1;
    transitTo(PlayerState::PAUSED);
    m_audioClock.setPaused(true);
    if (m_listener) m_listener->onEvent(PlayerEvent::PAUSED);
    return 0;
}

int PlayerImpl::stop() {
    m_abortFlag.store(true);
    // 通知条件变量唤醒阻塞的线程
    m_audioQueueCV.notify_all();
    m_videoQueueCV.notify_all();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        transitTo(PlayerState::STOPPED);
    }
    // 等待所有线程退出（必须先 join 线程，确保回调不再访问 m_audioRenderer）
    if (m_demuxThread.joinable()) m_demuxThread.join();
    if (m_audioDecoderThread.joinable()) m_audioDecoderThread.join();
    if (m_videoDecoderThread.joinable()) m_videoDecoderThread.join();
    if (m_prepareThread.joinable()) m_prepareThread.join();
    // 线程全部退出后，安全地停止并释放音频渲染器
    if (m_audioRenderer) {
        m_audioRenderer->stop();
        m_audioRenderer.reset();
        LOGI("stop: AudioRenderer stopped and released");
    }
    // 释放资源
    freeDemuxer();
    freePacketQueue(m_audioPacketQueue, m_audioQueueMutex);
    freePacketQueue(m_videoPacketQueue, m_videoQueueMutex);
    if (m_listener) m_listener->onEvent(PlayerEvent::STOPPED);
    return 0;
}

void PlayerImpl::release() {
    m_abortFlag.store(true);
    m_audioQueueCV.notify_all();
    m_videoQueueCV.notify_all();
    if (m_prepareThread.joinable()) m_prepareThread.join();
    if (m_demuxThread.joinable()) m_demuxThread.join();
    if (m_audioDecoderThread.joinable()) m_audioDecoderThread.join();
    if (m_videoDecoderThread.joinable()) m_videoDecoderThread.join();
    freeDemuxer();
    freePacketQueue(m_audioPacketQueue, m_audioQueueMutex);
    freePacketQueue(m_videoPacketQueue, m_videoQueueMutex);
    freeAudioDecoder();
    freeVideoDecoder();
    // [修改4] 新增：释放音频渲染器
    freeAudioRenderer();
    freeVideoRenderer();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state.store(PlayerState::IDLE);
        m_listener = nullptr;
    }
}

void PlayerImpl::freeDemuxer() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fmtCtx) {
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    m_audioStreamIndex = -1;
    m_videoStreamIndex = -1;
}

void PlayerImpl::freePacketQueue(std::queue<AVPacket*> &queue, std::mutex &mutex) {
    std::lock_guard<std::mutex> lock(mutex);
    while (!queue.empty()) {
        AVPacket *packet = queue.front();
        queue.pop();
        av_packet_free(&packet);
    }
}

void PlayerImpl::freeAudioDecoder() {
    // AudioDecoder 由 unique_ptr 管理，自动释放
}

// [修改5] 新增：释放音频渲染器
void PlayerImpl::freeAudioRenderer() {
    if (m_audioRenderer) {
        m_audioRenderer->release();
        m_audioRenderer.reset();
        LOGI("freeAudioRenderer: released");
    }
}

void PlayerImpl::freeVideoDecoder() {
    // VideoDecoder 由 unique_ptr 管理，自动释放
}

void PlayerImpl::freeVideoRenderer() {
    // 先释放渲染器（内部会释放 EGL 资源，但不释放 window 引用）
    {
        std::lock_guard<std::mutex> lock(m_windowMutex);
        if (m_videoRenderer) {
            m_videoRenderer->release();
            m_videoRenderer.reset();
        }
        // 释放 ANativeWindow 引用（PlayerImpl 拥有该引用）
        if (m_nativeWindow) {
            ANativeWindow_release(reinterpret_cast<ANativeWindow*>(m_nativeWindow));
            m_nativeWindow = nullptr;
        }
    }
    LOGI("freeVideoRenderer: released");
}

int PlayerImpl::seekTo(long msec) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_fmtCtx || m_audioStreamIndex < 0) return -1;
    int64_t ts = av_rescale_q(msec, AV_TIME_BASE_Q, m_fmtCtx->streams[m_audioStreamIndex]->time_base);
    int ret = av_seek_frame(m_fmtCtx, m_audioStreamIndex, ts, AVSEEK_FLAG_BACKWARD);
    // 真正拖动还要清空 PacketQueue、重置解码器和时钟，否则花屏跳音
    if (ret < 0) postError(ErrorCode::DECODE_ERROR, "seek failed");
    return ret;
}

long PlayerImpl::getCurrentPosition() {
    return static_cast<long>(m_audioClock.getStreamTime() * 1000.0);
}

long PlayerImpl::getDuration() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_info.duration_ms;
}

int PlayerImpl::setVolume(float left, float right) {
    // 后面透传给音频渲染层
    (void)left; (void)right;
    return 0;
}

int PlayerImpl::setPlaybackSpeed(float speed) {
    (void)speed;
    return 0;
}

void PlayerImpl::setListener(PlayerListener *listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listener = listener;
}

PlayerState PlayerImpl::getState() const {
    return m_state.load();
}

void PlayerImpl::setVideoSurface(void* window) {
    std::lock_guard<std::mutex> lock(m_windowMutex);
    LOGI("setVideoSurface: window=%p", window);

    // 释放旧的 window 引用
    if (m_nativeWindow) {
        ANativeWindow_release(reinterpret_cast<ANativeWindow*>(m_nativeWindow));
    }
    // 存储新的 window 指针（调用方通过 ANativeWindow_fromSurface 获取的引用会转移给我们）
    m_nativeWindow = window;
    if (window) {
        ANativeWindow_acquire(reinterpret_cast<ANativeWindow*>(window));
    }

    // 如果视频渲染器未初始化且有窗口，则创建渲染器
    if (!m_videoRenderer && window) {
        LOGI("setVideoSurface: creating VideoRenderer");
        m_videoRenderer = std::make_unique<VideoRenderer>();
        bool initResult = m_videoRenderer->init(window);
        LOGI("setVideoSurface: VideoRenderer::init returned %d", initResult);
        if (!initResult) {
            LOGE("setVideoSurface: VideoRenderer init FAILED! Video will not render.");
            m_videoRenderer.reset();
        }
    } else if (!window) {
        LOGI("setVideoSurface: window is null, releasing renderer");
        if (m_videoRenderer) {
            m_videoRenderer->release();
            m_videoRenderer.reset();
        }
    }
}

    void PlayerImpl::updateSurface(void* window) {
    LOGI("updateSurface: window=%p", window);

    if (!window) {
        LOGE("updateSurface: window is null!");
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    // 1. 保存新的窗口指针
    m_nativeWindow = static_cast<ANativeWindow*>(window);

    // 2. 如果还没有创建渲染器，则创建它
    if (!m_videoRenderer) {
        m_videoRenderer = std::make_unique<VideoRenderer>();
    }

    // 3. 尝试初始化渲染器
    if (!m_videoRenderer->init(m_nativeWindow)) {
        LOGE("updateSurface: VideoRenderer init FAILED!");
        // 初始化失败，释放渲染器，下次再试
        m_videoRenderer.reset();
        return;
    }

    LOGI("updateSurface: VideoRenderer init SUCCESS!");
}

void PlayerImpl::doDemux() {
    LOGI("doDemux: thread started");
    AVPacket *packet = av_packet_alloc();
    int videoPacketCount = 0;
    int audioPacketCount = 0;
    while (!m_abortFlag.load()) {
        int ret = av_read_frame(m_fmtCtx, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                LOGI("doDemux: EOF reached, video_packets=%d, audio_packets=%d",
                     videoPacketCount, audioPacketCount);
                // 流结束 -> 通知上层
                if (m_listener) m_listener->onEvent(PlayerEvent::COMPLETED);
                break;
            }
            LOGE("doDemux: av_read_frame failed %d", ret);
            postError(ErrorCode::DECODE_ERROR, "read frame failed");
            break;
        }
        // 根据 stream_index 分发到不同队列
        if (packet->stream_index == m_audioStreamIndex) {
            std::lock_guard<std::mutex> lock(m_audioQueueMutex);
            m_audioPacketQueue.push(av_packet_clone(packet));
            m_audioQueueCV.notify_one();
            audioPacketCount++;
        } else if (packet->stream_index == m_videoStreamIndex) {
            std::lock_guard<std::mutex> lock(m_videoQueueMutex);
            m_videoPacketQueue.push(av_packet_clone(packet));
            m_videoQueueCV.notify_one();
            videoPacketCount++;
            if (videoPacketCount <= 5) {
                LOGI("doDemux: video packet #%d, stream_index=%d, size=%d",
                     videoPacketCount, packet->stream_index, packet->size);
            }
        } else {
            // 字幕等非音视频流直接丢弃
            LOGI("doDemux: dropping non-audio/video packet, stream_index=%d", packet->stream_index);
            av_packet_unref(packet);
            packet = av_packet_alloc();
            continue;
        }
        // 当前包数据已经拷贝到队列里了，准备新包给下一次 read_frame 用
        av_packet_unref(packet);
        packet = av_packet_alloc();
    }
    av_packet_free(&packet);
    LOGI("doDemux: thread exiting");
}

void PlayerImpl::doAudioDecode() {
    LOGI("doAudioDecode: thread started");
    while (!m_abortFlag.load()) {
        AVPacket *packet = popAudioPacket();
        if (!packet) {
            LOGI("doAudioDecode: no more packets, exiting");
            break;
        }

        if (m_audioDecoder) {
            m_audioDecoder->decode(packet);
        } else {
            LOGE("doAudioDecode: m_audioDecoder is null!");
        }
        av_packet_free(&packet);
    }
    LOGI("doAudioDecode: thread exiting");
}

void PlayerImpl::doVideoDecode() {
    LOGI("doVideoDecode: thread started");
    int decodeCount = 0;
    while (!m_abortFlag.load()) {
        AVPacket *packet = popVideoPacket();
        if (!packet) {
            LOGI("doVideoDecode: no more packets, exiting");
            break;
        }

        if (m_videoDecoder) {
            m_videoDecoder->decode(packet);
            decodeCount++;
            if (decodeCount <= 10) {
                LOGI("doVideoDecode: decoded packet #%d, size=%d", decodeCount, packet->size);
            }
        } else {
            LOGE("doVideoDecode: m_videoDecoder is null!");
        }
        av_packet_free(&packet);
    }
    LOGI("doVideoDecode: thread exiting, total decoded=%d", decodeCount);
}

void PlayerImpl::onVideoFrame(const VideoFrame& frame) {
    if (m_abortFlag.load()) return;

    LOGI("onVideoFrame: width=%d, height=%d, pts=%.3f, yuv.empty=%d",
         frame.width, frame.height, frame.pts, frame.yuv.empty());

    if (frame.yuv.empty()) {
        LOGE("onVideoFrame: frame.yuv is empty, skipping");
        return;
    }

    // 时钟同步：以音频为 master clock
    double audioTime = m_audioClock.getStreamTime();
    double diff = frame.pts - audioTime;

    // 视频明显落后，直接丢帧追进度
    if (diff < -0.15) {
        LOGI("onVideoFrame: dropping late frame (diff=%.3f)", diff);
        return;
    }

    // 视频超前，等待到接近音频时钟
    if (diff > 0.02) {
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(diff * 1000)));
    }

    if (m_abortFlag.load()) return;

    // 确保视频渲染器已初始化
    {
        std::lock_guard<std::mutex> lock(m_windowMutex);
        if (!m_videoRenderer && m_nativeWindow) {
            LOGI("onVideoFrame: lazy initializing VideoRenderer");
            m_videoRenderer = std::make_unique<VideoRenderer>();
            bool initResult = m_videoRenderer->init(m_nativeWindow);
            LOGI("onVideoFrame: VideoRenderer::init returned %d", initResult);
            if (!initResult) {
                LOGE("onVideoFrame: VideoRenderer init FAILED!");
                m_videoRenderer.reset();
            }
        } else if (!m_nativeWindow) {
            LOGE("onVideoFrame: m_nativeWindow is null! Video cannot render.");
        }
    }

    if (m_videoRenderer) {
        bool renderResult = m_videoRenderer->render(frame);
        static int frameCount = 0;
        frameCount++;
        if (frameCount <= 10 || frameCount % 30 == 0) {
            LOGI("onVideoFrame: render() returned %d, frame #%d", renderResult, frameCount);
        }
    } else {
        LOGE("onVideoFrame: m_videoRenderer is null! Frame dropped.");
    }

    // 更新视频时钟（用于无音频流时的备用同步）
    m_audioClock.setStreamTime(frame.pts);
}

// [修改6] 核心修复：实现音频帧回调，将解码后的音频数据推送到 AudioRenderer
void PlayerImpl::onAudioFrame(uint8_t *data, int size, int sample_rate, int channels) {
    if (m_abortFlag.load()) return;
    if (!data || size <= 0) {
        LOGE("onAudioFrame: invalid data (data=%p, size=%d)", data, size);
        return;
    }
    if (!m_audioRenderer) {
        LOGE("onAudioFrame: m_audioRenderer is null! Audio cannot play.");
        return;
    }

    // 将解码后的音频数据写入 AAudio 流
    int written = m_audioRenderer->play(data, size);
    if (written < 0) {
        LOGE("onAudioFrame: AudioRenderer::play failed (size=%d)", size);
    } else {
        static int frameCount = 0;
        frameCount++;
        if (frameCount <= 20 || frameCount % 50 == 0) {
            LOGI("onAudioFrame: played %d frames (%d bytes), total=%d",
                 written, size, frameCount);
        }
    }
}

void PlayerImpl::transitTo(PlayerState state) {
    m_state.store(state);
}

void PlayerImpl::postError(ErrorCode code, const std::string &msg) {
    if (m_listener) m_listener->onError(code, msg);
}

AVPacket* PlayerImpl::popAudioPacket() {
    std::unique_lock<std::mutex> lock(m_audioQueueMutex);
    // 等待：队列有数据 或 收到中止信号
    m_audioQueueCV.wait(lock, [this]() {
        return !m_audioPacketQueue.empty() || m_abortFlag.load();
    });
    if (m_abortFlag.load() && m_audioPacketQueue.empty()) {
        return nullptr;
    }
    AVPacket *packet = m_audioPacketQueue.front();
    m_audioPacketQueue.pop();
    return packet;
}

AVPacket* PlayerImpl::popVideoPacket() {
    std::unique_lock<std::mutex> lock(m_videoQueueMutex);
    // 等待：队列有数据 或 收到中止信号
    m_videoQueueCV.wait(lock, [this]() {
        return !m_videoPacketQueue.empty() || m_abortFlag.load();
    });
    if (m_abortFlag.load() && m_videoPacketQueue.empty()) {
        return nullptr;
    }
    AVPacket *packet = m_videoPacketQueue.front();
    m_videoPacketQueue.pop();
    return packet;
}

} // namespace player_sdk