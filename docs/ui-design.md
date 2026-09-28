# module-host 可视化界面·设计文档

**版本**：v1（草案）
**基线 commit**：`da7b62a`
**状态**：设计冻结中，未实现

---

## 一、定位与目标

为 module-host 提供可视化界面，用于观察脚本运行时的状态机、锁定与决策。

**核心目标**：**UI 实现方式可替换**。

- 今天用 Web，明天想换 Tauri / Qt / 终端 TUI / 远程网页，
  **不需要重新编译 core.exe**。
- 达成手段：core 只暴露一个稳定的遥测接口，UI 只是它的消费者。

**判据**：换 UI 实现时，core 是否需要重编译？答案必须是「否」。

---

## 二、非目标（v1 明确不做）

| # | 不做 | 理由 |
|---|---|---|
| N1 | 画面叠加（检测框 / me / target / 攻击区） | 需要视频流传输，最重部分，放二期 |
| N2 | 反向控制（启停 / 调参 / 注入按键） | 需要权限与并发同步，放三期 |
| N3 | 远程鉴权 | 同机 localhost，二期再说 |
| N4 | 帧级录制回放（场景 D） | 已有 `script_replay`，二期接同一份前端 |
| N5 | 引入 Qt / ImGui / Electron | 违背「实现可替换」，见 §三 |

---

## 三、设计原则

| # | 原则 |
|---|---|
| **U1** | **core 零感知 UI**：UI 挂了 core 照跑；core 不知道 UI 存在 |
| **U2** | **接口冻结**：遥测数据契约一旦定版，只加不改 |
| **U3** | **不碰 `core_contract.h`**：遥测是宿主内部能力，不是插件契约 |
| **U4** | **依赖单向**：`telemetry` 只被 `main.cpp` 调用，不反向依赖脚本内部 |
| **U5** | **前端无框架**：原生 JS + Canvas，越薄越好换 |
| **U6** | **字段对齐 recorder**：实时流与 JSONL 回放共用一套解析 |

---

## 四、架构概览

```
┌──────────────────────────────────────────────┐
│  core.exe                                     │
│                                               │
│   主循环（不动）                               │
│     └── TelemetryServer.Publish(bundle)       │
│           降频 10Hz                            │
│                                               │
│   HTTP :6601                                  │
│     ├── GET /            → 前端静态资源        │
│     └── GET /ws          → WebSocket 遥测流    │
└──────────────────────────────────────────────┘
                    │
                    ▼
┌──────────────────────────────────────────────┐
│  UI（浏览器 / Tauri 壳 / 任意消费者）          │
│                                               │
│   DataSource 抽象                              │
│     ├── LiveSource   ← WebSocket              │
│     └── FileSource   ← JSONL（二期）           │
│                                               │
│   视图层                                       │
│     └── 状态面板（v1 唯一视图）                │
└──────────────────────────────────────────────┘
```

**关键**：core 只发，不读。UI 只读，不控。二者通过 HTTP + WebSocket 解耦。

---

## 五、数据契约：FrameBundle v1

**版本号**：`v1`，随每条消息发送。

**格式**：JSON，单行，一帧一条。

**字段**：

```json
{
  "v": 1,
  "frame": 12345,
  "t": 123456789,
  "script": {
    "state": 1,
    "facing": 1,
    "me_locked": true,
    "me_fx": 0.500,
    "me_fy": 0.720,
    "target_locked": true,
    "target_cx": 0.630,
    "active_key": 0,
    "desired_e": false
  },
  "dets": [
    {"cls": 0, "conf": 0.90, "cx": 0.50, "cy": 0.70, "w": 0.05, "h": 0.10, "id": 1}
  ]
}
```

**字段说明**：

