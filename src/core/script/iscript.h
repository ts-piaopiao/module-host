#pragma once

#include "core_contract.h"
#include <cstdint>
#include <string>

struct ScriptWorld {
    uint64_t frame_index = 0;
    uint64_t now_ms = 0;
    bool     me_valid = false;
    float    me_fx = 0.0f;
    float    me_fy = 0.0f;
    const core_detections* dets = nullptr;
};

class IScript {
public:
    virtual ~IScript() = default;
    virtual bool Init(const std::string& config) = 0;
    virtual void OnFrame(const ScriptWorld& world) = 0;
    virtual void GetDecision(core_decision* out) = 0;
    virtual void Shutdown() = 0;
};
