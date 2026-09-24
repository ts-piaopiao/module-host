# 阶段 10：Combat 状态机重构（含跳跃与卡住检测）

## 一、背景

阶段 7 的 Combat 是 if-else 堆叠，经过 v3/v4 两轮补丁后已能追怪 + 攻击 + 目标沿用，
但存在 3 个未覆盖的场景：

1. 跨平台追击（怪在上层，角色在下层）
2. 角色卡住（相机 + 角色位置不变）
3. 拟人化节奏（当前按键是固定逻辑，不像人）

本阶段重写 Combat 为状态机，覆盖以上场景。

## 二、参考数据

### 2.1 跳跃物理参数（从 records_jump 实测）

| 参数 | 值 | 备注 |
|---|---|---|
| 跳跃高度 | 0.058 归一化 | 4 次原地跳平均 |
| 滞空时间 | 600 ms | 500-667 ms |
| 上升/下降比例 | 36% / 64% | — |
| 方向跳水平位移 | 0.013 归一化 | 单次样本 |
| 跳跃期间 tracker id | 稳定 | 5/5 无切换 |

### 2.2 拟人化参数（从 records_baseline 提取，作为初始参考值）

| 参数 | 参考值 | 数据来源 |
|---|---|---|
| E 点按时长 | 133 ms | 251 次 E 的中位按住时长 |
| E 连发间隔 | 250 ms | 战斗段 down 间隔主峰 |
| 方向短点按时长 | 300 ms | — |
| 方向长按移动时长 | 900 ms | 方向键长按段中位 |
| 静止后反应延迟 | 1700 ms | 106 段静止后的首次按键中位 |

**重要**：这些参数只是参考值，后续会用更多操作数据精调。
所有参数必须从 config 读，不硬编码在代码里。

## 三、状态机设计

### 3.1 顶层状态

| 状态 | 职责 |
|---|---|
| IDLE | 无目标，松所有键 |
| CHASE | 有目标，同平台追击（按住方向键） |
| JUMP | 跨平台 + 水平接近 + 卡住，跳上去 |
| ATTACK | 按 E |
| RECOVERY | 850 ms 僵直，不可移动/攻击 |

### 3.2 状态迁移

```
                    有目标（沿用或新选）
        ┌──────────────────────────────────────┐
        │                                      ▼
     ┌──────┐   目标丢失超时   ┌──────┐
     │ IDLE │◄────────────────│ 全局  │
     └──┬───┘                 └──────┘
        │ 有目标
        ▼
     ┌──────┐  进入攻击带   ┌─────────┐  E 完成一击   ┌───────────┐
     │CHASE │─────────────►│ ATTACK  │─────────────►│ RECOVERY  │
     └──┬───┘              └────┬────┘              └─────┬─────┘
        │                       │                         │
        │ 跨平台+接近+卡住       │ 脱离攻击带               │ 850ms 到
        ▼                       ▼                         ▼
     ┌──────┐              回 CHASE                    回 CHASE
     │ JUMP │──────────────────┘（落地后重评攻击带）
     └──────┘
        │
        └── 滞空结束/落地 ──► 回 CHASE 或 ATTACK
```

迁移条件（按优先级，每帧只迁一次）：

| # | 从 | 到 | 条件 |
|---:|---|---|---|
| 1 | 任意 | IDLE | me 丢失，或目标丢失超过 `target_lose_ms`（沿用 v4 的 500 ms 容忍后仍无目标） |
| 2 | IDLE | CHASE | 有确认目标（`seen >= confirm_frames`，沿用 v4） |
| 3 | RECOVERY | CHASE | `now - recovery_start >= recovery_ms`（850 ms） |
| 4 | ATTACK | RECOVERY | 本次 E 点按完成（按住 `e_tap_ms` 后释放） |
| 5 | ATTACK | CHASE | 目标脱出攻击带（带宽变化先松 E，不进 RECOVERY） |
| 6 | CHASE | ATTACK | 满足攻击带判定（沿用 v4 的 `atk_min/max` + 跨平台 `atk_cross` + 垂直带） |
| 7 | CHASE | JUMP | **跨平台**（`|dy| > same_plat_y`）**且**水平已接近（`|dx| <= jump_dx_max`）**且**触发跳（见 3.4） |
| 8 | JUMP | CHASE | 滞空时间超过 `air_time_ms`（约 600 ms），或 cy 回到起跳前高度（落地） |
| 9 | JUMP | ATTACK | 落地后立刻满足攻击带 |
| 10 | CHASE | CHASE（重置计时） | 相机在动（有位移），卡住检测清零 |

