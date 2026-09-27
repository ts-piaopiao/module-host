#pragma once

#include "script_config.h"
#include "script_types.h"

#include <cstdint>
#include <random>

// 脚本运行时状态：跨帧、跨状态共享的全部可变字段。
// 步骤 5a：从 CppScript::Impl 原样搬入，字段名/默认值/顺序/注释均未改动。
struct Runtime {
    uint64_t last_log_frame = 0;
    bool inited = false;

    StateId state = StateId::IDLE;

    MeLockState me;
    TargetLockState target;
    Classified cls;   // 每帧分类缓存

    uint64_t attack_start_ms = 0;
    uint64_t recovery_start_ms = 0;
    uint64_t last_attack_release_ms = 0;   // 上次 E 释放时刻；用于独立技能冷却检查
    bool e_pressed = false;
    int current_e_tap_ms = 150;   // 初始值；实际每次攻击前重新采样

    // 朝向
    int facing = 1;
    uint64_t last_dir_ms = 0;
    uint64_t dir_press_start_ms = 0;   // 当前方向键按住起点；0 = 未按住
    uint64_t dir_min_hold_ms = 0;      // 本次按住目标时长；按开始时从方向键分位表采样
    int active_dir_key = 0;   // 当前实际按住的方向键（0 / 0x25 / 0x27）
    uint64_t pending_dir_release_ms = 0;  // 计划松方向键的时刻；0=无计划

    // 转身攻击（并行倒计时）
    uint64_t turn_start_ms = 0;
    uint64_t turn_dir_release_ms = 0;   // 方向键松开时刻
    uint64_t turn_e_press_ms = 0;       // E 按下时刻
    uint64_t turn_e_release_ms = 0;     // E 释放时刻
    int turn_dir_key = 0;
    bool turn_key_pressed = false;
    bool turn_e_pressed = false;

    // 攻击延迟 (S8)
    bool pending_attack = false;
    uint64_t pending_attack_start_ms = 0;
    int pending_attack_delay_ms = 0;

    // RECOVERY 后延迟 (S8)
    bool pending_chase = false;
    uint64_t pending_chase_start_ms = 0;
    int pending_chase_delay_ms = 0;

    ScriptConfig cfg;

    std::mt19937 rng;

    int active_key = 0;
    bool desired_e = false;
};
