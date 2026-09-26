#include "cpp_script.h"

#include "human_profile.h"

#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>

namespace {

constexpr float kPriorX = 0.5f;
constexpr float kPriorY = 0.72f;
constexpr float kMeLockRange = 0.05f;
constexpr uint64_t kMeRelockMs = 1000;
constexpr float kTargetMatchX = 0.025f;
constexpr float kTargetMatchY = 0.037f;
constexpr uint64_t kTargetLoseMs = 500;
constexpr float kSamePlatY = 0.028f;
constexpr float kBandXMin = 0.010f;
// 冰冻术硬范围 300px，缩 20px 留容错 → 280px，归一化 280/1920 = 0.1458
constexpr float kBandXMaxSame = 0.1458f;
constexpr float kBandXMaxCross = 0.104f;
constexpr float kBandYMin = -0.074f;
constexpr float kBandYMax = 0.019f;

struct MeLockState {
    bool valid = false;
    float fx = 0.0f;
    float fy = 0.0f;
    bool locked = false;
    float lock_fx = 0.0f;
    float lock_fy = 0.0f;
    uint64_t lost_since = 0;
};

struct TargetLockState {
    bool has = false;
    float cx = 0.0f;
    float cy = 0.0f;
    float fy = 0.0f;
    float h = 0.0f;
    float w = 0.0f;
    bool locked = false;
    float lock_cx = 0.0f;
    float lock_cy = 0.0f;
    uint64_t lost_since = 0;
};

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
            const float dx = cands[i].fx - kPriorX;
            const float dy = cands[i].fy - kPriorY;
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
    float best_d2 = kMeLockRange * kMeLockRange;
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
    if (elapsed < kMeRelockMs) {
        m->valid = true;
    } else {
        m->locked = false;
        m->lost_since = 0;
        m->valid = false;
    }
}

void SelectTarget(const ScriptWorld& world, const MeLockState& me,
                  TargetLockState* t) {
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
        if (std::fabs(d_fy) > kSamePlatY) continue;
        if (n >= CORE_MAX_DETECTIONS) break;
        cands[n].cx = d.cx;
        cands[n].cy = d.cy;
        cands[n].fy = d.cy + d.h * 0.5f;
        cands[n].h = d.h;
        cands[n].w = d.w;
        n++;
    }

    if (t->locked) {
        int matched = -1;
        float best_d2 = 1e9f;
        for (uint32_t i = 0; i < n; ++i) {
            const float dx = std::fabs(cands[i].cx - t->lock_cx);
            const float dy = std::fabs(cands[i].cy - t->lock_cy);
            if (dx > kTargetMatchX || dy > kTargetMatchY) continue;
            const float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best_d2 = d2; matched = (int)i; }
        }
        if (matched >= 0) {
            t->lock_cx = cands[matched].cx;
            t->lock_cy = cands[matched].cy;
            t->cx = cands[matched].cx;
            t->cy = cands[matched].cy;
            t->fy = cands[matched].fy;
            t->h = cands[matched].h;
            t->w = cands[matched].w;
            t->lost_since = 0;
            t->has = true;
            return;
        }
        if (t->lost_since == 0) {
            t->lost_since = now;
        }
        if (now - t->lost_since < kTargetLoseMs) {
            t->has = true;
            return;
        }
        t->locked = false;
        t->lost_since = 0;
    }

    if (n == 0) {
        t->has = false;
        return;
    }
    float best_d = 1e9f;
    int best = -1;
    for (uint32_t i = 0; i < n; ++i) {
        const float d = std::fabs(cands[i].cx - me.lock_fx);
        if (d < best_d) { best_d = d; best = (int)i; }
    }
    if (best >= 0) {
        t->locked = true;
        t->lock_cx = cands[best].cx;
        t->lock_cy = cands[best].cy;
        t->cx = cands[best].cx;
        t->cy = cands[best].cy;
        t->fy = cands[best].fy;
        t->h = cands[best].h;
        t->w = cands[best].w;
        t->lost_since = 0;
        t->has = true;
    }
}

