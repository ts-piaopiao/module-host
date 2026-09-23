#ifndef MODULE_HOST_CORE_REMOTE_SERVER_H
#define MODULE_HOST_CORE_REMOTE_SERVER_H

class RemoteServer {
public:
    RemoteServer();
    ~RemoteServer();

    RemoteServer(const RemoteServer&) = delete;
    RemoteServer& operator=(const RemoteServer&) = delete;

    bool Start(int port);
    void Stop();

private:
    struct Impl;
    Impl* impl_;
};

#endif
