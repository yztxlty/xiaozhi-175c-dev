#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace ygsoul::ydp {

constexpr char kSpecVersion[] = "1.0";
constexpr char kProductKey[] = "ESP32S3";

struct YdpEnvelope {
    std::string spec_version;
    std::string message_id;
    std::string request_id;
    std::string device_id;
    std::string product_key;
    std::string message_type;
    std::string occurred_at;
    std::string expires_at;
    uint64_t sequence;
    std::map<std::string, std::string> payload;
    std::string signature;
};

std::string GenerateMessageId();
std::string GenerateRequestId(const std::string& prefix);
std::string FormatTimestamp();
std::string FormatExpiresAt(int seconds_from_now);

std::string CanonicalJson(const std::map<std::string, std::string>& fields);
std::string SignMessage(const std::map<std::string, std::string>& fields, const std::string& secret);
std::string Base64UrlEncode(const uint8_t* data, size_t length);
std::string Base64UrlEncode(const std::string& data);

std::string SerializeYdpMessage(const YdpEnvelope& envelope, const std::string& secret);
bool DeserializeYdpMessage(const std::string& json, YdpEnvelope& envelope);

bool VerifySignature(const YdpEnvelope& envelope, const std::string& secret);

}  // namespace ygsoul::ydp
