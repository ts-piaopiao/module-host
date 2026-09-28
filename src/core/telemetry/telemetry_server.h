#pragma once

#include "telemetry_protocol.h"

#include <string>

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

    // 广播一条原始 JSON 字符串（用于控制消息等非 FrameBundle 数据）。
    // 调用者负责保证是单行合法 JSON。线程安全。
    void PublishRaw(const std::string& json_line);

    // 设置"当前处于回放模式"标记。
    // 为 true 时，新 WebSocket 连接建立后会自动补发一次 replay_begin，
    // 确保任何时刻连上的客户端都能进入回放缓存模式（见 docs/ui-design.md §十四）。
    // 线程安全。
    void SetReplayMode(bool on);

private:
    struct Impl;
    Impl* impl_;
};
