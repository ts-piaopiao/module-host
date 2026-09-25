# 阶段 3：契约扩展与真实插件集成

## 一、背景

阶段 0/1/2 完成了契约、加载器、假插件闭环和运行期错误验收。
阶段 3 引入真实硬件：采集卡作为画面源，串口作为键鼠输出。
但现有契约（ABI=1）无法承载真实业务，必须扩展。
本次扩展将 CORE_ABI_VERSION 从 1 升到 2。

## 二、契约扩展清单

### 2.1 core_frame 加时间戳

新增字段：
  int64_t pts_ms;
放在结构体末尾。
语义：该帧的采集时间戳，单位毫秒，从某个固定起点（如进程启动）计算。
capture 插件负责填充。宿主不得依赖具体起点，只用于计算帧间隔或超时。

### 2.2 core_frame 生命周期规则

在契约头文件里以注释形式写死：
- data 指针由 capture 插件持有。
- 有效期到下一次 plugin_capture 调用前。
- 宿主不得跨帧保存 data 指针。
- 宿主不得修改 data 指向的像素。

### 2.3 core_decision 从 int32_t items[8] 扩成动作序列

新增枚举 core_action_kind：
  CORE_ACTION_NONE = 0
  CORE_ACTION_POINTER_MOVE = 1
  CORE_ACTION_POINTER_BUTTON = 2
  CORE_ACTION_KEY = 3
  CORE_ACTION_WAIT = 4
  CORE_ACTION_CUSTOM = 5

新增结构体 core_action：
  core_action_kind kind;
  int32_t a;
  int32_t b;
  int32_t c;

语义表：
| kind | a | b | c |
|---|---|---|---|
| NONE | 忽略 | 忽略 | 忽略 |
| POINTER_MOVE | dx | dy | 忽略 |
| POINTER_BUTTON | button_id | 1按下/0抬起 | 忽略 |
| KEY | Windows Virtual-Key 码 | 1按下/0抬起 | 忽略 |
| WAIT | 毫秒 | 忽略 | 忽略 |
| CUSTOM | 插件自定义 | 插件自定义 | 插件自定义 |

button_id 约定：
  1 = 左键
  2 = 右键
  3 = 中键

core_decision 改为：
  core_action actions[CORE_DECISION_CAPACITY];
  uint32_t out_count;
CORE_DECISION_CAPACITY 保持 8。

### 2.4 plugin_init 加配置入口

旧签名：core_error plugin_init(uint32_t host_abi);
新签名：core_error plugin_init(uint32_t host_abi, const char* config);

config 为 UTF-8 字符串，采用 key=value 格式（与宿主 --config 一致），
多个键之间用换行分隔。宿主只透传，不解释内容。
未提供配置时传空字符串 ""。
插件必须容忍 config == nullptr 或空字符串。

### 2.5 plugin_execute 加结果反馈

新增结构体 core_execute_result：
  core_error status;
  int32_t detail;

旧签名：core_error plugin_execute(const core_decision* decision);
新签名：core_error plugin_execute(const core_decision* decision, core_execute_result* out);

out 允许为 nullptr，此时插件只需执行，不需要回填结果。
status 由插件填执行结果（CORE_OK 或错误码）。
detail 由插件自定义，宿主不解释。

plugin_execute 每次决策只调用一次。
参数 decision 包含全部待执行动作（actions[0..out_count-1]）。
input 插件负责遍历 actions 并逐条执行。
宿主不得按 out_count 次数重复调用 plugin_execute。
这样可以支持动作间的等待、失败重试、超时控制等策略。

3a 阶段假插件的 plugin_execute 实现：
  - 签名改为 core_error plugin_execute(const core_decision*, core_execute_result*)
  - 如果 out 非空，填 out->status = CORE_OK, out->detail = 0
  - 返回 CORE_OK
3a 阶段 main.cpp 的处理：
  - 调用 plugin_execute(&decision, &result)
  - 检查返回值 == CORE_OK 且 result.status == CORE_OK
  - 任一失败打印 [错误] 执行失败 退出非 0
  - 5 帧循环里不再按 out_count 重复调用

## 三、不受影响的接口

- plugin_meta：签名不变
- plugin_release：签名不变
- plugin_capture：签名不变，但需要填充 pts_ms
- plugin_decide：签名不变，但 core_decision 结构变了

## 四、受影响的文件清单

