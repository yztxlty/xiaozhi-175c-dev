#include "ydp_bootstrap.h"

#include <algorithm>
#include <cstring>

#include "board.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_hmac.h"
#include "esp_log.h"
#include "esp_random.h"
#include "system_info.h"

namespace ygsoul::ydp {

namespace {
constexpr char kTag[] = "YdpBootstrap";
constexpr char kChallengePath[] = "/ydp/v1/activation/challenge";
constexpr char kActivatePath[] = "/ydp/v1/activate";

std::string GenerateNonce() {
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    std::string result;
    result.reserve(sizeof(bytes) * 2);
    for (uint8_t byte : bytes) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", byte);
        result.append(hex);
    }
    return result;
}

std::string ComputeActivationSignature(const std::string& plain) {
    uint8_t digest[32];
    if (esp_hmac_calculate(HMAC_KEY0, plain.data(), plain.size(), digest) != ESP_OK) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string signature;
    signature.reserve(sizeof(digest) * 2);
    for (uint8_t byte : digest) {
        signature.push_back(kHex[byte >> 4]);
        signature.push_back(kHex[byte & 0x0f]);
    }
    return signature;
}

std::string CanonicalDeviceId() {
    std::string device_id = SystemInfo::GetMacAddress();
    device_id.erase(std::remove(device_id.begin(), device_id.end(), ':'), device_id.end());
    return device_id;
}

bool PostJson(const std::string& url, std::string payload, std::string& response, std::string& error) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetContent(std::move(payload));
    if (!http->Open("POST", url)) {
        error = "http_open_failed";
        return false;
    }
    const int status = http->GetStatusCode();
    response = http->ReadAll();
    http->Close();
    if (status < 200 || status >= 300) {
        error = "http_status_" + std::to_string(status);
        return false;
    }
    return true;
}
}  // namespace

YdpBootstrap& YdpBootstrap::GetInstance() {
    static YdpBootstrap instance;
    return instance;
}

void YdpBootstrap::SetProductKey(const std::string& product_key) {
    product_key_ = product_key;
}

void YdpBootstrap::SetDeviceId(const std::string& device_id) {
    device_id_ = device_id;
}

void YdpBootstrap::SetAuthKey(const std::string& auth_key) {
    auth_key_ = auth_key;
}

void YdpBootstrap::SetEnvironment(const std::string& environment) {
    environment_ = environment;
}

void YdpBootstrap::Bootstrap(const std::string& ydp_endpoint, BootstrapCallback callback) {
    std::string response;
    std::string error;
    if (!PostJson(ydp_endpoint + "/ydp/v1/bootstrap", "{}", response, error)) {
        if (callback) callback(false, {}, error);
        return;
    }
    cJSON* json = cJSON_Parse(response.c_str());
    if (!json) {
        if (callback) callback(false, {}, "bootstrap_response_invalid");
        return;
    }
    auto string_value = [json](const char* key) {
        auto* item = cJSON_GetObjectItem(json, key);
        return cJSON_IsString(item) ? std::string(item->valuestring) : std::string();
    };
    BootstrapConfig config;
    config.mqtt_endpoint = string_value("mqttHost");
    config.ca_version = string_value("caVersion");
    config.ydp_endpoint = ydp_endpoint;
    auto* port = cJSON_GetObjectItem(json, "mqttPort");
    if (cJSON_IsNumber(port)) config.mqtt_port = std::to_string(port->valueint);
    cJSON_Delete(json);
    if (callback) callback(true, config, "");
}

