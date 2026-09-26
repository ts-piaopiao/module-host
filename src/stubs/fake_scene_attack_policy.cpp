#include "core_contract.h"

namespace {

uint64_t g_frame_counter = 0;

constexpr int32_t kMeCls = 0;
constexpr int32_t kMonsterCls = 1;
constexpr int32_t kMeTrackId = 1;
constexpr int32_t kMonsterTrackId = 2;

// 每 120 帧一个循环：
//   帧 0-59：   me + monster 在攻击带内（dx=0.05 < 0.135）→ 脚本进入 ATTACK，按 E
//   帧 60-119： 仅 me → 脚本回 IDLE
constexpr uint64_t kCycleLen = 120;
constexpr uint64_t kMonsterVisibleLen = 60;

// me 在 (0.5, 0.66) → fy = 0.72
// monster 在 (0.55, 0.66) → dx = 0.05，落在攻击带 [0.010, 0.135] 内
constexpr float kMeCx = 0.5f;
constexpr float kMeCy = 0.66f;
constexpr float kMeW = 0.10f;
constexpr float kMeH = 0.12f;

constexpr float kMonsterCx = 0.55f;
constexpr float kMonsterCy = 0.66f;
constexpr float kMonsterW = 0.10f;
constexpr float kMonsterH = 0.12f;

}  // namespace

extern "C" {

const char* plugin_meta(void) {
    return "fake_scene_attack_policy|1.0.0|4|policy";
}

core_error plugin_init(uint32_t host_abi, const char* config) {
    (void)config;
    if (host_abi != CORE_ABI_VERSION) {
        return CORE_ERR_ABI_MISMATCH;
    }
    g_frame_counter = 0;
    return CORE_OK;
}

core_error plugin_release(void) {
    return CORE_OK;
}

core_error plugin_capture(core_frame* out) {
    (void)out;
    return CORE_OK;
}

core_error plugin_decide(const core_intent* intent, core_decision* out) {
    if (out == nullptr) {
        return CORE_ERR_DECIDE;
    }
    out->out_count = 0;

    if (intent != nullptr && intent->detections_out != nullptr) {
        core_detections* dets = intent->detections_out;
        dets->count = 0;

        core_detection& me = dets->items[dets->count++];
        me.cls = kMeCls;
        me.track_id = kMeTrackId;
        me.conf = 0.9f;
        me.cx = kMeCx;
        me.cy = kMeCy;
        me.w = kMeW;
        me.h = kMeH;

        const uint64_t phase = g_frame_counter % kCycleLen;
        if (phase < kMonsterVisibleLen) {
            core_detection& mo = dets->items[dets->count++];
            mo.cls = kMonsterCls;
            mo.track_id = kMonsterTrackId;
            mo.conf = 0.9f;
            mo.cx = kMonsterCx;
            mo.cy = kMonsterCy;
            mo.w = kMonsterW;
            mo.h = kMonsterH;
        }
    }

    ++g_frame_counter;
    return CORE_OK;
}

core_error plugin_execute(const core_decision* decision, core_execute_result* out) {
    (void)decision;
    if (out != nullptr) {
        out->status = CORE_OK;
        out->detail = 0;
    }
    return CORE_OK;
}

}  // extern "C"
