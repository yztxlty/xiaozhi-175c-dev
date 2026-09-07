#include "ydp_client.h"
#include "ydp_protocol.h"
#include <esp_log.h>

#define TAG "YDP"

namespace ygsoul::ydp {

YdpClient& YdpClient::GetInstance() {
    static YdpClient instance;
    return instance;
}

void YdpClient::SetYdpEndpoint(const std::string& endpoint) {
    ydp_endpoint_ = endpoint;
}

void YdpClient::SetProductKey(const std::string& product_key) {
    product_key_ = product_key;
    YdpBootstrap::GetInstance().SetProductKey(product_key);
}

void YdpClient::SetDeviceId(const std::string& device_id) {
    device_id_ = device_id;
    YdpBootstrap::GetInstance().SetDeviceId(device_id);
}

void YdpClient::SetAuthKey(const std::string& auth_key) {
    auth_key_ = auth_key;
    YdpBootstrap::GetInstance().SetAuthKey(auth_key);
}

void YdpClient::Connect(ConnectCallback callback) {
    if (connected_) {
        ESP_LOGW(TAG, "YDP client already connected");
        if (callback) callback(true, "");
        return;
    }

    ESP_LOGI(TAG, "Starting YDP Bootstrap...");
    YdpBootstrap::GetInstance().Bootstrap(ydp_endpoint_, [this, callback](bool success, const BootstrapConfig& config, const std::string& error) {
        if (!success) {
            ESP_LOGE(TAG, "YDP Bootstrap failed: %s", error.c_str());
            if (callback) callback(false, "Bootstrap failed: " + error);
            return;
        }

        ESP_LOGI(TAG, "YDP Bootstrap succeeded: mqtt_endpoint=%s:%s", config.mqtt_endpoint.c_str(), config.mqtt_port.c_str());
        bootstrap_config_ = config;

        ESP_LOGI(TAG, "Starting YDP secure activation...");
        YdpBootstrap::GetInstance().Activate(ydp_endpoint_, [this, callback](bool success, const ActivateCredentials& credentials, const std::string& error) {
            if (!success) {
                ESP_LOGE(TAG, "YDP Activate failed: %s", error.c_str());
                if (callback) callback(false, "Activate failed: " + error);
                return;
            }

            ESP_LOGI(TAG, "YDP Activate succeeded for device %s", credentials.device_number.c_str());
            activate_credentials_ = credentials;
            connected_ = true;

            if (callback) callback(true, "");
        });
    });
}

void YdpClient::Disconnect() {
    connected_ = false;
    ESP_LOGI(TAG, "YDP client disconnected");
}

}  // namespace ygsoul::ydp
