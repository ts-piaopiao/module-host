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
