# systemPatterns — 架构、设计模式与组件关系

> 有架构级变化时更新本文件。

## 1. 总体分层

```
┌─────────────────────────────────────────────────────────────────┐
│ core.exe  (src/core/main.cpp)                                   │
│                                                                 │
│  capture_plugin.dll ─┐                                          │
│                      ├─ core_contract.h（唯一契约，ABI 4）       │
│  policy_plugin.dll ──┘                                          │
│                                                                 │
│  决策层（宿主内，可替换脚本）                                      │
│    IScript ← ScriptHost ← CppScript                            │
│                 │                                               │
│                 ├─ script_perception   me_lock / 目标筛选 / 层     │
│                 ├─ script_geometry     攻击带判定 IsInBand        │
│                 ├─ script_fsm          状态机骨架 + 进出带策略      │
│                 ├─ script_states       各状态具体动作              │
│                 ├─ script_sampling     按键采样（最短按住/强制松键） │
│                 ├─ script_config       全部可调常量               │
│                 └─ human_profile       人类行为参数                │
│                                                                 │
│  输出                                                           │
│    OutputManager ── SendScript（主线程，帧同步）                  │
│                  └─ SendAsync（网络线程，远程人工事件直达串口）     │
│    Recorder（JSONL 录制） / RemoteServer（JPEG 推流+上行键鼠）     │
│    TelemetryServer（HTTP + WS，仅被 main.cpp 调用）               │
└─────────────────────────────────────────────────────────────────┘
        │ HTTP/WS :6601
        ▼
   浏览器前端（无框架，原生 JS + Canvas，单文件内嵌于 telemetry_server.cpp）
```

## 2. 关键架构决策（与早期设计的偏差）

