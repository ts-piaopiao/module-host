# progress — 已完成、待办与已知问题

> **最后更新**：2026-09-30 ｜ HEAD = origin/master（ahead=0 / behind=0）｜ 工作区干净（仅 `?? AGENTS.md`）｜ 具体 SHA 见 `git log -1`

## 1. 已完成

### 1.1 框架阶段（阶段 0/1/2）— 全部完成

- 阶段 0 契约定义：`core_contract.h` + C11/C++17 双编译断言测试。
- 阶段 1 加载器与加载类错误：**11/11 场景**（`stage1_build_gate` + 10 个 bad_plugins）。
- 阶段 2 正常闭环与运行期错误：**5/5 场景**（`stage2_build_gate` + 4 个 runtime_errors）。

### 1.2 后续阶段 — 全部完成

- **阶段 3** 契约扩展与真实插件集成：ABI 2 → 3 → 4；`real_capture`（Media Foundation）、
  `yolo_policy`（ONNX Runtime + DirectML）、`test_policy`；串口输出内化为 `OutputManager`
  （115200 8N1，`mk.*`）；`remote_server` JPEG 推流 + 上行键鼠；F12 控制模式。
- **阶段 4** 远程操作与仲裁：结论是**架构演进后天然不需要仲裁**
  （`SendScript` 帧同步 vs `SendAsync` 事件驱动，两路径从源头独立）。
- **阶段 5** YOLO 推理集成。
- **阶段 6** 宿主内决策层与 `me_lock`：`IScript` / `ScriptHost` / `CppScript` 三层。
- **阶段 7** 战斗状态机：5 状态 `IDLE/CHASE/ATTACK/ATTACK_TURN/RECOVERY`。

### 1.3 遥测与可视化 UI — 已完成到 UI-3c-2

| 分期 | 内容 | commit |
|---|---|---|
| UI-1c-1 | HTTP + WS 服务器，占位页 | `930ebb2` |
| UI-1c-2 | 状态面板替换占位 | `d1051b9` |
| UI-2b | FrameBundle 填 `dets` | `622f1d6` |
| UI-2c | 画布场景视图（抽象画布） | `447bdf7` |
| UI-3a | `script_replay` trace 输出完整 `dets` | `ae6e7f9` |
| UI-3b | `core --replay` 把 trace 重发到遥测 | `bf9213e` |
| UI-3c-1a | 回放期间**新连客户端补发** `replay_begin` | `f85acf6` / `bc3e8a0` |
| UI-3c-2 | **回放控制条 + 本地缓冲三态机**（断线不清空缓冲） | `e630436` |

### 1.4 攻击带几何 v3 — 完成（`05f65bc`）

- 形状由「近端弧 + 远端椭圆弧扇段」改为**五边形**：尖点 + 45° 斜边 + 上下水平边 + 远端竖直线。
- **删除** `kBandXMin` / `kBandXBulge`（grep 确认 `src/` 内无第三处引用）；
  **新增** `kBandXApex = 0.005`、`kBandApexSlope = 1.0`（顶点总角 90°）；
  `kBandXMaxSame = 0.1458` 保留，语义改为远端竖直边 x。
- 判定：`x_inner(y) = kBandXApex + |y| / kBandApexSlope`；`x_outer = kBandXMaxSame`（常数）。
- 前端 `drawBand` 改 5 顶点直连，常量块换为 `BAND_X_APEX` / `BAND_X_FAR` / `BAND_APEX_SLOPE`；
  `docs/ui-design.md` §13.5 整节重写；修 `script_geometry.h/.cpp`「椭圆扇段」注释与 L328。
- **v3 是双向改动**：近端 `|y|>0` 处**收紧**（v2 恒 ~0.010 → v3 = `0.005+|y|`，最大 0.079），
  远端**放宽**（椭圆弧 → 恒 0.1458），只在 `y=0` 与 v2 重合，整带右移。
- **实测**：v2 基线用 HEAD worktree 重建 `script_replay.exe` 实算核实（与本文件旧截断值逐字吻合）；
  6 fixture **仅 `real_session_long` SHA 变化**（`9a3b6e19…b467188` → `551f79b6…5fcfa15`，
  3586/18404 行不同、锁定目标切换、**497 翻转帧**），其余 5 个 **0 翻转帧、字节相同**。
  翻转帧数为逐帧实跑 v2/v3 判定所得，非推断。
- 验收：构建 exit=0、`run_all.ps1` **11/11**、I1–I11 **66/66 PASS**（6 fixture × 11 项，FAIL=0）。
- 附带效果：**K1 空带缺陷消除**；原待办「L440–441 跳过 vs 夹取」随夹取语句删除而消失。

### 1.5 验收脚本与工具

- `scripts/run_all.ps1` 11 步主门禁（不依赖硬件）。
- `run_contract_acceptance / run_acceptance / run_script_acceptance / run_output_probe /
  run_real_acceptance / run_capture_soak / run_jitter_test` 及 `prepare_*.ps1` 场景构造器。
- `experiments/`：`script_replay`、`output_probe`、`capture_probe`、`capture_preview`、
  `serial_probe`、`remote_client`、`human_log_analyzer`、`send_log_analyzer`。

### 1.6 文档

`docs/` 下 11 份：framework、contract-change-procedure、stage3–7、
output-manager、recorder、ui-design、env-traps（E1–E9）。

---

## 2. 待办

| # | 事项 | 优先级 | 归属 |
|---|---|---|---|
| 1 | **窄框 fixture**：构造 `narrow_box_scene.jsonl`，v2(`2b26779`)/v3(`05f65bc`) 双构建对比 trace，预期 SHA 不同 | **低**（已降级：`real_session_long` 497 翻转帧已提供端到端观测证据） | 可后置，单开 |
| 5 | `docs/contract-change-procedure.md` 中 13/13、6/6 为三插件时代数字，与现状不符 | 低 | 待改 |
| 6 | UI-3c-2 收尾项（5.6 验证、实时 `ui3c2_live.cfg` 观察） | 低 | UI 包 |

