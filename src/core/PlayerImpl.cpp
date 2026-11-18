


//
// Created by 谢稳 on 2026/10/21.
//
#include "PlayerImpl.h"
#include <android/log.h>
#include <android/native_window.h>
#include "../src/render/AudioRenderer.h"  // [修改1] 新增：引入音频渲染器头文件

#include "player/Player.h"
#include <functional>
#include <time.h>      // [修改1] clock_gettime：高精度单调时钟
#include <chrono>
#include <thread>
#include <atomic>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PlayerSDK", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PlayerSDK", __VA_ARGS__)

namespace player_sdk {

// ============================================================================
// [修改2] 音频主时钟辅助设施（单调墙钟锚点 + 丢帧保护计数）
//
// 为什么需要：主时钟 m_audioClock 是"累加式"推进的（每写一次音频就 +elapsed），
// 一旦 elapsed 算错（单位/声道/采样率），时钟就会跑飞，且没有任何机制能把它拉回来。
// 这里额外维护一个"墙钟锚点"，给主时钟加一个硬上限：
//     audioClock <= 锚点pts + 真实经过时间 + 容差
// 这样无论上游怎么算错，视频都不可能再出现"快进播完"。
//
// 说明：本播放器为单实例使用场景，故用文件级 static 保存锚点，避免改动 PlayerImpl.h。
// 若将来需要多实例并发，请把这几个变量搬进 PlayerImpl 成为成员变量。
// ============================================================================
static inline long long nowUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

static std::atomic<bool>       s_clockAnchorValid{false};
static std::atomic<long long>  s_clockAnchorWallUs{0};
static std::atomic<double>     s_clockAnchorPts{0.0};

// 连续丢帧计数 + 上一帧的 pts / 墙钟（用于丢帧保护节流）
static std::atomic<int>        s_consecutiveDrops{0};
static std::atomic<double>     s_lastFramePts{-1.0};
static std::atomic<long long>  s_lastFrameWallUs{0};

// 重置锚点：start / seekTo / stop 时调用
static inline void resetAudioClockAnchor(double pts) {
    s_clockAnchorPts.store(pts);
    s_clockAnchorWallUs.store(nowUs());
    s_clockAnchorValid.store(true);
    s_consecutiveDrops.store(0);
    s_lastFramePts.store(-1.0);
    s_lastFrameWallUs.store(0);
}

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

    // [修改3] 起播时重置音频主时钟锚点与丢帧保护状态
    resetAudioClockAnchor(m_audioClock.getStreamTime());

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
    // 真正拖动还要清空 PacketQueue、重置解码器，否则花屏跳音

    // [修改6] seek 成功后把主时钟对齐到目标位置，并重置墙钟锚点。
    // 主时钟是累加式推进的，不重置会让 diff = pts - audioTime 出现巨大负值，
    // 视频帧被全部判为"落后"而丢弃 -> 拖动进度条后画面卡死。
    if (ret >= 0) {
        double targetSec = msec / 1000.0;
        m_audioClock.setStreamTime(targetSec);
        resetAudioClockAnchor(targetSec);
        LOGI("seekTo: audio clock reset to %.3f s", targetSec);
    }

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

    // ===== 时钟同步：以音频为 master clock =====
    double audioTime = m_audioClock.getStreamTime();
    double diff = frame.pts - audioTime;

    // 视频明显落后，直接丢帧追进度
    if (diff < -0.15) {
        int drops = s_consecutiveDrops.fetch_add(1) + 1;
        LOGI("onVideoFrame: dropping late frame (diff=%.3f, consecutive=%d)", diff, drops);

        // [修改4] 丢帧保护：连续丢帧 >= 5 说明主时钟不可信（例如被错误地快速推进）。
        // 此时若直接 return，视频解码线程就完全没有节流 -> 画面"快进"播完（本次故障现象）。
        // 因此退化为"按 PTS 间隔 + 墙钟"节流，保证播放速度始终接近 1x。
        if (drops >= 5) {
            long long now = nowUs();
            double lastPts = s_lastFramePts.load();
            if (lastPts >= 0.0 && frame.pts > lastPts) {
                double gap = frame.pts - lastPts;
                if (gap > 0.0 && gap < 0.5) {
                    long long gapUs = (long long) (gap * 1000000.0);
                    long long sinceLast = now - s_lastFrameWallUs.load();
                    if (sinceLast < gapUs) {
                        std::this_thread::sleep_for(std::chrono::microseconds(gapUs - sinceLast));
                    }
                }
            }
            s_lastFramePts.store(frame.pts);
            s_lastFrameWallUs.store(nowUs());
        }
        return;
    }
    s_consecutiveDrops.store(0);

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

