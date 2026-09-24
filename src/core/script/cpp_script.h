#pragma once

#include "iscript.h"

class CppScript : public IScript {
public:
    CppScript();
    ~CppScript() override;

    bool Init(const std::string& config) override;
    void OnFrame(const ScriptWorld& world) override;
    void GetDecision(core_decision* out) override;
    void Shutdown() override;

private:
    struct Impl;
    Impl* impl_;
};
