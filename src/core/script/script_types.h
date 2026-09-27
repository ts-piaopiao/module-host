#pragma once
#include <cstdint>

enum class StateId : int {
    IDLE = 0,
    CHASE = 1,
    ATTACK = 2,
    ATTACK_TURN = 3,
    RECOVERY = 4,
    COUNT = 5
};

struct Classified {
    bool in_band_front = false;
    bool in_band_back  = false;
    bool in_band_any   = false;
    float dx = 0.0f;
    float abs_dx = 0.0f;
    int target_dir = 0;
    bool is_front = false;
    bool fresh_target = false;
};

struct Intent {
    int desired_dir = 0;
    bool desired_e = false;
};

struct MeLockState {
    bool valid = false;
    float fx = 0.0f;
    float fy = 0.0f;
    bool locked = false;
    float lock_fx = 0.0f;
    float lock_fy = 0.0f;
    uint64_t lost_since = 0;
};

struct TargetLockState {
    bool has = false;
    float cx = 0.0f;
    float cy = 0.0f;
    float fy = 0.0f;
    float h = 0.0f;
    float w = 0.0f;
    bool locked = false;
    float lock_cx = 0.0f;
    float lock_cy = 0.0f;
    uint64_t lost_since = 0;
    uint64_t last_target_switch_ms = 0;
};
