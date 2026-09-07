#pragma once

#include <functional>
#include <string>

namespace ygsoul::ydp {

struct BootstrapConfig {
    std::string mqtt_endpoint;
    std::string mqtt_port;
    std::string ca_version;
    std::string server_time;
    std::string config_expires_at;
    std::string ydp_endpoint;
};

struct ActivateCredentials {
    std::string device_number;
    std::string expires_at;
};

class YdpBootstrap {
public:
    using BootstrapCallback = std::function<void(bool success, const BootstrapConfig& config, const std::string& error)>;
    using ActivateCallback = std::function<void(bool success, const ActivateCredentials& credentials, const std::string& error)>;

    static YdpBootstrap& GetInstance();

    void SetProductKey(const std::string& product_key);
    void SetDeviceId(const std::string& device_id);
    void SetAuthKey(const std::string& auth_key);
    void SetEnvironment(const std::string& environment);

    void Bootstrap(const std::string& ydp_endpoint, BootstrapCallback callback);
    void Activate(const std::string& ydp_endpoint, ActivateCallback callback);

private:
    YdpBootstrap() = default;

    std::string product_key_;
    std::string device_id_;
    std::string auth_key_;
    std::string environment_;
};

}  // namespace ygsoul::ydp
