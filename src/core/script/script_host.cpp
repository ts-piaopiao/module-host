#include "script_host.h"
#include "cpp_script.h"

struct ScriptHost::Impl {
    std::unique_ptr<IScript> script;
};

ScriptHost::ScriptHost() : impl_(new Impl()) {}
ScriptHost::~ScriptHost() { Stop(); delete impl_; }

bool ScriptHost::Start(const std::string& config) {
    impl_->script = std::make_unique<CppScript>();
    return impl_->script->Init(config);
}

void ScriptHost::Stop() {
    if (impl_->script) {
        impl_->script->Shutdown();
        impl_->script.reset();
    }
}

void ScriptHost::OnFrame(uint64_t frame_index, uint64_t now_ms,
                         bool me_valid, float me_fx, float me_fy,
                         const core_detections* dets) {
    if (!impl_->script) return;
    ScriptWorld w;
    w.frame_index = frame_index;
    w.now_ms = now_ms;
    w.me_valid = me_valid;
    w.me_fx = me_fx;
    w.me_fy = me_fy;
    w.dets = dets;
    impl_->script->OnFrame(w);
}

void ScriptHost::GetDecision(core_decision* out) {
    if (out == nullptr) return;
    out->out_count = 0;
    if (impl_->script) {
        impl_->script->GetDecision(out);
    }
}

bool ScriptHost::GetMeLock(float* fx, float* fy) const {
    if (!impl_->script) return false;
    return impl_->script->GetMeLock(fx, fy);
}

void ScriptHost::GetDebugInfo(CppScriptDebugInfo* out) const {
    if (out == nullptr) return;
    if (!impl_->script) return;
    auto* cpp = dynamic_cast<CppScript*>(impl_->script.get());
    if (cpp != nullptr) {
        cpp->GetDebugInfo(out);
    }
}
