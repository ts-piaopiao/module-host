#pragma once

#include "iscript.h"
#include "cpp_script.h"   // CppScriptDebugInfo
#include <memory>
#include <string>

class ScriptHost {
public:
    ScriptHost();
    ~ScriptHost();

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // 创建并初始化脚本。config 为原始配置字符串。
    // 返回 false 表示初始化失败。
    bool Start(const std::string& config);

    // 停止并销毁脚本。幂等。
    void Stop();

    // 每帧调用。宿主组装 world，脚本更新状态。
    void OnFrame(uint64_t frame_index, uint64_t now_ms,
                 bool me_valid, float me_fx, float me_fy,
                 const core_detections* dets);

    // 读取本帧决策。out->out_count 初始为 0。
    void GetDecision(core_decision* out);

    // 查询脚本内部锁定的 me 位置（用于 recorder）。
    // 空脚本返回 false。
    bool GetMeLock(float* fx, float* fy) const;

    // 读取脚本内部调试状态（用于 telemetry）。
    // 空脚本或非 CppScript 实现时，out 保持默认值。
    void GetDebugInfo(CppScriptDebugInfo* out) const;

private:
    struct Impl;
    Impl* impl_;
};