void YdpBootstrap::Activate(const std::string& ydp_endpoint, ActivateCallback callback) {
    const std::string device_id = CanonicalDeviceId();
    const std::string product_key = product_key_.empty() ? "ESP32S3" : product_key_;
    const std::string firmware_version = esp_app_get_description()->version;
    const std::string device_nonce = GenerateNonce();

    cJSON* challenge_request = cJSON_CreateObject();
    cJSON_AddStringToObject(challenge_request, "deviceId", device_id.c_str());
    cJSON_AddStringToObject(challenge_request, "productKey", product_key.c_str());
    cJSON_AddStringToObject(challenge_request, "firmwareVersion", firmware_version.c_str());
    cJSON_AddStringToObject(challenge_request, "deviceNonce", device_nonce.c_str());
    char* challenge_text = cJSON_PrintUnformatted(challenge_request);
    std::string challenge_payload(challenge_text ? challenge_text : "{}");
    cJSON_free(challenge_text);
    cJSON_Delete(challenge_request);

    std::string challenge_response;
    std::string error;
    if (!PostJson(ydp_endpoint + kChallengePath, challenge_payload, challenge_response, error)) {
        if (callback) callback(false, {}, error);
        return;
    }
    cJSON* challenge_json = cJSON_Parse(challenge_response.c_str());
    auto* challenge = challenge_json ? cJSON_GetObjectItem(challenge_json, "challenge") : nullptr;
    auto* server_time = challenge_json ? cJSON_GetObjectItem(challenge_json, "serverTime") : nullptr;
    if (!cJSON_IsString(challenge) || !cJSON_IsNumber(server_time)) {
        if (challenge_json) cJSON_Delete(challenge_json);
        if (callback) callback(false, {}, "challenge_response_invalid");
        return;
    }
    const std::string challenge_value = challenge->valuestring;
    const std::string timestamp = std::to_string(static_cast<long long>(server_time->valuedouble));
    cJSON_Delete(challenge_json);

    const std::string signature = ComputeActivationSignature(
        device_id + "\n" + product_key + "\n" + firmware_version + "\n" + device_nonce + "\n" + timestamp + "\n" + challenge_value);
    if (signature.empty()) {
        if (callback) callback(false, {}, "efuse_hmac_unavailable");
        return;
    }

    cJSON* activation_request = cJSON_CreateObject();
    cJSON_AddStringToObject(activation_request, "deviceId", device_id.c_str());
    cJSON_AddStringToObject(activation_request, "productKey", product_key.c_str());
    cJSON_AddStringToObject(activation_request, "firmwareVersion", firmware_version.c_str());
    cJSON_AddStringToObject(activation_request, "deviceNonce", device_nonce.c_str());
    cJSON_AddNumberToObject(activation_request, "timestamp", std::stoll(timestamp));
    cJSON_AddStringToObject(activation_request, "challenge", challenge_value.c_str());
    cJSON_AddStringToObject(activation_request, "signature", signature.c_str());
    char* activation_text = cJSON_PrintUnformatted(activation_request);
    std::string activation_payload(activation_text ? activation_text : "{}");
    cJSON_free(activation_text);
    cJSON_Delete(activation_request);

    std::string activation_response;
    if (!PostJson(ydp_endpoint + kActivatePath, activation_payload, activation_response, error)) {
        if (callback) callback(false, {}, error);
        return;
    }
    cJSON* response_json = cJSON_Parse(activation_response.c_str());
    ActivateCredentials credentials;
    auto get_string = [&](const char* key) -> std::string {
        cJSON* item = cJSON_GetObjectItem(response_json, key);
        return cJSON_IsString(item) ? item->valuestring : "";
    };
    credentials.device_number = get_string("deviceNumber");
    auto* expires_at = cJSON_GetObjectItem(response_json, "expireAt");
    if (cJSON_IsNumber(expires_at)) credentials.expires_at = std::to_string(static_cast<long long>(expires_at->valuedouble));

    cJSON_Delete(response_json);

    if (credentials.device_number != device_id) {
        if (callback) callback(false, {}, "activation_response_invalid");
        return;
    }

    ESP_LOGI(kTag, "Secure activation succeeded for device %s", device_id.c_str());
    if (callback) callback(true, credentials, "");
}

}  // namespace ygsoul::ydp
