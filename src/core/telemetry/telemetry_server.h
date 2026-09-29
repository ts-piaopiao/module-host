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
    // false→true：向当前已连接客户端广播一次 replay_begin，并置位；
    //   此后每个新 WebSocket 连接建立时也补发一次 replay_begin，
    //   保证任何时刻连上的客户端首条消息就是 replay_begin
    //   （见 docs/ui-design.md §十四）。
    // true→false：只清标记，不再补发（此后的连接收不到 replay_begin）。
    // 幂等：重复置位为 true 不会重复广播。
    // 线程安全。
    void SetReplayMode(bool on);

private:
    struct Impl;
    Impl* impl_;
};