| 字段 | 来源 | 备注 |
|---|---|---|
| `v` | 常量 1 | 契约版本，只加不改 |
| `frame` | `frame_index` | |
| `t` | 微秒，QPC 时间戳（与 recorder 的 `t` 同一来源） | 不加 `now_ms` |
| `script.*` | `CppScriptDebugInfo` 9 字段 | **与现有调试接口完全对齐，不增删改名**（C2 硬约束） |
| `dets[]` | `core_detections` | 字段名与 recorder `det` 行一致：`cls/conf/cx/cy/w/h/id` |

**与 recorder JSONL 的关系**：

- **不是镜像**。recorder 是录制（只写必要字段），FrameBundle 是遥测（面向 UI）。
- **字段名尽量对齐**，便于前端共用解析，但 FrameBundle 含 recorder `dec` 行没有的脚本状态字段（`state/facing/...`）。
- `script.*` 与 `CppScriptDebugInfo` 逐字段对齐，来源见 §七。

**v1 实现时 `dets` 可选发送**（状态面板不需要），但契约先定好，二期画面叠加直接启用。

**与 §十二 待办 1 的关系**：recorder 字段核对已完成，本契约已对齐实际 JSONL。

---

## 六、通道设计

| 通道 | 协议 | 方向 | 端口 | 状态 |
|---|---|---|---|---|
| 静态资源 | HTTP GET `/` | core → UI | 6601 | v1 |
| 遥测流 | WebSocket `/ws` | core → UI | 6601 | v1 |
| 控制通道 | WebSocket `/ctl` | UI → core | 6602 | **v1 不实现，端口与路径预留** |

**预留控制通道的理由**：协议一旦上线就难改，现在定好路径和消息骨架，
三期实现时不必重开设计。

控制消息骨架（v1 只占位，不实现）：

```json
{"v":1, "cmd":"start|stop|set_config", "payload":{}}
```

---

## 七、core 侧模块

```
src/core/telemetry/
├── telemetry_protocol.h      FrameBundle 结构定义（冻结）
└── telemetry_server.h/.cpp   HTTP + WebSocket 服务器

前端资源内嵌在 telemetry_server.cpp 的 raw string literal 中
（不挂载 static/ 目录，避免运行目录依赖）。
UI-1c-1 内置占位页；UI-1c-2 换成正式状态面板。
```

**依赖**：`cpp-httplib`（单头文件，MIT 许可证，放 `third_party/httplib/httplib.h`）。
**WebSocket 需 `cpp-httplib >= 0.14`**。

**`ScriptHost` 新增转发**（不改 `CppScript` 本体）：

```cpp
// script_host.h
#include "cpp_script.h"    // 需要 CppScriptDebugInfo 类型

// 读取脚本内部调试状态。空脚本时 out 保持默认值。
void GetDebugInfo(CppScriptDebugInfo* out) const;
```

```cpp
// script_host.cpp
void ScriptHost::GetDebugInfo(CppScriptDebugInfo* out) const {
    if (out == nullptr) return;
    if (!impl_->script) return;
    auto* cpp = dynamic_cast<CppScript*>(impl_->script.get());
    if (cpp) cpp->GetDebugInfo(out);
}
```

**`main.cpp` 改动清单**（共 4 处）：

1. include 区：`#include "telemetry/telemetry_server.h"`
2. 主循环外（与 recorder 同层）：构造 `TelemetryServer telemetry;`
   和 `telemetry.Start(6601);`（失败不阻塞，只是不开遥测）
3. 主循环内 486 行后、488 行前，插入降频调用：
   ```cpp
   if (frame_index % 3 == 0) {
       FrameBundle b;
       b.frame = static_cast<uint64_t>(frame_index);
       b.t = ...;                              // 与 recorder NowMicros 同源
       script_host.GetDebugInfo(&b.script);
       for (uint32_t di = 0; di < detections.count; ++di) { b.dets.push_back(...); }
       telemetry.Publish(b);
   }
   ```
4. `main.cpp` 的 `#else`（非 STAGE2）分支**不加**，该分支无帧循环

