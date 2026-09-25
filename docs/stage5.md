# 阶段 5：YOLO 推理集成

## 一、背景

阶段 4 打通了采集卡推流、远程操作、人工优先仲裁。
阶段 5 引入自动决策：policy 插件内部跑 YOLO 推理，输出检测结果，
为后续的动作决策（打怪状态机）提供输入。

本阶段分 4 步：
- 5a 契约变更：core_intent 加 frame 字段（ABI 2 → 3）
- 5b ONNX Runtime 集成 + policy 骨架 + 独立推理线程
- 5c 移植 YOLOv8 后处理（decode / NMS / 跟踪）
- 5d 动作决策（暂不做，脚本后续单独设计）

## 二、契约变更

### 2.1 变更内容

core_intent 结构体加一个字段：

  typedef struct core_intent {
      int32_t param1;
      const core_frame* frame;   // 新增。宿主填充，有效期到 plugin_decide 返回
  } core_intent;

### 2.2 原因

policy 插件需要访问当前帧的像素做 YOLO 推理。
现有契约里 plugin_decide 只能拿到 core_intent（只有 param1），拿不到 frame。

### 2.3 生命周期约束

- frame 由宿主提供，指向 capture 插件产出、且在本帧内有效的 core_frame。
- 宿主保证 frame 的有效期到 plugin_decide 返回前。
- policy 插件不得跨帧保存 frame 指针，必须拷贝像素到自己内部。
- frame 指针可以为 nullptr（例如无 capture 插件时），policy 必须容忍。

### 2.4 ABI 变更

CORE_ABI_VERSION 从 2 升到 3。

### 2.5 受影响文件

- core_contract.h：结构体 + 断言 + ABI
- tests/test_contract_c.c、test_contract_cpp.cpp：ABI 断言 + intent_var 初始化
- 所有 20 个桩：plugin_meta 的 ABI 段从 "2" 改 "3"
- src/bad_plugins/abi_mismatch_capture.cpp：ABI 段从 "3" 改 "4"（保持不匹配语义）
- src/core/main.cpp：5 帧循环里填充 intent.frame = &frame
- src/normal/normal_policy.cpp：改用 intent->frame（可选，可为 nullptr）

## 三、policy 插件的内部架构（5b）

policy 插件内部有两个线程：

- 推理线程：独立循环，从 latest_frame 快照读像素 → ONNX Runtime 推理
  → 后处理 → 写 detections
- 主线程（plugin_decide）：更新 latest_frame 快照，读 detections 缓存，
  组装 core_decision 返回

线程间同步：
- latest_frame 用 mutex 保护。decide 写入时拷贝像素；推理线程读出时也拷贝。
  为避免每帧拷贝 8MB，可以只在"上一帧推理已完成"时更新快照。
- detections 用 mutex 保护。推理线程写，decide 读。

节流：
- 推理线程以固定间隔（默认 10fps，可配）跑一次。
- 如果最新快照和上次推理时是同一帧，跳过。
- 推理线程不阻塞主线程。

## 四、配置项（5b 起）

插件从 config 读（前缀 policy_）：

  policy_model_path    ONNX 模型文件路径，必填
  policy_input_size    模型输入尺寸，默认 640
  policy_conf          置信度阈值，默认 0.25
  policy_iou           NMS IoU 阈值，默认 0.45
  policy_fps           推理频率，默认 10
  policy_gpu           1 = 用 DirectML，0 = 用 CPU，默认 1
  policy_labels        "me,monster" 逗号分隔，默认 "me,monster"

## 五、YOLOv8 后处理（5c）

模型输出格式（来自 Python 项目的 detector.py）：

- 输出 shape: [1, 4+nc, N]，nc=2 → [1, 6, 8400]（YOLOv8 解耦头）
- 无 anchor，前 4 行是 cx, cy, w, h（canvas 像素空间，640x640）
- 接下来 nc 行是类别分数（已 sigmoid）
- decode：cx/cy/w/h → xyxy → 映射回原图归一化 0~1
- NMS：自写 CPU 贪心 NMS，IoU 阈值 0.45
- 尺寸门：目标占裁剪区比例 < 30px 折算比例 → 丢弃

