#include "cpp_script.h"

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
    // S1 每 30 帧打印一次，避免刷屏
    if (world.frame_index - impl_->last_log_frame >= 30) {
        impl_->last_log_frame = world.frame_index;
        std::printf("[script] frame=%llu me_valid=%d me=(%.3f,%.3f) dets=%u\n",
                    (unsigned long long)world.frame_index,
                    world.me_valid ? 1 : 0,
                    world.me_fx, world.me_fy,
                    world.dets ? world.dets->count : 0);
        std::fflush(stdout);
    }
}

void CppScript::GetDecision(core_decision* out) {
    out->out_count = 0;  // S1 不输出任何动作
}

void CppScript::Shutdown() {
    std::printf("[script] 脚本已停止\n");
    std::fflush(stdout);
    impl_->inited = false;
}
