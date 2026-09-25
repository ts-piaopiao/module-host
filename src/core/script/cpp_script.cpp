#include "cpp_script.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

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
constexpr float kBandXMaxSame = 0.135f;
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

    struct Cand { float cx, cy, fy; };
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
        t->lost_since = 0;
        t->has = true;
    }
}

bool IsInBand(const TargetLockState& t, const MeLockState& me) {
    if (!t.has || !me.valid) return false;
    const float dx = std::fabs(t.cx - me.fx);
    const float dy = t.cy - me.fy;
    if (dy < kBandYMin || dy > kBandYMax) return false;
    const float x_max = (std::fabs(t.fy - me.fy) < kSamePlatY) ? kBandXMaxSame
                                                                : kBandXMaxCross;
    return dx >= kBandXMin && dx <= x_max;
}

bool IsInBand(float me_fx, float me_fy, float t_cx, float t_cy, float t_h) {
    const float t_fy = t_cy + t_h * 0.5f;
    const float dx = std::fabs(t_cx - me_fx);
    const float dy = t_cy - me_fy;
    if (dy < kBandYMin || dy > kBandYMax) return false;
    const float x_max = (std::fabs(t_fy - me_fy) < kSamePlatY) ? kBandXMaxSame
                                                                : kBandXMaxCross;
    return dx >= kBandXMin && dx <= x_max;
}

}  // namespace

struct CombatConfig {
    // E 时长（偏态）
    int e_common_min_ms = 160;
    int e_common_max_ms = 190;
    int e_common_prob = 70;
    int e_rare_lo_min_ms = 120;
    int e_rare_lo_max_ms = 160;
    int e_rare_hi_min_ms = 190;
    int e_rare_hi_max_ms = 300;

    // CHASE → ATTACK 延迟
    int attack_react_min_ms = 80;
    int attack_react_max_ms = 200;

    // RECOVERY → CHASE 延迟
    int recovery_chase_min_ms = 30;
    int recovery_chase_max_ms = 100;
};

struct CppScript::Impl {
    static constexpr int kETapMsMin = 150;
    static constexpr int kETapMsMax = 300;
    static constexpr uint64_t kRecoveryMs = 850;
    static constexpr int kTurnPressDelayMinMs = 100;
    static constexpr int kTurnPressDelayMaxMs = 200;
    static constexpr int kTurnKeyReleaseDelayMs = 100;
    static constexpr uint64_t kTurnBounceMs = 200;
    static constexpr uint64_t kChaseTurnCooldownMs = 300;
    static constexpr float kNearFaceThreshold = 0.010f;
    static constexpr int kReverseDurationMinMs = 200;
    static constexpr int kReverseDurationMaxMs = 500;

    uint64_t last_log_frame = 0;
    bool inited = false;

    enum class State { IDLE, CHASE, ATTACK, ATTACK_TURN, RECOVERY };
    State state = State::IDLE;

    MeLockState me;
    TargetLockState target;

    uint64_t attack_start_ms = 0;
    uint64_t recovery_start_ms = 0;
    bool e_pressed = false;
    int current_e_tap_ms = kETapMsMin;

    // 朝向
    int facing = 1;
    uint64_t last_dir_ms = 0;

    // 转身攻击
    int turn_phase = 0;
    uint64_t turn_phase_start_ms = 0;
    int turn_dir_key = 0;
    int turn_press_delay_ms = 0;
    int turn_e_tap_ms = 0;
    bool turn_key_pressed = false;
    bool turn_e_pressed = false;

    // 贴脸后退
    bool reversing = false;
    uint64_t reverse_start_ms = 0;

    // 攻击延迟 (S8)
    bool pending_attack = false;
    uint64_t pending_attack_start_ms = 0;
    int pending_attack_delay_ms = 0;

    // RECOVERY 后延迟 (S8)
    bool pending_chase = false;
    uint64_t pending_chase_start_ms = 0;
    int pending_chase_delay_ms = 0;

    CombatConfig cfg;

