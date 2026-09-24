#include "core_contract.h"
#include "core_contract.h"

CORE_STATIC_ASSERT(CORE_ABI_VERSION == 3, "CORE_ABI_VERSION must be 3");
CORE_STATIC_ASSERT(CORE_DECISION_CAPACITY == 8, "CORE_DECISION_CAPACITY must be 8");
CORE_STATIC_ASSERT(CORE_PARAM1 == 87, "CORE_PARAM1 must be 87");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_NAME_MAX == 63, "CORE_PLUGIN_META_NAME_MAX must be 63");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_VERSION_MAX == 31, "CORE_PLUGIN_META_VERSION_MAX must be 31");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_NAME_BUF == 64, "CORE_PLUGIN_META_NAME_BUF must be 64");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_VERSION_BUF == 32, "CORE_PLUGIN_META_VERSION_BUF must be 32");
CORE_STATIC_ASSERT(CORE_OK == 0, "CORE_OK must be 0");

static const core_frame frame_var = {0};
static core_intent intent_var = {CORE_PARAM1, NULL};
static core_decision decision_var = {{0}, 0};

const char* (*fn_plugin_meta)(void) = plugin_meta;
core_error (*fn_plugin_init)(uint32_t, const char*) = plugin_init;
core_error (*fn_plugin_release)(void) = plugin_release;
core_error (*fn_plugin_capture)(core_frame*) = plugin_capture;
core_error (*fn_plugin_decide)(const core_intent*, core_decision*) = plugin_decide;
core_error (*fn_plugin_execute)(const core_decision*, core_execute_result*) = plugin_execute;
