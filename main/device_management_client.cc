#include "device_management_client.h"

#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>

#include <algorithm>
#include <exception>
#include <string>

#define TAG "DeviceManagement"

std::string DeviceManagementClient::NormalizeWebSocketUrl(std::string url) {
    if (url.rfind("https://", 0) == 0) {
        url.replace(0, 8, "wss://");
    } else if (url.rfind("http://", 0) == 0) {
        url.replace(0, 7, "ws://");
    } else if (!url.empty() && url.find("://") == std::string::npos) {
        url = "wss://" + url;
    }
    return url;
}

DeviceManagementClient::DeviceManagementClient() {
    Settings settings("management", false);
    url_ = NormalizeWebSocketUrl(settings.GetString("url"));
    token_ = settings.GetString("token");
    heartbeat_seconds_ = settings.GetInt("heartbeat", 60);
}

DeviceManagementClient::~DeviceManagementClient() {
    Stop();
}

void DeviceManagementClient::OnConnected(std::function<void()> callback) {
    on_connected_ = std::move(callback);
}

void DeviceManagementClient::OnMessage(std::function<void(const std::string&)> callback) {
    on_message_ = std::move(callback);
}

void DeviceManagementClient::OnHeartbeat(std::function<void()> callback) {
    on_heartbeat_ = std::move(callback);
}

void DeviceManagementClient::Start() {
    if (started_ || url_.empty() || token_.empty()) {
        return;
    }
    started_ = true;
    running_ = true;
    ESP_LOGI(TAG, "Management url=%s", url_.c_str());
    xTaskCreate(Run, "device_mgmt", 8192, this, 2, nullptr);
}

void DeviceManagementClient::Stop() {
    running_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    websocket_.reset();
}

bool DeviceManagementClient::IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_ && websocket_ != nullptr && websocket_->IsConnected();
}

bool DeviceManagementClient::Send(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_ && websocket_ != nullptr && websocket_->Send(message);
}

void DeviceManagementClient::Run(void* arg) {
    auto* client = static_cast<DeviceManagementClient*>(arg);
    try {
        int retry_seconds = 2;
        while (client->running_) {
            auto network = Board::GetInstance().GetNetwork();
            auto websocket = network->CreateWebSocket(2);
            websocket->SetHeader("Authorization", ("Bearer " + client->token_).c_str());
            websocket->SetHeader("Protocol-Version", "1");
            websocket->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
            websocket->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
            websocket->OnData([client](const char* data, size_t len, bool binary) {
                if (!binary && client->on_message_) {
                    client->on_message_(std::string(data, len));
                }
            });
            websocket->OnDisconnected([client]() {
                std::lock_guard<std::mutex> lock(client->mutex_);
                client->connected_ = false;
            });
            if (!client->running_) {
                break;
            }
            if (!websocket->Connect(client->url_.c_str())) {
                ESP_LOGW(TAG, "Management connect failed url=%s, retry in %d seconds",
                         client->url_.c_str(), retry_seconds);
                vTaskDelay(pdMS_TO_TICKS(retry_seconds * 1000));
                retry_seconds = std::min(retry_seconds * 2, 60);
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(client->mutex_);
                client->websocket_ = std::move(websocket);
                client->connected_ = true;
            }
            retry_seconds = 2;
            client->Send("{\"type\":\"hello\",\"channel\":\"management\"}");
            if (client->on_connected_) {
                client->on_connected_();
            }
            while (client->running_ && client->IsConnected()) {
                vTaskDelay(pdMS_TO_TICKS(std::max(client->heartbeat_seconds_, 10) * 1000));
                if (!client->running_) {
                    break;
                }
                if (!client->Send("{\"type\":\"heartbeat\"}")) {
                    std::lock_guard<std::mutex> lock(client->mutex_);
                    client->connected_ = false;
                } else if (client->on_heartbeat_) {
                    client->on_heartbeat_();
                }
            }
            {
                std::lock_guard<std::mutex> lock(client->mutex_);
                client->websocket_.reset();
            }
            ESP_LOGW(TAG, "Management disconnected, reconnecting");
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Management task exception: %s", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "Management task unknown exception");
    }
    vTaskDelete(NULL);
}
