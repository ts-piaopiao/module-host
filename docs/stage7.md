# 阶段 7：战斗状态机

## 一、背景

阶段 6 完成了宿主决策层：policy 只做推理，Decider 锁定"我"。
阶段 7 加战斗逻辑：看到怪物后，自动追怪、攻击、恢复。

参考 Python 项目 D:/dev/COMPortService/scripts/combat_controller.py，但**不照搬**。
Python 是完整的 1000+ 行状态机，阶段 7 分 4 步逐步实现。

## 二、分步实现计划

- 7a 最小决策：有怪就朝怪移动，在攻击带就按 E
- 7b 加状态机：idle / moving / attack / recovery
- 7c 参数从 config 读
- 7d 高级特性：me 速度外推、双击、随机延迟

每步独立可验证。

## 三、模块架构

新增 src/core/combat.h 和 src/core/combat.cpp：

  class Combat {
  public:
      Combat();
      ~Combat();
      Combat(const Combat&) = delete;
      Combat& operator=(const Combat&) = delete;

      // 每帧调用。me 是已锁定的我方位置（fx, fy 为脚底中心，归一化），
      // me_valid 表示 me_lock 是否有效。
      // dets 是全部检测结果。
      // out 输出决策。
      void Update(bool me_valid,
                  float me_fx, float me_fy,
                  const core_detections* dets,
                  core_decision* out);

  private:
      struct Impl;
      Impl* impl_;
  };

Decider 在 me_lock 有效时调用 Combat，否则输出空决策。

### Decider 与 Combat 的所有权

Decider 内部持有 Combat：

  struct Decider::Impl {
      Combat combat;   // 值成员
      // ... me_lock 状态
  };

Decider::Update 的流程：
1. 走 me_lock 逻辑（同 6d）
2. 如果 me_lock 有效 → 调用 combat.Update(true, lock_fx, lock_fy, dets, out)
3. 如果 me_lock 无效 → out->out_count = 0

main.cpp 不变，仍然只调 decider.Update。

### 7a 的简化说明

7a 的攻击带判定只看 x 轴（|dx|），忽略 y 轴。
理由：7a 的目标是"能跑通"，y 轴判定在 7b 加。

7a 不设攻击冷却，每帧按 E。
理由：有些游戏 E 键有冷却，有些没有。7a 不区分，跑起来看。

## 四、7a 的最小逻辑

状态：只有 idle 和 chase 两个。

每帧：
1. 从 dets 里收集 cls=1（monster）
2. 如果 me_valid == false → 输出空
3. 如果 monsters 为空 → 输出空（idle）
4. 选最近的怪（按脚底中心 x 方向距离）
5. 计算 dx = monster.cx - me_fx
6. 如果 |dx| > attack_band（比如 0.15）→ 按方向键朝怪移动：
   - dx > 0 → VK_RIGHT（0x27）
   - dx < 0 → VK_LEFT（0x25）
7. 如果 |dx| <= attack_band → 按 E 键攻击：
   - kind = CORE_ACTION_KEY
   - a = 0x45（VK_E）
   - b = 1（按下）
8. 每帧输出 1-2 个动作，out_count 最多 2

这一版的目的是：看到"角色朝怪移动并攻击"的可见行为。

## 五、7a 参数

硬编码在 combat.cpp 里（7c 才移到 config）：

- attack_band = 0.15（攻击带，归一化）
- 不设攻击时长限制（每帧按 E，连续攻击）

## 六、7b 的状态机（后续）

在 7a 跑通后设计，暂不实现。

## 七、7c 的参数（后续）

所有参数从 config 读，前缀 decider_：

- decider_attack_band
- decider_recovery_ms
- decider_monster_lose_ms
- ...（7c 时列出完整表）

## 八、不在本阶段范围

- 完整的 combat_controller 移植（部分在 7b/7c/7d）
- 主动移动验证（6d 已做）
- 多怪优先级（7b）
- 双击、随机延迟（7d）

