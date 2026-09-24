#include "decider.h"

#include "combat.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <windows.h>

namespace {

// 临时测试开关：1 = 把 monster 也当作 me 候选（用于测试验证流程）
// 生产环境必须为 0
static const int kTestIncludeMonster = 0;

unsigned long long NowMs() {
    return GetTickCount64();
}

int RandPressDuration() {
    return 200 + (rand() % 301);  // 200-500ms
}

enum class VerifyState {
    IDLE, PRESS_RIGHT, WAIT_AFTER_RIGHT,
    PRESS_LEFT, WAIT_AFTER_LEFT,
    ANALYZE
};

struct VerifySample {
    core_detection items[CORE_MAX_DETECTIONS];
    uint32_t count = 0;
    unsigned long long time = 0;
};

// 从 detections 里拷贝所有 cls=0 到 sample
void CaptureSample(const core_detections* dets, VerifySample* s) {
    s->count = 0;
    s->time = NowMs();
    if (dets != nullptr) {
        for (uint32_t i = 0; i < dets->count; ++i) {
            const core_detection& d = dets->items[i];
            if (d.cls != 0 && !(kTestIncludeMonster && d.cls == 1)) continue;
            if (s->count >= CORE_MAX_DETECTIONS) break;
            s->items[s->count++] = d;
        }
    }
    std::fprintf(stdout, "[decider] sample @%llu ms: %u 候选:\n",
                 (unsigned long long)s->time, s->count);
    for (uint32_t i = 0; i < s->count; ++i) {
        std::fprintf(stdout, "  [%u] id=%d cx=%.3f cy=%.3f w=%.3f h=%.3f\n",
                     i, s->items[i].track_id,
                     s->items[i].cx, s->items[i].cy,
                     s->items[i].w, s->items[i].h);
    }
}

// 在 sample 里找离 (cx, cy) 最近且 < kMaxMatchDist 的候选索引
// 返回 -1 表示找不到
int FindNearest(const VerifySample& s, float cx, float cy, float max_dist) {
    int best = -1;
    float best_d2 = max_dist * max_dist;
    for (uint32_t i = 0; i < s.count; ++i) {
        const float dx = s.items[i].cx - cx;
        const float dy = s.items[i].cy - cy;
        const float d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = (int)i;
        }
    }
    return best;
}

// 分析：T0->T1 dx > thresh 且 T1->T2 dx < -thresh 的唯一胜出者
int AnalyzeMoves(const VerifySample& t0, const VerifySample& t1, const VerifySample& t2) {
    const float kDxThresh = 0.01f;
    const float kMaxMatchDist = 0.30f;
    int winner = -1;
    int winner_count = 0;
    for (uint32_t i = 0; i < t0.count; ++i) {
        const auto& d0 = t0.items[i];
        const int idx1 = FindNearest(t1, d0.cx, d0.cy, kMaxMatchDist);
        float dx1 = 0.0f;
        if (idx1 >= 0) {
            dx1 = t1.items[idx1].cx - d0.cx;
        }
        const int idx2 = (idx1 >= 0)
            ? FindNearest(t2, t1.items[idx1].cx, t1.items[idx1].cy, kMaxMatchDist)
            : -1;
        float dx2 = 0.0f;
        if (idx2 >= 0 && idx1 >= 0) {
            dx2 = t2.items[idx2].cx - t1.items[idx1].cx;
        }

        std::fprintf(stdout,
                     "[decider] analyze T0[%u] id=%d cx=%.3f -> T1 %s (idx=%d cx=%.3f dx1=%.3f) -> T2 %s (idx=%d cx=%.3f dx2=%.3f)\n",
                     i, d0.track_id, d0.cx,
                     idx1 < 0 ? "NO_MATCH" : "OK",
                     idx1, (idx1 >= 0 ? t1.items[idx1].cx : 0.0f), dx1,
                     idx2 < 0 ? "NO_MATCH" : "OK",
                     idx2, (idx2 >= 0 ? t2.items[idx2].cx : 0.0f), dx2);

        if (idx1 < 0 || idx2 < 0) continue;
        if (dx1 > kDxThresh && dx2 < -kDxThresh) {
            if (winner < 0) {
                winner = (int)i;
                winner_count = 1;
            } else {
                winner_count++;
            }
        }
    }
    std::fprintf(stdout, "[decider] analyze done: winner=%d count=%d thresh=%.3f match=%.3f\n",
                 winner, winner_count, kDxThresh, kMaxMatchDist);
    if (winner_count == 1) return winner;
    return -1;
}

}  // namespace

