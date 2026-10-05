# activeContext — 当前工作焦点

> **更新最频繁**的文件。任务开始读它，任务结束写它。

- **最后更新**：2026-09-30
- **HEAD**：与 `origin/master` 一致（ahead=0 / behind=0）；具体 SHA 见 `git log -1`
- **工作区**：干净（已跟踪文件）；`AGENTS.md` 为唯一未跟踪项
- **origin/master**：与本地同步

---

## 1. 当前焦点

**攻击带几何 v3（尖点 + 45° 斜边 + 远端竖直线）已全部完成并入库**，当前处于两包之间的空档：

| 包 | 状态 |
|---|---|
| UI-3c-2 回放控制条（Commit 1，`e630436`） | ✅ 完成 |
| 攻击带几何 v2（Commit 2，`2b26779`） | ✅ 完成，6 fixture SHA 不变已解释 |
| 攻击带几何 v3（尖点 + 45° 斜边 + 远端竖直线，`05f65bc`） | ✅ 完成，1/6 fixture SHA 变化已解释 |
| 窄框 fixture（v3 针对性回归，非唯一证据） | ⬜ 降级，可后置 |
| `docs/env-traps.md` 追加 E7/E8 | ⬜ 单开一包，未开工 |
| 方向 A：UI 文档对齐（D1–D5 + S3/S4 + E9） | ✅ 完成，`aebdb17` / `c3cf698` |

---

## 2. 近期变更

### Commit `c3cf698` / `aebdb17` — 方向 A 文档对齐

- `ui-design.md`：头部「未实现」→「已实现 UI-1c~UI-3c-2 + 几何 v3」；§二 N1 画面叠加标已实现；
  §九加「实现变更记录」（UI-3 实际为 replay 控制，FileSource 顺延）；§十 localhost → 127.0.0.1；
  §十二待办 2–5 标完成；§十三状态改已实现；§13.7 补 v3 常量名。
- `env-traps.md`：新增 **E9**（`localhost:6601` 502，须用 `127.0.0.1`）。
- `techContext.md` §4：索引 E1–E8 → E1–E9。
- `progress.md`：env-traps 引用 E1–E6 → E1–E9；「最近三笔」语义化。

### Commit `05f65bc` — 攻击带几何 v3（尖点 + 45° 斜边 + 远端竖直线）

5 文件 +84 −128：`script_config.h` / `script_geometry.h` / `script_geometry.cpp` /
`telemetry_server.cpp` / `docs/ui-design.md`

- **删除** `kBandXMin = 0.010f` 与 `kBandXBulge = 0.005f`（前置 grep 确认 `src/` 内无第三处引用）。
- **新增** `kBandXApex = 0.005f`（尖点 x，贴角色正前方）、
  `kBandApexSlope = 1.0f`（顶点半角正切，1.0 → 顶点总角 90°）。
- `kBandXMaxSame = 0.1458f` 保留，语义改为**远端竖直边 x**（= 280px @1920）。
- 判定：`x_inner(y) = kBandXApex + |y| / kBandApexSlope`；`x_outer = kBandXMaxSame`（常数）。
  早期排除阈值由 `kBandXMin - kBandXBulge` 改为 `kBandXApex`（2 处）。
- 形状由「近端弧 + 远端椭圆弧扇段」改为**五边形**；前端 `drawBand` 改为 5 顶点直连
  （尖点 → 上肩点 → 远端上 → 远端下 → 下肩点 → closePath），常量块 `BAND_X_APEX` /
  `BAND_X_FAR` / `BAND_APEX_SLOPE` 取代 `BAND_X_MIN` / `BAND_X_BULGE` / `BAND_X_MAX` /
  `BAND_Y_HALF_UP` / `BAND_Y_HALF_DN`。
- 文档 `docs/ui-design.md` §13.5 整节重写（标题去「椭圆」→「攻击带绘制规范」），
  并修复包外同类 doc bug：`script_geometry.h/.cpp` 的「椭圆扇段」注释、`ui-design.md` L328。
- **v3 是双向改动**：近端在 `|y|>0` 处**收紧**（`x_inner` v2 恒 ~0.010 → v3 = 0.005+|y|，
  `|y|=0.074` 处达 0.079），远端**放宽**（椭圆弧 → 恒 0.1458），只在 `y=0` 与 v2 重合，整带右移。
