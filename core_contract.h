#ifndef CORE_CONTRACT_H
#define CORE_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define CORE_BEGIN_DECLS extern "C" {
#define CORE_END_DECLS   }
#else
#define CORE_BEGIN_DECLS
#define CORE_END_DECLS
#endif

#ifndef CORE_API
#if defined(CORE_BUILDING_PLUGIN) && (defined(_WIN32) || defined(__CYGWIN__))
#define CORE_API __declspec(dllexport)
#else
#define CORE_API
#endif
#endif

#if defined(__cplusplus)
#define CORE_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define CORE_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#define CORE_ABI_VERSION              1
#define CORE_PLUGIN_META_NAME_MAX     63
#define CORE_PLUGIN_META_VERSION_MAX  31
#define CORE_PLUGIN_META_NAME_BUF     64
#define CORE_PLUGIN_META_VERSION_BUF  32
#define CORE_DECISION_CAPACITY        8
#define CORE_PARAM1                   87
#define CORE_PLUGIN_META_SEP          '|'

typedef enum core_error {
    CORE_OK              = 0,
    CORE_ERR_META_INVALID = 1,
    CORE_ERR_ABI_MISMATCH = 2,
    CORE_ERR_INIT         = 3,
    CORE_ERR_CAPTURE      = 4,
    CORE_ERR_DECIDE       = 5,
    CORE_ERR_EXECUTE      = 6
} core_error;

typedef enum core_kind {
    CORE_KIND_CAPTURE = 0,
    CORE_KIND_POLICY  = 1,
    CORE_KIND_INPUT   = 2
} core_kind;

typedef enum core_pixel_format {
    CORE_PIXEL_FORMAT_BGRA8 = 0
} core_pixel_format;

typedef struct core_frame {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    core_pixel_format format;
    const uint8_t* data;
    size_t size;
} core_frame;

typedef struct core_intent {
    int32_t param1;
} core_intent;

typedef struct core_decision {
    int32_t items[CORE_DECISION_CAPACITY];
    uint32_t out_count;
} core_decision;

CORE_STATIC_ASSERT(CORE_ABI_VERSION == 1, "CORE_ABI_VERSION must be 1");
CORE_STATIC_ASSERT(CORE_DECISION_CAPACITY == 8, "CORE_DECISION_CAPACITY must be 8");
CORE_STATIC_ASSERT(CORE_PARAM1 == 87, "CORE_PARAM1 must be 87");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_NAME_MAX == 63, "CORE_PLUGIN_META_NAME_MAX must be 63");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_VERSION_MAX == 31, "CORE_PLUGIN_META_VERSION_MAX must be 31");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_NAME_BUF == CORE_PLUGIN_META_NAME_MAX + 1,
                   "CORE_PLUGIN_META_NAME_BUF must be NAME_MAX + 1");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_VERSION_BUF == CORE_PLUGIN_META_VERSION_MAX + 1,
                   "CORE_PLUGIN_META_VERSION_BUF must be VERSION_MAX + 1");
CORE_STATIC_ASSERT(CORE_OK == 0, "CORE_OK must be 0");
CORE_STATIC_ASSERT(sizeof(((core_decision*)0)->items) / sizeof(int32_t) == 8,
                   "core_decision.items must have 8 elements");

CORE_BEGIN_DECLS

CORE_API const char* plugin_meta(void);
CORE_API core_error plugin_init(uint32_t host_abi);
CORE_API core_error plugin_release(void);
CORE_API core_error plugin_capture(core_frame* out);
CORE_API core_error plugin_decide(const core_intent* intent, core_decision* out);
CORE_API core_error plugin_execute(const core_decision* decision);

CORE_END_DECLS

#endif