**CMakeLists.txt 改动**：
- 第 14 行 source 列表末尾加 `src/core/telemetry/telemetry_server.cpp`
- `target_include_directories` 已有 `third_party/`，cpp-httplib 直接可用
- 链接库已有 `ws2_32`，WebSocket 需要，无需新增

**不碰**：`core_contract.h`、插件、`script/` 下任何文件、`output_manager`、`recorder`。

---

## 八、前端结构

```
index.html      页面骨架
app.js
  ├── LiveSource      WebSocket 客户端
  ├── FileSource      （二期）读 JSONL
  └── render()        状态面板渲染
style.css
```

**无框架**。状态面板 = 一组 DOM 节点 + 每帧 `textContent` 更新。

前端源码（index.html/app.js/style.css）内嵌到 server cpp；
二期若复杂化再拆为 `static/` 目录。

**DataSource 抽象**（前端侧）：

```js
class LiveSource {
  constructor(url) { ... }
  onFrame(cb) { ... }
}

class FileSource {   // 二期
  constructor(jsonlText) { ... }
  onFrame(cb) { ... }
}
```

视图层只调 `onFrame`，不关心数据来自实时还是文件。

---

## 九、分期计划

| 阶段 | 内容 | 验收 |
|---|---|---|
| **UI-0** | 本文档冻结 | 文档评审通过，契约定版 |
| **UI-1a** | 修正本文档（5 处不符） | 文档 commit |
| **UI-1b** | `ScriptHost::GetDebugInfo` 转发 + main.cpp 取得脚本状态 | run_all 全绿 + 6 fixture IDENTICAL |
| **UI-1c** | cpp-httplib + telemetry 模块 + 前端 | 浏览器 `localhost:6601` 看到 state 变化 |
| **UI-2** | 画面叠加（检测框 / me / target / 攻击区，B1 抽象画布） | 见 §13 |
| **UI-2a** | 设计文档 | 已冻结（本文档） |
| **UI-2b** | 后端填 `dets` | 6 fixture IDENTICAL |
| **UI-2c** | 前端 canvas | 人眼确认元素显示正确 |
| **UI-3** | FileSource 接 JSONL（场景 D） | 与 `script_replay` 回放帧一致 |
| **UI-4** | 控制通道（场景 C） | 权限与安全评审通过 |

**每阶段独立 commit**。

---

## 十、验收标准（UI-1c）

```text
1. core.exe 启动后，浏览器访问 http://localhost:6601 能打开页面。
2. 页面每帧显示 state / facing / me_locked / target_locked / active_key。
3. 断开 UI（关浏览器），core 不崩溃，继续跑。
4. 停掉 core，UI 显示"连接断开"，不崩溃。
5. run_all.ps1 11 步仍全绿。
6. 不开启 UI 时，core 行为与基线逐字节一致（6 fixture IDENTICAL）。
```

**UI-1b 验收标准（更早一步）**：

```text
1. run_all.ps1 11 步全绿。
2. 6 fixture MH_SCRIPT_SEED=42 逐字节 IDENTICAL。
3. main.cpp 能从 script_host.GetDebugInfo 拿到 state/facing/... 9 字段，
   仅 printf 打印验证，不接 telemetry。
```

**第 6 条是关键**：遥测是旁路，不能改变脚本行为。

---

## 十一、与项目约定的关系

| 约定 | 本设计如何遵守 |
|---|---|
| C1–C7 契约约束 | 不碰 `core_contract.h`，ABI 仍为 4 |
| P1 不改行为 | 遥测旁路，6 fixture 逐字节仍 IDENTICAL |
| P2 不留空壳 | v1 只实现 UI-1，控制通道仅占位不写代码 |
| P3 依赖单向 | `telemetry` ← `main.cpp`，不反向 |
| P4 不过度设计 | 无框架、无鉴权、无画面叠加 |
| P6 每步独立 commit | UI-0 … UI-4 各自独立 |
| **C2 硬约束** | `script.*` 9 字段与 `CppScriptDebugInfo` 逐字段对齐，不增删改名 |

