#ifndef MODULE_HOST_CORE_RECORDER_H
#define MODULE_HOST_CORE_RECORDER_H

#include "core_contract.h"
#include <cstdint>
#include <string>

class Recorder {
public:
    Recorder();
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // 启动。返回 false 表示无法创建文件（不阻塞程序运行，只是不记录）。
    bool Start(const std::string& record_dir);

    // 停止并 flush。幂等。
    void Stop();

    // 以下方法线程安全，热路径调用，只入队不写盘。
    void RecordDetection(uint64_t frame, const core_detection& d);
    void RecordDecision(uint64_t frame, bool me_valid,
                        float me_fx, float me_fy,
                        const core_decision* dec);
    void RecordHuman(uint64_t frame, const core_action& a);
    void RecordError(const char* msg);

private:
    struct Impl;
    Impl* impl_;
};

#endif
