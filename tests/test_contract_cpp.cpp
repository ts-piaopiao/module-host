#include "core_contract.h"
#include "core_contract.h"

CORE_STATIC_ASSERT(CORE_ABI_VERSION == 1, "CORE_ABI_VERSION must be 1");
CORE_STATIC_ASSERT(CORE_DECISION_CAPACITY == 8, "CORE_DECISION_CAPACITY must be 8");
CORE_STATIC_ASSERT(CORE_PARAM1 == 87, "CORE_PARAM1 must be 87");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_NAME_MAX == 63, "CORE_PLUGIN_META_NAME_MAX must be 63");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_VERSION_MAX == 31, "CORE_PLUGIN_META_VERSION_MAX must be 31");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_NAME_BUF == 64, "CORE_PLUGIN_META_NAME_BUF must be 64");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_VERSION_BUF == 32, "CORE_PLUGIN_META_VERSION_BUF must be 32");
CORE_STATIC_ASSERT(CORE_OK == 0, "CORE_OK must be 0");

static_assert(CORE_ABI_VERSION == 1, "CORE_ABI_VERSION must be 1");
static_assert(CORE_DECISION_CAPACITY == 8, "CORE_DECISION_CAPACITY must be 8");
static_assert(CORE_PARAM1 == 87, "CORE_PARAM1 must be 87");
static_assert(CORE_PLUGIN_META_NAME_MAX == 63, "CORE_PLUGIN_META_NAME_MAX must be 63");
static_assert(CORE_PLUGIN_META_VERSION_MAX == 31, "CORE_PLUGIN_META_VERSION_MAX must be 31");
static_assert(CORE_PLUGIN_META_NAME_BUF == 64, "CORE_PLUGIN_META_NAME_BUF must be 64");
static_assert(CORE_PLUGIN_META_VERSION_BUF == 32, "CORE_PLUGIN_META_VERSION_BUF must be 32");
static_assert(CORE_OK == 0, "CORE_OK must be 0");

static const core_frame frame_var = {};
static core_intent intent_var = {CORE_PARAM1};
static core_decision decision_var = {{}, 0};

const char* (*fn_plugin_meta)(void) = plugin_meta;
core_error (*fn_plugin_init)(uint32_t) = plugin_init;
core_error (*fn_plugin_release)(void) = plugin_release;
core_error (*fn_plugin_capture)(core_frame*) = plugin_capture;
core_error (*fn_plugin_decide)(const core_intent*, core_decision*) = plugin_decide;
core_error (*fn_plugin_execute)(const core_decision*) = plugin_execute;