    // [修改4] 记录本帧 pts 与墙钟，供丢帧保护节流使用
    s_lastFramePts.store(frame.pts);
    s_lastFrameWallUs.store(nowUs());

    // 更新视频时钟（用于无音频流时的备用同步）
    if (!m_audioRenderer) {
        m_audioClock.setStreamTime(frame.pts);
    }

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
    // 注意：play() 返回的是**实际写入的字节数**，不是采样帧数！
    int written = m_audioRenderer->play(data, size);

    // [修改5] 核心修复：字节数 -> 采样帧数
    // 日志铁证：
    //   AudioRenderer::play: wrote 1114 frames (4456 bytes)   <- 内部知道是 1114 帧
    //   onAudioFrame:        played 4456 frames (4460 bytes)  <- 这里拿到的 written = 4456 = 字节数
    // 原代码 written / sample_rate 相当于 4456/48000 = 92.8ms，
    // 而真实时长只有 1114/48000 = 23.2ms  ->  主时钟被放大 4 倍（= channels(2) * sizeof(int16)(2)）。
    // 结果：audioTime 以约 4x 真实速度前进，每个视频帧都 diff < -0.15 被判"落后"直接 return，
    // 视频线程零节流 -> 画面"快进"播完；音频仍由 AAudio 缓冲按实时吐出来，所以听感正常。
    const int bytesPerSample = 2;                       // AAudio 配置为 AAUDIO_FORMAT_PCM_I16
    const int ch = (channels > 0) ? channels : 2;       // 日志：48000Hz, 2ch
    int bytesPerFrame = bytesPerSample * ch;            // = 4
    if (bytesPerFrame <= 0) bytesPerFrame = 4;

    long frames = written / bytesPerFrame;
    if (frames <= 0) frames = size / bytesPerFrame;    // 兜底：用入参 size 换算

    if (frames > 0 && sample_rate > 0 && m_state.load() == PlayerState::STARTED) {
        double elapsed = (double) frames / (double) sample_rate;
        double newTime = m_audioClock.getStreamTime() + elapsed;

        // [修改5-附] 墙钟锚定钳制（第二道保险）：
        // 主时钟不允许超过"锚点 + 真实经过时间 + 容差"。
        // 容差取 100ms，覆盖 AAudio 缓冲深度（日志 frameCount=1924 @48kHz ≈ 40ms）与调度抖动。
        long long now = nowUs();
        if (!s_clockAnchorValid.load()) {
            resetAudioClockAnchor(newTime);
        } else {
            double wallTime = s_clockAnchorPts.load()
                              + (now - s_clockAnchorWallUs.load()) / 1000000.0;
            const double kMaxLead = 0.10;
            if (newTime > wallTime + kMaxLead) {
                newTime = wallTime + kMaxLead;
            }
            m_audioClock.setStreamTime(newTime);
        }
    }

