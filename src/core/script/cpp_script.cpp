#include "cpp_script.h"

#include "script_config.h"

#include "human_profile.h"

#include "script_types.h"

#include "script_geometry.h"

#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>

namespace {

void SelectMe(const ScriptWorld& world, MeLockState* m) {
    if (world.dets == nullptr) {
        m->valid = false;
        return;
    }

    const uint64_t now = world.now_ms;

    struct Cand { float fx, fy; };
    Cand cands[CORE_MAX_DETECTIONS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < world.dets->count; ++i) {
        const auto& d = world.dets->items[i];
        if (d.cls != 0) continue;
        if (n >= CORE_MAX_DETECTIONS) break;
        cands[n].fx = d.cx;
        cands[n].fy = d.cy + d.h * 0.5f;
        n++;
    }

    if (!m->locked) {
        if (n == 0) {
            m->valid = false;
            return;
        }
        float best_d2 = 1e9f;
        int best = -1;
        for (uint32_t i = 0; i < n; ++i) {
            const float dx = cands[i].fx - ScriptConfig::kPriorX;
            const float dy = cands[i].fy - ScriptConfig::kPriorY;
            const float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best_d2 = d2; best = (int)i; }
        }
        if (best >= 0) {
            m->locked = true;
            m->lock_fx = cands[best].fx;
            m->lock_fy = cands[best].fy;
            m->fx = cands[best].fx;
            m->fy = cands[best].fy;
            m->lost_since = 0;
            m->valid = true;
        }
        return;
    }

    int matched = -1;
    float best_d2 = ScriptConfig::kMeLockRange * ScriptConfig::kMeLockRange;
    for (uint32_t i = 0; i < n; ++i) {
        const float dx = cands[i].fx - m->lock_fx;
        const float dy = cands[i].fy - m->lock_fy;
        const float d2 = dx * dx + dy * dy;
        if (d2 < best_d2) { best_d2 = d2; matched = (int)i; }
    }

    if (matched >= 0) {
        m->lock_fx = cands[matched].fx;
        m->lock_fy = cands[matched].fy;
        m->fx = cands[matched].fx;
        m->fy = cands[matched].fy;
        m->lost_since = 0;
        m->valid = true;
        return;
    }

    if (m->lost_since == 0) {
        m->lost_since = now;
    }
    const uint64_t elapsed = now - m->lost_since;
    if (elapsed < ScriptConfig::kMeRelockMs) {
        m->valid = true;
    } else {
        m->locked = false;
        m->lost_since = 0;
        m->valid = false;
    }
}