不在表中的组合一律保持当前状态（含 v4 的目标丢失 500 ms 容忍期：容忍期内不迁状态、不改按键）。

#### 迁移优先级（同帧多条件满足时，从高到低）

1. 任意 → IDLE：me 丢失，或目标丢失超时
2. RECOVERY → CHASE：僵直结束
3. ATTACK → CHASE：目标脱离攻击带 **且** E 未按住（e_tap_ms 未到）
4. ATTACK → RECOVERY：e_tap_ms 到（E 按住完成）
5. CHASE → ATTACK：进攻击带
6. CHASE → JUMP：跨平台 + 水平接近 + stuck
7. JUMP → CHASE / ATTACK：滞空结束或 cy 恢复
8. IDLE → CHASE：有确认目标

规则 3 与 4 的判定：

- 若 ATTACK 中按住 E 尚未到 e_tap_ms（即刚按下不到 133ms）→ 无论是否脱离攻击带，都先完成 e_tap_ms 再判
- 若 ATTACK 中按住 E 已到 e_tap_ms（E 该松了）→ 按规则 3/4 的顺序：先看是否脱离攻击带

### 3.3 卡住检测

独立于状态的小模块，只产出布尔量 `stuck`，供 3.4 的 JUMP 触发使用。

判定输入：

- `me_fx, me_fy`：me_lock 脚底中心（已有）
- 相机运动估计：用同帧内**非 me、非 chosen 目标**的检测框做简易光流代理（可选二期）
  - 一期简化：若 me 与 chosen 的归一化位置在窗口内几乎不动，且我们在按方向键 → 视为卡住

规则（一期）：

```
stuck = 连续 stuck_ms（默认 800ms）内
        所有帧 active_dir_key == 同一个非零值（同一个方向）
        且 |me_fx - me_fx_起始| < 0.002
        且 |me_fy - me_fy_起始| < 0.002
```

说明：用 `active_dir_key`（实际按下的键）而不是 `desired_dir`，避免“切换方向”被误判为“持续按同方向”。

- 窗口默认 `stuck_ms = 800`（config 可调）
- 触发后 `stuck` 保持到：me 产生足够位移，或离开 CHASE
- 卡住不直接改状态，只允许参与 3.4 的 JUMP 进入条件；跨平台卡住才跳（避免平地对墙乱跳时，可加 `allow_ground_jump` 开关，默认关）

### 3.4 JUMP 进入与键序列

进入条件（7）三者同时满足：

1. 跨平台：`|chosen.cy - me_fy| > same_plat_y` 且怪在上方（`chosen.cy < me_fy`，屏幕 y 向下）
2. 水平接近：`|chosen.cx - me_fx| <= jump_dx_max`（默认 0.06，保证跳的方向够得着）
3. 触发跳：`stuck == true` **或** `cross_chase_ms >= cross_chase_ms_max`（在平台下追了超过 1.2 s 仍上不去）

键序列（拟人化，带 config 时长）：

```
进入 JUMP:
  1) 若需要朝怪的水平方向：按下对应方向键（与 CHASE 相同）
  2) 按下 SPACE（VK 0x20），保持 jump_hold_ms（默认 80 ms）
  3) 释放 SPACE
  4) 进入滞空等待（air_time_ms 默认 600 ms，或检测 cy 回升前值）
滞空期间：
  - 不按 E
  - 方向键可保持（方向跳，实测水平位移 0.013）
落地（迁出 JUMP）：
  - 松开跳相关键（若仍需追击则方向键状态由 CHASE 接管）
```

