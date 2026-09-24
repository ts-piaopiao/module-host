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

#define CORE_ABI_VERSION              4
#define CORE_PLUGIN_META_NAME_MAX     63
#define CORE_PLUGIN_META_VERSION_MAX  31
#define CORE_PLUGIN_META_NAME_BUF     64
#define CORE_PLUGIN_META_VERSION_BUF  32
#define CORE_DECISION_CAPACITY        8
#define CORE_MAX_DETECTIONS           16
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

// data 指针由 capture 插件持有。
// 有效期到下一次 plugin_capture 调用前。
// 宿主不得跨帧保存 data 指针。
// 宿主不得修改 data 指向的像素。
typedef struct core_frame {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    core_pixel_format format;
    const uint8_t* data;
    size_t size;
    int64_t pts_ms;
} core_frame;

typedef struct core_detection {
    int32_t cls;
    int32_t track_id;
    float conf;
    float cx, cy;
    float w, h;
} core_detection;

typedef struct core_detections {
    core_detection items[CORE_MAX_DETECTIONS];
    uint32_t count;
} core_detections;

// frame 指向当前帧，由宿主填充。
// 有效期到 plugin_decide 返回前。
// policy 插件不得跨帧保存 frame 指针。
// frame 可以为 nullptr。
// detections_out 由宿主分配并初始化 count=0。
// policy 若做检测，填 items/count；不检测则不动。
// detections_out 可以为 nullptr，policy 必须容忍。
typedef struct core_intent {
    int32_t param1;
    const core_frame* frame;
    core_detections* detections_out;
} core_intent;

typedef enum core_action_kind {
    CORE_ACTION_NONE = 0,
    CORE_ACTION_POINTER_MOVE = 1,
    CORE_ACTION_POINTER_BUTTON = 2,
    CORE_ACTION_KEY = 3,
    CORE_ACTION_WAIT = 4,
    CORE_ACTION_CUSTOM = 5
} core_action_kind;

typedef struct core_action {
    core_action_kind kind;
    int32_t a;
    int32_t b;
    int32_t c;
} core_action;

typedef struct core_decision {
    core_action actions[CORE_DECISION_CAPACITY];
    uint32_t out_count;
} core_decision;

typedef struct core_execute_result {
    core_error status;
    int32_t detail;
} core_execute_result;

CORE_STATIC_ASSERT(CORE_ABI_VERSION == 4, "CORE_ABI_VERSION must be 4");
CORE_STATIC_ASSERT(CORE_DECISION_CAPACITY == 8, "CORE_DECISION_CAPACITY must be 8");
CORE_STATIC_ASSERT(CORE_MAX_DETECTIONS == 16, "CORE_MAX_DETECTIONS must be 16");
CORE_STATIC_ASSERT(CORE_PARAM1 == 87, "CORE_PARAM1 must be 87");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_NAME_MAX == 63, "CORE_PLUGIN_META_NAME_MAX must be 63");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_VERSION_MAX == 31, "CORE_PLUGIN_META_VERSION_MAX must be 31");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_NAME_BUF == CORE_PLUGIN_META_NAME_MAX + 1,
                   "CORE_PLUGIN_META_NAME_BUF must be NAME_MAX + 1");
CORE_STATIC_ASSERT(CORE_PLUGIN_META_VERSION_BUF == CORE_PLUGIN_META_VERSION_MAX + 1,
                   "CORE_PLUGIN_META_VERSION_BUF must be VERSION_MAX + 1");
CORE_STATIC_ASSERT(CORE_OK == 0, "CORE_OK must be 0");
CORE_STATIC_ASSERT(sizeof(((core_decision*)0)->actions) / sizeof(core_action) == 8,
                   "core_decision.actions must have 8 elements");
CORE_STATIC_ASSERT(sizeof(((core_detections*)0)->items) / sizeof(core_detection) == CORE_MAX_DETECTIONS,
                   "core_detections.items must have CORE_MAX_DETECTIONS elements");
CORE_STATIC_ASSERT(offsetof(core_intent, frame) >= sizeof(int32_t),
                   "core_intent.frame must come after param1");

CORE_BEGIN_DECLS

CORE_API const char* plugin_meta(void);
CORE_API core_error plugin_init(uint32_t host_abi, const char* config);
CORE_API core_error plugin_release(void);
CORE_API core_error plugin_capture(core_frame* out);
CORE_API core_error plugin_decide(const core_intent* intent, core_decision* out);
CORE_API core_error plugin_execute(const core_decision* decision, core_execute_result* out);

CORE_END_DECLS

#endif