bool IsInBand(const TargetLockState& t, const MeLockState& me, int facing) {
    if (!t.has || !me.valid) return false;
    // 攻击区：椭圆扇段（X 前方 19px~300px 死角+弧线；Y 上方 80px / 下方 10px）
    const float t_left = t.cx - t.w * 0.5f;
    const float t_right = t.cx + t.w * 0.5f;
    float x_near, x_far;
    if (facing > 0) {
        x_near = t_left - me.fx;
        x_far  = t_right - me.fx;
    } else {
        x_near = me.fx - t_right;
        x_far  = me.fx - t_left;
    }
    if (x_far < kBandXMin) return false;
    if (x_near > kBandXMaxSame) return false;

    // Y 轴：怪物 bbox 与 [me.fy + kBandYMin, me.fy + kBandYMax] 相交
    const float t_top = t.cy - t.h * 0.5f;
    const float t_bot = t.cy + t.h * 0.5f;
    const float band_top = me.fy + kBandYMin;
    const float band_bot = me.fy + kBandYMax;
    if (t_bot < band_top || t_top > band_bot) return false;

    // 椭圆约束：X 最远随 Y 高度衰减
    // 用 bbox 与 band 相交区间的中点作为"代表高度"
    const float y_clip_top = (t_top > band_top) ? t_top : band_top;
    const float y_clip_bot = (t_bot < band_bot) ? t_bot : band_bot;
    const float y_rep = (y_clip_top + y_clip_bot) * 0.5f - me.fy;
    const float y_half = (y_rep < 0.0f) ? (-kBandYMin) : kBandYMax;
    if (y_half <= 0.0f) return false;
    const float ny = y_rep / y_half;
    const float scale = 1.0f - ny * ny;
    if (scale <= 0.0f) return false;
    const float x_max_at_y = kBandXMaxSame * std::sqrt(scale);

    if (x_far < kBandXMin) return false;
    if (x_near > x_max_at_y) return false;
    return true;
}

bool IsInBand(float me_fx, float me_fy, float t_cx, float t_cy,
              float t_h, float t_w, int facing) {
    const float t_left = t_cx - t_w * 0.5f;
    const float t_right = t_cx + t_w * 0.5f;
    float x_near, x_far;
    if (facing > 0) {
        x_near = t_left - me_fx;
        x_far  = t_right - me_fx;
    } else {
        x_near = me_fx - t_right;
        x_far  = me_fx - t_left;
    }
    if (x_far < kBandXMin) return false;
    if (x_near > kBandXMaxSame) return false;

    const float t_top = t_cy - t_h * 0.5f;
    const float t_bot = t_cy + t_h * 0.5f;
    const float band_top = me_fy + kBandYMin;
    const float band_bot = me_fy + kBandYMax;
    if (t_bot < band_top || t_top > band_bot) return false;

    const float y_clip_top = (t_top > band_top) ? t_top : band_top;
    const float y_clip_bot = (t_bot < band_bot) ? t_bot : band_bot;
    const float y_rep = (y_clip_top + y_clip_bot) * 0.5f - me_fy;
    const float y_half = (y_rep < 0.0f) ? (-kBandYMin) : kBandYMax;
    if (y_half <= 0.0f) return false;
    const float ny = y_rep / y_half;
    const float scale = 1.0f - ny * ny;
    if (scale <= 0.0f) return false;
    const float x_max_at_y = kBandXMaxSame * std::sqrt(scale);

    if (x_far < kBandXMin) return false;
    if (x_near > x_max_at_y) return false;
    return true;
}

}  // namespace

struct CombatConfig {
    // CHASE → ATTACK 延迟：脚本快速反应（游戏机制，不是真人犹豫）
    int attack_react_min_ms = 40;
    int attack_react_max_ms = 70;

    // RECOVERY → CHASE 延迟：脚本快速反应
    int recovery_chase_min_ms = 30;
    int recovery_chase_max_ms = 60;
};

