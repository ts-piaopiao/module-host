#include "decider.h"

#include <cmath>
#include <cstdio>
#include <windows.h>

struct Decider::Impl {
    int lock_id = -1;
    float lock_fx = 0.5f;
    float lock_fy = 0.72f;
    unsigned long long lost_since = 0;   // GetTickCount64()

    static constexpr float kLockRange = 0.05f;
    static constexpr float kPriorX = 0.5f;
    static constexpr float kPriorY = 0.72f;
    static constexpr unsigned long long kRelockMs = 3000;
};

Decider::Decider() : impl_(new Impl()) {}

Decider::~Decider() {
    delete impl_;
    impl_ = nullptr;
}

void Decider::Update(const core_detections* dets, core_decision* out) {
    if (out == nullptr) return;
    out->out_count = 0;

    if (dets == nullptr) return;

    // 收集所有 cls=0 的候选
    struct Cand { int track_id; float fx; float fy; };
    Cand cands[CORE_MAX_DETECTIONS];
    uint32_t cand_count = 0;
    for (uint32_t i = 0; i < dets->count; ++i) {
        const auto& d = dets->items[i];
        if (d.cls != 0) continue;
        if (cand_count >= CORE_MAX_DETECTIONS) break;
        Cand c;
        c.track_id = d.track_id;
        c.fx = d.cx;
        c.fy = d.cy + d.h * 0.5f;
        cands[cand_count++] = c;
    }

    const unsigned long long now = GetTickCount64();

    // 无锁：选距先验最近
    if (impl_->lock_id < 0) {
        if (cand_count == 0) return;
        float best_dist2 = 1e9f;
        int best_tid = -1;
        float best_fx = 0, best_fy = 0;
        for (uint32_t i = 0; i < cand_count; ++i) {
            const float dx = cands[i].fx - Impl::kPriorX;
            const float dy = cands[i].fy - Impl::kPriorY;
            const float d2 = dx*dx + dy*dy;
            if (d2 < best_dist2) {
                best_dist2 = d2;
                best_tid = cands[i].track_id;
                best_fx = cands[i].fx;
                best_fy = cands[i].fy;
            }
        }
        if (best_tid >= 0) {
            impl_->lock_id = best_tid;
            impl_->lock_fx = best_fx;
            impl_->lock_fy = best_fy;
            impl_->lost_since = 0;
            std::fprintf(stdout,
                         "[decider] me_lock: id=%d fx=%.3f fy=%.3f\n",
                         best_tid, best_fx, best_fy);
        }
        return;
    }

    // 有锁：找同 track_id 的候选
    const Cand* matched = nullptr;
    for (uint32_t i = 0; i < cand_count; ++i) {
        if (cands[i].track_id == impl_->lock_id) {
            matched = &cands[i];
            break;
        }
    }

    if (matched != nullptr) {
        const float dx = matched->fx - impl_->lock_fx;
        const float dy = matched->fy - impl_->lock_fy;
        const float dist = std::sqrt(dx*dx + dy*dy);
        if (dist <= Impl::kLockRange) {
            impl_->lock_fx = matched->fx;
            impl_->lock_fy = matched->fy;
            impl_->lost_since = 0;
            std::fprintf(stdout,
                         "[decider] me_lock: id=%d fx=%.3f fy=%.3f\n",
                         impl_->lock_id, impl_->lock_fx, impl_->lock_fy);
            return;
        }
    }

    // 未匹配 / 圈外：保护期内保持
    if (impl_->lost_since == 0) {
        impl_->lost_since = now;
    }
    const unsigned long long elapsed = now - impl_->lost_since;
    if (elapsed >= Impl::kRelockMs) {
        // 保护期超时：清锁，下次无锁逻辑重选
        impl_->lock_id = -1;
        impl_->lost_since = 0;
        std::fprintf(stdout, "[decider] me_lock: lost, relock next frame\n");
    } else {
        std::fprintf(stdout,
                     "[decider] me_lock: id=%d held (lost %llu ms)\n",
                     impl_->lock_id, elapsed);
    }
    // out 保持空
}