| # | 早期设计 | 实际 | 理由 / 出处 |
|---|---|---|---|
| 1 | 三插件 capture/policy/**input** | 两插件 + `output_manager` | input 插件移除，串口职责内化，避免 33ms 梳状帧。`docs/stage4.md` §十三 |
| 2 | `class Decider` | `IScript` / `ScriptHost` / `CppScript` 三层 | 决策做成可替换脚本，便于离线回放验证。`docs/stage6.md` §九 |
| 3 | `class Combat` | `script_fsm.cpp` + `script_states.cpp` | 状态机拆分。`docs/stage7.md` §十一 |
| 4 | 决策层嵌在宿主 / policy | policy 只推理，决策在宿主 | `docs/stage6.md` §九 |
| 5 | 远程 vs 脚本需要**仲裁** | **架构演进后天然不需要** | 两条输出路径从源头独立。`docs/stage4.md` §十三 |
| 6 | `me_lock` 在宿主侧 | 在 `script_perception.cpp` 的 `SelectMe()` | `docs/stage6.md` §九 |

## 3. 契约模式

- `core_contract.h` 是**唯一**契约头，同时被 C11（插件）与 C++17（宿主）包含。
- 全部约束用**编译期断言**表达；ABI 号硬编码在契约中，运行期做严格解析比对。
- 错误消息有**固定模板**（缺 DLL / 缺符号 / ABI 不匹配 / 元数据无效 / kind 不匹配 / init 失败），
  验收脚本按关键词匹配。
- **契约变更 = 单开一个「契约变更阶段」**，重新验收所有插件
  （`docs/contract-change-procedure.md`）。历史 ABI 演进：2 → 3 → 4。

### 插件输出目录隔离（模式）

同一 target 名（`capture_plugin.dll` / `policy_plugin.dll`）分发到**不同目录**，
坏插件 / 运行期错误插件 / 正常插件 / 真实插件互不污染：

```
build/Release/stubs/            正常假插件（OFF / 阶段1/2）
build/Release/plugins/          正常插件（ON / 阶段2）
build/Release/bad_plugins/<x>/  10 个加载类错误场景
build/Release/runtime_errors/<x>/ 4 个运行期错误场景
build/Release/plugins_real/     真实采集卡 + test_policy（阶段3）
build/Release/plugins_yolo/     yolo_policy（阶段5）
```

## 4. 决策层内部模式

### 4.1 几何判定（`script_geometry.cpp`）

攻击带是**五边形**（归一化坐标，`facing` 取符号）：

```text
尖端 (kBandXApex, 0)
上斜边 → (kBandXApex + 0.074/slope, kBandYMin)
上水平边 → (kBandXMaxSame, kBandYMin)
远端竖直线：x = kBandXMaxSame
下水平边 → (kBandXApex + 0.019/slope, kBandYMax)
下斜边 → 回到尖端
```

- 判定（给定代表高度 y_rep）：
  ```text
  x_inner(y) = kBandXApex + |y| / kBandApexSlope
  x_outer    = kBandXMaxSame（常数）
  ```
- 三个判定：`in_band_front`（正向）、`in_band_back`（反向，x 取反）、
  `in_band_any = front || back`。**`need_move = !in_band_any`**（读 any！）
- 常量：`kBandXApex=0.005`、`kBandApexSlope=1.0`（顶点总角 90°）、
  `kBandXMaxSame=0.1458`（= 280px @1920）。
- **v3 双向性**：近端在 `|y|>0` 处收紧（`x_inner`：v2 恒 ~0.010 → v3 = 0.005+|y|，最大 0.079），
  远端放宽（椭圆弧 → 恒 0.1458）。只在 `y=0` 与 v2 重合，整带右移。
- 早期排除（快速 return）阈值用 `kBandXApex`；精确判定用 `x_inner_at_y`。
- **前后端同一套几何**：`script_geometry.cpp::IsInBand` 与
  `telemetry_server.cpp::drawBand` 必须一致，改一处必须改另一处。

### 4.2 状态与输入模式

- `StateId`：`IDLE=0, CHASE=1, ATTACK=2, ATTACK_TURN=3, RECOVERY=4`
- `script_states.cpp` 的分支普遍要求 `in_band_* && is_front && fresh_target` 三者同时成立。
- `cpp_script.cpp`：`need_move = !in_band_any`；进带强制松键；最短按住时长保护。

### 4.3 目标筛选（`script_perception.cpp`）

候选过滤：`cls == 1`、同平台 `|d_fy| ≤ 0.028`，再按 `layer_of` 分层。

## 5. 遥测与 UI 模式（`docs/ui-design.md`）

### 原则 U1–U6

| # | 原则 |
|---|---|
| U1 | **core 零感知 UI**：UI 挂了 core 照跑 |
| U2 | **接口冻结**：遥测契约只加不改 |
| U3 | **不碰 `core_contract.h`**：遥测是宿主内部能力，不是插件契约 |
| U4 | **依赖单向**：`telemetry` 只被 `main.cpp` 调用 |
| U5 | **前端无框架**：原生 JS + Canvas |
| U6 | **字段对齐 recorder**：实时流与 JSONL 回放共用一套解析 |

### 回放三态机（`live / buffering / playing`）

```
收到 meta=replay_begin → setMode("buffering")：帧进本地 buffer，不渲染，控条隐藏
收到 meta=replay_end   → setMode("playing") + playStart()：控条显示，**自动播放**
onclose                → mode==="playing" 则**保留 buffer**并继续本地播放
                          否则清空 buffer、回 live
```

- 控条只在 `playing` 态显示（`setMode` 中 `m === "playing"` 才 `remove("hidden")`）。
- 三态机实测结论：**回放期间页面显示 `--` 是正常的**，必须等 `replay_end`。
- `SetReplayMode(true)` 广播一次 `replay_begin`；回放期间新连上的客户端由服务器**补发**
  （UI-3c-1a）。

### 控制消息契约 v0

`{"v":0,"meta":"replay_begin"}` / `replay_end`（`docs/ui-design.md` §十四）。

## 6. 离线回放验证模式（`experiments/script_replay`）

```
原始事件 fixture (*.jsonl，keys = h,id,type,w,cx,t,frame,cls,conf,cy)
        │  script_replay.exe --input F [--output T]  (MH_SCRIPT_SEED=42)
        ▼
trace 输出（keys = active_key,desired_e,dets,facing,frame,me_fx,me_fy,
                  me_locked,out,state,t,target_cx,target_locked）
        │  对比 SHA256
        ▼
     回归判据
```

- **fixture（输入）与 trace（输出）字段不同**：fixture 无 `me_locked/state/target_*`。
- 因此 `core --replay <fixture>` 恒 `me_locked=0` → 前端画不出攻击区；
  要在前端看，需 `core --replay <trace>`。
- 确定性来源：固定 seed + 固定输入 → 逐字节相同的 trace。
- 「变了 / 没变**都是有效结论**，必须如实报告，不能期望它变」。

## 7. 测试与验收模式

- **一键门禁**：`scripts/run_all.ps1`（11 步，见 `projectbrief.md` §6）。
- 场景由 `prepare_*.ps1` **构造目录**（复制坏 DLL / 运行期错误插件到目标位置），
  再由 `run_acceptance.ps1` 逐场景跑并按关键词断言。
- 独立探针（`experiments/` 下）用于**证明某条逻辑确实生效**，
  作为 fixture 无差异时的辅助证据。
- 人眼确认：起 `core.exe` + 浏览器 `http://127.0.0.1:6601`，
  可用 canvas 像素级扫描量化验证（比截图更硬的证据）。