契约变更必须同步修改的文件：
- core_contract.h
- tests/test_contract_c.c
- tests/test_contract_cpp.cpp
- 所有 20 个桩源文件：
  src/stubs/stub_capture.cpp / stub_policy.cpp / stub_input.cpp
  src/bad_plugins/*.cpp（9 个）
  src/normal/*.cpp（3 个）
  src/runtime_errors/*.cpp（5 个）

宿主侧需同步修改：
- src/core/main.cpp（plugin_init 调用、plugin_execute 调用、5 帧循环里填 core_intent / core_decision）

## 五、验收流程

按 docs/contract-change-procedure.md 走：

1. 改 core_contract.h，CORE_ABI_VERSION 从 1 升到 2。
2. 改测试文件里的断言。
3. 重跑阶段 0：C11 + C++17 编译通过。
4. 同步改 20 个桩和 main.cpp。
5. 重跑阶段 1：OFF 下 run_acceptance.ps1 -Stage stage1 必须 13/13 PASS。
6. 重跑阶段 2：ON 下 run_acceptance.ps1 -Stage stage2 必须 6/6 PASS，正常 plugins 输出 5 帧退出 0。
7. 全部通过后 commit，message 以 "contract:" 开头。

注意：所有桩的 meta 里 ABI 段要从 "1" 改成 "2"。
bad_plugins 里 abi_mismatch_capture 的 ABI 段要改成 "3"，保持不匹配语义。

## 六、不在本阶段范围

- 真实 capture 插件（阶段 3b）
- 真实 input 插件（阶段 3c）
- 真实 policy 插件（阶段 3d，等 YOLO 接入）
- 远程画面推流
- 人工操作与 YOLO 自动决策的仲裁

## 七、一句话原则

先扩契约，再重跑全部旧验收，全绿后才接真实硬件。

## 八、实现状态（截至当前）

### 已完成

- 契约扩展（历经 ABI 2 → 3 → 4 三次变更）：
  - ABI=2：`core_frame.pts_ms`、`core_action`、`core_decision.actions`、
    `core_execute_result`、`plugin_init(host_abi, config)`、
    `plugin_execute(decision, out)`
  - ABI=3：`core_intent.frame`
  - ABI=4：`core_detection`、`core_detections`、`core_intent.detections_out`
- 阶段 0/1/2 全部回归通过（当前 11/11 + 5/5，两插件架构下）
- 真实 capture 插件：`src/real/real_capture.cpp`，从采集卡 Media Foundation 拉帧，
  输出 1920x1080@30 BGRA8，帧缓冲在插件内部复用，
  `ReadSample` 遇 STREAMTICK 重试（1 秒超时）
- 真实 policy：`src/real/yolo_policy.cpp`，ONNX Runtime + DirectML，
  推理线程独立跑，输出 detections
- 测试 policy：`src/real/test_policy.cpp`，每帧产出"按 I 一次"两个动作
- 串口输出：不再作为 input 插件，而是宿主内的 `src/core/output_manager.cpp`，
  独占串口，115200 8N1，命令格式 `mk.*`
- 远程操作：`src/core/remote_server.cpp` + `experiments/remote_client`，
  JPEG 推流 + 上行键鼠事件
- F12 控制模式：client → server → `OutputManager::SetScriptPaused`
- 真实插件输出到 `build\Release\plugins_real\`，
  与假插件 `plugins\` 分离，不影响无硬件环境下的 CI
- 插件配置通过宿主 `--config` 文件原文透传，前缀键 `capture_` / `policy_`
  由宿主放行、插件自行解释

### 未完成

无（就本阶段定义的范围）。

### 与早期设计的偏差

1. **input 插件已移除**，串口职责内化进 `OutputManager`。
   理由：远程操作需要"拿到就发"，不经过帧循环，避免 33ms 梳状。
   详见 `docs/stage4.md` 第十三节。
2. **`plugin_execute` 每次决策只调用一次的设计**（2.5 节）在 input 插件移除后不再适用——
   决策输出改由 `OutputManager::SendScript` 直接入队。
3. **真实 policy 从占位演进为推理**：`test_policy` 保留用于无 YOLO 环境；`yolo_policy` 是正式实现。
4. **人工/自动仲裁未实现**——架构演进后不再需要（两条输出路径独立）。
   详见 `docs/stage4.md` 第十三节。
5. **ABI 从 2 继续升到 3 和 4**（`docs/stage5.md`、`docs/stage6.md` 各一次契约变更）。

### 已验证（补充）

- **capture 长稳**：20000 帧（1920×1080@30，约 11 分钟）连续拉帧，内存稳定在 552–553 MB，无增长、无错误、无崩溃。测试脚本 `scripts/run_capture_soak.ps1`（不纳入 `run_all.ps1`，需真实采集卡）。

### 已知未验证项

- 串口高频发送（>30 次/秒）时是否丢包（由 `OutputManager` 承担）
- 采集卡被其他程序占用时的错误恢复
- YOLO 推理与 Python 项目在同帧同输入下的一致性（对比置信度、坐标、类别）
