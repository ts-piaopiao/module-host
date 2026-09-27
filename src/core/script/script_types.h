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
