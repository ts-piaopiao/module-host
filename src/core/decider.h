#ifndef MODULE_HOST_CORE_DECIDER_H
#define MODULE_HOST_CORE_DECIDER_H

#include "core_contract.h"

class Decider {
public:
    Decider();
    ~Decider();

    Decider(const Decider&) = delete;
    Decider& operator=(const Decider&) = delete;

    // 每帧调用。输入 policy 产出的检测结果，输出决策。
    void Update(const core_detections* dets, core_decision* out);

    // 获取当前锁定的 me 位置和有效性
    bool GetMeLock(float* fx, float* fy) const;

private:
    struct Impl;
    Impl* impl_;
};

#endif
