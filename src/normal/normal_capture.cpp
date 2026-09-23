#include "core_contract.h"

const char* plugin_meta(void) {
    return "fake_capture|1.0.0|1|capture";
}

core_error plugin_init(uint32_t host_abi) {
    if (host_abi == CORE_ABI_VERSION) {
        return CORE_OK;
    }
    return CORE_ERR_ABI_MISMATCH;
}

core_error plugin_release(void) {
    return CORE_OK;
}

core_error plugin_capture(core_frame* out) {
    if (out == nullptr) {
        return CORE_ERR_CAPTURE;
    }
    static uint8_t buf[640 * 480 * 4];
    out->width = 640;
    out->height = 480;
    out->stride = 640 * 4;
    out->format = CORE_PIXEL_FORMAT_BGRA8;
    out->data = buf;
    out->size = sizeof(buf);
    return CORE_OK;
}

core_error plugin_decide(const core_intent* intent, core_decision* out) {
    (void)intent;
    (void)out;
    return CORE_OK;
}

core_error plugin_execute(const core_decision* decision) {
    (void)decision;
    return CORE_OK;
}