## 九、验收

- 7a：游戏里角色看到怪会朝它移动，靠近后按 E
- 7b：能连续打怪，有停顿
- 7c：改 config 参数不重编译
- 7d：行为看起来像人

## 十、一句话原则

先动起来，再拟人化；先硬编码，再走配置。

## 十一、实现状态（截至当前）

### 已完成

7a–7c 的核心目标已实现，但形式与原设计不同：

- 战斗状态机：已实现，在 `src/core/script/script_fsm.cpp` + `script_states.cpp`
- 参数从 config 读：已实现，前缀为 `combat_*`（非 `decider_*`）
- E 时长分布、攻击反应延迟、RECOVERY→CHASE 延迟：已实现

### 未采用的设计

设计稿里的 `class Combat`（`src/core/combat.h` / `combat.cpp`）未创建。

`Decider` 类也未创建（见 `docs/stage6.md` 第九节）。

### 实际实现

决策逻辑在 `CppScript`（`src/core/script/cpp_script.cpp`）顶层组装，
状态机拆入 `script_fsm.cpp` + `script_states.cpp`。

状态机有 5 个状态：`IDLE / CHASE / ATTACK / ATTACK_TURN / RECOVERY`。

| 状态 | 职责 |
|---|---|
| IDLE | me 或 target 未锁定 |
| CHASE | 朝目标移动（含贴脸后退、面向翻转） |
| ATTACK | 正面攻击带内，按 E |
| ATTACK_TURN | 背面进带，先转身再按 E |
| RECOVERY | 攻击后冷却 |

### 配置项（实际使用）

前缀 `combat_*`：

- `combat_e_common_min_ms` / `combat_e_common_max_ms` / `combat_e_common_prob`
- `combat_e_rare_lo_min_ms` / `combat_e_rare_lo_max_ms`
- `combat_e_rare_hi_min_ms` / `combat_e_rare_hi_max_ms`
- `combat_attack_react_min_ms` / `combat_attack_react_max_ms`
- `combat_recovery_chase_min_ms` / `combat_recovery_chase_max_ms`

原设计稿的 `decider_*` 前缀（`decider_attack_band` / `decider_recovery_ms` /
`decider_monster_lose_ms` 等）未实现；几何与时序参数集中在 `script_config.h`
的 `ScriptConfig` 常量里。

### 与设计文档的偏差

1. 状态机 5 个状态，设计稿只列了 4 个（idle / moving / attack / recovery）。
   实际把"moving"拆成 `CHASE` 和 `ATTACK_TURN`，两者行为不同。
2. 多目标长按逻辑曾实现过（`e_long_hold` + `combat_e_hold_max_ms`），已删除。
   理由：过度设计，多目标与单目标行为一致（都是单击 E）。
3. 7d 高级特性只实现了一部分：
   - 随机延迟（E 时长、攻击反应、RECOVERY→CHASE）：已实现
   - me 速度外推、双击：未实现
4. 7b / 7c / 7d 的分步边界在实际开发中被打破——先实现了完整状态机，
   再逐步精修；不严格按 7a→7b→7c→7d 推进。

### 验收方式

通过 `experiments/script_replay` 回放工具 + 9 条 invariant 验证：

- I1：me 未锁定 → 不动键 / 不发 E
- I2：active_key 只能是 0x00 / 0x25 / 0x27
- I3：desired_e=true → state 只能是 ATTACK / ATTACK_TURN
- I4：desired_e 连续段时长 >= 100ms（无上限）
- I5：两次 E release 间隔 >= 800ms（被 IDLE 打断时豁免）
- I6：进入 ATTACK_TURN 的前一帧 facing 与 target 反号
- I7：state 只能是 0/1/2/3/4
- I8：state 跨帧转移合法
- I9：KEY 动作 press/release 合法（不重复按下 / 不未按先放）

入口脚本：`scripts/run_script_acceptance.ps1`，纳入 `scripts/run_all.ps1` 主验收流。
