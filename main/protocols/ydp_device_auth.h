#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ygsoul::ydp {

// Shared YGSoul Device Auth v2 canonical bytes. Must match
// device_auth_protocol.DeviceAuthProof.signing_bytes() byte-for-byte:
// 11 UTF-8 lines joined by '\n' and no trailing newline.
inline constexpr int kDeviceAuthVersion = 2;
inline constexpr char kDeviceAuthProtocolLabel[] = "YGSoul-Device-Auth-v2";
inline constexpr char kPurposeActivate[] = "activate";
inline constexpr char kPurposeRefresh[] = "refresh";
inline constexpr char kPurposeProvision[] = "provision";
inline constexpr char kTransportMqtt[] = "mqtt";
inline constexpr char kTransportWebsocket[] = "websocket";

inline std::string DecimalWithoutLeadingZeros(long long value) {
    return std::to_string(value);
}

inline std::string ToLowerHex(const uint8_t* data, size_t length) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0f]);
    }
    return out;
}

inline bool IsLowerHex(std::string_view value, size_t expected_len) {
    if (value.size() != expected_len) {
        return false;
    }
    for (char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

inline bool IsAllowedPurpose(std::string_view purpose) {
    return purpose == kPurposeActivate || purpose == kPurposeRefresh || purpose == kPurposeProvision;
}

inline bool IsAllowedTransport(std::string_view transport) {
    return transport == kTransportMqtt || transport == kTransportWebsocket;
}

inline bool IsValidDeviceId(std::string_view value) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    for (unsigned char c : value) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '.' || c == ':' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

inline bool IsValidProductKey(std::string_view value) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    for (unsigned char c : value) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

inline bool IsValidFirmwareVersion(std::string_view value) {
    if (value.empty() || value.size() > 32) {
        return false;
    }
    if (static_cast<unsigned char>(value.front()) < 0x21 ||
        static_cast<unsigned char>(value.back()) < 0x21) {
        return false;
    }
    for (unsigned char c : value) {
        if (c < 0x20 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

inline bool IsValidKeyVersion(int key_version) {
    return key_version >= 1;
}

inline std::string BuildDeviceAuthSigningMessage(std::string_view device_id,
                                                 std::string_view product_key,
                                                 int key_version,
                                                 std::string_view firmware_version,
                                                 std::string_view device_nonce,
                                                 std::string_view server_time,
                                                 std::string_view challenge,
                                                 std::string_view purpose,
                                                 std::string_view transport) {
    std::string message;
    message.reserve(256);
    message.append(kDeviceAuthProtocolLabel);
    message.push_back('\n');
    message.append(device_id.begin(), device_id.end());
    message.push_back('\n');
    message.append(product_key.begin(), product_key.end());
    message.push_back('\n');
    message.append(DecimalWithoutLeadingZeros(key_version));
    message.push_back('\n');
    message.append(firmware_version.begin(), firmware_version.end());
    message.push_back('\n');
    message.append(device_nonce.begin(), device_nonce.end());
    message.push_back('\n');
    message.append(server_time.begin(), server_time.end());
    message.push_back('\n');
    message.append(challenge.begin(), challenge.end());
    message.push_back('\n');
    message.append(purpose.begin(), purpose.end());
    message.push_back('\n');
    message.append(transport.begin(), transport.end());
    message.push_back('\n');
    message.append(DecimalWithoutLeadingZeros(kDeviceAuthVersion));
    return message;
}

}  // namespace ygsoul::ydp
