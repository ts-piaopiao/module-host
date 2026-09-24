#include "core_contract.h"

extern "C" {

const char* plugin_meta(void) {
    return "test_policy|1.0.0|3|policy";
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
    (void)out;
    return CORE_OK;
}

core_error plugin_decide(const core_intent* intent, core_decision* out) {
    (void)intent;
    if (out == nullptr) {
        return CORE_ERR_DECIDE;
    }
    out->actions[0].kind = CORE_ACTION_KEY;
    out->actions[0].a = 'I';
    out->actions[0].b = 1;
    out->actions[0].c = 0;
    out->actions[1].kind = CORE_ACTION_KEY;
    out->actions[1].a = 'I';
    out->actions[1].b = 0;
    out->actions[1].c = 0;
    out->out_count = 2;
    return CORE_OK;
}

core_error plugin_execute(const core_decision* decision, core_execute_result* out) {
    (void)decision;
    (void)out;
    return CORE_OK;
}

}  // extern "C"