struct Decider::Impl {
    Combat combat;

    bool dry_run = false;

    int lock_id = -1;
    float lock_fx = 0.5f;
    float lock_fy = 0.72f;
    unsigned long long lost_since = 0;

    VerifyState verify_state = VerifyState::IDLE;
    unsigned long long verify_start = 0;
    unsigned long long press_duration = 200;
    int verify_retry = 0;
    unsigned long long verify_cooldown_until = 0;
    VerifySample sample_t0, sample_t1, sample_t2;

    static constexpr float kLockRange = 0.05f;
    static constexpr float kPriorX = 0.5f;
    static constexpr float kPriorY = 0.72f;
    static constexpr float kUniqueMaxDist = 0.5f;
    static constexpr unsigned long long kRelockMs = 3000;
    static constexpr float kDxThresh = 0.01f;
    static constexpr float kMaxMatchDist = 0.30f;
    static constexpr int kMaxRetry = 3;
    static constexpr unsigned long long kVerifyCooldownMs = 30000;
};

Decider::Decider() : impl_(new Impl()) {
    std::srand(static_cast<unsigned>(GetTickCount64()));
}

Decider::~Decider() {
    delete impl_;
    impl_ = nullptr;
}

bool Decider::GetMeLock(float* fx, float* fy) const {
    if (impl_->lock_id < 0) return false;
    *fx = impl_->lock_fx;
    *fy = impl_->lock_fy;
    return true;
}

void Decider::SetDryRun(bool dry) {
    impl_->dry_run = dry;
}

