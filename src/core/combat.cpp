#include "combat.h"

#include <cstdint>
#include <cstdio>

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
    // 上一帧按下的方向键：0=无，0x25=左，0x27=右
    int last_dir_key = 0;
    // 上一帧是否按了 E
    bool last_e_down = false;

    static constexpr float kAttackBand = 0.15f;
};

Combat::Combat() : impl_(new Impl()) {}

Combat::~Combat() {
    delete impl_;
    impl_ = nullptr;
}

void Combat::Update(bool me_valid, float me_fx, float me_fy,
                    const core_detections* dets, core_decision* out) {
    out->out_count = 0;

    // 收集所有 cls=1 的怪
    struct M { float fx; };
    M ms[CORE_MAX_DETECTIONS];
    uint32_t m_count = 0;
    if (dets != nullptr) {
        for (uint32_t i = 0; i < dets->count; ++i) {
            if (dets->items[i].cls != 1) continue;
            if (m_count >= CORE_MAX_DETECTIONS) break;
            ms[m_count].fx = dets->items[i].cx;
            m_count++;
        }
    }

    uint32_t best = 0;
    float dx = 0.0f;
    const bool have_target = me_valid && m_count > 0;

    if (!have_target) {
        // 无 me 或无怪：释放所有旧按键，输出空
        if (impl_->last_dir_key != 0) {
            PushKey(out, impl_->last_dir_key, 0);
            impl_->last_dir_key = 0;
        }
        if (impl_->last_e_down) {
            PushKey(out, 0x45, 0);
            impl_->last_e_down = false;
        }
    } else {
        // 选最近怪（按 |fx - me_fx|）
        float best_d = 1e9f;
        for (uint32_t i = 0; i < m_count; ++i) {
            const float d = (ms[i].fx > me_fx) ? (ms[i].fx - me_fx) : (me_fx - ms[i].fx);
            if (d < best_d) { best_d = d; best = i; }
        }

        dx = ms[best].fx - me_fx;

        if (dx > Impl::kAttackBand || dx < -Impl::kAttackBand) {
            // 不在攻击带：朝怪移动
            const int want_key = (dx > 0) ? 0x27 : 0x25;  // RIGHT / LEFT
            // 需要换方向：先释放旧方向
            if (impl_->last_dir_key != 0 && impl_->last_dir_key != want_key) {
                PushKey(out, impl_->last_dir_key, 0);
            }
            // 按新方向
            PushKey(out, want_key, 1);
            impl_->last_dir_key = want_key;
            // 移动时释放 E
            if (impl_->last_e_down) {
                PushKey(out, 0x45, 0);
                impl_->last_e_down = false;
            }
        } else {
            // 在攻击带：释放方向键，按 E
            if (impl_->last_dir_key != 0) {
                PushKey(out, impl_->last_dir_key, 0);
                impl_->last_dir_key = 0;
            }
            PushKey(out, 0x45, 1);
            impl_->last_e_down = true;
        }
    }

    // 临时诊断：out 已填完（跑完删）
    std::fprintf(stdout, "[combat] me=(%.3f,%.3f) monsters=%u best_fx=%.3f dx=%.3f out=%u last_dir=0x%02X last_e=%d\n",
                 me_fx, me_fy, m_count,
                 (m_count > 0 ? ms[best].fx : 0.0f),
                 (m_count > 0 ? ms[best].fx - me_fx : 0.0f),
                 out->out_count, impl_->last_dir_key, impl_->last_e_down ? 1 : 0);
}
