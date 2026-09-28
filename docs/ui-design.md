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

**版本号**：`v1`，随每条消息发送，便于将来并存。

**格式**：JSON，单行，一帧一条。

**字段**（草案，实现前需与 `recorder` 实际 JSONL 字段核对并对齐命名）：

```json
{
  "v": 1,
  "frame": 12345,
  "now_ms": 1234567,
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
  }
}
```

**说明**：

| 字段 | 来源 | 备注 |
|---|---|---|
| `v` | 常量 1 | 契约版本，只加不改 |
| `frame` | `world.frame_index` | |
| `now_ms` | `world.now_ms` | |
| `script.*` | `CppScriptDebugInfo` 9 字段 | **与现有调试接口完全对齐，不增删改名** |

**待核对项**（实现前必须确认，不能猜）：

1. `recorder` 输出的 JSONL 中，决策相关字段（`dec`）的实际命名与结构。
2. 检测框（`det`）是否需要在本期发送——v1 状态面板不需要，但为了二期画面叠加，
   建议**现在就定好 `dets` 数组格式**，避免二期改契约。

若 `dets` 现在就定，格式建议对齐 `core_detection`：

```json
"dets": [{"cls":0,"cx":0.5,"cy":0.7,"w":0.05,"h":0.1}]
```

**v1 实现时可选发送 `dets`**，但契约里先占位，前端忽略即可。

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
├── telemetry_server.h/.cpp   HTTP + WebSocket 服务器
└── static/                   前端资源（HTML/JS/CSS）
    ├── index.html
    ├── app.js
    └── style.css
```

**依赖**：`cpp-httplib`（单头文件，无外部依赖，放 `third_party/`）。

**`main.cpp` 改动**：仅加一行降频调用：

```cpp
if (frame_index % 3 == 0) {          // 10Hz 降频（30fps 主循环）
    telemetry_server.Publish(bundle);
}
```

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
| **UI-1** | core 遥测服务器 + 前端状态面板 | 浏览器打开 `localhost:6601` 实时看到 state 变化 |
| **UI-2** | 画面叠加（检测框 / me / target / 攻击区） | 叠加位置与 YOLO 输出一致 |
| **UI-3** | FileSource 接 JSONL（场景 D） | 与 `script_replay` 回放帧一致 |
| **UI-4** | 控制通道（场景 C） | 权限与安全评审通过 |

**每阶段独立 commit**。

---

## 十、验收标准（UI-1）

```text
1. core.exe 启动后，浏览器访问 http://localhost:6601 能打开页面。
2. 页面每帧显示 state / facing / me_locked / target_locked / active_key。
3. 断开 UI（关浏览器），core 不崩溃，继续跑。
4. 停掉 core，UI 显示"连接断开"，不崩溃。
5. run_all.ps1 11 步仍全绿（遥测不影响验收流程）。
6. 不开启 UI 时，core 行为与基线逐字节一致（6 fixture IDENTICAL）。
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

---

## 十二、待办（实现前必须解决）

1. **核对 `recorder` JSONL 字段**，确认 `FrameBundle` 命名与之对齐。
2. **确认 `cpp-httplib` 版本**与许可证，落 `third_party/`。
3. **确认端口 6601/6602 未被占用**。
4. **确认 `main.cpp` 中降频点位置**，不改变原有控制流。
5. **与执行 AI 约定**：任何与本文档不符处，停止并贴回，不猜。

---

**文档完**。
