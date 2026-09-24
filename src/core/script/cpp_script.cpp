#include "cpp_script.h"

#include <cmath>
#include <cstdio>

namespace {

constexpr float kPriorX = 0.5f;
constexpr float kPriorY = 0.72f;
constexpr float kMeLockRange = 0.05f;
constexpr uint64_t kMeRelockMs = 1000;
constexpr float kTargetMatchX = 0.025f;
constexpr float kTargetMatchY = 0.037f;
constexpr uint64_t kTargetLoseMs = 500;

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

    struct Cand { float cx, cy; };
    Cand cands[CORE_MAX_DETECTIONS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < world.dets->count; ++i) {
        const auto& d = world.dets->items[i];
        if (d.cls != 1) continue;
        if (n >= CORE_MAX_DETECTIONS) break;
        cands[n].cx = d.cx;
        cands[n].cy = d.cy;
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
        t->lost_since = 0;
        t->has = true;
    }
}

}  // namespace

struct CppScript::Impl {
    uint64_t last_log_frame = 0;
    bool inited = false;

    enum class State { IDLE, CHASE };
    State state = State::IDLE;

    MeLockState me;
    TargetLockState target;

    int active_key = 0;
    int last_pressed = 0;
};

CppScript::CppScript() : impl_(new Impl()) {}
CppScript::~CppScript() { delete impl_; }

bool CppScript::Init(const std::string& config) {
    (void)config;
    std::printf("[script] 脚本已启动\n");
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

    if (!impl_->me.valid || !impl_->target.has) {
        impl_->state = Impl::State::IDLE;
    } else {
        impl_->state = Impl::State::CHASE;
    }

    if (impl_->state == Impl::State::IDLE) {
        impl_->active_key = 0;
    } else {
        const float dx = impl_->target.cx - impl_->me.fx;
        if (std::fabs(dx) < 0.02f) {
            impl_->active_key = 0;
        } else {
            impl_->active_key = (dx > 0) ? 0x27 : 0x25;
        }
    }

    if (world.frame_index - impl_->last_log_frame >= 30) {
        impl_->last_log_frame = world.frame_index;
        const char* state_str = (impl_->state == Impl::State::IDLE) ? "IDLE" : "CHASE";
        std::printf("[script] frame=%llu state=%s me_locked=%d me=(%.3f,%.3f) target_locked=%d target_cx=%.3f key=0x%02X\n",
                    (unsigned long long)world.frame_index,
                    state_str,
                    impl_->me.locked ? 1 : 0,
                    impl_->me.fx, impl_->me.fy,
                    impl_->target.locked ? 1 : 0,
                    impl_->target.cx,
                    impl_->active_key);
        std::fflush(stdout);
    }
}

void CppScript::GetDecision(core_decision* out) {
    out->out_count = 0;

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
}

void CppScript::Shutdown() {
    std::printf("[script] 脚本已停止\n");
    std::fflush(stdout);
    impl_->inited = false;
}
