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
| **UI-2** | 画面叠加（检测框 / me / target / 攻击区） | 叠加位置与 YOLO 输出一致 |
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

**文档完**。
