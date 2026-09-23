#include "core_contract.h"

const char* plugin_meta(void) {
    return "fake_capture|1.0.0|1|capture";
}

core_error plugin_init(uint32_t host_abi) {
    (void)host_abi;
    return CORE_ERR_INIT;
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
    if (out != nullptr) {
        out->out_count = 0;
    }
    return CORE_OK;
}

core_error plugin_execute(const core_decision* decision) {
    (void)decision;
    return CORE_OK;
}
