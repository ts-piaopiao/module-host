#include "combat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <windows.h>

namespace {

void PushKey(core_decision* out, int vk, int down) {
    if (out->out_count >= CORE_DECISION_CAPACITY) return;
    out->actions[out->out_count].kind = CORE_ACTION_KEY;
    out->actions[out->out_count].a = vk;
    out->actions[out->out_count].b = down;
    out->actions[out->out_count].c = 0;
    out->out_count++;
}

}  // namespace

struct Combat::Impl {
    struct TrackedMonster {
        float cx = 0, cy = 0, w = 0, h = 0;
        int seen = 0;
        unsigned long long last_seen = 0;
        bool alive = true;
    };

    CombatState state = CombatState::IDLE;
    unsigned long long state_since = 0;

    std::vector<TrackedMonster> monsters;

    // 参数（归一化）
    static constexpr float kSamePlatY = 0.028f;
    static constexpr float kAtkMin = 0.010f;
    static constexpr float kAtkMax = 0.135f;
    static constexpr float kAtkCross = 0.104f;
    static constexpr float kAtkHyst = 0.021f;
    static constexpr float kAtkVUp = 0.074f;
    static constexpr float kAtkVDown = 0.019f;
    static constexpr int kConfirmFrames = 2;
    static constexpr unsigned long long kLoseMs = 200;
    static constexpr float kMatchX = 0.0125f;
    static constexpr float kMatchY = 0.0185f;
    static constexpr float kTargetMatchX = 0.025f;  // 48px/1920，比 kMatchX 宽 2 倍
    static constexpr float kTargetMatchY = 0.037f;  // 40px/1080

    // 目标沿用
    bool has_target = false;
    float target_cx = 0, target_cy = 0, target_w = 0, target_h = 0;
    unsigned long long target_lost_since = 0;  // 沿用目标丢失起始时间

    // 按键状态
    int active_dir_key = 0;
    bool active_e = false;

    static bool IsInBand(const TrackedMonster& m, float me_fx, float me_fy) {
        const float dxs = std::fabs(m.cx - me_fx);
        const float dys = m.cy - me_fy;
        const bool on_same = std::fabs(dys) <= kSamePlatY;
        const float x_max = on_same ? kAtkMax : kAtkCross;
        const float m_top = m.cy - m.h;
        return (dxs >= kAtkMin && dxs <= x_max &&
                dys >= -kAtkVUp && m_top <= me_fy + kAtkVDown);
    }
};

Combat::Combat() : impl_(new Impl()) {}

Combat::~Combat() {
    delete impl_;
    impl_ = nullptr;
}

void Combat::ReleaseAll(core_decision* out) {
    if (out == nullptr) return;
    out->out_count = 0;
    if (impl_->active_dir_key != 0) {
        PushKey(out, impl_->active_dir_key, 0);
        impl_->active_dir_key = 0;
    }
    if (impl_->active_e) {
        PushKey(out, 0x45, 0);
        impl_->active_e = false;
    }
}

