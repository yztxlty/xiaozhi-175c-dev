#include "ydp_protocol.h"

#include <chrono>
#include <cstring>
#include <random>
#include <sstream>

#include "cJSON.h"
#include "esp_random.h"
#include "mbedtls/md.h"

namespace ygsoul::ydp {

namespace {
constexpr char kTimestampFormat[] = "%Y-%m-%d %H:%M:%S";

std::string FormatTimePoint(std::chrono::system_clock::time_point tp) {
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    struct tm tm_val;
    localtime_r(&time_t_val, &tm_val);
    char buffer[32];
    strftime(buffer, sizeof(buffer), kTimestampFormat, &tm_val);
    return std::string(buffer);
}

std::string GenerateRandomHex(size_t bytes) {
    std::string result;
    result.reserve(bytes * 2);
    std::vector<uint8_t> random_bytes(bytes);
    esp_fill_random(random_bytes.data(), bytes);
    for (uint8_t byte : random_bytes) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", byte);
        result.append(hex);
    }
    return result;
}
}  // namespace

std::string GenerateMessageId() {
    return "msg_" + GenerateRandomHex(16);
}

std::string GenerateRequestId(const std::string& prefix) {
    return "req_" + prefix + "_" + GenerateRandomHex(4);
}

std::string FormatTimestamp() {
    return FormatTimePoint(std::chrono::system_clock::now());
}

std::string FormatExpiresAt(int seconds_from_now) {
    auto now = std::chrono::system_clock::now();
    auto expires = now + std::chrono::seconds(seconds_from_now);
    return FormatTimePoint(expires);
}

std::string Base64UrlEncode(const uint8_t* data, size_t length) {
    static const char kBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((length + 2) / 3) * 4);

    for (size_t i = 0; i < length; i += 3) {
        uint32_t triple = (data[i] << 16);
        if (i + 1 < length) triple |= (data[i + 1] << 8);
        if (i + 2 < length) triple |= data[i + 2];

        result.push_back(kBase64Chars[(triple >> 18) & 0x3F]);
        result.push_back(kBase64Chars[(triple >> 12) & 0x3F]);
        if (i + 1 < length) result.push_back(kBase64Chars[(triple >> 6) & 0x3F]);
        if (i + 2 < length) result.push_back(kBase64Chars[triple & 0x3F]);
    }

    while (result.size() % 4 != 0) result.push_back('=');

    for (char& c : result) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }

    size_t pad = result.find_last_not_of('=');
    if (pad != std::string::npos) {
        result = result.substr(0, pad + 1);
    } else {
        result.clear();
    }

    return result;
}

std::string Base64UrlEncode(const std::string& data) {
    return Base64UrlEncode(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::string CanonicalJson(const std::map<std::string, std::string>& fields) {
    cJSON* root = cJSON_CreateObject();
    for (const auto& [key, value] : fields) {
        cJSON_AddStringToObject(root, key.c_str(), value.c_str());
    }
    char* json_str = cJSON_PrintUnformatted(root);
    std::string result(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return result;
}

std::string SignMessage(const std::map<std::string, std::string>& fields, const std::string& secret) {
    std::string canonical = CanonicalJson(fields);

    uint8_t digest[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const uint8_t*>(secret.data()), secret.size());
    mbedtls_md_hmac_update(&ctx, reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size());
    mbedtls_md_hmac_finish(&ctx, digest);
    mbedtls_md_free(&ctx);

    return Base64UrlEncode(digest, sizeof(digest));
}

std::string SerializeYdpMessage(const YdpEnvelope& envelope, const std::string& secret) {
    std::map<std::string, std::string> fields;
    fields["specVersion"] = envelope.spec_version;
    fields["messageId"] = envelope.message_id;
    fields["requestId"] = envelope.request_id;
    fields["deviceId"] = envelope.device_id;
    fields["productKey"] = envelope.product_key;
    fields["messageType"] = envelope.message_type;
    fields["occurredAt"] = envelope.occurred_at;
    if (!envelope.expires_at.empty()) fields["expiresAt"] = envelope.expires_at;
    fields["sequence"] = std::to_string(envelope.sequence);

    cJSON* payload_json = cJSON_CreateObject();
    for (const auto& [key, value] : envelope.payload) {
        cJSON_AddStringToObject(payload_json, key.c_str(), value.c_str());
    }
    char* payload_str = cJSON_PrintUnformatted(payload_json);
    fields["payload"] = std::string(payload_str);
    cJSON_free(payload_str);
    cJSON_Delete(payload_json);

    std::string signature = SignMessage(fields, secret);
    fields["signature"] = signature;

    cJSON* root = cJSON_CreateObject();
    for (const auto& [key, value] : fields) {
        if (key == "payload") {
            cJSON_AddRawToObject(root, key.c_str(), value.c_str());
        } else {
            cJSON_AddStringToObject(root, key.c_str(), value.c_str());
        }
    }
    char* json_str = cJSON_PrintUnformatted(root);
    std::string result(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);

    return result;
}

bool DeserializeYdpMessage(const std::string& json, YdpEnvelope& envelope) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return false;

    auto get_string = [&](const char* key) -> std::string {
        cJSON* item = cJSON_GetObjectItem(root, key);
        return cJSON_IsString(item) ? item->valuestring : "";
    };

    envelope.spec_version = get_string("specVersion");
    envelope.message_id = get_string("messageId");
    envelope.request_id = get_string("requestId");
    envelope.device_id = get_string("deviceId");
    envelope.product_key = get_string("productKey");
    envelope.message_type = get_string("messageType");
    envelope.occurred_at = get_string("occurredAt");
    envelope.expires_at = get_string("expiresAt");
    envelope.signature = get_string("signature");

    cJSON* seq = cJSON_GetObjectItem(root, "sequence");
    envelope.sequence = cJSON_IsNumber(seq) ? static_cast<uint64_t>(seq->valuedouble) : 0;

    cJSON* payload = cJSON_GetObjectItem(root, "payload");
    envelope.payload.clear();
    if (cJSON_IsObject(payload)) {
        cJSON* child = payload->child;
        while (child) {
            if (cJSON_IsString(child)) {
                envelope.payload[child->string] = child->valuestring;
            }
            child = child->next;
        }
    }

    cJSON_Delete(root);
    return true;
}

bool VerifySignature(const YdpEnvelope& envelope, const std::string& secret) {
    std::map<std::string, std::string> fields;
    fields["specVersion"] = envelope.spec_version;
    fields["messageId"] = envelope.message_id;
    fields["requestId"] = envelope.request_id;
    fields["deviceId"] = envelope.device_id;
    fields["productKey"] = envelope.product_key;
    fields["messageType"] = envelope.message_type;
    fields["occurredAt"] = envelope.occurred_at;
    if (!envelope.expires_at.empty()) fields["expiresAt"] = envelope.expires_at;
    fields["sequence"] = std::to_string(envelope.sequence);

    cJSON* payload_json = cJSON_CreateObject();
    for (const auto& [key, value] : envelope.payload) {
        cJSON_AddStringToObject(payload_json, key.c_str(), value.c_str());
    }
    char* payload_str = cJSON_PrintUnformatted(payload_json);
    fields["payload"] = std::string(payload_str);
    cJSON_free(payload_str);
    cJSON_Delete(payload_json);

    std::string expected = SignMessage(fields, secret);
    return expected == envelope.signature;
}

}  // namespace ygsoul::ydp
