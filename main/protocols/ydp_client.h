#pragma once

#include "ydp_bootstrap.h"
#include <functional>
#include <string>

namespace ygsoul::ydp {

class YdpClient {
public:
    static YdpClient& GetInstance();

    using ConnectCallback = std::function<void(bool success, const std::string& error)>;

    void SetYdpEndpoint(const std::string& endpoint);
    void SetProductKey(const std::string& product_key);
    void SetDeviceId(const std::string& device_id);
    void SetAuthKey(const std::string& auth_key);

    void Connect(ConnectCallback callback);
    void Disconnect();
    bool IsConnected() const { return connected_; }

    const BootstrapConfig& GetBootstrapConfig() const { return bootstrap_config_; }
    const ActivateCredentials& GetActivateCredentials() const { return activate_credentials_; }

private:
    YdpClient() = default;
    ~YdpClient() = default;

    std::string ydp_endpoint_;
    std::string product_key_;
    std::string device_id_;
    std::string auth_key_;

    BootstrapConfig bootstrap_config_;
    ActivateCredentials activate_credentials_;
    bool connected_ = false;
};

}  // namespace ygsoul::ydp