    int active_key = 0;
    int last_pressed = 0;
    bool desired_e = false;
    bool last_desired_e = false;
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
    if (cfg->e_common_max_ms < cfg->e_common_min_ms) {
        cfg->e_common_max_ms = cfg->e_common_min_ms;
    }
    if (cfg->e_rare_lo_max_ms < cfg->e_rare_lo_min_ms) {
        cfg->e_rare_lo_max_ms = cfg->e_rare_lo_min_ms;
    }
    if (cfg->e_rare_hi_max_ms < cfg->e_rare_hi_min_ms) {
        cfg->e_rare_hi_max_ms = cfg->e_rare_hi_min_ms;
    }
    if (cfg->attack_react_max_ms < cfg->attack_react_min_ms) {
        cfg->attack_react_max_ms = cfg->attack_react_min_ms;
    }
    if (cfg->recovery_chase_max_ms < cfg->recovery_chase_min_ms) {
        cfg->recovery_chase_max_ms = cfg->recovery_chase_min_ms;
    }
    if (cfg->e_common_prob < 0) cfg->e_common_prob = 0;
    if (cfg->e_common_prob > 100) cfg->e_common_prob = 100;
}

static int SampleEHoldMs(const CombatConfig& cfg) {
    const int r = std::rand() % 100;
    if (r < cfg.e_common_prob) {
        const int span = cfg.e_common_max_ms - cfg.e_common_min_ms + 1;
        return cfg.e_common_min_ms + (std::rand() % span);
    }
    // rare：两段等分
    const bool lo = (std::rand() % 2) == 0;
    if (lo) {
        const int span = cfg.e_rare_lo_max_ms - cfg.e_rare_lo_min_ms + 1;
        return cfg.e_rare_lo_min_ms + (std::rand() % span);
    } else {
        const int span = cfg.e_rare_hi_max_ms - cfg.e_rare_hi_min_ms + 1;
        return cfg.e_rare_hi_min_ms + (std::rand() % span);
    }
}

CppScript::CppScript() : impl_(new Impl()) {}
CppScript::~CppScript() { delete impl_; }

