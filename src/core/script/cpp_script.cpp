#include "cpp_script.h"

#include "script_config.h"

#include "human_profile.h"

#include "script_types.h"

#include "script_geometry.h"

#include "script_perception.h"

#include "script_fsm.h"

#include "script_sampling.h"

#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

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
        StateContext ctx(impl_->rt, world, now);
        TickFsm(ctx);
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