void SelectTarget(const ScriptWorld& world, const MeLockState& me,
                  int facing, TargetLockState* t) {
    if (world.dets == nullptr || !me.valid) {
        t->has = false;
        return;
    }

    const uint64_t now = world.now_ms;

    struct Cand { float cx, cy, fy, h, w; };
    Cand cands[CORE_MAX_DETECTIONS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < world.dets->count; ++i) {
        const auto& d = world.dets->items[i];
        if (d.cls != 1) continue;
        const float d_fy = (d.cy + d.h * 0.5f) - me.lock_fy;
        if (std::fabs(d_fy) > ScriptConfig::kSamePlatY) continue;
        if (n >= CORE_MAX_DETECTIONS) break;
        cands[n].cx = d.cx;
        cands[n].cy = d.cy;
        cands[n].fy = d.cy + d.h * 0.5f;
        cands[n].h = d.h;
        cands[n].w = d.w;
        n++;
    }

    // 层级：0=前带（facing 方向攻击带），1=后带（反方向），2=远处
    auto layer_of = [&](uint32_t i) -> int {
        if (IsInBand(me.fx, me.fy, cands[i].cx, cands[i].cy,
                     cands[i].h, cands[i].w, facing)) return 0;
        if (IsInBand(me.fx, me.fy, cands[i].cx, cands[i].cy,
                     cands[i].h, cands[i].w, -facing)) return 1;
        return 2;
    };

    // 选 candidate：L1 > L2 > L3；同层选最近
    int cand_idx = -1;
    int cand_layer = 99;
    float cand_d = 1e9f;
    for (uint32_t i = 0; i < n; ++i) {
        const int layer = layer_of(i);
        const float d = std::fabs(cands[i].cx - me.lock_fx);
        if (layer < cand_layer || (layer == cand_layer && d < cand_d)) {
            cand_layer = layer;
            cand_d = d;
            cand_idx = (int)i;
        }
    }

    if (t->locked) {
        // 当前锁定目标在本帧对应的怪（位置匹配）
        int cur_idx = -1;
        float best_d2 = 1e9f;
        for (uint32_t i = 0; i < n; ++i) {
            const float dx = std::fabs(cands[i].cx - t->lock_cx);
            const float dy = std::fabs(cands[i].cy - t->lock_cy);
            if (dx > ScriptConfig::kTargetMatchX || dy > ScriptConfig::kTargetMatchY) continue;
            const float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best_d2 = d2; cur_idx = (int)i; }
        }

        if (cur_idx >= 0) {
            // 当前目标仍在视野 —— 判断是否切换
            bool switch_target = false;
            if (cand_idx >= 0 && cand_idx != cur_idx) {
                const int cur_layer = layer_of((uint32_t)cur_idx);
                if (cur_layer > 1) {
                    // 当前脱离攻击带 → 立即切（无冷却）
                    switch_target = true;
                } else if (now - t->last_target_switch_ms >= ScriptConfig::kTargetSwitchCooldownMs) {
                    // 冷却已过 → 评估
                    if (cand_layer < cur_layer) {
                        switch_target = true;  // 层级更优
                    } else if (cand_layer == cur_layer) {
                        const float cur_d = std::fabs(cands[cur_idx].cx - me.lock_fx);
                        if (cand_d < cur_d) switch_target = true;  // 同层更近
                    }
                }
                // 冷却中且当前在带内 → 保持
            }

            if (switch_target && cand_idx >= 0) {
                cur_idx = cand_idx;
                t->last_target_switch_ms = now;
            }

            t->lock_cx = cands[cur_idx].cx;
            t->lock_cy = cands[cur_idx].cy;
            t->cx = cands[cur_idx].cx;
            t->cy = cands[cur_idx].cy;
            t->fy = cands[cur_idx].fy;
            t->h = cands[cur_idx].h;
            t->w = cands[cur_idx].w;
            t->lost_since = 0;
            t->has = true;
            return;
        }

        // 当前目标丢失 —— 宽容期
        if (t->lost_since == 0) t->lost_since = now;
        if (now - t->lost_since < ScriptConfig::kTargetLoseMs) {
            t->has = true;
            return;
        }
        t->locked = false;
        t->lost_since = 0;
    }

    // 未锁定（或刚解锁）→ 选 candidate
    if (cand_idx >= 0) {
        t->locked = true;
        t->lock_cx = cands[cand_idx].cx;
        t->lock_cy = cands[cand_idx].cy;
        t->cx = cands[cand_idx].cx;
        t->cy = cands[cand_idx].cy;
        t->fy = cands[cand_idx].fy;
        t->h = cands[cand_idx].h;
        t->w = cands[cand_idx].w;
        t->lost_since = 0;
        t->last_target_switch_ms = now;
        t->has = true;
    } else {
        t->has = false;
    }
}

}  // namespace

struct CppScript::Impl {
    uint64_t last_log_frame = 0;
    bool inited = false;

    StateId state = StateId::IDLE;

    MeLockState me;
    TargetLockState target;
    Classified cls;   // 每帧分类缓存

    uint64_t attack_start_ms = 0;
    uint64_t recovery_start_ms = 0;
    uint64_t last_attack_release_ms = 0;   // 上次 E 释放时刻；用于独立技能冷却检查
    bool e_pressed = false;
    int current_e_tap_ms = 150;   // 初始值；实际每次攻击前重新采样

    // 朝向
    int facing = 1;
    uint64_t last_dir_ms = 0;
    uint64_t dir_press_start_ms = 0;   // 当前方向键按住起点；0 = 未按住
    uint64_t dir_min_hold_ms = 0;      // 本次按住目标时长；按开始时从方向键分位表采样
    int active_dir_key = 0;   // 当前实际按住的方向键（0 / 0x25 / 0x27）
    uint64_t pending_dir_release_ms = 0;  // 计划松方向键的时刻；0=无计划