struct CppScript::Impl {
    // RECOVERY 时长（攻击后回到 CHASE 的过渡）。技能冷却不再由它承担。
    static constexpr uint64_t kRecoveryMs = 300;
    // 技能冷却硬下限：冰冻术 810ms。CHASE 进 ATTACK 前检查。
    static constexpr uint64_t kSkillCooldownMs = 810;
    static constexpr int kTurnPressDelayMinMs = 100;
    static constexpr int kTurnPressDelayMaxMs = 200;
    static constexpr int kTurnKeyReleaseDelayMs = 100;
    static constexpr uint64_t kTurnBounceMs = 200;

    uint64_t last_log_frame = 0;
    bool inited = false;

    enum class State { IDLE, CHASE, ATTACK, ATTACK_TURN, RECOVERY };
    State state = State::IDLE;

    MeLockState me;
    TargetLockState target;

    uint64_t attack_start_ms = 0;
    uint64_t recovery_start_ms = 0;
    uint64_t last_attack_release_ms = 0;   // 上次 E 释放时刻；用于独立技能冷却检查
    bool e_pressed = false;
    int current_e_tap_ms = 150;   // 初始值；实际每次攻击前重新采样

    // 朝向
    int facing = 1;
    uint64_t last_dir_ms = 0;
    uint64_t dir_press_start_ms = 0;   // 当前方向键按住起点；0 = 未按住
    uint64_t dir_min_hold_ms = 0;      // 本次按住目标时长；按开始时从 E 分位表采样
    int active_dir_key = 0;   // 当前实际按住的方向键（0 / 0x25 / 0x27）

    // 转身攻击
    int turn_phase = 0;
    uint64_t turn_phase_start_ms = 0;
    int turn_dir_key = 0;
    int turn_press_delay_ms = 0;
    int turn_e_tap_ms = 0;
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

    CombatConfig cfg;

    std::mt19937 rng;

    int active_key = 0;
    bool desired_e = false;
};

static void Trim(const std::string& s, std::string* out) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r' || s[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    *out = s.substr(begin, end - begin);
}

static bool ParseInt(const std::string& text, int* out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return false;
    *out = static_cast<int>(value);
    return true;
}

