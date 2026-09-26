#include "core_contract.h"
#include <windows.h>
#include <cstdint>

extern "C" {

const char* plugin_meta(void) {
    return "fake_scene_capture|1.0.0|4|capture";
}

core_error plugin_init(uint32_t host_abi, const char* config) {
    (void)config;
    if (host_abi != CORE_ABI_VERSION) {
        return CORE_ERR_ABI_MISMATCH;
    }
    return CORE_OK;
}

core_error plugin_release(void) {
    return CORE_OK;
}

core_error plugin_capture(core_frame* out) {
    // 模拟 real_capture 的帧节拍：每帧 33ms。
    // 用于让脚本以真实时间推进（GetTickCount64 依赖挂钟）。
    Sleep(33);
    if (out != nullptr) {
        out->width = 0;
        out->height = 0;
        out->stride = 0;
        out->format = CORE_PIXEL_FORMAT_BGRA8;
        out->data = nullptr;
        out->size = 0;
        out->pts_ms = 0;
    }
    return CORE_OK;
}

core_error plugin_decide(const core_intent* intent, core_decision* out) {
    (void)intent;
    if (out == nullptr) {
        return CORE_ERR_DECIDE;
    }
    out->out_count = 0;
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