    // 转身攻击（并行倒计时）
    uint64_t turn_start_ms = 0;
    uint64_t turn_dir_release_ms = 0;   // 方向键松开时刻
    uint64_t turn_e_press_ms = 0;       // E 按下时刻
    uint64_t turn_e_release_ms = 0;     // E 释放时刻
    int turn_dir_key = 0;
    bool turn_key_pressed = false;
    bool turn_e_pressed = false;

    // 攻击延迟 (S8)
    bool pending_attack = false;
    uint64_t pending_attack_start_ms = 0;
    int pending_attack_delay_ms = 0;

    // RECOVERY 后延迟 (S8)
    bool pending_chase = false;
    uint64_t pending_chase_start_ms = 0;
    int pending_chase_delay_ms = 0;

    ScriptConfig cfg;

    std::mt19937 rng;

    int active_key = 0;
    bool desired_e = false;
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
        impl_->rng.seed(static_cast<uint32_t>(std::atol(seed_env)));
    } else {
        const auto now_ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        impl_->rng.seed(static_cast<uint32_t>(std::time(nullptr)) ^
                        static_cast<uint32_t>(now_ticks));
    }

    impl_->cfg = ScriptConfig::FromString(config);

    NormalizeConfig(&impl_->cfg);

    std::printf("[script] 配置: react=[%d,%d], chase=[%d,%d]\n",
        impl_->cfg.attack_react_min_ms, impl_->cfg.attack_react_max_ms,
        impl_->cfg.recovery_chase_min_ms, impl_->cfg.recovery_chase_max_ms);
    std::fflush(stdout);

    std::printf("[script] 脚本已启动\n");
    std::fflush(stdout);
    impl_->inited = true;
    return true;
}