跳跃高度/滞空以 2.1 为初始值，仅用于估时，不参与像素级预测；落地以 **cy 恢复** 为主、超时为辅。

### 3.5 ATTACK / RECOVERY 拟人化

一击的键节奏（替代当前“在带内每帧 desired_e=true 长按”）：

```
进入 ATTACK（首次进入或 RECOVERY 结束后重回）:
  e_tap: 按下 E → 保持 e_tap_ms（133） → 释放 E
  → 迁 RECOVERY，recovery_start = now

RECOVERY:
  不按任何攻击键；是否允许微调方向由 config recovery_allow_move（默认 0）控制
  超时 recovery_ms（850） → 回 CHASE，重新评攻击带

连发：
  两次 ATTACK 进入之间至少间隔 e_interval_ms（250）
  若目标仍在带内但间隔未到 → 停在 CHASE（松 E，方向键按需微调），模拟人的节奏
```

反应延迟：从 IDLE→CHASE 或从长时间无操作恢复时，可插入 `react_delay_ms`（1700）再开始按方向键；一期可默认 0，二期打开。

### 3.6 与 v4 兼容的保留项

重构时**保留**以下已验证逻辑，迁入状态机实现：

- 目标沿用宽窗 `kTargetMatchX/Y`、丢失容忍 500 ms（3a/3a.5/3a.6 语义并入“全局/目标子状态”，不单独做第五个顶层状态）
- 怪物 `confirm_frames = 2`、`kLoseMs = 200` 的跟踪老化
- 攻击带滞回 `atk_hyst`（防 desired_e 抖动）
- `ReleaseAll`：me 丢失、开始验证时松全部键
- 按键差分输出：只在 `active_dir_key` / `active_e` 变化时 push（`out_count <= 8`）

## 四、Config 参数表

### config 结构

新增结构体：

```cpp
struct CombatConfig {
    int recovery_ms = 850;
    int e_tap_ms = 133;
    int e_interval_ms = 250;
    int dir_tap_ms = 300;
    int dir_hold_ms = 900;
    int react_delay_ms = 0;
    int stuck_ms = 800;
    float stuck_move_eps = 0.002f;
    int cross_chase_ms_max = 1200;
    float jump_dx_max = 0.06f;
    int jump_hold_ms = 80;
    int air_time_ms = 600;
    float same_plat_y = 0.028f;
    float atk_min = 0.010f;
    float atk_max = 0.135f;
    float atk_cross = 0.104f;
    float atk_hyst = 0.021f;
    float atk_v_up = 0.074f;
    float atk_v_down = 0.019f;
    float target_match_x = 0.025f;
    float target_match_y = 0.037f;
    int target_lose_ms = 500;
    int confirm_frames = 2;
    int allow_ground_jump = 0;
    int recovery_allow_move = 0;
    float dir_switch_lead = 0.0625f;
    int dir_cooldown_near_ms = 800;
    int dir_cooldown_far_ms = 300;
    int turn_bounce_ms = 200;
    int turn_pause_min_ms = 200;
    int turn_pause_max_ms = 500;
    float double_tap_prob_high = 0.02f;
    float double_tap_prob_mid = 0.10f;
    float double_tap_prob_low = 0.18f;
    int dir_release_min_ms = 60;
    int dir_release_max_ms = 320;
    int dir_release_jitter_ms = 100;
    int reverse_near_ms_min = 200;
    int reverse_near_ms_max = 500;
    int atk_miss_max = 3;
    int me_predict_ms = 2000;
    int empty_freeze_ms = 500;
};
```

在 CoreConfig 里加：

```cpp
CombatConfig combat;
```

`config.cpp` 的 `LoadConfigFile` 加所有 `combat_*` 键的解析（前缀 `combat_`）。