- **实测**：v2 基线用 HEAD worktree 重建核实（与 `progress.md` 旧截断值逐字吻合）；
  6 fixture 中**仅 `real_session_long` SHA 变化**（`9a3b6e19…` → `551f79b6…`，
  3586/18404 行不同，锁定目标切换，**497 翻转帧**），其余 5 个 **0 翻转帧、字节相同**。
  翻转帧数为实测（逐帧分别跑 v2/v3 判定），非推断。
- 验收：构建 exit=0、`run_all.ps1` **11/11**、I1–I11 **66/66 PASS**。
- 附带效果：**K1 空带缺陷消除**（`x_outer` 恒 0.1458 > `x_inner` 最大 0.079）；
  原待办「L440–441 跳过 vs 夹取」措辞分歧随夹取语句整段删除而消失。

### Commit `2b26779` — 攻击带几何 v2（近端凸弧）

4 文件 +71 −13：`script_config.h` / `script_geometry.cpp` / `telemetry_server.cpp` / `docs/ui-design.md`

- 新常量 `ScriptConfig::kBandXBulge = 0.005f`。
- **近端**：`x_inner(y) = kBandXMin - kBandXBulge × √(1 - (y/y_half)²)`
  → 弧顶 0.005、两端 0.010（凸向角色）。
- **远端保持 v1 的椭圆弧** `x_max(y) = kBandXMaxSame × √(1 - (y/y_half)²)`，
  **不是**竖直直线（用户已明确拍板）。
- 早期排除阈值由 `kBandXMin` 放宽到 `kBandXMin - kBandXBulge`（2 处），
  否则新内弧会变成死代码。
- 精确判定改用 `x_inner_at_y`（2 处）。
- 前端 `telemetry_server.cpp` 新增 `BAND_X_BULGE`，`drawBand` 近端改弧、远端改直线收尾。
- 文档 `docs/ui-design.md` §13.5：表格行、几何形状说明、JS 常量块、绘制步骤、
  组合多边形描述**全部改为 v2**（含 4 处包外同类 doc bug）。

### Commit `e630436` — UI-3c-2 回放控制条

`telemetry_server.cpp` +235 −14，本地缓冲三态机 `live / buffering / playing`。

---

## 3. 下一包

**方向 A 已收口**。可选后续方向：

| 方向 | 内容 | 优先级 |
|---|---|---|
| B | UI 自动化验收脚本（`run_ui_acceptance.ps1`，纳入 run_all） | 中 |
| C | UI-3c-2 实时模式验证（`ui3c2_live.cfg`）+ `127.0.0.1` 提示 | 低 |
| D | UI-4 控制通道 `/ctl` 6602（契约已在 §六/§十四） | 中 |
| E | FileSource 接本地 JSONL | 中 |
| F | 改进 `core --replay` 直吃 fixture（现须先跑 `script_replay`） | 待评估 |

---

## 4. 待定 / 未决事项

| # | 事项 | 说明 |
|---|---|---|
| 3 | `docs/contract-change-procedure.md` 里的 13/13、6/6 | 三插件时代数字，两插件架构下已失意义（framework §十三 已注明），文档本身未改 |
| 4 | UI-3c-2 的 5.6 项与实时 `ui3c2_live.cfg` 验证 | 属 UI-3c-2 收尾，与几何包无交集 |
| 5 | ~~`origin/master` 未 push~~ | **已关闭**：v3 收尾已 push，`HEAD = origin/master = 3a35fff` 之后继续跟进 |

---

## 5. 会话工作约定（来自 `AGENTS.md`）

1. **任务开始前**：先读 `memory-bank/` 全部 6 个文件。
2. **写代码前**：先输出 Implementation Plan（改哪些文件、为什么、风险、测试命令），
   **等用户确认再动手**。
3. **任务完成后**：更新 `activeContext.md` + `progress.md`；
   有架构变化则同步 `systemPatterns.md`。
4. 如实报告：几何公式或 old 串定位有疑问就**停下来贴回实际代码，不猜**。