> 原待办 #4（`ui-design.md` L440–441「跳过 vs 夹取」措辞）已随 v3 删除：
> 绘制步骤改为 5 顶点直连，`if (xOuter < BAND_X_MIN)` 夹取语句整段移除，分歧不复存在。

### 窄框 fixture 构造条件（v2 时期判据，v3 下须重推——构造条件见本节下方，原理见 systemPatterns）

> 以下四条为 **v2 时期**判据，v3 后近端收紧 + 远端放宽，分歧缝位置已变，
> **不能直接沿用**；重推方法见 `activeContext.md` §3。

```text
cls=1、同平台 |d_fy| ≤ 0.028
窄框 w ≤ 0.015（约 ≤29px @1920）
正前方 x_far ∈ [0.005, 0.010) 且 x_far > w/2（保证 is_front=true）
反向带须 false：w < x_far + 0.010（否则 in_band_any 恒真，无法分歧）
脚本处于 CHASE 且已锁定该目标 → need_move 翻转 → active_key 变化可观测
```

---

## 3. 已知问题

| # | 问题 | 影响 | 处置 |
|---|---|---|---|
| K1 | ~~空带~~：`x_max_at_y < x_inner_at_y`，横向区间可能为空 | v1/v2 有，**v3 已消除**（`x_outer` 恒 0.1458 > `x_inner` 最大 0.079） | **关闭**，已记入 `docs/ui-design.md` §13.5 |
| K2 | ~~6 fixture 对几何变更无差异~~ → v3 后 `real_session_long` **已变化**（497 翻转帧，3586 行不同，SHA `9a3b6e19…` → `551f79b6…`）；其余 5 个 0 翻转帧属**轨迹覆盖不足**，非缺陷 | 端到端观测证据已具备 | 待办 #1 窄框 fixture 降为针对性回归 |
| K3 | `core --replay <fixture>` 恒 `me_locked=0`（fixture 输入无该字段） | 前端画不出攻击区 | 喂 `script_replay` 产出的 **trace** |
| K5 | `docs/contract-change-procedure.md` 的 13/13、6/6 已失效 | 误导 | 待办 #5 |

> 原 K4（`ui-design.md` L440–441「跳过」vs「夹取」）已随 v3 夹取语句删除而**消失**。

---

## 4. 回归基线

### 6 fixture SHA256（v3 基线，`MH_SCRIPT_SEED=42`，前 16 + 后 7 位）

| fixture | v3 SHA256 前缀 | 行数 | 相对 v2 |
|---|---|---|---|
| real_session_long.jsonl | `551f79b65db76612…5fcfa15` | 18404 | **CHANGED**（3586 行不同，497 翻转帧） |
| real_session_600f.jsonl | `205d384e26cee051…8545b2c9` | 600 | SAME（0 翻转帧） |
| turn_scene.jsonl | `e2af495fc383f733…43fcf9e45` | 300 | SAME（0 翻转帧） |
| long_idle_scene.jsonl | `49d730a01f0b655f…94f7b293` | 600 | SAME（0 翻转帧） |
| max_dets_scene.jsonl | `70e59d1b434bb9cc…a372e4c9` | 300 | SAME（0 翻转帧） |
| multi_target_scene.jsonl | `f758b9856a400579…1b5d0e42f` | 300 | SAME（0 翻转帧） |

**v2 基线**（用 HEAD `2b26779` worktree 重建 `script_replay.exe` 实算，与旧表逐字吻合）：

```text
real_session_long   9a3b6e19add12164d2705e8dcb41e022f389757a9bfb34549bff87d57b467188
real_session_600f   205d384e26cee0515751c4a2cdd3e486f579da7fbd6a564b9b625c378545b2c9
turn_scene          e2af495fc383f7333264aef97f302215974b147d1cb250bf2b3c89a43fcf9e45
long_idle_scene     49d730a01f0b655fa61569ab5670b9a20637bf22ec7837bcaeb4b89494f7b293
max_dets_scene      70e59d1b434bb9cc5c75fe3a21ce592c813b9388b5a22aa270c72b82a372e4c9
multi_target_scene  f758b9856a40057959340fbe2e80a4d9939b9000ed68fe742991a721b5d0e42f
```

判据：`run_all.ps1` **11/11 exit=0**；`script_replay` 6/6 exit=0，
I1–I11 各 11 项 PASS、FAIL=0（`MH_SCRIPT_SEED=42`）。
**v3 预期变化而实际只 1/6 变**：v3 是「近端收紧 + 远端放宽」双向改动，
5 个 fixture 实测 0 翻转帧（轨迹未落进 v2/v3 分歧缝），属覆盖不足而非缺陷。

---

## 5. 会话历史速览

- 2026-09-23 ～ 09-29：187 个提交，从阶段 0 一路到 UI-3c-2 + 几何 v2。
- 最近三笔：见 `git log -3 --oneline`。
- 2026-09-30：攻击带几何 v3 入库（`05f65bc`），1/6 fixture SHA 变化，已解释为 v3 双向性
  （近端收紧 + 远端放宽，5 个 fixture 实测 0 翻转帧）。
- 后续按 `AGENTS.md`：读 memory-bank → 出 Implementation Plan → 等确认 → 实施 → 回写。
- 2026-09-30：v3 收尾包（`e47591e` / `bd76709` / `b0d2a0c` / `5a36773`）— memory-bank 6/6 跟踪、R6/R7/R8 修正、push 到 origin/master、env-traps E7/E8
