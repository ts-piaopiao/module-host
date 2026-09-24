#include "core_contract.h"

const char* plugin_meta(void) {
    return "fake_policy|1.0.0|3|policy";
}

core_error plugin_init(uint32_t host_abi, const char* config) {
    (void)config;
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
    if (intent == nullptr || out == nullptr) {
        return CORE_ERR_DECIDE;
    }
    out->out_count = 1;
    out->actions[0].kind = CORE_ACTION_NONE;
    out->actions[0].a = CORE_PARAM1;
    out->actions[0].b = 0;
    out->actions[0].c = 0;
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
