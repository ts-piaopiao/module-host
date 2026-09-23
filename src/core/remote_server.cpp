#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <objbase.h>

#include "remote_server.h"
#include "jpeg_encoder.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {

void WriteU32BE(uint8_t* buf, uint32_t v) {
    buf[0] = (v >> 24) & 0xFF;
    buf[1] = (v >> 16) & 0xFF;
    buf[2] = (v >> 8) & 0xFF;
    buf[3] = v & 0xFF;
}

uint32_t ReadU32BE(const uint8_t* buf) {
    return (static_cast<uint32_t>(buf[0]) << 24) |
           (static_cast<uint32_t>(buf[1]) << 16) |
           (static_cast<uint32_t>(buf[2]) << 8) |
           static_cast<uint32_t>(buf[3]);
}

struct FrameSnapshot {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;
};

}  // namespace

struct RemoteServer::Impl {
    std::atomic<bool> stop_flag{false};
    SOCKET listen_socket = INVALID_SOCKET;
    SOCKET client_socket = INVALID_SOCKET;
    std::thread thread;
    bool wsa_started = false;
    bool started = false;

    std::mutex frame_mutex;
    std::deque<FrameSnapshot> frame_queue;

    std::mutex human_mutex;
    std::deque<core_action> human_queue;

    int jpeg_quality = 80;
    JpegEncoder encoder;

    void Loop() {
        bool com_ok = false;
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr == S_OK || hr == S_FALSE || hr == RPC_E_CHANGED_MODE) {
            com_ok = (hr == S_OK);
        } else {
            return;
        }

        while (!stop_flag.load()) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(listen_socket, &read_fds);
            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 500 * 1000;
            const int sel = select(0, &read_fds, nullptr, nullptr, &tv);
            if (stop_flag.load()) {
                break;
            }
            if (sel <= 0) {
                continue;
            }
            sockaddr_in addr = {};
            int addr_len = sizeof(addr);
            SOCKET client = accept(listen_socket, reinterpret_cast<sockaddr*>(&addr), &addr_len);
            if (client == INVALID_SOCKET) {
                continue;
            }
            if (client_socket != INVALID_SOCKET) {
                closesocket(client);
                continue;
            }
            client_socket = client;
            ServeClient();
            if (client_socket != INVALID_SOCKET) {
                closesocket(client_socket);
                client_socket = INVALID_SOCKET;
            }
        }

        if (listen_socket != INVALID_SOCKET) {
            closesocket(listen_socket);
            listen_socket = INVALID_SOCKET;
        }
        if (client_socket != INVALID_SOCKET) {
            closesocket(client_socket);
            client_socket = INVALID_SOCKET;
        }

        if (com_ok) {
            CoUninitialize();
        }
    }

    void ServeClient() {
        if (!encoder.Init()) {
            return;
        }

        std::vector<uint8_t> rx_buf;
        auto last_heartbeat = std::chrono::steady_clock::now();
        uint8_t header[8];
        while (!stop_flag.load()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_heartbeat >= std::chrono::seconds(2)) {
                WriteU32BE(header, 4);
                WriteU32BE(header + 4, 2);
                if (send(client_socket, reinterpret_cast<const char*>(header), 8, 0) == SOCKET_ERROR) {
                    return;
                }
                last_heartbeat = now;
            }

            {
                FrameSnapshot snap;
                bool has_frame = false;
                {
                    std::lock_guard<std::mutex> lk(frame_mutex);
                    if (!frame_queue.empty()) {
                        snap = std::move(frame_queue.front());
                        frame_queue.pop_front();
                        has_frame = true;
                    }
                }
                if (has_frame) {
                    std::vector<uint8_t> jpeg;
                    if (encoder.Encode(snap.bgra.data(), snap.width, snap.height,
                                       jpeg_quality, &jpeg) && !jpeg.empty()) {
                        uint32_t payload_len = 4 + static_cast<uint32_t>(jpeg.size());
                        std::vector<uint8_t> header_and_payload;
                        header_and_payload.resize(4 + payload_len);
                        WriteU32BE(header_and_payload.data(), payload_len);
                        WriteU32BE(header_and_payload.data() + 4, 1);
                        std::memcpy(header_and_payload.data() + 8, jpeg.data(), jpeg.size());
                        const int total = static_cast<int>(header_and_payload.size());
                        int sent = 0;
                        while (sent < total) {
                            const int n = send(client_socket,
                                               reinterpret_cast<const char*>(header_and_payload.data() + sent),
                                               total - sent, 0);
                            if (n == SOCKET_ERROR || n == 0) {
                                return;
                            }
                            sent += n;
                        }
                    }
                }
            }

            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(client_socket, &read_fds);
            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 500 * 1000;
            const int sel = select(0, &read_fds, nullptr, nullptr, &tv);
            if (stop_flag.load()) {
                return;
            }
            if (sel <= 0) {
                continue;
            }
            char buf[4096];
            const int n = recv(client_socket, buf, sizeof(buf), 0);
            if (n == 0) {
                return;
            }
            if (n == SOCKET_ERROR) {
                return;
            }

            rx_buf.insert(rx_buf.end(), buf, buf + n);

            while (rx_buf.size() >= 8) {
                const uint32_t len = ReadU32BE(rx_buf.data());
                if (len < 4 || len > 65536) {
                    // 协议错误，清空缓冲，断开连接
                    return;
                }
                if (rx_buf.size() < 4 + len) {
                    break;  // 消息未完整，等下一次 recv
                }
                const uint32_t type = ReadU32BE(rx_buf.data() + 4);
                const uint8_t* payload = rx_buf.data() + 8;
                const uint32_t payload_len = len - 4;

                core_action action;
                bool valid = false;

                if (type == 1 && payload_len == 8) {
                    // 键盘：int32 key_code, int32 down
                    const int32_t key = static_cast<int32_t>(ReadU32BE(payload));
                    const int32_t down = static_cast<int32_t>(ReadU32BE(payload + 4));
                    action.kind = CORE_ACTION_KEY;
                    action.a = key;
                    action.b = (down != 0) ? 1 : 0;
                    action.c = 0;
                    valid = true;
                } else if (type == 2 && payload_len == 8) {
                    // 鼠标移动：int32 dx, int32 dy
                    const int32_t dx = static_cast<int32_t>(ReadU32BE(payload));
                    const int32_t dy = static_cast<int32_t>(ReadU32BE(payload + 4));
                    action.kind = CORE_ACTION_POINTER_MOVE;
                    action.a = dx;
                    action.b = dy;
                    action.c = 0;
                    valid = true;
                } else if (type == 3 && payload_len == 8) {
                    // 鼠标按键：int32 button_id, int32 down
                    const int32_t btn = static_cast<int32_t>(ReadU32BE(payload));
                    const int32_t down = static_cast<int32_t>(ReadU32BE(payload + 4));
                    action.kind = CORE_ACTION_POINTER_BUTTON;
                    action.a = btn;
                    action.b = (down != 0) ? 1 : 0;
                    action.c = 0;
                    valid = true;
                }
                // 其他类型（例如心跳）直接忽略，不报错

                if (valid) {
                    std::lock_guard<std::mutex> lk(human_mutex);
                    human_queue.push_back(action);
                }

                // 消费掉这条消息
                rx_buf.erase(rx_buf.begin(), rx_buf.begin() + 4 + len);
            }
        }
    }
};

