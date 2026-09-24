# 阶段 6：宿主内决策层与 me_lock

## 一、背景

阶段 5 完成了 YOLO 推理集成，policy 插件能输出检测框（cls + track_id + conf + cx/cy/w/h）。
但 policy 只管"看"，不管"决策"。
阶段 6 把决策从 policy 里剥出来，放到宿主内，policy 只负责推理。

本阶段分 4 步：
- 6a 契约变更：core_detections 结构 + core_intent 输出槽（ABI 3 → 4）
- 6b 宿主 main 里加决策层模块，读检测结果，me_lock 锁定"我"
- 6c 决策层输出空决策（out_count = 0），只验证通路
- 6d 移植 combat_controller 状态机（后续独立设计）

## 二、契约变更

### 2.1 新增 core_detection

```c
typedef struct core_detection {
    int32_t cls;          // 0=me, 1=monster（由 policy 定义）
    int32_t track_id;     // tracker 分配的 id，>=1；无则为 0
    float conf;           // 0~1
    float cx, cy;         // 归一化 0~1，中心点制，原图空间
    float w, h;           // 归一化 0~1
} core_detection;
```

### 2.2 新增 core_detections

```c
#define CORE_MAX_DETECTIONS 16

typedef struct core_detections {
    core_detection items[CORE_MAX_DETECTIONS];
    uint32_t count;       // 0 ~ CORE_MAX_DETECTIONS
} core_detections;
```

### 2.3 core_intent 加输出槽

```c
typedef struct core_intent {
    int32_t param1;
    const core_frame* frame;
    core_detections* detections_out;  // 新增
} core_intent;
```

### 2.4 语义

- 宿主分配 core_detections，count 初始为 0。
- 宿主调用 plugin_decide 前挂到 intent.detections_out。
- policy 若做检测，填 items / count。
- policy 不做检测（例如 test_policy）则不动 count，宿主读到 count=0。
- detections_out 可以为 nullptr（宿主不关心检测结果时），policy 必须容忍。

### 2.5 ABI 变更

CORE_ABI_VERSION 从 3 升到 4。

### 2.6 受影响文件

- core_contract.h（结构体 + 断言 + ABI）
- tests/test_contract_c.c / test_contract_cpp.cpp（断言 + intent_var 初始化）
- 所有 20 个桩 + 3 个 real 插件的 plugin_meta 的 ABI 段从 "3" 改成 "4"
- src/bad_plugins/abi_mismatch_capture.cpp 的 ABI 段从 "4" 改成 "5"
- src/core/main.cpp：分配 core_detections，挂到 intent，传给 policy
- src/real/yolo_policy.cpp：删除直接打印检测结果的代码，改成填 detections_out

## 三、宿主决策层架构

在 src/core/ 下新增：

```
src/core/decider.h
src/core/decider.cpp
```

Decider 类：

```cpp
class Decider {
public:
    Decider();
    ~Decider();

    // 每帧调用。输入 policy 产出的检测结果，输出决策。
    void Update(const core_detections* dets, core_decision* out);

private:
    struct Impl;
    Impl* impl_;
};
```

Decider 内部维护：
- me_lock 状态（锁定的 track_id + 位置 + 丢失时间）
- （后续 6d）状态机

第一阶段（6b / 6c）只做 me_lock：
- 无锁：选距先验 (0.5, 0.72) 最近的 cls=0
- 有锁：只接受同 track_id 且位置连续的候选
- 丢失：进入保护期（3000ms），期间不切换
- 保护期超时：清锁，等下一次无锁逻辑

## 四、main.cpp 集成

在 ON 5 帧循环里，capture 之后、policy.decide 之后，插入：

```cpp
core_detections dets = {};
intent.detections_out = &dets;

if (states[1].decide(&intent, &decision_policy) != CORE_OK) { ... }

core_decision decision_decided = {};
decider.Update(&dets, &decision_decided);

// 后续 6c / 6d 用 decision_decided 走仲裁
// 现在 decision_decided.out_count = 0
```

仲裁层（阶段 4 已有的）读 decision_decided，与人工决策比对。

## 五、yolo_policy 改造

plugin_decide 里删掉：
- 所有 fprintf 打印检测结果的代码
- me_lock 相关代码（已删除）

改为：
- 从共享结果读出 detections（同现在）
- 把 detections 填进 intent->detections_out（最多 16 个）
- out->out_count = 0
- return CORE_OK

## 六、不在本阶段范围

- 完整 combat_controller 状态机（6d，独立设计）
- 主动移动验证（可能 6e，独立设计）
- 多 policy 插件共存

## 七、验收

- 6a：阶段 0/1/2 全绿
- 6b：宿主日志出现 me_lock 的 id 和位置（由 Decider 打印）
- 6c：整个链路不崩，5 帧循环退出码 0
- 6d：留给独立阶段

## 八、一句话原则

policy 只看，宿主决策；契约只加，不改。
