#pragma once

#include <cstdint>
#include <string>

struct ScriptConfig {
    // 原匿名 namespace 14 常量
    static constexpr float kPriorX = 0.5f;
    static constexpr float kPriorY = 0.72f;
    static constexpr float kMeLockRange = 0.05f;
    static constexpr uint64_t kMeRelockMs = 1000;
    static constexpr float kTargetMatchX = 0.025f;
    static constexpr float kTargetMatchY = 0.037f;
    static constexpr uint64_t kTargetLoseMs = 500;
    static constexpr uint64_t kTargetSwitchCooldownMs = 500;
    static constexpr float kSamePlatY = 0.028f;
    static constexpr float kBandXMin = 0.010f;
    // 冰冻术硬范围 300px，缩 20px 留容错 → 280px，归一化 280/1920 = 0.1458
    static constexpr float kBandXMaxSame = 0.1458f;
    static constexpr float kBandXMaxCross = 0.104f;
    static constexpr float kBandYMin = -0.074f;
    static constexpr float kBandYMax = 0.019f;

    // 原 CombatConfig 4 字段（运行时可解析）
    int attack_react_min_ms = 40;
    int attack_react_max_ms = 70;
    int recovery_chase_min_ms = 30;
    int recovery_chase_max_ms = 60;

    // 原 Impl::kXxx 5 常量
    static constexpr uint64_t kRecoveryMs = 300;
    static constexpr uint64_t kSkillCooldownMs = 810;
    static constexpr int kTurnPressDelayMinMs = 100;
    static constexpr int kTurnPressDelayMaxMs = 200;
    static constexpr uint64_t kTurnBounceMs = 200;

    static ScriptConfig FromString(const std::string& config);
};
