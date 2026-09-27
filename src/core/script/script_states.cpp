#include "script_fsm.h"

#include "script_sampling.h"
#include "human_profile.h"

#include <cstdio>

void StateIdle(StateContext& ctx) {
    if (ctx.rt.me.valid && ctx.rt.target.has) {
        ctx.rt.state = StateId::CHASE;
    }
}

void StateChase(StateContext& ctx) {
    const bool in_band_front = ctx.rt.cls.in_band_front;
    const bool in_band_back  = ctx.rt.cls.in_band_back;
    const int target_dir     = ctx.rt.cls.target_dir;
    const bool is_front      = ctx.rt.cls.is_front;
    const bool fresh_target  = ctx.rt.cls.fresh_target;

    if (!ctx.rt.me.valid || !ctx.rt.target.has) {
        ctx.rt.pending_attack = false;
        ctx.rt.state = StateId::IDLE;
    }
    // 已在 pending：检查延迟
    else if (ctx.rt.pending_attack) {
        if (ctx.now - ctx.rt.pending_attack_start_ms >=
            static_cast<uint64_t>(ctx.rt.pending_attack_delay_ms)) {
            ctx.rt.pending_attack = false;
            // 延迟到点，重新检查
            if (in_band_front && is_front && fresh_target) {
                ctx.rt.state = StateId::ATTACK;
                ctx.rt.attack_start_ms = ctx.now;
                ctx.rt.e_pressed = true;
                ctx.rt.current_e_tap_ms = SampleEHoldMs(ctx.rt.rng);
                // 技能动作内随机时刻松方向键（模拟人类攻击时手离方向键）
    if (ctx.rt.active_dir_key != 0) {
        // E 按下后 300~800ms 内随机抬起方向键（攻击僵直期间按住无影响）
        std::uniform_int_distribution<int> dist(300, 800);
        ctx.rt.pending_dir_release_ms = ctx.now + static_cast<uint64_t>(dist(ctx.rt.rng));
    }
                std::printf("[script] E 按下时长: %d ms\n",
                            ctx.rt.current_e_tap_ms);
                std::fflush(stdout);
            }
        }
    }
    // 背面进带：S7 逻辑（转身）
    else if (in_band_back && !is_front && fresh_target) {
        // 技能冷却检查：与正面进带同样对待。
        // 若上次 E release 距"预计实际按 E"（now + bounce + turn_press_delay 下限）还不到
        // kSkillCooldownMs，则不进入 ATTACK_TURN，留在 CHASE 等冷却。
        // 注意：bounce 是入场门闩（可能已过），不计入时间保守项；
        // 只用 turn_press_delay 下限做保守估计。
        const bool cooldown_ok_turn =
            (ctx.rt.last_attack_release_ms == 0) ||
            ((ctx.now - ctx.rt.last_attack_release_ms) +
                static_cast<uint64_t>(ScriptConfig::kTurnPressDelayMinMs)
                    >= ScriptConfig::kSkillCooldownMs);
        if (cooldown_ok_turn && ctx.now - ctx.rt.last_dir_ms >= ScriptConfig::kTurnBounceMs) {
            ctx.rt.state = StateId::ATTACK_TURN;
            ctx.rt.turn_start_ms = ctx.now;
            ctx.rt.turn_dir_key = (target_dir > 0) ? 0x27 : 0x25;
            ctx.rt.facing = target_dir;
            ctx.rt.last_dir_ms = ctx.now;
            ctx.rt.turn_key_pressed = true;
            ctx.rt.turn_e_pressed = false;

            // 采样 E 时长
            ctx.rt.current_e_tap_ms = SampleEHoldMs(ctx.rt.rng);

            // E 按下时刻 = now + [100, 200]
            {
                std::uniform_int_distribution<int> ep(
                    ScriptConfig::kTurnPressDelayMinMs, ScriptConfig::kTurnPressDelayMaxMs);
                ctx.rt.turn_e_press_ms = ctx.now + static_cast<uint64_t>(ep(ctx.rt.rng));
            }
            ctx.rt.turn_e_release_ms = ctx.rt.turn_e_press_ms +
                static_cast<uint64_t>(ctx.rt.current_e_tap_ms);

            // 方向键松开时刻 = now + [dir_min_hold, 800]
            uint64_t dir_min_hold_ms = 300;
            {
                const int64_t us = SampleFromProfile(
                    kDirHoldProfile, kDirHoldProfileSize, ctx.rt.rng);
                dir_min_hold_ms = static_cast<uint64_t>(us / 1000);
            }
            {
                std::uniform_int_distribution<uint64_t> dd(dir_min_hold_ms, 800);
                ctx.rt.turn_dir_release_ms = ctx.now + dd(ctx.rt.rng);
            }
        }
        // 冷却未到或 bounce 未过：留在 CHASE，下一帧再试
    }
    // 正面进带：设 pending (S8)；先检查技能冷却
    else if (in_band_front && is_front && fresh_target) {
        // 技能冷却硬下限检查：上次 E release 到"实际按 E"（now + pending_delay）
        // 必须 ≥ 810ms。用 pending_delay 下限做保守检查。
        const bool cooldown_ok =
            (ctx.rt.last_attack_release_ms == 0) ||
            ((ctx.now - ctx.rt.last_attack_release_ms) +
                static_cast<uint64_t>(ctx.rt.cfg.attack_react_min_ms)
                    >= ScriptConfig::kSkillCooldownMs);
        if (cooldown_ok) {
            ctx.rt.pending_attack = true;
            ctx.rt.pending_attack_start_ms = ctx.now;
            {
                std::uniform_int_distribution<int> dist(
                    ctx.rt.cfg.attack_react_min_ms, ctx.rt.cfg.attack_react_max_ms);
                ctx.rt.pending_attack_delay_ms = dist(ctx.rt.rng);
            }
            std::printf("[script] 攻击反应延迟: %d ms\n",
                        ctx.rt.pending_attack_delay_ms);
            std::fflush(stdout);
        }
        // 冷却未到：不设 pending，继续 CHASE，下一帧再试
    }
}