---

## 十二、待办（实现前必须解决）

1. ~~核对 `recorder` JSONL 字段~~ —— **已完成**，见 §五。
2. **引入 `cpp-httplib`**：版本 ≥ 0.14（需 WebSocket），放
   `third_party/httplib/httplib.h`。执行 AI 无法联网，需人工放置。
3. **确认端口 6601/6602 未被占用** —— 已确认 6601 空闲。
4. **`main.cpp` 降频点**：主循环 486–490 行之间，仅 STAGE2 分支。
5. **确认 `t` 时间戳来源**：与 recorder `NowMicros()` 同源，
   建议在 telemetry 模块内自带一个同样的 QPC 微秒函数，
   或复用（需评估是否把 `NowMicros` 从 `recorder.cpp` 的匿名
   namespace 提出来）。
6. **与执行 AI 约定**：任何与本文档不符处，停止并贴回，不猜。

---

---

## 十三、UI-2 设计：画面叠加（抽象画布）

**状态**：设计冻结中，UI-2b/2c 待实现。

### 13.1 目标与非目标

**目标**：在状态面板下方加一块 canvas，以归一化坐标系绘制
检测框、me 点、target 点、攻击区椭圆，让脚本的空间决策可视化。

**非目标**：

- 不传真实视频帧（B2 方案）。
- 不接入 `remote_server` 的视频流（B3 方案，后续评估）。
- 不做时间回溯、不显示历史帧。
- 不做缩放/平移交互。

**采用 B1 方案**（抽象 2D 画布）。

### 13.2 画布

| 项 | 值 |
|---|---|
| CSS 尺寸 | 800×450（16:9） |
| 内部分辨率 | `800 * dpr × 450 * dpr`，`ctx.scale(dpr, dpr)` |
| DPR 探测 | `window.devicePixelRatio \|\| 1` |
| 背景 | `#181818`（比页面 `#1e1e1e` 略深） |
| 网格 | 每 0.1 归一化一条线，颜色 `#2a2a2a` |

### 13.3 坐标映射

**归一化直接映射，无需分辨率**：

```
canvas_x = cx * canvas_css_width
canvas_y = cy * canvas_css_height
```

**16:9 保真性质**：因为 canvas 宽高比与 1920×1080 相同，
按各自维度映射后，像素坐标下圆形的物体在画布上仍是圆形。

**证明**：像素半径 r=100 的圆，归一化半径 x 方向 `100/1920`、
y 方向 `100/1080`；画布上 x 像素 = `(100/1920)*800 = 41.7`、
y 像素 = `(100/1080)*450 = 41.7`，相等。

**y 方向**：与屏幕坐标一致，y 向下增大。`me.fy` 是脚底中心，
所以 me 点显示在偏低位置，攻击区主要在屏幕上方。

### 13.4 绘制元素

| 元素 | 数据源 | 颜色 | 绘制方式 |
|---|---|---|---|
| 背景网格 | 常量 | `#2a2a2a` | 每 0.1 一条线 |
| 检测框 | `dets[]` | 见下表 | 矩形边框 1px |
| me 点 | `script.me_fx/fy` | `#4ec9b0` | 实心圆 r=5 + 十字 |
| target 位置线 | `script.target_cx` | `#f48771` | 竖直虚线，全高 |
| target 点 | `(target_cx, me_fy)` | `#f48771` | 空心圆 r=6，虚线描边 |
| 攻击区 | `script.me_fx/fy/facing` + 常量 | 半透明 `#0e639c` 填充 + `#4fc3f7` 描边 | 多边形路径 |

**检测框按 cls 着色**：

| cls | 名称 | 颜色 |
|---|---|---|
| 0 | me | `#4ec9b0`（青绿） |
| 1 | monster | `#e0e0e0`（灰白） |

**绘制条件**（缺则跳过，不报错）：

