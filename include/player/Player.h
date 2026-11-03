//
// Created by 谢稳 on 2026/10/21.
//

#ifndef RAYPLAYER_PLAYER_H
#define RAYPLAYER_PLAYER_H

#include <string>
#include <memory>
#include "PlayerListener.h" // 引入监听器接口
#include "PlayerConfig.h"   // 引入配置项

namespace player_sdk {
    // 播放状态枚举
    enum class PlayerState {
        IDLE,           // 空闲
        INITIALIZED,    // 已初始化
        PREPARING,      // 准备中（异步）
        PREPARED,       // 准备完成
        STARTED,        // 播放中
        PAUSED,         // 暂停
        STOPPED,        // 停止
        COMPLETED,      // 播放结束
        ERROR           // 错误
    };


class Player {
    public:

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // 1. 工厂方法：创建播放器实例
    // 不直接 new Player()，而是通过 create，方便后续扩展不同的后端
    static std::unique_ptr<Player> create();

    virtual ~Player();

    // --- 生命周期控制 ---

    // 设置数据源 (支持本地路径 file:// 或 网络 URL http://)
    virtual int setDataSource(const std::string &url) = 0;

    // 异步准备 (开始解封装、探测流信息)
    // 准备完成后会通过 Listener 回调 onPrepared
    virtual int prepareAsync() = 0;

    // 同步准备 (阻塞直到准备好，简单场景可用)
    virtual int prepare() = 0;

    // 开始播放
    virtual int start() = 0;

    // 暂停
    virtual int pause() = 0;

    // 停止并重置
    virtual int stop() = 0;

    // 释放资源
    virtual void release() = 0;

    // --- 播放控制 ---

    // 跳转进度 (单位：毫秒)
    virtual int seekTo(long msec) = 0;

    // 获取当前进度 (单位：毫秒)
    virtual long getCurrentPosition() = 0;

    // 获取总时长 (单位：毫秒)
    virtual long getDuration() = 0;

    // 设置音量 (0.0 ~ 1.0)
    virtual int setVolume(float left, float right) = 0;

    // 设置播放倍速 (1.0 为正常，0.5 为半速，2.0 为倍速)
    virtual int setPlaybackSpeed(float speed) = 0;

    // --- 状态与回调 ---

    // 设置事件监听器
    virtual void setListener(player_sdk::PlayerListener *listener) = 0;

    // 获取当前状态
    virtual player_sdk::PlayerState getState() const = 0;



protected:
    // 构造函数保护，强制使用 create()
    Player() = default;

};

}
#endif //RAYPLAYER_PLAYER_H