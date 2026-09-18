#include "ydp_bootstrap.h"

#include "ydp_device_auth.h"

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
constexpr char kRefreshPath[] = "/ydp/v1/credentials/refresh";

std::string GenerateNonce() {
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    return ToLowerHex(bytes, sizeof(bytes));
}

std::string ComputeHardwareSignature(const std::string& plain) {
    uint8_t digest[32];
    if (esp_hmac_calculate(HMAC_KEY0, plain.data(), plain.size(), digest) != ESP_OK) {
        return {};
    }
    return ToLowerHex(digest, sizeof(digest));
}

std::string CanonicalDeviceId() {
    std::string device_id = SystemInfo::GetMacAddress();
    device_id.erase(std::remove(device_id.begin(), device_id.end(), ':'), device_id.end());
    return device_id;
}

std::string PrintJson(cJSON* object) {
    char* text = cJSON_PrintUnformatted(object);
    std::string payload(text ? text : "{}");
    cJSON_free(text);
    return payload;
}

cJSON* BuildAuthObject(const std::string& device_id, const std::string& product_key, int key_version,
                       const std::string& firmware_version, const std::string& device_nonce,
                       const std::string& purpose, const std::string& transport,
                       const std::string* timestamp, const std::string* challenge,
                       const std::string* signature) {
    cJSON* request = cJSON_CreateObject();
    cJSON_AddNumberToObject(request, "authVersion", kDeviceAuthVersion);
    cJSON_AddStringToObject(request, "deviceId", device_id.c_str());
    cJSON_AddStringToObject(request, "productKey", product_key.c_str());
    cJSON_AddNumberToObject(request, "keyVersion", key_version);
    cJSON_AddStringToObject(request, "firmwareVersion", firmware_version.c_str());
    cJSON_AddStringToObject(request, "deviceNonce", device_nonce.c_str());
    cJSON_AddStringToObject(request, "purpose", purpose.c_str());
    cJSON_AddStringToObject(request, "transport", transport.c_str());
    if (timestamp && challenge && signature) {
        cJSON_AddNumberToObject(request, "timestamp", std::stoll(*timestamp));
        cJSON_AddStringToObject(request, "challenge", challenge->c_str());
        cJSON_AddStringToObject(request, "signature", signature->c_str());
    }
    return request;
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

void YdpBootstrap::SetAuthKey(const std::string&) {
    // Device Auth v2 signs with hardware HMAC_KEY0. Plaintext factory keys are never imported.
}

void YdpBootstrap::SetKeyVersion(int key_version) {
    if (IsValidKeyVersion(key_version)) {
        key_version_ = key_version;
    }
}

void YdpBootstrap::SetTransport(const std::string& transport) {
    if (IsAllowedTransport(transport)) {
        transport_ = transport;
    }
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
    Prove(ydp_endpoint, kPurposeActivate, kActivatePath, callback);
}

void YdpBootstrap::Refresh(const std::string& ydp_endpoint, ActivateCallback callback) {
    Prove(ydp_endpoint, kPurposeRefresh, kRefreshPath, callback);
}

void YdpBootstrap::Prove(const std::string& ydp_endpoint, const char* purpose, const char* path,
                         ActivateCallback callback) {
    const std::string device_id = device_id_.empty() ? CanonicalDeviceId() : device_id_;
    const std::string product_key = product_key_.empty() ? "ESP32S3" : product_key_;
    const std::string firmware_version = esp_app_get_description()->version;
    const std::string device_nonce = GenerateNonce();
    const std::string transport = transport_.empty() ? kTransportMqtt : transport_;
    const int key_version = key_version_;

    if (!IsValidDeviceId(device_id) || !IsValidProductKey(product_key) ||
        !IsValidKeyVersion(key_version) || !IsValidFirmwareVersion(firmware_version) ||
        !IsAllowedPurpose(purpose) || !IsAllowedTransport(transport)) {
        if (callback) callback(false, {}, "auth_fields_invalid");
        return;
    }

    cJSON* challenge_request =
        BuildAuthObject(device_id, product_key, key_version, firmware_version, device_nonce, purpose,
                        transport, nullptr, nullptr, nullptr);
    std::string challenge_payload = PrintJson(challenge_request);
    cJSON_Delete(challenge_request);

    std::string challenge_response;
    std::string error;
    if (!PostJson(ydp_endpoint + kChallengePath, std::move(challenge_payload), challenge_response,
                  error)) {
        if (callback) callback(false, {}, error);
        return;
    }
    cJSON* challenge_json = cJSON_Parse(challenge_response.c_str());
    auto* challenge = challenge_json ? cJSON_GetObjectItem(challenge_json, "challenge") : nullptr;
    auto* server_time = challenge_json ? cJSON_GetObjectItem(challenge_json, "serverTime") : nullptr;
    auto* auth_version = challenge_json ? cJSON_GetObjectItem(challenge_json, "authVersion") : nullptr;
    auto* response_key_version =
        challenge_json ? cJSON_GetObjectItem(challenge_json, "keyVersion") : nullptr;
    if (!cJSON_IsString(challenge) || !cJSON_IsNumber(server_time) ||
        !IsLowerHex(challenge->valuestring, 64) ||
        (auth_version &&
         !(cJSON_IsNumber(auth_version) && auth_version->valueint == kDeviceAuthVersion)) ||
        (response_key_version &&
         !(cJSON_IsNumber(response_key_version) && response_key_version->valueint == key_version))) {
        if (challenge_json) cJSON_Delete(challenge_json);
        if (callback) callback(false, {}, "challenge_response_invalid");
        return;
    }
    const std::string challenge_value = challenge->valuestring;
    const std::string timestamp = DecimalWithoutLeadingZeros(static_cast<long long>(server_time->valuedouble));
    cJSON_Delete(challenge_json);

    const std::string signing_message =
        BuildDeviceAuthSigningMessage(device_id, product_key, key_version, firmware_version,
                                      device_nonce, timestamp, challenge_value, purpose, transport);
    const std::string signature = ComputeHardwareSignature(signing_message);
    if (signature.empty()) {
        if (callback) callback(false, {}, "efuse_hmac_unavailable");
        return;
    }

    cJSON* proof_request =
        BuildAuthObject(device_id, product_key, key_version, firmware_version, device_nonce, purpose,
                        transport, &timestamp, &challenge_value, &signature);
    std::string proof_payload = PrintJson(proof_request);
    cJSON_Delete(proof_request);

    std::string proof_response;
    if (!PostJson(ydp_endpoint + path, std::move(proof_payload), proof_response, error)) {
        if (callback) callback(false, {}, error);
        return;
    }
    cJSON* response_json = cJSON_Parse(proof_response.c_str());
    if (!response_json) {
        if (callback) callback(false, {}, "activation_response_invalid");
        return;
    }
    ActivateCredentials credentials;
    auto get_string = [&](const char* key) -> std::string {
        cJSON* item = cJSON_GetObjectItem(response_json, key);
        return cJSON_IsString(item) ? item->valuestring : "";
    };
    // MQTT voucher uses deviceNumber; websocket voucher historically used deviceId.
    credentials.device_number = get_string("deviceNumber");
    if (credentials.device_number.empty()) {
        credentials.device_number = get_string("deviceId");
    }
    auto* expires_at = cJSON_GetObjectItem(response_json, "expireAt");
    if (cJSON_IsNumber(expires_at))
        credentials.expires_at = DecimalWithoutLeadingZeros(static_cast<long long>(expires_at->valuedouble));

    cJSON_Delete(response_json);

    if (credentials.device_number != device_id) {
        ESP_LOGE(kTag, "Activate response device mismatch got=%s want=%s",
                 credentials.device_number.c_str(), device_id.c_str());
        if (callback) callback(false, {}, "activation_response_invalid");
        return;
    }

    ESP_LOGI(kTag, "Secure %s succeeded for device %s", purpose, device_id.c_str());
    if (callback) callback(true, credentials, "");
}

}  // namespace ygsoul::ydp
