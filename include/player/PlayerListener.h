//
// Created by 谢稳 on 2026/10/21.
//

#ifndef RAYPLAYER_PLAYERLISTENER_H
#define RAYPLAYER_PLAYERLISTENER_H


#include <string>
#include <stdexcept>

namespace player_sdk {

    // 错误码枚举
    enum class ErrorCode {
        SUCCESS = 0,            // 无错误
        UNKNOWN = -1,           // 未知错误
        IO_ERROR = -2,          // 文件读取/网络IO错误
        INVALID_SOURCE = -3,    // 数据源路径非法
        DECODE_ERROR = -4,      // 解封装或解码错误
        AUDIO_RENDER_ERROR = -5,// 音频输出设备错误（如耳机拔出）
    };

    // 播放器事件枚举
    enum class PlayerEvent {
        PREPARED,               // 准备完成，可以开始播放
        STARTED,                // 开始播放
        PAUSED,                 // 已暂停
        STOPPED,                // 已停止
        COMPLETED,              // 播放完成
        SEEK_COMPLETE,          // 拖动进度完成
    };

    // 播放器信息键值对
    struct PlayerInfo {
        long duration_ms = 0;       // 媒体总时长（毫秒）
        int width = 0;              // 视频宽度
        int height = 0;             // 视频高度
        int audio_sample_rate = 0;  // 音频采样率
        int audio_channel_count = 0;// 音频声道数
    };

    // 核心监听器抽象类（契约接口）
    class PlayerListener {
    public:
        virtual ~PlayerListener() = default;

        // 1. 状态与事件回调
        virtual void onEvent(PlayerEvent event) = 0;

        // 2. 错误回调 (携带错误码与人类可读的错误信息)
        virtual void onError(ErrorCode code, const std::string &msg) = 0;

        // 3. 准备完成时回调的媒体基础信息
        virtual void onPrepared(const PlayerInfo &info) = 0;

        // 4. (预留) 缓冲状态回调：解决网络卡顿时的进度条加载动画
        // virtual void onBufferingStarted(long msec) = 0;
        // virtual void onBufferingFinished() = 0;
    };

} // namespace player_sdk
#endif //RAYPLAYER_PLAYERLISTENER_H