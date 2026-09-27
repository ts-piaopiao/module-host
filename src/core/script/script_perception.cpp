#include "script_perception.h"

#include "script_config.h"
#include "script_geometry.h"

#include <cmath>

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
