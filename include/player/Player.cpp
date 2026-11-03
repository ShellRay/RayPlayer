//
// Created by 谢稳 on 2026/10/22.
//
#include "Player.h"
#include "../src/core/PlayerImpl.h"

namespace player_sdk {

    Player::~Player() {
        // 这里需要包含基类析构的逻辑
        // 例如：释放互斥锁、清空列表、销毁子对象等
        // 如果确实没有逻辑，也必须保留这个空实现
    }

    std::unique_ptr<Player> Player::create() {
        return std::make_unique<PlayerImpl>();
    }
}