    if (written < 0) {
        LOGI("onAudioFrame: AudioRenderer::play failed (size=%d)", size);
    } else {
        static int frameCount = 0;
        frameCount++;
        if (frameCount <= 20 || frameCount % 50 == 0) {
            LOGI("onAudioFrame: wrote %d bytes = %ld frames, audioClock=%.3f s, total=%d",
                 written, frames, m_audioClock.getStreamTime(), frameCount);
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


// 我帮你把音频这条线从头到尾串起来，按数据流动的顺序拆成 5 步：
//
// 音频完整数据流
//
// 文件 → doDemux(解封装) → 音频队列 → doAudioDecode(取包) → AudioDecoder::decode(解码) → 回调onAudioFrame → AudioRenderer::play(播放) → 扬声器
//
// 第一步：doDemux（解封装线程）——"拆包工"
//
// 你的理解是对的：while(!m_abortFlag.load()) 无线循环，不断 av_read_frame 从文件里取包。
//
// 关键动作就一个：按 stream_index 分拣。
//
// av_read_frame 拿到一个 packet
//   ├─ stream_index == audioIdx → 克隆后推入 m_audioPacketQueue，唤醒 m_audioQueueCV
//   ├─ stream_index == videoIdx → 克隆后推入 m_videoPacketQueue，唤醒 m_videoQueueCV
//   └─ 其他（字幕等）          → 丢弃
//
// 你容易困惑的点：这里只是"拆包"，不做任何解码。音频包和视频包被分到了两个不同的队列里。
//
// 第二步：doAudioDecode（音频解码线程）——"取包工人"
//
// 这个线程在 start() 时被创建：
//
// m_audioDecoderThread = std::thread(&PlayerImpl::doAudioDecode, this);
//
// 它的逻辑非常简单，就是一个循环：
//
// while(!m_abortFlag) {
//     调用 popAudioPacket() 从音频队列取一个包（队列空了就阻塞等待）
//     如果取到 null → 说明没数据了或要停了，退出循环
//     调用 m_audioDecoder->decode(packet) → 把包交给解码器去解码
//     av_packet_free(packet) → 释放这个包
// }
//
// 你的疑问"解码器从解封装后的音频队列中取数据"——对的，就是通过 popAudioPacket() 取的。
//
// 第三步：popAudioPacket（阻塞取包）——"等菜上桌"
//
// 这个方法的核心是用条件变量阻塞等待：
//
// AVPacket* PlayerImpl::popAudioPacket() {
//     std::unique_lock<std::mutex> lock(m_audioQueueMutex);
//     // 关键：队列空的时候线程会阻塞在这里，不会空转消耗CPU
//     m_audioQueueCV.wait(lock, this {
//         return !m_audioPacketQueue.empty() || m_abortFlag.load();
//     });
//     // 被唤醒后，如果是要停了且队列空了 → 返回null告诉解码线程可以退出了
//     if (m_abortFlag.load() && m_audioPacketQueue.empty()) {
//         return nullptr;
//     }
//     // 否则从队列头取出一个包
//     AVPacket *packet = m_audioPacketQueue.front();
//     m_audioPacketQueue.pop();
//     return packet;
// }
//
// 面试可以说的点：这里用 condition_variable 避免了忙等待（busy wait），队列没数据时线程挂起不占 CPU，解封装线程 push 完数据后 notify_one() 把它叫醒。
//
// 第四步：AudioDecoder::decode（真正的解码）——"厨房做菜"
//
// 这个类的实现不在 PlayerImpl.cpp 里，它在另一个文件（应该是 AudioDecoder.cpp）中。你看不到的原因就是这个。
//
// 但根据代码中的调用方式，可以推断它的流程：
//
// // PlayerImpl.cpp 中调用的：
// m_audioDecoder->decode(packet);
//
// // AudioDecoder 内部大概长这样（推断）：
// void AudioDecoder::decode(AVPacket *packet) {
//     // 1. 把压缩音频包送入FFmpeg解码器
//     avcodec_send_packet(m_codecCtx, packet);
//
//     // 2. 循环取出解码后的PCM帧
//     AVFrame *frame = av_frame_alloc();
//     while (avcodec_receive_frame(m_codecCtx, frame) == 0) {
//         // 3. 解码出一帧PCM数据后，调用注册的回调函数
//         if (m_callback) {
//             m_callback(
//                 frame->data[0],        // PCM数据指针
//                 frame->nb_samples * m_codecCtx->channels * 2,  // 数据大小
//                 m_codecCtx->sample_rate, // 采样率
//                 m_codecCtx->channels     // 声道数
//             );
//         }
//     }
//     av_frame_free(&frame);
// }
//
// 关键点：decode() 是同步调用——调它就等着它解完一帧（或多帧）返回。解完的数据不是通过返回值拿的，而是通过回调函数通知上去的。
//
// 第五步：onAudioFrame（回调）+ AudioRenderer（播放）——"上菜"
//
// 回调是在 doPrepare() 中注册的：
//
// m_audioDecoder->setCallback( {
//     onAudioFrame(data, size, sample_rate, channels);
// });
//
// 当 AudioDecoder::decode() 内部解出一帧 PCM 数据后，就会自动调用 onAudioFrame：
//
// void PlayerImpl::onAudioFrame(uint8_t *data, int size, int sample_rate, int channels) {
//     // 1. 检查状态
//     if (m_abortFlag.load()) return;
//     if (!data || size <= 0) return;
//     if (!m_audioRenderer) return;
//
//     // 2. 把PCM数据写入AudioRenderer（底层是AAudio流）
//     int written = m_audioRenderer->play(data, size);
//     if (written < 0) {
//         LOGE("AudioRenderer::play failed");
//     }
// }
//
// AudioRenderer::play() 内部做的事情（推断）：
// 把 PCM 数据写入 AAudio 的输出缓冲区
// AAudio 底层从缓冲区取数据发给音频硬件（扬声器/耳机）
// 如果缓冲区空了就会自动填充，保证音频连续不中断
//
// 整条线串起来（面试可以画这个图）
//
// ┌─────────────┐     ┌──────────────┐     ┌─────────────────┐
// │  doDemux    │────▶│ 音频队列       │────▶│ doAudioDecode   │
// │  (解封装)    │◀────│  (Mutex+CV)   │────▶│  (解码线程)      │
// │  av_read_   │  notify│  push/pop   │────▶│  decode()       │
// │  frame()    │◀─────▶│              │     │  AVCodec        │
// └─────────────┘       └──────────────┘     └────────┬────────┘
//                                                       │
//                                                       ▼
//                                               ┌─────────────────┐
//                                               │  回调 callback   │
//                                               │  onAudioFrame()  │
//                                               └────────┬────────┘
//                                                        │
//                                                        ▼
//                                               ┌─────────────────┐
//                                               │ AudioRenderer    │
//                                               │  ::play()        │
//                                               │  (AAudio流)      │
//                                               └────────┬────────┘
//                                                        │
//                                                        ▼
//                                                  扬声器/耳机
//
// 你之前没看懂的核心原因
//
// 你卡在的地方应该是：m_audioDecoder->decode(packet) 这行调用之后，数据去哪了？
//
// 答案就是：decode() 内部解完数据后，通过你之前用 setCallback 注册的回调函数，自动调用 onAudioFrame()，然后 onAudioFrame 把数据塞给 AudioRenderer 播放出去。
//
// 这就像你点了一份外卖（decode），不需要你亲自去厨房端菜（不用手动取PCM数据），厨房做好了会自动打电话叫你（回调），你只需要开门拿菜（onAudioFrame 里调 play）就行。
//
//
//音频解码是否使用硬解也可以？ 最好还是软解
// 音频硬解确实可以用 MediaCodec，因为 MediaCodec 支持音频编解码。但实际产品中音频走硬解的情况比视频少很多——一是音频解码计算量小，
// 软解功耗差异不明显；二是 FFmpeg 支持的音频格式更全；三是 MediaCodec 音频的缓冲区管理比 FFmpeg 更复杂。所以主流播放器一般是"视频硬解 +
// 音频软解"的混合方案。如果确实需要做音频硬解（比如低功耗场景），可以通过 MediaCodec 的音频解码器实现，但要注意不同芯片厂商的实现差异，
// 以及 MediaCodec 音频是"拉模式"（主动 dequeue）和 FFmpeg 的"推模式"（回调通知）在架构设计上的不同。
//
//解封装是用FFmpeg吗？ FFmpeg兼容好 MediaExtractor相对于标准视频格式较好
//解封装主要有两种方案：FFmpeg 的 libavformat 和 Android 原生的 MediaExtractor。FFmpeg 格式和协议覆盖最广，适合需要播各种野格式、流媒体的场景，
//抖音快手这类自研播放器基本都用它；MediaExtractor 是系统原生 API，和 MediaCodec 配套方便、包体积小，但支持的格式有限。实际产品中，ExoPlayer
//默认走 MediaExtractor 体系的 Extractor，遇到不支持的格式可以挂 FFmpeg 扩展做兜底；而完全自研的播放器一般直接用 FFmpeg 做解封装，省心。