void Combat::Update(bool me_valid, float me_fx, float me_fy,
                    const core_detections* dets, core_decision* out) {
    if (out == nullptr) return;
    out->out_count = 0;
    if (!me_valid) {
        ReleaseAll(out);
        return;
    }

    const unsigned long long now = GetTickCount64();

    // 1. 从 dets 收集本帧 cls=1
    struct Det { float cx, cy, w, h; };
    Det fresh[CORE_MAX_DETECTIONS];
    uint32_t fresh_n = 0;
    if (dets != nullptr) {
        for (uint32_t i = 0; i < dets->count; ++i) {
            if (dets->items[i].cls != 1) continue;
            if (fresh_n >= CORE_MAX_DETECTIONS) break;
            fresh[fresh_n].cx = dets->items[i].cx;
            fresh[fresh_n].cy = dets->items[i].cy;
            fresh[fresh_n].w = dets->items[i].w;
            fresh[fresh_n].h = dets->items[i].h;
            fresh_n++;
        }
    }

    // 2. 匹配已存在的怪（位置匹配 kMatchX / kMatchY）
    std::vector<bool> fresh_matched(fresh_n, false);
    std::vector<bool> mon_updated(impl_->monsters.size(), false);
    for (uint32_t fi = 0; fi < fresh_n; ++fi) {
        int best = -1;
        float best_d = 1e9f;
        for (size_t mi = 0; mi < impl_->monsters.size(); ++mi) {
            if (mon_updated[mi]) continue;
            if (!impl_->monsters[mi].alive) continue;
            const float dx = std::fabs(fresh[fi].cx - impl_->monsters[mi].cx);
            const float dy = std::fabs(fresh[fi].cy - impl_->monsters[mi].cy);
            if (dx > Impl::kMatchX || dy > Impl::kMatchY) continue;
            const float d2 = dx * dx + dy * dy;
            if (d2 < best_d) { best_d = d2; best = static_cast<int>(mi); }
        }
        if (best >= 0) {
            auto& m = impl_->monsters[best];
            m.cx = fresh[fi].cx;
            m.cy = fresh[fi].cy;
            m.w = fresh[fi].w;
            m.h = fresh[fi].h;
            m.seen += 1;
            m.last_seen = now;
            m.alive = true;
            mon_updated[best] = true;
            fresh_matched[fi] = true;
        }
    }
    // 未匹配的 fresh 创建新怪
    for (uint32_t fi = 0; fi < fresh_n; ++fi) {
        if (fresh_matched[fi]) continue;
        Impl::TrackedMonster m;
        m.cx = fresh[fi].cx;
        m.cy = fresh[fi].cy;
        m.w = fresh[fi].w;
        m.h = fresh[fi].h;
        m.seen = 1;
        m.last_seen = now;
        m.alive = true;
        impl_->monsters.push_back(m);
        mon_updated.push_back(false);
    }
    // 老化：last_seen 距今 > kLoseMs 且本轮未更新 → alive=false
    for (size_t mi = 0; mi < impl_->monsters.size(); ++mi) {
        if (mi < mon_updated.size() && mon_updated[mi]) continue;
        auto& m = impl_->monsters[mi];
        if (m.alive && now - m.last_seen > Impl::kLoseMs) {
            m.alive = false;
        }
    }
    // 移除已死的
    impl_->monsters.erase(
        std::remove_if(impl_->monsters.begin(), impl_->monsters.end(),
                       [](const Impl::TrackedMonster& m) { return !m.alive; }),
        impl_->monsters.end());

    // 3. 选目标（含沿用）
    const Impl::TrackedMonster* chosen = nullptr;

    // 3a. 沿用：用放宽的窗口找
    if (impl_->has_target) {
        float best_d = 1e9f;
        for (const auto& m : impl_->monsters) {
            if (m.seen < Impl::kConfirmFrames) continue;
            const float dx = std::fabs(m.cx - impl_->target_cx);
            const float dy = std::fabs(m.cy - impl_->target_cy);
            if (dx > Impl::kTargetMatchX || dy > Impl::kTargetMatchY) continue;
            const float d2 = dx * dx + dy * dy;
            if (d2 < best_d) { best_d = d2; chosen = &m; }
        }
    }

    // 3a.5 沿用命中：重置丢失计时
    if (chosen != nullptr) {
        impl_->target_lost_since = 0;
    }

    // 3a.6 沿用失败：进入丢失容忍期
    if (chosen == nullptr && impl_->has_target) {
        if (impl_->target_lost_since == 0) {
            impl_->target_lost_since = now;
        }
        const unsigned long long lost_ms = now - impl_->target_lost_since;
        if (lost_ms < 500) {
            // 容忍期内：保持上一帧的按键状态，不切换目标
            // out 保持 out_count = 0，active_dir_key / active_e 不变
            return;
        } else {
            // 超时，放弃旧目标
            impl_->has_target = false;
            impl_->target_lost_since = 0;
        }
    }

    // 3b. 重选：只有 has_target = false 时才选最近的
    if (chosen == nullptr && !impl_->has_target) {
        float best_d = 1e9f;
        for (const auto& m : impl_->monsters) {
            if (m.seen < Impl::kConfirmFrames) continue;
            const float d = std::fabs(m.cx - me_fx);
            if (d < best_d) { best_d = d; chosen = &m; }
        }
    }

    // 沿用命中 / 新选：更新目标坐标
    if (chosen != nullptr) {
        impl_->target_cx = chosen->cx;
        impl_->target_cy = chosen->cy;
        impl_->target_w = chosen->w;
        impl_->target_h = chosen->h;
        impl_->has_target = true;
        impl_->target_lost_since = 0;
    }

    // 4. 状态迁移（10a：IDLE / CHASE / ATTACK，行为与 v4 等价）
    const bool in_band = (chosen != nullptr) && Impl::IsInBand(*chosen, me_fx, me_fy);

    switch (impl_->state) {
    case CombatState::IDLE:
        if (chosen != nullptr) {
            impl_->state = in_band ? CombatState::ATTACK : CombatState::CHASE;
            impl_->state_since = now;
        }
        break;
    case CombatState::CHASE:
        if (chosen == nullptr) {
            if (!impl_->has_target) {
                impl_->state = CombatState::IDLE;
                impl_->state_since = now;
            }
        } else if (in_band) {
            impl_->state = CombatState::ATTACK;
            impl_->state_since = now;
        }
        break;
    case CombatState::ATTACK:
        if (chosen == nullptr) {
            if (!impl_->has_target) {
                impl_->state = CombatState::IDLE;
                impl_->state_since = now;
            }
        } else if (!in_band) {
            impl_->state = CombatState::CHASE;
            impl_->state_since = now;
        }
        break;
    }

    // 5. 按键输出（差分）
    int desired_dir = 0;
    bool desired_e = false;

    switch (impl_->state) {
    case CombatState::IDLE:
        desired_dir = 0;
        desired_e = false;
        break;
    case CombatState::CHASE:
        if (chosen != nullptr) {
            const float dx_signed = chosen->cx - me_fx;
            desired_dir = (dx_signed > 0) ? 0x27 : 0x25;
        }
        desired_e = false;
        break;
    case CombatState::ATTACK:
        desired_dir = 0;
        desired_e = true;
        break;
    }

    if (impl_->active_dir_key != desired_dir) {
        if (impl_->active_dir_key != 0) {
            PushKey(out, impl_->active_dir_key, 0);
        }
        if (desired_dir != 0) {
            PushKey(out, desired_dir, 1);
        }
        impl_->active_dir_key = desired_dir;
    }
    if (impl_->active_e != desired_e) {
        PushKey(out, 0x45, desired_e ? 1 : 0);
        impl_->active_e = desired_e;
    }
}
