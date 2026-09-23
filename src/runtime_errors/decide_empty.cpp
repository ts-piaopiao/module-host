#include "core_contract.h"

const char* plugin_meta(void) {
    return "fake_policy|1.0.0|1|policy";
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
    (void)out;
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

core_error plugin_execute(const core_decision* decision) {
    (void)decision;
    return CORE_OK;
}
