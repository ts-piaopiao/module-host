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