static void NormalizeConfig(CombatConfig* cfg) {
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
    const auto now_ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    impl_->rng.seed(static_cast<uint32_t>(std::time(nullptr)) ^
                    static_cast<uint32_t>(now_ticks));

    impl_->cfg = CombatConfig{};

    if (!config.empty()) {
        size_t pos = 0;
        const std::string content(config);
        while (pos <= content.size()) {
            size_t nl = content.find('\n', pos);
            if (nl == std::string::npos) nl = content.size();
            std::string line = content.substr(pos, nl - pos);
            pos = nl + 1;

            std::string trimmed;
            Trim(line, &trimmed);
            if (trimmed.empty() || trimmed[0] == '#') continue;

            const size_t comment = trimmed.find('#');
            if (comment != std::string::npos) {
                trimmed = trimmed.substr(0, comment);
                Trim(trimmed, &trimmed);
                if (trimmed.empty()) continue;
            }

            const size_t eq = trimmed.find('=');
            if (eq == std::string::npos) continue;

            std::string key = trimmed.substr(0, eq);
            std::string value = trimmed.substr(eq + 1);
            Trim(key, &key);
            Trim(value, &value);

            int num = 0;
            if (!ParseInt(value, &num)) continue;

            if (key == "combat_attack_react_min_ms") {
                impl_->cfg.attack_react_min_ms = num;
            } else if (key == "combat_attack_react_max_ms") {
                impl_->cfg.attack_react_max_ms = num;
            } else if (key == "combat_recovery_chase_min_ms") {
                impl_->cfg.recovery_chase_min_ms = num;
            } else if (key == "combat_recovery_chase_max_ms") {
                impl_->cfg.recovery_chase_max_ms = num;
            }
            // 未知键忽略
        }
    }

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
        SelectTarget(world, impl_->me, &impl_->target);
    } else {
        impl_->target.has = false;
    }

    const uint64_t now = world.now_ms;

    if (!impl_->me.valid || !impl_->target.has) {
        if (impl_->state != Impl::State::IDLE) {
            impl_->state = Impl::State::IDLE;
            impl_->e_pressed = false;
            impl_->turn_key_pressed = false;
            impl_->turn_e_pressed = false;
            impl_->pending_attack = false;
            impl_->pending_chase = false;
        }
    } else {
        const bool in_band = IsInBand(impl_->target, impl_->me, impl_->facing);
        const float dx_target = impl_->target.cx - impl_->me.fx;
        const int target_dir = (dx_target > 0) ? 1 : -1;
        const bool is_front = (target_dir == impl_->facing);
        const bool fresh_target = (impl_->target.lost_since == 0);

        switch (impl_->state) {
            case Impl::State::IDLE:
                if (impl_->me.valid && impl_->target.has) {
                    impl_->state = Impl::State::CHASE;
                }
                break;

            case Impl::State::CHASE:
                if (!impl_->me.valid || !impl_->target.has) {
                    impl_->pending_attack = false;
                    impl_->state = Impl::State::IDLE;
                }
                // 已在 pending：检查延迟
                else if (impl_->pending_attack) {
                    if (now - impl_->pending_attack_start_ms >=
                        static_cast<uint64_t>(impl_->pending_attack_delay_ms)) {
                        impl_->pending_attack = false;
                        // 延迟到点，重新检查
                        if (in_band && is_front && fresh_target) {
                            impl_->state = Impl::State::ATTACK;
                            impl_->attack_start_ms = now;
                            impl_->e_pressed = true;
                            impl_->current_e_tap_ms = SampleEHoldMs(impl_->rng);
                            std::printf("[script] E 按下时长: %d ms\n",
                                        impl_->current_e_tap_ms);
                            std::fflush(stdout);
                        }
                    }
                }
                // 背面进带：S7 逻辑（转身）
                else if (in_band && !is_front && fresh_target) {
                    if (now - impl_->last_dir_ms >= Impl::kTurnBounceMs) {
                        impl_->state = Impl::State::ATTACK_TURN;
                        impl_->turn_phase = 0;
                        impl_->turn_phase_start_ms = now;
                        impl_->turn_dir_key = (target_dir > 0) ? 0x27 : 0x25;
                        impl_->facing = target_dir;                // 新增：与 turn_dir_key 同帧同步 facing
                        impl_->last_dir_ms = now;                  // 新增：与 CHASE 内转向一致，重置冷却计时
                        {
                            std::uniform_int_distribution<int> dist(
                                Impl::kTurnPressDelayMinMs, Impl::kTurnPressDelayMaxMs);
                            impl_->turn_press_delay_ms = dist(impl_->rng);
                        }
                        impl_->turn_e_tap_ms = SampleEHoldMs(impl_->rng);
                        impl_->turn_key_pressed = false;
                        impl_->turn_e_pressed = false;
                    }
                }
                // 正面进带：设 pending (S8)；先检查技能冷却
                else if (in_band && is_front && fresh_target) {
                    // 技能冷却硬下限检查：上次 E release 到"实际按 E"（now + pending_delay）
                    // 必须 ≥ 810ms。用 pending_delay 下限做保守检查。
                    const bool cooldown_ok =
                        (impl_->last_attack_release_ms == 0) ||
                        ((now - impl_->last_attack_release_ms) +
                            static_cast<uint64_t>(impl_->cfg.attack_react_min_ms)
                                >= Impl::kSkillCooldownMs);
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

            case Impl::State::ATTACK: {
                if (!impl_->me.valid || !impl_->target.has) {
                    impl_->state = Impl::State::IDLE;
                    impl_->e_pressed = false;
                    break;
                }

                if (now - impl_->attack_start_ms >=
                    static_cast<uint64_t>(impl_->current_e_tap_ms)) {
                    impl_->e_pressed = false;
                    impl_->last_attack_release_ms = now;
                    impl_->state = Impl::State::RECOVERY;
                    impl_->recovery_start_ms = now;
                }
                break;
            }

            case Impl::State::ATTACK_TURN: {
                if (impl_->turn_phase == 0) {
                    if (!impl_->turn_key_pressed) {
                        impl_->turn_key_pressed = true;
                        impl_->facing = target_dir;
                        impl_->last_dir_ms = now;
                    }
                    if (now - impl_->turn_phase_start_ms >=
                        static_cast<uint64_t>(impl_->turn_press_delay_ms)) {
                        impl_->turn_phase = 1;
                        impl_->turn_phase_start_ms = now;
                    }
                } else if (impl_->turn_phase == 1) {
                    if (!impl_->turn_e_pressed) {
                        impl_->turn_e_pressed = true;
                    }
                    if (now - impl_->turn_phase_start_ms >=
                        static_cast<uint64_t>(impl_->turn_e_tap_ms)) {
                        impl_->turn_phase = 2;
                        impl_->turn_phase_start_ms = now;
                    }
                } else if (impl_->turn_phase == 2) {
                    if (now - impl_->turn_phase_start_ms >=
                        Impl::kTurnKeyReleaseDelayMs) {
                        impl_->turn_phase = 3;
                    }
                } else if (impl_->turn_phase == 3) {
                    impl_->turn_key_pressed = false;
                    impl_->turn_e_pressed = false;
                    impl_->last_attack_release_ms = now;
                    impl_->state = Impl::State::RECOVERY;
                    impl_->recovery_start_ms = now;
                }
                break;
            }

            case Impl::State::RECOVERY:
                if (!impl_->me.valid || !impl_->target.has) {
                    impl_->pending_chase = false;
                    impl_->state = Impl::State::IDLE;
                }
                else if (now - impl_->recovery_start_ms >= Impl::kRecoveryMs) {
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
                        impl_->state = Impl::State::CHASE;
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

    if (impl_->state == Impl::State::IDLE) {
        impl_->dir_press_start_ms = 0;
        impl_->dir_min_hold_ms = 0;
        impl_->active_dir_key = 0;
    }

    switch (impl_->state) {
        case Impl::State::IDLE:
            desired_dir = 0;
            desired_e = false;
            break;

        case Impl::State::CHASE: {
            const float abs_dx = std::fabs(dx_target);
            const bool need_move = (abs_dx >= 0.02f);
            const int want_dir = need_move ? ((dx_target > 0) ? 0x27 : 0x25) : 0;
            desired_dir = want_dir;
            desired_e = false;
            break;
        }

        case Impl::State::ATTACK:
            desired_dir = 0;
            desired_e = true;
            break;

        case Impl::State::ATTACK_TURN: {
            if (impl_->turn_phase <= 2) {
                desired_dir = impl_->turn_dir_key;
            } else {
                desired_dir = 0;
            }
            desired_e = (impl_->turn_phase == 1 || impl_->turn_phase == 2);
            break;
        }

        case Impl::State::RECOVERY:
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

    if (impl_->state != Impl::State::IDLE && impl_->active_dir_key != 0) {
        const bool want_change = (desired_dir != impl_->active_dir_key);
        if (want_change) {
            const bool held_long_enough =
                (now - impl_->dir_press_start_ms) >= impl_->dir_min_hold_ms;
            const bool target_behind =
                (impl_->facing > 0 && dx_target < 0) ||
                (impl_->facing < 0 && dx_target > 0);
            if (!held_long_enough && !target_behind) {
                desired_dir = impl_->active_dir_key;
            }
        }
    }

    // 更新计时：按键变化时重置或清空
    if (desired_dir != impl_->active_dir_key) {
        if (desired_dir != 0) {
            impl_->dir_press_start_ms = now;
            // 方向键最短按住：与 E 键同源（真人分位表采样，微秒转毫秒）
            const int64_t hold_us = SampleFromProfile(kEHoldProfile, kEHoldProfileSize, impl_->rng);
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
            case Impl::State::IDLE: state_str = "IDLE"; break;
            case Impl::State::CHASE: state_str = "CHASE"; break;
            case Impl::State::ATTACK: state_str = "ATTACK"; break;
            case Impl::State::ATTACK_TURN: state_str = "ATTACK_TURN"; break;
            case Impl::State::RECOVERY: state_str = "RECOVERY"; break;
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