void StateAttack(StateContext& ctx) {
    if (!ctx.rt.me.valid || !ctx.rt.target.has) {
        ctx.rt.state = StateId::IDLE;
        if (ctx.rt.e_pressed) { ctx.rt.last_attack_release_ms = ctx.now; }
        ctx.rt.e_pressed = false;
        return;
    }

    if (ctx.now - ctx.rt.attack_start_ms >=
        static_cast<uint64_t>(ctx.rt.current_e_tap_ms)) {
        ctx.rt.e_pressed = false;
        ctx.rt.last_attack_release_ms = ctx.now;
        ctx.rt.state = StateId::RECOVERY;
        ctx.rt.recovery_start_ms = ctx.now;
    }
}

void StateAttackTurn(StateContext& ctx) {
    // E 按下
    if (!ctx.rt.turn_e_pressed && ctx.now >= ctx.rt.turn_e_press_ms) {
        ctx.rt.turn_e_pressed = true;
    }
    // E 释放
    if (ctx.rt.turn_e_pressed && ctx.now >= ctx.rt.turn_e_release_ms) {
        ctx.rt.turn_e_pressed = false;
        ctx.rt.last_attack_release_ms = ctx.now;
    }
    // 方向键释放
    if (ctx.rt.turn_key_pressed && ctx.now >= ctx.rt.turn_dir_release_ms) {
        ctx.rt.turn_key_pressed = false;
    }
    // 两者都完成 → RECOVERY
    if (!ctx.rt.turn_key_pressed && !ctx.rt.turn_e_pressed) {
        ctx.rt.state = StateId::RECOVERY;
        ctx.rt.recovery_start_ms = ctx.now;
    }
}

void StateRecovery(StateContext& ctx) {
    if (!ctx.rt.me.valid || !ctx.rt.target.has) {
        ctx.rt.pending_chase = false;
        ctx.rt.state = StateId::IDLE;
    }
    else if (ctx.now - ctx.rt.recovery_start_ms >= ScriptConfig::kRecoveryMs) {
        if (!ctx.rt.pending_chase) {
            ctx.rt.pending_chase = true;
            ctx.rt.pending_chase_start_ms = ctx.now;
            {
                std::uniform_int_distribution<int> dist(
                    ctx.rt.cfg.recovery_chase_min_ms, ctx.rt.cfg.recovery_chase_max_ms);
                ctx.rt.pending_chase_delay_ms = dist(ctx.rt.rng);
            }
        }
        if (ctx.now - ctx.rt.pending_chase_start_ms >=
            static_cast<uint64_t>(ctx.rt.pending_chase_delay_ms)) {
            ctx.rt.pending_chase = false;
            ctx.rt.state = StateId::CHASE;
        }
    }
}