- `script.me_locked == true` → 画 me 点、攻击区
- `script.target_locked == true` → 画 target 位置线、target 点
- `dets` 非空 → 画检测框
- 其它情况对应元素跳过

**dets 过滤**：前端不做过滤，信任 `yolo_policy` 已过滤
（面积、边界在 policy 里已处理）。全部显示。

**target 点 y 坐标**：`CppScriptDebugInfo` 只有 `target_cx`，
没有 `target_cy`。用 `me_fy` 近似——perception 里
`kSamePlatY = 0.028` 意味着 target 与 me 的脚底 y 差 ≤ 0.028。
UI-2 不改协议，用虚线圆表达"位置近似"。

### 13.5 攻击区椭圆绘制规范

**几何定义**（源自 `script_geometry.cpp`，与 `IsInBand` 逐条对应）：

以 `me.fx / me.fy` 为原点，facing 方向为 x 正方向：

| 边界 | 公式 |
|---|---|
| 内边界 x_min | `kBandXMin = 0.010` |
| 外边界 x_max(y) | `kBandXMaxSame * sqrt(1 - (y/y_half)^2)` |
| 上边界 y_top | `kBandYMin = -0.074` |
| 下边界 y_bot | `kBandYMax = +0.019` |
| 上侧半短轴 y_half(y<0) | `-kBandYMin = 0.074` |
| 下侧半短轴 y_half(y>0) | `kBandYMax = 0.019` |

**注意**：上下半短轴不同，所以**不是椭圆**，是"上下不对称的双弧扇段"。
几何形状是：x_min 竖线 + 上下不对称椭圆弧。

**关键常量**（前端硬编码，与 `ScriptConfig` 同值）：

```js
var BAND_X_MIN     = 0.010;
var BAND_X_MAX     = 0.1458;
var BAND_Y_TOP     = -0.074;
var BAND_Y_BOT     = 0.019;
var BAND_Y_HALF_UP = 0.074;   // = -BAND_Y_TOP
var BAND_Y_HALF_DN = 0.019;   // = BAND_Y_BOT
```

前端**硬编码**这些值，不通过遥测传输——它们属于"脚本几何契约"，
变化频率极低（重构期间从未变过）。若将来 `ScriptConfig` 改了这些常量，
前端需同步。这一风险记入 §13.7 的"协议-前端同步契约"。

**绘制步骤**：

1. 采样外弧：对 `y ∈ [BAND_Y_TOP, BAND_Y_BOT]` 取 N=32 个点
   - `y_half = (y < 0) ? BAND_Y_HALF_UP : BAND_Y_HALF_DN`
   - `x_outer = BAND_X_MAX * sqrt(1 - (y/y_half)^2)`
   - 若 `x_outer < BAND_X_MIN`，跳过（理论上不会发生，
     因为 `(BAND_X_MIN/BAND_X_MAX)^2 = 0.0047 << 1`）
2. 内边界：从 `(BAND_X_MIN, BAND_Y_TOP)` 到 `(BAND_X_MIN, BAND_Y_BOT)`
3. 组合为闭合多边形：
   ```
   起点: (x_min, y_top)
   → 沿 x_min 竖线到 (x_min, y_bot)
   → 沿外弧反向采样回到 (x_min, y_top)
   → closePath
   ```
4. 坐标变换：
   - `canvas_x = me_fx * W + facing * norm_x * W`
   - `canvas_y = me_fy * H + norm_y * H`
5. 填充 + 描边

**归一化到像素的转换**：全部乘 canvas CSS 尺寸，按各自维度。

**若 `me_locked == false`**：不画攻击区。

### 13.6 布局

