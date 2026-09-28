#pragma once

#include "telemetry_protocol.h"

class TelemetryServer {
public:
    TelemetryServer();
    ~TelemetryServer();

    TelemetryServer(const TelemetryServer&) = delete;
    TelemetryServer& operator=(const TelemetryServer&) = delete;

    // 启动 HTTP + WebSocket 服务器。返回 false 表示启动失败
    // （端口被占用等）。失败不阻塞 core 主流程。
    bool Start(int port);

    // 停止并 join 内部线程。幂等。
    void Stop();

    // 广播一帧 FrameBundle 给所有已连接客户端。
    // 线程安全，非阻塞（但若客户端发送缓冲满，可能短暂阻塞）。
    // 未启动时静默丢弃。
    void Publish(const telemetry::FrameBundle& bundle);

private:
    struct Impl;
    Impl* impl_;
};
