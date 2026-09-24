#include "cpp_script.h"

#include <cmath>
#include <cstdio>

struct CppScript::Impl {
    uint64_t last_log_frame = 0;
    bool inited = false;
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
    if (world.frame_index - impl_->last_log_frame < 30) return;
    impl_->last_log_frame = world.frame_index;

    if (world.dets == nullptr) {
        std::printf("[script] frame=%llu dets=nullptr\n",
                    (unsigned long long)world.frame_index);
        std::fflush(stdout);
        return;
    }

    uint32_t total = world.dets->count;
    uint32_t cls0_n = 0;
    uint32_t cls1_n = 0;
    int nearest_idx = -1;
    float nearest_dist = 1e9f;
    for (uint32_t i = 0; i < total; ++i) {
        const auto& d = world.dets->items[i];
        if (d.cls == 0) cls0_n++;
        else if (d.cls == 1) {
            cls1_n++;
            const float dx = d.cx - world.me_fx;
            const float dist = std::fabs(dx);
            if (dist < nearest_dist) {
                nearest_dist = dist;
                nearest_idx = (int)i;
            }
        }
    }

    if (nearest_idx >= 0) {
        const auto& d = world.dets->items[nearest_idx];
        std::printf("[script] frame=%llu total=%u cls0=%u cls1=%u nearest(cx=%.3f cy=%.3f conf=%.3f)\n",
                    (unsigned long long)world.frame_index,
                    total, cls0_n, cls1_n,
                    d.cx, d.cy, d.conf);
    } else {
        std::printf("[script] frame=%llu total=%u cls0=%u cls1=0 无怪\n",
                    (unsigned long long)world.frame_index,
                    total, cls0_n);
    }
    std::fflush(stdout);
}

void CppScript::GetDecision(core_decision* out) {
    out->out_count = 0;  // S1 不输出任何动作
}

void CppScript::Shutdown() {
    std::printf("[script] 脚本已停止\n");
    std::fflush(stdout);
    impl_->inited = false;
}
