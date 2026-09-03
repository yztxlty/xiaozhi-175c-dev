#ifndef DEVICE_MANAGEMENT_CLIENT_H
#define DEVICE_MANAGEMENT_CLIENT_H

#include <memory>
#include <functional>
#include <mutex>
#include <string>

class WebSocket;

class DeviceManagementClient {
public:
    DeviceManagementClient();
    ~DeviceManagementClient();

    void Start();
    void Stop();
    bool IsConnected() const;
    bool Send(const std::string& message);
    void OnConnected(std::function<void()> callback);
    void OnMessage(std::function<void(const std::string&)> callback);
    void OnHeartbeat(std::function<void()> callback);

private:
    std::unique_ptr<WebSocket> websocket_;
    std::function<void()> on_connected_;
    std::function<void(const std::string&)> on_message_;
    std::function<void()> on_heartbeat_;
    mutable std::mutex mutex_;
    std::string url_;
    std::string token_;
    int heartbeat_seconds_ = 60;
    bool connected_ = false;
    bool started_ = false;
    bool running_ = false;

    static void Run(void* arg);
    static std::string NormalizeWebSocketUrl(std::string url);
};

#endif
