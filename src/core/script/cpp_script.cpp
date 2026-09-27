#include "cpp_script.h"

#include "script_config.h"

#include "human_profile.h"

#include "script_types.h"

#include "script_geometry.h"

#include "script_perception.h"

#include "script_fsm.h"

#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>

namespace {

}  // namespace

struct CppScript::Impl {
    Runtime rt;
};

static void NormalizeConfig(ScriptConfig* cfg) {
    if (cfg->attack_react_max_ms < cfg->attack_react_min_ms) {
        cfg->attack_react_max_ms = cfg->attack_react_min_ms;
    }
    if (cfg->recovery_chase_max_ms < cfg->recovery_chase_min_ms) {
        cfg->recovery_chase_max_ms = cfg->recovery_chase_min_ms;
    }
}

// 从分位表分段线性采样。u ∈ [0,100] 均匀取，在相邻分位点插值。
static int64_t SampleFromProfile(const int64_t profile[][2], int n,
                                 std::mt19937& rng) {
    std::uniform_real_distribution<double> dist(0.0, 100.0);
    const double u = dist(rng);
    for (int i = 0; i < n - 1; ++i) {
        if (u <= static_cast<double>(profile[i + 1][0])) {
            const double p0 = static_cast<double>(profile[i][0]);
            const double p1 = static_cast<double>(profile[i + 1][0]);
            const double v0 = static_cast<double>(profile[i][1]);
            const double v1 = static_cast<double>(profile[i + 1][1]);
            if (p1 <= p0) return static_cast<int64_t>(v0);
            const double t = (u - p0) / (p1 - p0);
            return static_cast<int64_t>(v0 + t * (v1 - v0));
        }
    }
    return profile[n - 1][1];
}

// E 键按住时长：从真人分位表采样（微秒转毫秒）
static int SampleEHoldMs(std::mt19937& rng) {
    const int64_t us = SampleFromProfile(kEHoldProfile, kEHoldProfileSize, rng);
    return static_cast<int>(us / 1000);
}

CppScript::CppScript() : impl_(new Impl()) {}
CppScript::~CppScript() { delete impl_; }

bool CppScript::Init(const std::string& config) {
    // RNG 种子：
    //   - 若环境变量 MH_SCRIPT_SEED 已设 → 用它（测试用：固定种子下 trace 可逐字节对比）
    //   - 否则 → 用现有 time ^ steady_clock（产品运行逻辑不变）
    const char* seed_env = std::getenv("MH_SCRIPT_SEED");
    if (seed_env != nullptr && seed_env[0] != '\0') {
        impl_->rt.rng.seed(static_cast<uint32_t>(std::atol(seed_env)));
    } else {
        const auto now_ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        impl_->rt.rng.seed(static_cast<uint32_t>(std::time(nullptr)) ^
                        static_cast<uint32_t>(now_ticks));
    }

    impl_->rt.cfg = ScriptConfig::FromString(config);

    NormalizeConfig(&impl_->rt.cfg);

    std::printf("[script] 配置: react=[%d,%d], chase=[%d,%d]\n",
        impl_->rt.cfg.attack_react_min_ms, impl_->rt.cfg.attack_react_max_ms,
        impl_->rt.cfg.recovery_chase_min_ms, impl_->rt.cfg.recovery_chase_max_ms);
    std::fflush(stdout);

    std::printf("[script] 脚本已启动\n");
    std::fflush(stdout);
    impl_->rt.inited = true;
    return true;
}