```
┌──────────────────────────────────────┐
│  module-host monitor      [connected]│
├──────────────────────────────────────┤
│       state   [IDLE]                 │
│       frame   1234                   │
│       facing  1 (→)                  │
│       me      locked (0.50, 0.72)    │
│       target  locked cx=0.63         │
│       active_key  0x00               │
│       desired_e   false              │
├──────────────────────────────────────┤
│  ┌────────────────────────────────┐  │
│  │                                │  │
│  │       [canvas 800×450]          │  │
│  │                                │  │
│  └────────────────────────────────┘  │
└──────────────────────────────────────┘
```

- 状态面板保持现有布局（grid 两列，max-width 640）
- canvas 居中，`margin: 24px auto 0`
- 页面总宽 `max-width: 840px`

**移动端**：不做适配（本地调试工具，假定桌面）。

### 13.7 实现拆分

| 子步 | 内容 | 验收 |
|---|---|---|
| **UI-2a** | 本文档（设计冻结） | 文档 commit |
| **UI-2b** | 后端填 `dets`：`main.cpp` 组装 bundle 时遍历 `detections`，填 `frame_bundle.dets` | 6 fixture IDENTICAL + run_all 全绿 + 浏览器收到非空 `dets` |
| **UI-2c** | 前端 canvas：内嵌 HTML 加 `<canvas>` + 绘制逻辑 | 人眼确认元素显示正确 |

**协议-前端同步契约**（UI-2b 起生效）：

- 前端硬编码 §13.5 的 5 个常量。
- 若 `ScriptConfig` 中这 5 个常量发生变化，**必须同步更新前端**。
- 前端不加"从遥测读常量"的机制（P4 不过度设计）。

### 13.8 UI-2c 前端实现约束

- 继续无框架（原生 JS + Canvas）
- 继续 ES5 语法（`var` / 无箭头 / 无模板字符串）
- Canvas API 用 `getContext('2d')`
- 绘制在 `render(msg)` 中，每收到一帧消息重绘一次
- 无动画循环、无 `requestAnimationFrame`（数据驱动，收到就画）
- 无外部资源

---

---

## 十四、控制消息契约（v0）

**引入**：UI-3c-1。

**用途**：core → UI 的非 FrameBundle 消息，用于表达"回放开始/结束"等控制语义。
与普通帧并行在同一条 WebSocket 流上。

**区分方式**：

- `v == 1` → FrameBundle（普通帧），按 §五 解析
- `v == 0` → 控制消息，按本节的 `meta` 字段分流

**已定义的控制消息**：

| meta | 方向 | 时机 | 字段 |
|---|---|---|---|
| `replay_begin` | core → UI | core 回放模式开始推流前 | `{"v":0,"meta":"replay_begin"}` |
| `replay_end` | core → UI | core 回放模式所有帧推完后 | `{"v":0,"meta":"replay_end"}` |

**语义**：

- 前端收到 `replay_begin` → 进入"回放缓存模式"，此后 `v == 1` 的帧累积到缓存，不实时渲染
- 前端收到 `replay_end` → 缓存完成，切换到"本地播放"模式，显示控制条
- 未收到 `replay_begin` 而收到 `v == 1` → 实时模式，按现有逻辑渲染

**前端行为（UI-3c-2）**：

- `replay_begin` / `replay_end` 之间的普通帧，**全部缓存**到本地数组
- 缓存完成后，前端以 1x 播放缓存的帧，提供暂停 / 速度 / 步进 / 进度条
- 断线重连后，前端重新进入"未知模式"，等待 core 重新发 `replay_begin`（实时模式不会发）

**已知限制**：

- core 只在回放开始/结束时各发一次 `replay_begin` / `replay_end`。
  若客户端连接晚于 `replay_begin`，会错过，前端进入"未知模式"（按实时渲染）。
- UI-4 或后续版本可加"新客户端连接时重发 replay_begin"的能力。

**扩展预留**：

- 未来 `paused` / `resumed` / `seek` 等控制消息也走此路径，`v == 0`。
- 版本演进：若契约有破坏性变更，`v` 递增（`v == 0` 始终是"控制消息"分类）。

---

**文档完（含 UI-2、控制消息契约）。**
