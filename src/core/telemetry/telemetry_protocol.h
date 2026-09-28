#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace telemetry {

// 遥测契约版本。只加不改。
constexpr int kFrameBundleVersion = 1;

// 脚本状态快照。字段与 CppScriptDebugInfo 逐字段对齐（C2 硬约束）。
struct ScriptSnapshot {
    int state = 0;          // 0=IDLE 1=CHASE 2=ATTACK 3=ATTACK_TURN 4=RECOVERY
    int facing = 1;
    bool me_locked = false;
    float me_fx = 0.0f;
    float me_fy = 0.0f;
    bool target_locked = false;
    float target_cx = 0.0f;
    int active_key = 0;
    bool desired_e = false;
};

// 检测框快照。字段与 recorder 'det' 行对齐：cls/conf/cx/cy/w/h/id。
struct DetectionSnapshot {
    int cls = 0;
    float conf = 0.0f;
    float cx = 0.0f;
    float cy = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    int id = 0;
};

// 一帧遥测数据。UI-1b 只填 v/frame/t/script；dets 留空。
// UI-2（画面叠加）再填 dets。
struct FrameBundle {
    int v = kFrameBundleVersion;
    uint64_t frame = 0;
    uint64_t t = 0;         // 微秒，steady_clock
    ScriptSnapshot script;
    std::vector<DetectionSnapshot> dets;
};

// 微秒时间戳。steady_clock，无外部依赖。
// 若将来需与 recorder 的 QPC 时间戳精确对齐，可提取 recorder.cpp
// 匿名 namespace 中的 NowMicros() 复用。
inline uint64_t NowMicros() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace telemetry
