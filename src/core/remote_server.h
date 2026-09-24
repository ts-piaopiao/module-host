#ifndef MODULE_HOST_CORE_REMOTE_SERVER_H
#define MODULE_HOST_CORE_REMOTE_SERVER_H

#include "core_contract.h"
#include <cstdint>
#include <vector>

class OutputManager;

class RemoteServer {
public:
    RemoteServer();
    ~RemoteServer();

    RemoteServer(const RemoteServer&) = delete;
    RemoteServer& operator=(const RemoteServer&) = delete;

    bool Start(int port, int jpeg_quality);
    void Stop();

    void PushFrame(const uint8_t* bgra, uint32_t width, uint32_t height);

    // 主线程调用。取走当前队列里所有人工事件。
    // 若队列为空，返回空 vector。
    // 调用后队列清空。
    std::vector<core_action> PopHumanEvents();

    // 设置输出通道。remote_server 收到 human 事件后直接调 output->SendAsync。
    // 传 nullptr 表示不直通（仅入队）。
    void SetOutputSink(OutputManager* output);

private:
    struct Impl;
    Impl* impl_;
};

#endif