Decider 的构造函数接受 `const CombatConfig&`，传给 Combat。

### 键一览

| 键 | 默认 | 单位 | 说明 |
|---|---:|---|---|
| combat_recovery_ms | 850 | ms | 攻击后僵直 |
| combat_e_tap_ms | 133 | ms | E 点按保持 |
| combat_e_interval_ms | 250 | ms | E 连发最小间隔 |
| combat_dir_tap_ms | 300 | ms | 方向短点按（掉向/微调） |
| combat_dir_hold_ms | 900 | ms | 方向长按分段上限 |
| combat_react_delay_ms | 0 | ms | IDLE→CHASE 反应延迟（0=关） |
| combat_stuck_ms | 800 | ms | 按键中位移过小视为卡住 |
| combat_stuck_move_eps | 0.002 | — | me 位移阈值 |
| combat_cross_chase_ms_max | 1200 | ms | 平台下追怪超时触发跳 |
| combat_jump_dx_max | 0.06 | — | 跳前水平接近阈值 |
| combat_jump_hold_ms | 80 | ms | SPACE 按住时长 |
| combat_air_time_ms | 600 | ms | 滞空估时（超时强制落地） |
| combat_same_plat_y | 0.028 | — | 同平台 y 阈（沿用 v4） |
| combat_atk_min / atk_max / atk_cross | 0.010 / 0.135 / 0.104 | — | 攻击带（沿用 v4） |
| combat_atk_hyst | 0.021 | — | 攻击带滞回（沿用 v4） |
| combat_atk_v_up / atk_v_down | 0.074 / 0.019 | — | 垂直带（沿用 v4） |
| combat_target_match_x / y | 0.025 / 0.037 | — | 目标沿用宽窗（沿用 v4） |
| combat_target_lose_ms | 500 | ms | 目标丢失容忍 |
| combat_confirm_frames | 2 | 帧 | 怪物确认帧数 |
| combat_allow_ground_jump | 0 | 0/1 | 平地卡住是否允许跳 |
| combat_recovery_allow_move | 0 | 0/1 | 僵直期是否允许方向键 |
| combat_dir_switch_lead | 0.0625 | — | 换向领先门槛（120px/1920） |
| combat_dir_cooldown_near_ms | 800 | ms | 带内换向最小间隔 |
| combat_dir_cooldown_far_ms | 300 | ms | 远追换向最小间隔 |
| combat_turn_bounce_ms | 200 | ms | 转身防抖（刚转完不反向） |
| combat_turn_pause_min_ms | 200 | ms | 换向随机停顿下限 |
| combat_turn_pause_max_ms | 500 | ms | 换向随机停顿上限 |
| combat_double_tap_prob_high | 0.02 | — | 双击概率（高聚簇档） |
| combat_double_tap_prob_mid | 0.10 | — | 双击概率（中聚簇档） |
| combat_double_tap_prob_low | 0.18 | — | 双击概率（低聚簇档） |
| combat_dir_release_min_ms | 60 | ms | 出刀后延迟松方向下限 |
| combat_dir_release_max_ms | 320 | ms | 出刀后延迟松方向上限 |
| combat_dir_release_jitter_ms | 100 | ms | 松方向前随机抖动上限 |
| combat_reverse_near_ms_min | 200 | ms | 贴脸后退时长下限 |
| combat_reverse_near_ms_max | 500 | ms | 贴脸后退时长上限 |
| combat_atk_miss_max | 3 | 次 | 连续空刀阈值 |
| combat_me_predict_ms | 2000 | ms | me 缺失外推时长 |
| combat_empty_freeze_ms | 500 | ms | 整帧空检测冻结时长 |

加载方式：沿用 `LoadConfigFile`；所有键缺失时用上表默认值。不改 ABI，不动插件契约。

## 五、分步实现