void Decider::Update(const core_detections* dets, core_decision* out) {
    if (out == nullptr) return;
    out->out_count = 0;
    if (dets == nullptr) return;

    const unsigned long long now = NowMs();

    auto finish = [&]() {
        if (impl_->lock_id >= 0) {
            impl_->combat.Update(true, impl_->lock_fx, impl_->lock_fy, dets, out);
        } else {
            impl_->combat.Update(false, 0, 0, dets, out);
        }
        if (impl_->dry_run) {
            out->out_count = 0;
        }
        std::fprintf(stdout, "[decider] decision: out_count=%u\n", out->out_count);
    };

    // ===== 验证状态机 =====
    if (impl_->verify_state != VerifyState::IDLE) {
        switch (impl_->verify_state) {
        case VerifyState::PRESS_RIGHT:
            out->out_count = 1;
            out->actions[0].kind = CORE_ACTION_KEY;
            out->actions[0].a = 0x27;  // VK_RIGHT
            out->actions[0].b = 1;
            out->actions[0].c = 0;
            if (now - impl_->verify_start >= impl_->press_duration) {
                impl_->verify_state = VerifyState::WAIT_AFTER_RIGHT;
                impl_->verify_start = now;
            }
            break;
        case VerifyState::WAIT_AFTER_RIGHT:
            out->out_count = 1;
            out->actions[0].kind = CORE_ACTION_KEY;
            out->actions[0].a = 0x27;
            out->actions[0].b = 0;
            out->actions[0].c = 0;
            if (now - impl_->verify_start >= 500) {
                CaptureSample(dets, &impl_->sample_t1);
                impl_->verify_state = VerifyState::PRESS_LEFT;
                impl_->verify_start = now;
                impl_->press_duration = RandPressDuration();
                std::fprintf(stdout, "[decider] verify T1: %u 候选, 左移 %llu ms\n",
                             impl_->sample_t1.count,
                             (unsigned long long)impl_->press_duration);
            }
            break;
        case VerifyState::PRESS_LEFT:
            out->out_count = 1;
            out->actions[0].kind = CORE_ACTION_KEY;
            out->actions[0].a = 0x25;  // VK_LEFT
            out->actions[0].b = 1;
            out->actions[0].c = 0;
            if (now - impl_->verify_start >= impl_->press_duration) {
                impl_->verify_state = VerifyState::WAIT_AFTER_LEFT;
                impl_->verify_start = now;
            }
            break;
        case VerifyState::WAIT_AFTER_LEFT:
            out->out_count = 1;
            out->actions[0].kind = CORE_ACTION_KEY;
            out->actions[0].a = 0x25;
            out->actions[0].b = 0;
            out->actions[0].c = 0;
            if (now - impl_->verify_start >= 500) {
                CaptureSample(dets, &impl_->sample_t2);
                impl_->verify_state = VerifyState::ANALYZE;
                impl_->verify_start = now;
                std::fprintf(stdout, "[decider] verify T2: %u 候选\n",
                             impl_->sample_t2.count);
            }
            break;
        case VerifyState::ANALYZE: {
            const int winner = AnalyzeMoves(impl_->sample_t0,
                                            impl_->sample_t1,
                                            impl_->sample_t2);
            if (winner >= 0) {
                const auto& w0 = impl_->sample_t0.items[winner];
                const int idx2 = FindNearest(impl_->sample_t2,
                                             w0.cx, w0.cy, 0.5f);
                if (idx2 >= 0) {
                    const auto& w2 = impl_->sample_t2.items[idx2];
                    impl_->lock_id = w2.track_id;
                    impl_->lock_fx = w2.cx;
                    impl_->lock_fy = w2.cy + w2.h * 0.5f;
                    impl_->lost_since = 0;
                    impl_->verify_retry = 0;
                    impl_->verify_cooldown_until = 0;
                    impl_->verify_state = VerifyState::IDLE;
                    std::fprintf(stdout, "[decider] verify 成功锁定 id=%d\n",
                                 impl_->lock_id);
                } else {
                    impl_->verify_retry++;
                    if (impl_->verify_retry >= Impl::kMaxRetry) {
                        impl_->verify_state = VerifyState::IDLE;
                        impl_->verify_retry = 0;
                        impl_->verify_cooldown_until = GetTickCount64() + Impl::kVerifyCooldownMs;
                        std::fprintf(stdout, "[decider] verify 3 次失败，冷却 %llu ms\n",
                                     (unsigned long long)Impl::kVerifyCooldownMs);
                    } else {
                        impl_->verify_state = VerifyState::PRESS_RIGHT;
                        impl_->verify_start = now;
                        impl_->press_duration = RandPressDuration();
                        CaptureSample(dets, &impl_->sample_t0);
                        std::fprintf(stdout, "[decider] verify 重试 %d/%d\n",
                                     impl_->verify_retry, Impl::kMaxRetry);
                    }
                }
            } else {
                impl_->verify_retry++;
                if (impl_->verify_retry >= Impl::kMaxRetry) {
                    impl_->verify_state = VerifyState::IDLE;
                    impl_->verify_retry = 0;
                    impl_->verify_cooldown_until = GetTickCount64() + Impl::kVerifyCooldownMs;
                    std::fprintf(stdout, "[decider] verify 3 次失败，冷却 %llu ms\n",
                                 (unsigned long long)Impl::kVerifyCooldownMs);
                } else {
                    impl_->verify_state = VerifyState::PRESS_RIGHT;
                    impl_->verify_start = now;
                    impl_->press_duration = RandPressDuration();
                    CaptureSample(dets, &impl_->sample_t0);
                    std::fprintf(stdout, "[decider] verify 重试 %d/%d\n",
                                 impl_->verify_retry, Impl::kMaxRetry);
                }
            }
            break;
        }
        default:
            break;
        }
        if (impl_->dry_run) {
            out->out_count = 0;
        }
        std::fprintf(stdout, "[decider] decision: out_count=%u\n", out->out_count);
        return;
    }

    // ===== 正常 me_lock =====
    struct Cand { int track_id; float fx; float fy; };
    Cand cands[CORE_MAX_DETECTIONS];
    uint32_t cand_count = 0;
    for (uint32_t i = 0; i < dets->count; ++i) {
        const core_detection& d = dets->items[i];
        if (d.cls != 0 && !(kTestIncludeMonster && d.cls == 1)) continue;
        if (cand_count >= CORE_MAX_DETECTIONS) break;
        cands[cand_count].track_id = d.track_id;
        cands[cand_count].fx = d.cx;
        cands[cand_count].fy = d.cy + d.h * 0.5f;
        cand_count++;
    }

    // 无锁
    if (impl_->lock_id < 0) {
        if (cand_count == 0) {
            finish();
            return;
        }
        if (cand_count == 1) {
            const float dxp = cands[0].fx - Impl::kPriorX;
            const float dyp = cands[0].fy - Impl::kPriorY;
            const float dp = std::sqrt(dxp * dxp + dyp * dyp);
            if (dp <= Impl::kUniqueMaxDist) {
                impl_->lock_id = cands[0].track_id;
                impl_->lock_fx = cands[0].fx;
                impl_->lock_fy = cands[0].fy;
                impl_->lost_since = 0;
                impl_->verify_cooldown_until = 0;
                impl_->verify_retry = 0;
                std::fprintf(stdout, "[decider] me_lock: id=%d fx=%.3f fy=%.3f (unique, dp=%.3f)\n",
                             impl_->lock_id, impl_->lock_fx, impl_->lock_fy, dp);
            } else {
                std::fprintf(stdout, "[decider] me_lock: unique id=%d too far (dp=%.3f > %.3f), skip\n",
                             cands[0].track_id, dp, Impl::kUniqueMaxDist);
            }
            finish();
            return;
        }
        // 多候选，触发验证
        {
            const unsigned long long now_ms = GetTickCount64();
            if (now_ms < impl_->verify_cooldown_until) {
                std::fprintf(stdout, "[decider] me_lock: none (cooldown %llu ms left)\n",
                             (unsigned long long)(impl_->verify_cooldown_until - now_ms));
                std::fprintf(stdout, "[decider] decision: out_count=0\n");
                return;
            }
        }
        impl_->verify_state = VerifyState::PRESS_RIGHT;
        impl_->verify_start = now;
        impl_->press_duration = RandPressDuration();
        impl_->verify_retry = 0;
        CaptureSample(dets, &impl_->sample_t0);
        std::fprintf(stdout, "[decider] 触发验证: 多候选 %u, 右移 %llu ms\n",
                     cand_count, (unsigned long long)impl_->press_duration);
        finish();
        return;
    }

    // 有锁
    const Cand* matched = nullptr;
    for (uint32_t i = 0; i < cand_count; ++i) {
        if (cands[i].track_id == impl_->lock_id) { matched = &cands[i]; break; }
    }

    if (matched != nullptr) {
        const float dx = matched->fx - impl_->lock_fx;
        const float dy = matched->fy - impl_->lock_fy;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist <= Impl::kLockRange) {
            impl_->lock_fx = matched->fx;
            impl_->lock_fy = matched->fy;
            impl_->lost_since = 0;
            impl_->verify_cooldown_until = 0;
            impl_->verify_retry = 0;
            std::fprintf(stdout, "[decider] me_lock: id=%d fx=%.3f fy=%.3f\n",
                         impl_->lock_id, impl_->lock_fx, impl_->lock_fy);
            finish();
            return;
        }
    }

    // 未匹配 / 圈外
    if (impl_->lost_since == 0) impl_->lost_since = now;
    const unsigned long long elapsed = now - impl_->lost_since;
    if (elapsed >= Impl::kRelockMs) {
        impl_->lock_id = -1;
        impl_->lost_since = 0;
        std::fprintf(stdout, "[decider] me_lock: lost, relock next frame\n");
    } else {
        std::fprintf(stdout, "[decider] me_lock: id=%d held (lost %llu ms)\n",
                     impl_->lock_id, elapsed);
    }
    finish();
}