## 六、不在本阶段范围

- 动作决策（打怪状态机，5d）
- 脚本引擎（后续单独设计）
- 模型训练
- 多模型切换
- GPU 厂商特定优化（先 DirectML 通吃）

## 七、验收

- 5a 后：阶段 0/1/2 全绿（contract-change-procedure）
- 5b 后：core.exe 日志每秒打印约 10 次检测结果，进程不崩，不阻塞主循环
- 5c 后：检测结果与 Python 项目在同帧同输入下一致（对比置信度、坐标、类别）

## 八、一句话原则

先扩契约，再进推理线程；主线程不阻塞，推理线程独立跑。

## 九、实现状态（截至当前）

### 已完成

- 5a 契约变更：`core_intent.frame` 字段（ABI 2 → 3），已完成
- 5b ONNX Runtime 集成 + 独立推理线程：`src/real/yolo_policy.cpp`
  - 推理线程独立跑，主线程 `plugin_decide` 拷贝像素快照
  - `DirectML` 优先，失败自动回退 CPU
  - 输入张量去重：相同输入跳过推理
- 5c YOLOv8 后处理：已实现，含 decode / NMS / 尺寸门 / 额外过滤 / Tracker

### 5d 动作决策：不在 policy 里

`yolo_policy` 的 `plugin_decide` 只做一件事——把检测结果填进 `intent->detections_out`，`out->out_count = 0`。

动作决策（打怪状态机）由宿主内的脚本承担，源文件 `src/core/script/cpp_script.cpp`，详见 `docs/stage7.md` 第十一节。

### 配置项的实际状态

设计稿列了 7 个 `policy_*` 配置键，已全部实现读取：

| 键 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `policy_model_path` | 字符串 | `D:\dev\module-host\models\yolo11s.onnx` | ONNX 模型路径 |
| `policy_input_size` | 整数 > 0 | 640 | 模型输入尺寸 |
| `policy_conf` | 浮点 (0,1] | 0.7 | 置信度阈值 |
| `policy_iou` | 浮点 (0,1] | 0.45 | NMS IoU 阈值 |
| `policy_fps` | 整数 > 0 | 10 | 推理频率（每秒） |
| `policy_gpu` | 0 / 1 | 1 | 1 = DirectML 优先（失败回退 CPU），0 = 纯 CPU |
| `policy_labels` | 逗号分隔 | `me,monster` | 类别名；当前代码只用数量推 `num_classes`，不做名到类的映射 |

额外支持一个文档未提的键：

- `policy_verbose`：`"1"` 开启详细日志，其它值关闭。默认关闭。

非法值（空 / 越界 / 非数字）保持默认。

### 后处理的实现细节

- **输出 shape**：动态读取 `out_shape[2]` 作为 anchor 数量，不假设 8400。
- **类别数**：硬编码 2（me / monster）。
- **decode**：cx/cy/w/h → `(v - pad) / scale / orig_dim`，归一化到 0~1。
- **NMS**：自写 CPU 贪心，同类 IoU 阈值 0.45。
- **尺寸门**：两处
  - `PostprocessDetections`：`w < 20/640 && h < 20/640` 丢弃
  - `FilterDetections`（cls=1）：面积 < `1500/(1920*1080)` 丢弃；`cx < 0.05` 或 `cx > 0.95` 丢弃；`conf < 0.25` 丢弃
- **同类合并**：`FilterDetections` 里 IoU > 0.8 或中心距 < 0.03 时合并
- **Tracker**：IoU + 距离双门限匹配（`kMatchDx/kMatchDy = 0.06`），30 帧丢失后删除。

### 与设计文档的偏差

1. **5d 归属**：设计稿说"动作决策后续单独设计"，实际在 `cpp_script.cpp` 里实现（打怪状态机）。
2. **推理线程的输出接口**：设计稿说"推理线程写 detections 缓存，主线程读"，实际用 `g_frame_mutex` + `g_result_mutex` 双向同步，主线程拷贝像素快照、读检测结果。