- **10a** 骨架：状态机框架，行为对齐 v4
- **10a.5** 方向稳定性：锁侧 + 换向冷却 + 转身防抖 + 防摆头
- **10b** JUMP：跨平台判定 + 卡住检测 + SPACE 键序列
- **10c** 拟人化：E 点按 + 连发间隔 + 双击聚簇 + 延迟松方向
- **10d** 异常兜底：贴脸后退 + 空刀检测 + me 外推 + 空检测冻结
- **10e** 联调：三场景验证 + 数据对照

每步独立构建、独立可跑；10a 不引入新 config 键以外的行为变化。

## 五点五、特殊场景处理

参考 Python combat_controller 的经验，加入以下机制（按实现优先级排序）。

### 5.5.1 方向稳定性（10a.5）

| 机制 | 参数 | 说明 |
|---|---|---|
| 锁侧 | — | 当前朝向侧有确认怪时，不轻易换向；仅对侧怪进带（≤atk_max）或近出 switch_lead 才放行 |
| 换向领先门槛 | combat_dir_switch_lead = 0.0625（120px/1920） | 两侧都有怪时，对侧须满足 atk_max 内或领先当前侧 120px |
| 近身转向冷却 | combat_dir_cooldown_near_ms = 800 | 带内换向最小间隔 |
| 追击转向冷却 | combat_dir_cooldown_far_ms = 300 | 远追换向最小间隔 |
| 转身防抖 | combat_turn_bounce_ms = 200 | 刚转完不再反向 |
| 转身停顿 | combat_turn_pause_min_ms = 200 / max = 500 | 换向时随机停顿 |

### 5.5.2 攻击节奏拟人化（10c）

| 机制 | 参数 | 说明 |
|---|---|---|
| E 点按 | combat_e_tap_ms = 133 | 已列 |
| E 连发间隔 | combat_e_interval_ms = 250 | 已列 |
| 双击概率聚簇 | combat_double_tap_prob_high/mid/low = 0.02 / 0.10 / 0.18 | 按近期双击数动态调整 |
| 出刀后延迟松方向 | combat_dir_release_min_ms = 60 / max = 320 | 出刀时方向键延迟释放 |
| 松方向前随机抖动 | combat_dir_release_jitter_ms = 100 | 0-100ms 抖动 |

### 5.5.3 异常兜底（10d）

| 机制 | 参数 | 说明 |
|---|---|---|
| 贴脸后退 | combat_reverse_near_ms_min = 200 / max = 500 | d < atk_min 时反向走一段 |
| 连续空刀 | combat_atk_miss_max = 3 | 连续空刀 3 次 → 清朝向回 IDLE |
| me 缺失外推 | combat_me_predict_ms = 2000 | 遮挡时按速度外推 |
| 整帧空检测冻结 | combat_empty_freeze_ms = 500 | 无检测时不老化怪 |

## 六、不在本阶段范围

- 完整寻路 / 绕墙（卡住仅触发跳，不换路线）
- 多怪优先级与仇恨
- 攻击动画帧精确对齐（只用固定 RECOVERY 时长）
- 双击、闪避等 v4 未涉及的键
- 用更多 session 精调 2.2 参数（预留后续阶段）

## 七、验收

- 10a：与 v4 行为一致（desired_e 比例、切键次数不劣化），临时日志关闭后仍可 dry-run
- 10a.5：两侧有怪时不频繁摆头；换向满足 lead/冷却/防抖约束
- 10b：怪在上层时角色走到平台下 → 起跳 → 落地后继续追/攻击；跳跃期间 tracker id 不丢
- 10c：E 按住时长、连发间隔与 config 一致；录一段 hum 对照，分布形状接近参考值
- 10d：贴脸会后退；连续空刀后回 IDLE；me 短暂丢失有外推
- 10e：三场景（跨平台、卡住、拟人）在游戏内可见且无贴墙死循环（贴墙仅允许表现为“卡住→尝试跳”，不允许永久按住方向）

## 八、一句话原则

先状态清晰，再拟人；参数全进 config，时长只做参考、可被数据推翻。