void CppScript::OnFrame(const ScriptWorld& world) {
    SelectMe(world, &impl_->me);

    if (impl_->me.valid) {
        SelectTarget(world, impl_->me, impl_->facing, &impl_->target);
    } else {
        impl_->target.has = false;
    }

    const uint64_t now = world.now_ms;

    // 每帧分类缓存（替代后续 3 处重复计算）
    {
        impl_->cls.in_band_front = false;
        impl_->cls.in_band_back  = false;
        impl_->cls.in_band_any   = false;
        impl_->cls.dx            = 0.0f;
        impl_->cls.abs_dx        = 0.0f;
        impl_->cls.target_dir    = 0;
        impl_->cls.is_front      = false;
        impl_->cls.fresh_target  = false;

        if (impl_->me.valid && impl_->target.has) {
            impl_->cls.in_band_front = IsInBand(impl_->target, impl_->me, impl_->facing);
            impl_->cls.in_band_back  = IsInBand(impl_->target, impl_->me, -impl_->facing);
            impl_->cls.in_band_any   = impl_->cls.in_band_front || impl_->cls.in_band_back;
            impl_->cls.dx            = impl_->target.cx - impl_->me.fx;
            impl_->cls.abs_dx        = std::fabs(impl_->cls.dx);
            impl_->cls.target_dir    = (impl_->cls.dx > 0) ? 1 : -1;
            impl_->cls.is_front      = (impl_->cls.target_dir == impl_->facing);
            impl_->cls.fresh_target  = (impl_->target.lost_since == 0);
        }
    }

    if (!impl_->me.valid || !impl_->target.has) {
        if (impl_->state != StateId::IDLE) {
            impl_->state = StateId::IDLE;
            if (impl_->e_pressed) { impl_->last_attack_release_ms = now; }
            impl_->e_pressed = false;
            impl_->turn_key_pressed = false;
            if (impl_->turn_e_pressed) { impl_->last_attack_release_ms = now; }
            impl_->turn_e_pressed = false;
            impl_->pending_attack = false;
            impl_->pending_chase = false;
            impl_->pending_dir_release_ms = 0;
        }
    } else {
        // 从 cls 缓存读（每帧只算一次）
        const bool in_band_front = impl_->cls.in_band_front;
        const bool in_band_back  = impl_->cls.in_band_back;
        const float dx_target = impl_->cls.dx;
        const int target_dir = impl_->cls.target_dir;
        const bool is_front = impl_->cls.is_front;
        const bool fresh_target = impl_->cls.fresh_target;

        switch (impl_->state) {
            case StateId::IDLE:
                if (impl_->me.valid && impl_->target.has) {
                    impl_->state = StateId::CHASE;
                }
                break;

            case StateId::CHASE:
                if (!impl_->me.valid || !impl_->target.has) {
                    impl_->pending_attack = false;
                    impl_->state = StateId::IDLE;
                }
                // 已在 pending：检查延迟
                else if (impl_->pending_attack) {
                    if (now - impl_->pending_attack_start_ms >=
                        static_cast<uint64_t>(impl_->pending_attack_delay_ms)) {
                        impl_->pending_attack = false;
                        // 延迟到点，重新检查
                        if (in_band_front && is_front && fresh_target) {
                            impl_->state = StateId::ATTACK;
                            impl_->attack_start_ms = now;
                            impl_->e_pressed = true;
                            impl_->current_e_tap_ms = SampleEHoldMs(impl_->rng);
                            // 技能动作内随机时刻松方向键（模拟人类攻击时手离方向键）
    if (impl_->active_dir_key != 0) {
        // E 按下后 300~800ms 内随机抬起方向键（攻击僵直期间按住无影响）
        std::uniform_int_distribution<int> dist(300, 800);
        impl_->pending_dir_release_ms = now + static_cast<uint64_t>(dist(impl_->rng));
    }
                            std::printf("[script] E 按下时长: %d ms\n",
                                        impl_->current_e_tap_ms);
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
                        (impl_->last_attack_release_ms == 0) ||
                        ((now - impl_->last_attack_release_ms) +
                            static_cast<uint64_t>(ScriptConfig::kTurnPressDelayMinMs)
                                >= ScriptConfig::kSkillCooldownMs);
                    if (cooldown_ok_turn && now - impl_->last_dir_ms >= ScriptConfig::kTurnBounceMs) {
                        impl_->state = StateId::ATTACK_TURN;
                        impl_->turn_start_ms = now;
                        impl_->turn_dir_key = (target_dir > 0) ? 0x27 : 0x25;
                        impl_->facing = target_dir;
                        impl_->last_dir_ms = now;
                        impl_->turn_key_pressed = true;
                        impl_->turn_e_pressed = false;

                        // 采样 E 时长
                        impl_->current_e_tap_ms = SampleEHoldMs(impl_->rng);

                        // E 按下时刻 = now + [100, 200]
                        {
                            std::uniform_int_distribution<int> ep(
                                ScriptConfig::kTurnPressDelayMinMs, ScriptConfig::kTurnPressDelayMaxMs);
                            impl_->turn_e_press_ms = now + static_cast<uint64_t>(ep(impl_->rng));
                        }
                        impl_->turn_e_release_ms = impl_->turn_e_press_ms +
                            static_cast<uint64_t>(impl_->current_e_tap_ms);

                        // 方向键松开时刻 = now + [dir_min_hold, 800]
                        uint64_t dir_min_hold_ms = 300;
                        {
                            const int64_t us = SampleFromProfile(
                                kDirHoldProfile, kDirHoldProfileSize, impl_->rng);
                            dir_min_hold_ms = static_cast<uint64_t>(us / 1000);
                        }
                        {
                            std::uniform_int_distribution<uint64_t> dd(dir_min_hold_ms, 800);
                            impl_->turn_dir_release_ms = now + dd(impl_->rng);
                        }
                    }
                    // 冷却未到或 bounce 未过：留在 CHASE，下一帧再试
                }
                // 正面进带：设 pending (S8)；先检查技能冷却
                else if (in_band_front && is_front && fresh_target) {
                    // 技能冷却硬下限检查：上次 E release 到"实际按 E"（now + pending_delay）
                    // 必须 ≥ 810ms。用 pending_delay 下限做保守检查。
                    const bool cooldown_ok =
                        (impl_->last_attack_release_ms == 0) ||
                        ((now - impl_->last_attack_release_ms) +
                            static_cast<uint64_t>(impl_->cfg.attack_react_min_ms)
                                >= ScriptConfig::kSkillCooldownMs);
                    if (cooldown_ok) {
                        impl_->pending_attack = true;
                        impl_->pending_attack_start_ms = now;
                        {
                            std::uniform_int_distribution<int> dist(
                                impl_->cfg.attack_react_min_ms, impl_->cfg.attack_react_max_ms);
                            impl_->pending_attack_delay_ms = dist(impl_->rng);
                        }
                        std::printf("[script] 攻击反应延迟: %d ms\n",
                                    impl_->pending_attack_delay_ms);
                        std::fflush(stdout);
                    }
                    // 冷却未到：不设 pending，继续 CHASE，下一帧再试
                }
                break;

            case StateId::ATTACK: {
                if (!impl_->me.valid || !impl_->target.has) {
                    impl_->state = StateId::IDLE;
                    if (impl_->e_pressed) { impl_->last_attack_release_ms = now; }
                    impl_->e_pressed = false;
                    break;
                }

                if (now - impl_->attack_start_ms >=
                    static_cast<uint64_t>(impl_->current_e_tap_ms)) {
                    impl_->e_pressed = false;
                    impl_->last_attack_release_ms = now;
                    impl_->state = StateId::RECOVERY;
                    impl_->recovery_start_ms = now;
                }
                break;
            }

            case StateId::ATTACK_TURN: {
                // E 按下
                if (!impl_->turn_e_pressed && now >= impl_->turn_e_press_ms) {
                    impl_->turn_e_pressed = true;
                }
                // E 释放
                if (impl_->turn_e_pressed && now >= impl_->turn_e_release_ms) {
                    impl_->turn_e_pressed = false;
                    impl_->last_attack_release_ms = now;
                }
                // 方向键释放
                if (impl_->turn_key_pressed && now >= impl_->turn_dir_release_ms) {
                    impl_->turn_key_pressed = false;
                }
                // 两者都完成 → RECOVERY
                if (!impl_->turn_key_pressed && !impl_->turn_e_pressed) {
                    impl_->state = StateId::RECOVERY;
                    impl_->recovery_start_ms = now;
                }
                break;
            }

            case StateId::RECOVERY:
                if (!impl_->me.valid || !impl_->target.has) {
                    impl_->pending_chase = false;
                    impl_->state = StateId::IDLE;
                }
                else if (now - impl_->recovery_start_ms >= ScriptConfig::kRecoveryMs) {
                    if (!impl_->pending_chase) {
                        impl_->pending_chase = true;
                        impl_->pending_chase_start_ms = now;
                        {
                            std::uniform_int_distribution<int> dist(
                                impl_->cfg.recovery_chase_min_ms, impl_->cfg.recovery_chase_max_ms);
                            impl_->pending_chase_delay_ms = dist(impl_->rng);
                        }
                    }
                    if (now - impl_->pending_chase_start_ms >=
                        static_cast<uint64_t>(impl_->pending_chase_delay_ms)) {
                        impl_->pending_chase = false;
                        impl_->state = StateId::CHASE;
                    }
                }
                break;
        }
    } // End of else block (me.valid && target.has)

    // 决定按键
    int desired_dir = 0;
    bool desired_e = false;
    const float dx_target = impl_->target.cx - impl_->me.fx;
    const int target_dir = (dx_target > 0) ? 1 : -1;

    if (impl_->state == StateId::IDLE) {
        impl_->dir_press_start_ms = 0;
        impl_->dir_min_hold_ms = 0;
        impl_->active_dir_key = 0;
    }

    switch (impl_->state) {
        case StateId::IDLE:
            desired_dir = 0;
            desired_e = false;
            break;

        case StateId::CHASE: {
            // 带内不追、带外追——用 cls 缓存判定，无中间地带。
            const bool need_move = !impl_->cls.in_band_any;

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
            desired_dir = impl_->turn_key_pressed ? impl_->turn_dir_key : 0;
            desired_e = impl_->turn_e_pressed;
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

    if (impl_->state != StateId::IDLE && impl_->active_dir_key != 0) {
        const bool want_change = (desired_dir != impl_->active_dir_key);
        if (want_change) {
            const bool held_long_enough =
                (now - impl_->dir_press_start_ms) >= impl_->dir_min_hold_ms;
            // 明确反向才触发（±0.02 阈值），避免 dx_target 在 0 附近抖动
            const bool target_behind =
                (impl_->facing > 0 && dx_target < -0.02f) ||
                (impl_->facing < 0 && dx_target >  0.02f);
            if (!held_long_enough && !target_behind) {
                desired_dir = impl_->active_dir_key;
            }
        }
    }

    // 进带 → 强制松方向键（不受最短按住约束，这是"到达"不是"抖动"）
    if (impl_->state == StateId::CHASE) {
        if (impl_->cls.in_band_any) {
            desired_dir = 0;
        }
    }

    // 强制松方向键：进 ATTACK 后到达随机松手时刻 → 无论最短按住是否满足都松
    if (impl_->pending_dir_release_ms != 0 && now >= impl_->pending_dir_release_ms) {
        desired_dir = 0;
        impl_->pending_dir_release_ms = 0;
    }

    // 更新计时：按键变化时重置或清空
    if (desired_dir != impl_->active_dir_key) {
        if (desired_dir != 0) {
            impl_->dir_press_start_ms = now;
            // 方向键最短按住：从真人方向键低分位表采样，消除碎步
            const int64_t hold_us = SampleFromProfile(kDirHoldProfile, kDirHoldProfileSize, impl_->rng);
            impl_->dir_min_hold_ms = static_cast<uint64_t>(hold_us / 1000);
        } else {
            impl_->dir_press_start_ms = 0;
            impl_->dir_min_hold_ms = 0;
        }
    }

    // facing 从 desired_dir 反推
    const int old_facing = impl_->facing;
    if (desired_dir == 0x27) {
        impl_->facing = 1;
    } else if (desired_dir == 0x25) {
        impl_->facing = -1;
    }
    if (impl_->facing != old_facing) {
        impl_->last_dir_ms = now;
    }

    impl_->active_dir_key = desired_dir;

    impl_->active_key = desired_dir;
    impl_->desired_e = desired_e;

    if (world.frame_index - impl_->last_log_frame >= 30) {
        impl_->last_log_frame = world.frame_index;
        const char* state_str = "IDLE";
        switch (impl_->state) {
            case StateId::IDLE: state_str = "IDLE"; break;
            case StateId::CHASE: state_str = "CHASE"; break;
            case StateId::ATTACK: state_str = "ATTACK"; break;
            case StateId::ATTACK_TURN: state_str = "ATTACK_TURN"; break;
            case StateId::RECOVERY: state_str = "RECOVERY"; break;
        }
        std::printf("[script] frame=%llu state=%s facing=%d me_locked=%d me=(%.3f,%.3f) target_locked=%d target_cx=%.3f key=0x%02X e=%d\n",
                    (unsigned long long)world.frame_index,
                    state_str,
                    impl_->facing,
                    impl_->me.locked ? 1 : 0,
                    impl_->me.fx, impl_->me.fy,
                    impl_->target.locked ? 1 : 0,
                    impl_->target.cx,
                    impl_->active_key,
                    desired_e ? 1 : 0);
        std::fflush(stdout);
    }
} // End of OnFrame

void CppScript::GetDecision(core_decision* out) {
    out->out_count = 0;

    // 新语义：输出"当前希望按住的键"（只输出 press，不输出 release）。
    // 释放由 OutputManager 对比上一帧意图自动产生。
    if (impl_->active_key != 0 && out->out_count < CORE_DECISION_CAPACITY) {
        out->actions[out->out_count].kind = CORE_ACTION_KEY;
        out->actions[out->out_count].a = impl_->active_key;
        out->actions[out->out_count].b = 1;
        out->actions[out->out_count].c = 0;
        out->out_count++;
    }
    if (impl_->desired_e && out->out_count < CORE_DECISION_CAPACITY) {
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
    impl_->inited = false;
} // End of Shutdown

void CppScript::GetDebugInfo(CppScriptDebugInfo* out) const {
    if (out == nullptr) return;
    out->state = static_cast<int>(impl_->state);
    out->facing = impl_->facing;
    out->me_locked = impl_->me.locked;
    out->me_fx = impl_->me.fx;
    out->me_fy = impl_->me.fy;
    out->target_locked = impl_->target.locked;
    out->target_cx = impl_->target.cx;
    out->active_key = impl_->active_key;
    out->desired_e = impl_->desired_e;
}

bool CppScript::GetMeLock(float* fx, float* fy) const {
    if (!impl_->me.valid) return false;
    if (fx) *fx = impl_->me.fx;
    if (fy) *fy = impl_->me.fy;
    return true;
}
