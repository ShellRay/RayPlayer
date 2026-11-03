
#ifndef RAYPLAYER_PLAYERIMPL_H
#define RAYPLAYER_PLAYERIMPL_H

#include "player/Player.h"
#include "../src/decoder/AudioDecoder.h"
#include "../src/decoder/VideoDecoder.h"
#include "../src/render/VideoRenderer.h"
#include "../src/render/AudioRenderer.h"
#include "Clock.h"
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <condition_variable>

// FFmpeg 头文件
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/time.h>
}

namespace player_sdk {

class PlayerImpl : public Player {
public:
    PlayerImpl();
    ~PlayerImpl() override;

    int setDataSource(const std::string &url) override;
    int prepareAsync() override;
    int prepare() override;
    int start() override;
    int pause() override;
    int stop() override;
    void release() override;

    int seekTo(long msec) override;
    long getCurrentPosition() override;
    long getDuration() override;
    int setVolume(float left, float right) override;
    int setPlaybackSpeed(float speed) override;

    void setListener(PlayerListener *listener) override;
    PlayerState getState() const override;

    // 设置视频渲染 Surface (JNI 调用)
    void setVideoSurface(void* window);

    // 在 setVideoSurface 的下面，添加这行
    void updateSurface(void* window);

    // === 解封装提供的数据包获取接口（供后续解码器调用） ===
    AVPacket* popAudioPacket();
    AVPacket* popVideoPacket();

private:
    // 准备阶段
    void doPrepare();

    // === 解封装线程 ===
    void doDemux();

    // === 音频解码线程 ===
    void doAudioDecode();

    // === 视频解码线程 ===
    void doVideoDecode();

    // === 视频帧回调（视频解码线程调用） ===
    void onVideoFrame(const VideoFrame& frame);

    // 状态切换与回调
    void transitTo(PlayerState state);
    void postError(ErrorCode code, const std::string &msg);

    // 资源释放
    void freeDemuxer();
    void freePacketQueue(std::queue<AVPacket*> &queue, std::mutex &mutex);
    void freeAudioDecoder();
    void freeVideoDecoder();
    void freeVideoRenderer();
    void freeAudioRenderer();
    // AudioDecoder 回调
    void onAudioFrame(uint8_t *data, int size, int sample_rate, int channels);

    // --- 成员变量 ---

    // 基础状态
    std::mutex m_mutex;
    std::atomic<PlayerState> m_state{PlayerState::IDLE};
    std::string m_url;
    std::atomic<bool> m_abortFlag{false};

    // 线程
    std::thread m_prepareThread;
    std::thread m_demuxThread;
    std::thread m_audioDecoderThread;
    std::thread m_videoDecoderThread;

    // FFmpeg 上下文
    AVFormatContext *m_fmtCtx = nullptr;
    int m_audioStreamIndex = -1;
    int m_videoStreamIndex = -1;
    PlayerInfo m_info;
    PlayerListener *m_listener = nullptr;

    // === 解封装队列 ===
    std::queue<AVPacket*> m_audioPacketQueue;
    std::queue<AVPacket*> m_videoPacketQueue;
    std::mutex m_audioQueueMutex;
    std::mutex m_videoQueueMutex;
    std::condition_variable m_audioQueueCV;
    std::condition_variable m_videoQueueCV;

    // === 音频解码器 ===
    std::unique_ptr<AudioDecoder> m_audioDecoder;

    // === 视频解码器 ===
    std::unique_ptr<VideoDecoder> m_videoDecoder;

    // === 视频渲染器 ===
    std::unique_ptr<VideoRenderer> m_videoRenderer;

    // === 音频渲染器（新增） ===
    std::unique_ptr<AudioRenderer> m_audioRenderer;

    // === 音频时钟 ===
    Clock m_audioClock;

    // === 视频 Surface ===
    void* m_nativeWindow = nullptr;
    std::mutex m_windowMutex;
};

} // namespace player_sdk

#endif // RAYPLAYER_PLAYERIMPL_H