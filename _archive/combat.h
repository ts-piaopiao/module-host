#ifndef MODULE_HOST_CORE_COMBAT_H
#define MODULE_HOST_CORE_COMBAT_H

#include "core_contract.h"

class Combat {
public:
    Combat();
    ~Combat();
    Combat(const Combat&) = delete;
    Combat& operator=(const Combat&) = delete;

    // me_fx / me_fy：脚底中心归一化坐标
    // dets：当前帧全部检测结果
    // out：输出决策
    void Update(bool me_valid, float me_fx, float me_fy,
                const core_detections* dets, core_decision* out);

    // 输出所有已按键的 release 动作到 out，并清空内部状态。
    // 用于异常路径（me 丢失、验证开始）。
    void ReleaseAll(core_decision* out);

private:
    enum class CombatState {
        IDLE,
        CHASE,
        ATTACK
    };

    struct Impl;
    Impl* impl_;
};

#endif