bool CppScript::Init(const std::string& config) {
    std::srand(static_cast<unsigned>(std::time(nullptr)));

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

            if (key == "combat_e_common_min_ms") {
                impl_->cfg.e_common_min_ms = num;
            } else if (key == "combat_e_common_max_ms") {
                impl_->cfg.e_common_max_ms = num;
            } else if (key == "combat_e_common_prob") {
                impl_->cfg.e_common_prob = num;
            } else if (key == "combat_e_rare_lo_min_ms") {
                impl_->cfg.e_rare_lo_min_ms = num;
            } else if (key == "combat_e_rare_lo_max_ms") {
                impl_->cfg.e_rare_lo_max_ms = num;
            } else if (key == "combat_e_rare_hi_min_ms") {
                impl_->cfg.e_rare_hi_min_ms = num;
            } else if (key == "combat_e_rare_hi_max_ms") {
                impl_->cfg.e_rare_hi_max_ms = num;
            } else if (key == "combat_attack_react_min_ms") {
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

    std::printf("[script] 配置加载完成: e_common=[%d,%d]%%%d, e_rare_lo=[%d,%d], e_rare_hi=[%d,%d], react=[%d,%d], chase=[%d,%d]\n",
        impl_->cfg.e_common_min_ms, impl_->cfg.e_common_max_ms, impl_->cfg.e_common_prob,
        impl_->cfg.e_rare_lo_min_ms, impl_->cfg.e_rare_lo_max_ms,
        impl_->cfg.e_rare_hi_min_ms, impl_->cfg.e_rare_hi_max_ms,
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
            impl_->reversing = false;
            impl_->turn_key_pressed = false;
            impl_->turn_e_pressed = false;
            impl_->pending_attack = false;
            impl_->pending_chase = false;
        }
    } else {
        const bool in_band = IsInBand(impl_->target, impl_->me);
        const float dx_target = impl_->target.cx - impl_->me.fx;
        const int target_dir = (dx_target > 0) ? 1 : -1;
        const bool is_front = (target_dir == impl_->facing);
        const bool too_close = impl_->me.valid && impl_->target.has &&
                               (std::fabs(dx_target) < Impl::kNearFaceThreshold);
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
                    impl_->reversing = false;
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
                            impl_->current_e_tap_ms = SampleEHoldMs(impl_->cfg);
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
                        impl_->turn_press_delay_ms =
                            Impl::kTurnPressDelayMinMs +
                            (std::rand() % (Impl::kTurnPressDelayMaxMs - Impl::kTurnPressDelayMinMs + 1));
                        impl_->turn_e_tap_ms =
                            Impl::kETapMsMin +
                            (std::rand() % (Impl::kETapMsMax - Impl::kETapMsMin + 1));
                        impl_->turn_key_pressed = false;
                        impl_->turn_e_pressed = false;
                    }
                }
                // 正面进带：设 pending (S8)
                else if (in_band && is_front && fresh_target) {
                    impl_->pending_attack = true;
                    impl_->pending_attack_start_ms = now;
                    impl_->pending_attack_delay_ms = impl_->cfg.attack_react_min_ms +
                        (std::rand() % (impl_->cfg.attack_react_max_ms - impl_->cfg.attack_react_min_ms + 1));
                    std::printf("[script] 攻击反应延迟: %d ms\n",
                                impl_->pending_attack_delay_ms);
                    std::fflush(stdout);
                }
                // 贴脸后退：S7 逻辑
                else if (too_close) {
                    if (!impl_->reversing) {
                        impl_->reversing = true;
                        impl_->reverse_start_ms = now;
                    }
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
                        impl_->pending_chase_delay_ms = impl_->cfg.recovery_chase_min_ms +
                            (std::rand() % (impl_->cfg.recovery_chase_max_ms - impl_->cfg.recovery_chase_min_ms + 1));
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
    const bool too_close = impl_->me.valid && impl_->target.has &&
                           (std::fabs(dx_target) < Impl::kNearFaceThreshold);

    switch (impl_->state) {
        case Impl::State::IDLE:
            desired_dir = 0;
            desired_e = false;
            break;

        case Impl::State::CHASE: {
            if (too_close && impl_->reversing) {
                const int reverse_duration =
                    Impl::kReverseDurationMinMs +
                    (std::rand() % (Impl::kReverseDurationMaxMs - Impl::kReverseDurationMinMs + 1));
                if (now - impl_->reverse_start_ms <
                    static_cast<uint64_t>(reverse_duration)) {
                    desired_dir = (dx_target > 0) ? 0x25 : 0x27;
                } else {
                    impl_->reversing = false;
                }
            } else {
                const float abs_dx = std::fabs(dx_target);
                if (abs_dx < 0.02f) {
                    desired_dir = 0;
                } else {
                    const int want_dir = (dx_target > 0) ? 0x27 : 0x25;
                    const int want_facing = (dx_target > 0) ? 1 : -1;
                    if (want_facing != impl_->facing) {
                        if (now - impl_->last_dir_ms < Impl::kChaseTurnCooldownMs) {
                            desired_dir = (impl_->facing > 0) ? 0x27 : 0x25;
                        } else {
                            desired_dir = (dx_target > 0) ? 0x27 : 0x25;
                            impl_->facing = (dx_target > 0) ? 1 : -1;
                            impl_->last_dir_ms = now;
                        }
                    } else {
                        desired_dir = (dx_target > 0) ? 0x27 : 0x25;
                    }
                }
            }
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

    // 方向键差分
    if (impl_->active_key != impl_->last_pressed) {
        if (impl_->last_pressed != 0 && out->out_count < CORE_DECISION_CAPACITY) {
            out->actions[out->out_count].kind = CORE_ACTION_KEY;
            out->actions[out->out_count].a = impl_->last_pressed;
            out->actions[out->out_count].b = 0;
            out->actions[out->out_count].c = 0;
            out->out_count++;
        }
        if (impl_->active_key != 0 && out->out_count < CORE_DECISION_CAPACITY) {
            out->actions[out->out_count].kind = CORE_ACTION_KEY;
            out->actions[out->out_count].a = impl_->active_key;
            out->actions[out->out_count].b = 1;
            out->actions[out->out_count].c = 0;
            out->out_count++;
        }
        impl_->last_pressed = impl_->active_key;
    }

    // E键差分（简化：仅根据 desired_e 和 e_pressed 状态）
    if (impl_->desired_e != impl_->last_desired_e) {
        if (impl_->last_desired_e && out->out_count < CORE_DECISION_CAPACITY) {
            out->actions[out->out_count].kind = CORE_ACTION_KEY;
            out->actions[out->out_count].a = 0x45;
            out->actions[out->out_count].b = 0;
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
        impl_->last_desired_e = impl_->desired_e;
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
    out->reversing = impl_->reversing;
}
