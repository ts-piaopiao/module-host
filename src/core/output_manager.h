#ifndef MODULE_HOST_CORE_OUTPUT_MANAGER_H
#define MODULE_HOST_CORE_OUTPUT_MANAGER_H

#include "core_contract.h"
#include <string>

class Recorder;

class OutputManager {
public:
    OutputManager();
    ~OutputManager();

    OutputManager(const OutputManager&) = delete;
    OutputManager& operator=(const OutputManager&) = delete;

    // 打开串口。config 为原始配置字符串（读 input_port / input_baud）。
    // 返回 false 表示串口打开失败。
    bool Start(const std::string& config);

    // 停止并关闭串口。幂等。
    void Stop();

    // 设置发送日志接收器。非空时，每个动作在发出前调 recorder->RecordSend。
    // 传 nullptr 表示不记录。可在 Start 前或后调用。
    void SetSendSink(Recorder* recorder);

    // 脚本入口：帧同步调用。内部检查 script_paused：
    //   - 若暂停：直接丢弃 decision 里所有 action
    //   - 未暂停：入队，由发送线程发出
    void SendScript(const core_decision* decision);

    // 远程入口：事件驱动调用。永不暂停。
    void SendAsync(const core_action* action);

    // 控制模式：true = 脚本输出被丢弃。
    void SetScriptPaused(bool paused);
    bool IsScriptPaused() const;

private:
    struct Impl;
    Impl* impl_;
};

#endif