#ifndef MODULE_HOST_CORE_REMOTE_SERVER_H
#define MODULE_HOST_CORE_REMOTE_SERVER_H

#include <cstdint>

class RemoteServer {
public:
    RemoteServer();
    ~RemoteServer();

    RemoteServer(const RemoteServer&) = delete;
    RemoteServer& operator=(const RemoteServer&) = delete;

    bool Start(int port, int jpeg_quality);
    void Stop();

    void PushFrame(const uint8_t* bgra, uint32_t width, uint32_t height);

private:
    struct Impl;
    Impl* impl_;
};

#endif