void CppScript::OnFrame(const ScriptWorld& world) {
    SelectMe(world, &impl_->rt.me);

    if (impl_->rt.me.valid) {
        SelectTarget(world, impl_->rt.me, impl_->rt.facing, &impl_->rt.target);
    } else {
        impl_->rt.target.has = false;
    }

    const uint64_t now = world.now_ms;

    // 每帧分类缓存（替代后续 3 处重复计算）
    {
        impl_->rt.cls.in_band_front = false;
        impl_->rt.cls.in_band_back  = false;
        impl_->rt.cls.in_band_any   = false;
        impl_->rt.cls.dx            = 0.0f;
        impl_->rt.cls.abs_dx        = 0.0f;
        impl_->rt.cls.target_dir    = 0;
        impl_->rt.cls.is_front      = false;
        impl_->rt.cls.fresh_target  = false;

        if (impl_->rt.me.valid && impl_->rt.target.has) {
            impl_->rt.cls.in_band_front = IsInBand(impl_->rt.target, impl_->rt.me, impl_->rt.facing);
            impl_->rt.cls.in_band_back  = IsInBand(impl_->rt.target, impl_->rt.me, -impl_->rt.facing);
            impl_->rt.cls.in_band_any   = impl_->rt.cls.in_band_front || impl_->rt.cls.in_band_back;
            impl_->rt.cls.dx            = impl_->rt.target.cx - impl_->rt.me.fx;
            impl_->rt.cls.abs_dx        = std::fabs(impl_->rt.cls.dx);
            impl_->rt.cls.target_dir    = (impl_->rt.cls.dx > 0) ? 1 : -1;
            impl_->rt.cls.is_front      = (impl_->rt.cls.target_dir == impl_->rt.facing);
            impl_->rt.cls.fresh_target  = (impl_->rt.target.lost_since == 0);
        }
    }

    if (!impl_->rt.me.valid || !impl_->rt.target.has) {
        if (impl_->rt.state != StateId::IDLE) {
            impl_->rt.state = StateId::IDLE;
            if (impl_->rt.e_pressed) { impl_->rt.last_attack_release_ms = now; }
            impl_->rt.e_pressed = false;
            impl_->rt.turn_key_pressed = false;
            if (impl_->rt.turn_e_pressed) { impl_->rt.last_attack_release_ms = now; }
            impl_->rt.turn_e_pressed = false;
            impl_->rt.pending_attack = false;
            impl_->rt.pending_chase = false;
            impl_->rt.pending_dir_release_ms = 0;
        }
    } else {
        // 从 cls 缓存读（每帧只算一次）
        const bool in_band_front = impl_->rt.cls.in_band_front;
        const bool in_band_back  = impl_->rt.cls.in_band_back;
        const float dx_target = impl_->rt.cls.dx;
        const int target_dir = impl_->rt.cls.target_dir;
        const bool is_front = impl_->rt.cls.is_front;
        const bool fresh_target = impl_->rt.cls.fresh_target;

        switch (impl_->rt.state) {
            case StateId::IDLE:
                if (impl_->rt.me.valid && impl_->rt.target.has) {
                    impl_->rt.state = StateId::CHASE;
                }
                break;

            case StateId::CHASE:
                if (!impl_->rt.me.valid || !impl_->rt.target.has) {
                    impl_->rt.pending_attack = false;
                    impl_->rt.state = StateId::IDLE;
                }
                // 已在 pending：检查延迟
                else if (impl_->rt.pending_attack) {
                    if (now - impl_->rt.pending_attack_start_ms >=
                        static_cast<uint64_t>(impl_->rt.pending_attack_delay_ms)) {
                        impl_->rt.pending_attack = false;
                        // 延迟到点，重新检查
                        if (in_band_front && is_front && fresh_target) {
                            impl_->rt.state = StateId::ATTACK;
                            impl_->rt.attack_start_ms = now;
                            impl_->rt.e_pressed = true;
                            impl_->rt.current_e_tap_ms = SampleEHoldMs(impl_->rt.rng);
                            // 技能动作内随机时刻松方向键（模拟人类攻击时手离方向键）
    if (impl_->rt.active_dir_key != 0) {
        // E 按下后 300~800ms 内随机抬起方向键（攻击僵直期间按住无影响）
        std::uniform_int_distribution<int> dist(300, 800);
        impl_->rt.pending_dir_release_ms = now + static_cast<uint64_t>(dist(impl_->rt.rng));
    }
                            std::printf("[script] E 按下时长: %d ms\n",
                                        impl_->rt.current_e_tap_ms);
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
                        (impl_->rt.last_attack_release_ms == 0) ||
                        ((now - impl_->rt.last_attack_release_ms) +
                            static_cast<uint64_t>(ScriptConfig::kTurnPressDelayMinMs)
                                >= ScriptConfig::kSkillCooldownMs);
                    if (cooldown_ok_turn && now - impl_->rt.last_dir_ms >= ScriptConfig::kTurnBounceMs) {
                        impl_->rt.state = StateId::ATTACK_TURN;
                        impl_->rt.turn_start_ms = now;
                        impl_->rt.turn_dir_key = (target_dir > 0) ? 0x27 : 0x25;
                        impl_->rt.facing = target_dir;
                        impl_->rt.last_dir_ms = now;
                        impl_->rt.turn_key_pressed = true;
                        impl_->rt.turn_e_pressed = false;

                        // 采样 E 时长
                        impl_->rt.current_e_tap_ms = SampleEHoldMs(impl_->rt.rng);

                        // E 按下时刻 = now + [100, 200]
                        {
                            std::uniform_int_distribution<int> ep(
                                ScriptConfig::kTurnPressDelayMinMs, ScriptConfig::kTurnPressDelayMaxMs);
                            impl_->rt.turn_e_press_ms = now + static_cast<uint64_t>(ep(impl_->rt.rng));
                        }
                        impl_->rt.turn_e_release_ms = impl_->rt.turn_e_press_ms +
                            static_cast<uint64_t>(impl_->rt.current_e_tap_ms);

                        // 方向键松开时刻 = now + [dir_min_hold, 800]
                        uint64_t dir_min_hold_ms = 300;
                        {
                            const int64_t us = SampleFromProfile(
                                kDirHoldProfile, kDirHoldProfileSize, impl_->rt.rng);
                            dir_min_hold_ms = static_cast<uint64_t>(us / 1000);
                        }
                        {
                            std::uniform_int_distribution<uint64_t> dd(dir_min_hold_ms, 800);
                            impl_->rt.turn_dir_release_ms = now + dd(impl_->rt.rng);
                        }
                    }
                    // 冷却未到或 bounce 未过：留在 CHASE，下一帧再试
                }
                // 正面进带：设 pending (S8)；先检查技能冷却
                else if (in_band_front && is_front && fresh_target) {
                    // 技能冷却硬下限检查：上次 E release 到"实际按 E"（now + pending_delay）
                    // 必须 ≥ 810ms。用 pending_delay 下限做保守检查。
                    const bool cooldown_ok =
                        (impl_->rt.last_attack_release_ms == 0) ||
                        ((now - impl_->rt.last_attack_release_ms) +
                            static_cast<uint64_t>(impl_->rt.cfg.attack_react_min_ms)
                                >= ScriptConfig::kSkillCooldownMs);
                    if (cooldown_ok) {
                        impl_->rt.pending_attack = true;
                        impl_->rt.pending_attack_start_ms = now;
                        {
                            std::uniform_int_distribution<int> dist(
                                impl_->rt.cfg.attack_react_min_ms, impl_->rt.cfg.attack_react_max_ms);
                            impl_->rt.pending_attack_delay_ms = dist(impl_->rt.rng);
                        }
                        std::printf("[script] 攻击反应延迟: %d ms\n",
                                    impl_->rt.pending_attack_delay_ms);
                        std::fflush(stdout);
                    }
                    // 冷却未到：不设 pending，继续 CHASE，下一帧再试
                }
                break;

            case StateId::ATTACK: {
                if (!impl_->rt.me.valid || !impl_->rt.target.has) {
                    impl_->rt.state = StateId::IDLE;
                    if (impl_->rt.e_pressed) { impl_->rt.last_attack_release_ms = now; }
                    impl_->rt.e_pressed = false;
                    break;
                }

                if (now - impl_->rt.attack_start_ms >=
                    static_cast<uint64_t>(impl_->rt.current_e_tap_ms)) {
                    impl_->rt.e_pressed = false;
                    impl_->rt.last_attack_release_ms = now;
                    impl_->rt.state = StateId::RECOVERY;
                    impl_->rt.recovery_start_ms = now;
                }
                break;
            }

            case StateId::ATTACK_TURN: {
                // E 按下
                if (!impl_->rt.turn_e_pressed && now >= impl_->rt.turn_e_press_ms) {
                    impl_->rt.turn_e_pressed = true;
                }
                // E 释放
                if (impl_->rt.turn_e_pressed && now >= impl_->rt.turn_e_release_ms) {
                    impl_->rt.turn_e_pressed = false;
                    impl_->rt.last_attack_release_ms = now;
                }
                // 方向键释放
                if (impl_->rt.turn_key_pressed && now >= impl_->rt.turn_dir_release_ms) {
                    impl_->rt.turn_key_pressed = false;
                }
                // 两者都完成 → RECOVERY
                if (!impl_->rt.turn_key_pressed && !impl_->rt.turn_e_pressed) {
                    impl_->rt.state = StateId::RECOVERY;
                    impl_->rt.recovery_start_ms = now;
                }
                break;
            }

            case StateId::RECOVERY:
                if (!impl_->rt.me.valid || !impl_->rt.target.has) {
                    impl_->rt.pending_chase = false;
                    impl_->rt.state = StateId::IDLE;
                }
                else if (now - impl_->rt.recovery_start_ms >= ScriptConfig::kRecoveryMs) {
                    if (!impl_->rt.pending_chase) {
                        impl_->rt.pending_chase = true;
                        impl_->rt.pending_chase_start_ms = now;
                        {
                            std::uniform_int_distribution<int> dist(
                                impl_->rt.cfg.recovery_chase_min_ms, impl_->rt.cfg.recovery_chase_max_ms);
                            impl_->rt.pending_chase_delay_ms = dist(impl_->rt.rng);
                        }
                    }
                    if (now - impl_->rt.pending_chase_start_ms >=
                        static_cast<uint64_t>(impl_->rt.pending_chase_delay_ms)) {
                        impl_->rt.pending_chase = false;
                        impl_->rt.state = StateId::CHASE;
                    }
                }
                break;
        }
    } // End of else block (me.valid && target.has)

    // 决定按键
    int desired_dir = 0;
    bool desired_e = false;
    const float dx_target = impl_->rt.target.cx - impl_->rt.me.fx;
    const int target_dir = (dx_target > 0) ? 1 : -1;

    if (impl_->rt.state == StateId::IDLE) {
        impl_->rt.dir_press_start_ms = 0;
        impl_->rt.dir_min_hold_ms = 0;
        impl_->rt.active_dir_key = 0;
    }

    switch (impl_->rt.state) {
        case StateId::IDLE:
            desired_dir = 0;
            desired_e = false;
            break;

        case StateId::CHASE: {
            // 带内不追、带外追——用 cls 缓存判定，无中间地带。
            const bool need_move = !impl_->rt.cls.in_band_any;

            const int want_dir = need_move ? ((dx_target > 0) ? 0x27 : 0x25) : 0;
            desired_dir = want_dir;
            desired_e = false;
            break;
        }

        case StateId::ATTACK:
            desired_dir = 0;
            desired_e = true;
            break;

        case StateId::ATTACK_TURN: {
            desired_dir = impl_->rt.turn_key_pressed ? impl_->rt.turn_dir_key : 0;
            desired_e = impl_->rt.turn_e_pressed;
            break;
        }

        case StateId::RECOVERY:
            desired_dir = 0;
            desired_e = false;
            break;
    }

    // ===== 统一的方向键最短按住约束 =====
    // 规则：
    //   1. IDLE 状态：me/target 丢失 → 该松，不受最短按住约束
    //   2. 其它状态：若正在按住某方向键且未满最短时长，且新输出想换键/松手
    //      （怪不在身后的前提下）→ 保持原键
    //   3. facing 永远从最终 out_dir 反推（I10）

    if (impl_->rt.state != StateId::IDLE && impl_->rt.active_dir_key != 0) {
        const bool want_change = (desired_dir != impl_->rt.active_dir_key);
        if (want_change) {
            const bool held_long_enough =
                (now - impl_->rt.dir_press_start_ms) >= impl_->rt.dir_min_hold_ms;
            // 明确反向才触发（±0.02 阈值），避免 dx_target 在 0 附近抖动
            const bool target_behind =
                (impl_->rt.facing > 0 && dx_target < -0.02f) ||
                (impl_->rt.facing < 0 && dx_target >  0.02f);
            if (!held_long_enough && !target_behind) {
                desired_dir = impl_->rt.active_dir_key;
            }
        }
    }

    // 进带 → 强制松方向键（不受最短按住约束，这是"到达"不是"抖动"）
    if (impl_->rt.state == StateId::CHASE) {
        if (impl_->rt.cls.in_band_any) {
            desired_dir = 0;
        }
    }

    // 强制松方向键：进 ATTACK 后到达随机松手时刻 → 无论最短按住是否满足都松
    if (impl_->rt.pending_dir_release_ms != 0 && now >= impl_->rt.pending_dir_release_ms) {
        desired_dir = 0;
        impl_->rt.pending_dir_release_ms = 0;
    }

    // 更新计时：按键变化时重置或清空
    if (desired_dir != impl_->rt.active_dir_key) {
        if (desired_dir != 0) {
            impl_->rt.dir_press_start_ms = now;
            // 方向键最短按住：从真人方向键低分位表采样，消除碎步
            const int64_t hold_us = SampleFromProfile(kDirHoldProfile, kDirHoldProfileSize, impl_->rt.rng);
            impl_->rt.dir_min_hold_ms = static_cast<uint64_t>(hold_us / 1000);
        } else {
            impl_->rt.dir_press_start_ms = 0;
            impl_->rt.dir_min_hold_ms = 0;
        }
    }

    // facing 从 desired_dir 反推
    const int old_facing = impl_->rt.facing;
    if (desired_dir == 0x27) {
        impl_->rt.facing = 1;
    } else if (desired_dir == 0x25) {
        impl_->rt.facing = -1;
    }
    if (impl_->rt.facing != old_facing) {
        impl_->rt.last_dir_ms = now;
    }

    impl_->rt.active_dir_key = desired_dir;

    impl_->rt.active_key = desired_dir;
    impl_->rt.desired_e = desired_e;

    if (world.frame_index - impl_->rt.last_log_frame >= 30) {
        impl_->rt.last_log_frame = world.frame_index;
        const char* state_str = "IDLE";
        switch (impl_->rt.state) {
            case StateId::IDLE: state_str = "IDLE"; break;
            case StateId::CHASE: state_str = "CHASE"; break;
            case StateId::ATTACK: state_str = "ATTACK"; break;
            case StateId::ATTACK_TURN: state_str = "ATTACK_TURN"; break;
            case StateId::RECOVERY: state_str = "RECOVERY"; break;
        }
        std::printf("[script] frame=%llu state=%s facing=%d me_locked=%d me=(%.3f,%.3f) target_locked=%d target_cx=%.3f key=0x%02X e=%d\n",
                    (unsigned long long)world.frame_index,
                    state_str,
                    impl_->rt.facing,
                    impl_->rt.me.locked ? 1 : 0,
                    impl_->rt.me.fx, impl_->rt.me.fy,
                    impl_->rt.target.locked ? 1 : 0,
                    impl_->rt.target.cx,
                    impl_->rt.active_key,
                    desired_e ? 1 : 0);
        std::fflush(stdout);
    }
} // End of OnFrame

void CppScript::GetDecision(core_decision* out) {
    out->out_count = 0;

    // 新语义：输出"当前希望按住的键"（只输出 press，不输出 release）。
    // 释放由 OutputManager 对比上一帧意图自动产生。
    if (impl_->rt.active_key != 0 && out->out_count < CORE_DECISION_CAPACITY) {
        out->actions[out->out_count].kind = CORE_ACTION_KEY;
        out->actions[out->out_count].a = impl_->rt.active_key;
        out->actions[out->out_count].b = 1;
        out->actions[out->out_count].c = 0;
        out->out_count++;
    }
    if (impl_->rt.desired_e && out->out_count < CORE_DECISION_CAPACITY) {
        out->actions[out->out_count].kind = CORE_ACTION_KEY;
        out->actions[out->out_count].a = 0x45;
        out->actions[out->out_count].b = 1;
        out->actions[out->out_count].c = 0;
        out->out_count++;
    }
} // End of GetDecision

void CppScript::Shutdown() {
    std::printf("[script] 脚本已停止\n");
    std::fflush(stdout);
    impl_->rt.inited = false;
} // End of Shutdown

void CppScript::GetDebugInfo(CppScriptDebugInfo* out) const {
    if (out == nullptr) return;
    out->state = static_cast<int>(impl_->rt.state);
    out->facing = impl_->rt.facing;
    out->me_locked = impl_->rt.me.locked;
    out->me_fx = impl_->rt.me.fx;
    out->me_fy = impl_->rt.me.fy;
    out->target_locked = impl_->rt.target.locked;
    out->target_cx = impl_->rt.target.cx;
    out->active_key = impl_->rt.active_key;
    out->desired_e = impl_->rt.desired_e;
}

bool CppScript::GetMeLock(float* fx, float* fy) const {
    if (!impl_->rt.me.valid) return false;
    if (fx) *fx = impl_->rt.me.fx;
    if (fy) *fy = impl_->rt.me.fy;
    return true;
}