RemoteServer::RemoteServer() : impl_(new Impl()) {}

RemoteServer::~RemoteServer() {
    Stop();
    delete impl_;
    impl_ = nullptr;
}

bool RemoteServer::Start(int port, int jpeg_quality) {
    if (impl_->started) {
        return false;
    }
    impl_->jpeg_quality = jpeg_quality;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }
    impl_->wsa_started = true;

    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        WSACleanup();
        impl_->wsa_started = false;
        return false;
    }

    int reuse = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (bind(listen_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listen_socket);
        WSACleanup();
        impl_->wsa_started = false;
        return false;
    }

    if (listen(listen_socket, 1) == SOCKET_ERROR) {
        closesocket(listen_socket);
        WSACleanup();
        impl_->wsa_started = false;
        return false;
    }

    impl_->listen_socket = listen_socket;
    impl_->stop_flag.store(false);
    impl_->thread = std::thread([this]() { impl_->Loop(); });
    impl_->started = true;
    return true;
}

void RemoteServer::Stop() {
    if (!impl_->started) {
        return;
    }
    impl_->stop_flag.store(true);
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    if (impl_->wsa_started) {
        WSACleanup();
        impl_->wsa_started = false;
    }
    impl_->started = false;
}

std::vector<core_action> RemoteServer::PopHumanEvents() {
    std::vector<core_action> result;
    std::lock_guard<std::mutex> lk(impl_->human_mutex);
    while (!impl_->human_queue.empty()) {
        result.push_back(impl_->human_queue.front());
        impl_->human_queue.pop_front();
    }
    return result;
}

void RemoteServer::PushFrame(const uint8_t* bgra, uint32_t width, uint32_t height) {
    if (impl_->stop_flag.load()) {
        return;
    }
    if (bgra == nullptr || width == 0 || height == 0) {
        return;
    }
    std::lock_guard<std::mutex> lk(impl_->frame_mutex);
    if (impl_->frame_queue.size() >= 2) {
        impl_->frame_queue.pop_front();
    }
    FrameSnapshot snap;
    snap.width = width;
    snap.height = height;
    snap.bgra.assign(bgra, bgra + static_cast<size_t>(width) * height * 4);
    impl_->frame_queue.push_back(std::move(snap));
}
