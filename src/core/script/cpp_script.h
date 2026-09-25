#pragma once

#include "iscript.h"

struct CppScriptDebugInfo {
    int state = 0;          // 0=IDLE 1=CHASE 2=ATTACK 3=ATTACK_TURN 4=RECOVERY
    int facing = 1;
    bool me_locked = false;
    float me_fx = 0, me_fy = 0;
    bool target_locked = false;
    float target_cx = 0;
    int active_key = 0;
    bool desired_e = false;
    bool e_long_hold = false;
};

class CppScript : public IScript {
public:
    CppScript();
    ~CppScript() override;

    bool Init(const std::string& config) override;
    void OnFrame(const ScriptWorld& world) override;
    void GetDecision(core_decision* out) override;
    void Shutdown() override;

    // 调试接口：返回内部状态。仅供回放工具使用。
    void GetDebugInfo(CppScriptDebugInfo* out) const;

private:
    struct Impl;
    Impl* impl_;
};
