#include "cpp_script.h"

#include <cmath>
#include <cstdio>

namespace {

constexpr float kPriorX = 0.5f;
constexpr float kPriorY = 0.72f;

bool SelectMe(const core_detections* dets, float* out_fx, float* out_fy) {
    if (dets == nullptr) return false;
    bool found = false;
    float best = 1e9f;
    for (uint32_t i = 0; i < dets->count; ++i) {
        const auto& d = dets->items[i];
        if (d.cls != 0) continue;
        const float fx = d.cx;
        const float fy = d.cy + d.h * 0.5f;
        const float dx = fx - kPriorX;
        const float dy = fy - kPriorY;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist < best) {
            best = dist;
            *out_fx = fx;
            *out_fy = fy;
            found = true;
        }
    }
    return found;
}

bool SelectTarget(const core_detections* dets, float me_fx,
                  float* out_cx, float* out_cy) {
    if (dets == nullptr) return false;
    bool found = false;
    float best = 1e9f;
    for (uint32_t i = 0; i < dets->count; ++i) {
        const auto& d = dets->items[i];
        if (d.cls != 1) continue;
        const float dist = std::fabs(d.cx - me_fx);
        if (dist < best) {
            best = dist;
            *out_cx = d.cx;
            *out_cy = d.cy;
            found = true;
        }
    }
    return found;
}

}  // namespace

struct CppScript::Impl {
    uint64_t last_log_frame = 0;
    bool inited = false;

    enum class State { IDLE, CHASE };
    State state = State::IDLE;

    bool me_valid = false;
    float me_fx = 0.0f;
    float me_fy = 0.0f;

    bool has_target = false;
    float target_cx = 0.0f;
    float target_cy = 0.0f;

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
    impl_->me_valid = SelectMe(world.dets, &impl_->me_fx, &impl_->me_fy);

    if (impl_->me_valid) {
        impl_->has_target = SelectTarget(world.dets, impl_->me_fx,
                                         &impl_->target_cx, &impl_->target_cy);
    } else {
        impl_->has_target = false;
    }

    if (!impl_->me_valid || !impl_->has_target) {
        impl_->state = Impl::State::IDLE;
    } else {
        impl_->state = Impl::State::CHASE;
    }

    if (impl_->state == Impl::State::IDLE) {
        impl_->active_key = 0;
    } else {
        const float dx = impl_->target_cx - impl_->me_fx;
        if (std::fabs(dx) < 0.02f) {
            impl_->active_key = 0;
        } else {
            impl_->active_key = (dx > 0) ? 0x27 : 0x25;
        }
    }

    if (world.frame_index - impl_->last_log_frame >= 30) {
        impl_->last_log_frame = world.frame_index;
        const char* state_str = (impl_->state == Impl::State::IDLE) ? "IDLE" : "CHASE";
        std::printf("[script] frame=%llu state=%s me_valid=%d me=(%.3f,%.3f) target=%d target_cx=%.3f key=0x%02X\n",
                    (unsigned long long)world.frame_index,
                    state_str,
                    impl_->me_valid ? 1 : 0,
                    impl_->me_fx, impl_->me_fy,
                    impl_->has_target ? 1 : 0,
                    impl_->target_cx,
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
