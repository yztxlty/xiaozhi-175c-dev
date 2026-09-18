#pragma once

// YGSoul universal BLE Wi-Fi scan protocol (A100_OPEN spec V1.0).
// All products: ESP32S3 / T5(A100) / future hardware use the same commands.
// 0x16 START_WIFI_SCAN  request {"scanId":"s..."}  reply SCANNING|FAILED
// 0x17 GET_WIFI_SCAN_RESULT request {"scanId":"...","index":N} reply SCANNING|READY|FAILED
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace ygsoul::wifi_scan {
constexpr uint8_t kRequest = 0x16;   // START_WIFI_SCAN
constexpr uint8_t kResponse = 0x17;  // GET_WIFI_SCAN_RESULT (same cmd for reply)
constexpr size_t kMaxNetworks = 30;
constexpr size_t kMaxPayload = 238;

enum class State { Idle, Scanning, Ready, Failed };
enum class Error {
    None,
    InvalidRequest,
    DeviceBusy,
    ScanFailed,
    ScanTimeout,
    NoMemory,
    ScanExpired,
    IndexOutOfRange,
};

struct Request {
    std::string scan_id;  // 1..16 ASCII alnum; empty for invalid
    int index = 0;        // 0..29 for GET
    bool start = false;   // true when command is 0x16
};

struct AccessPoint {
    std::string ssid;  // raw bytes 1..32
    int rssi = 0;      // null represented as has_rssi=false
    bool has_rssi = true;
    int channel = 0;   // 1..14
    int auth = 0;      // ESP-IDF wifi_auth_mode_t
};

inline bool ValidScanId(const std::string& value) {
    if (value.empty() || value.size() > 16) return false;
    for (char c : value) {
        if (!std::isalnum(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

inline const char* SecurityName(int auth) {
    // 0 OPEN 1 WEP 2 WPA 3 WPA2 4 WPA_WPA2 5 ENTERPRISE 6 WPA3 7 WPA2_WPA3
    switch (auth) {
        case 0: return "OPEN";
        case 1: return "WEP";
        case 2: return "WPA";
        case 3: return "WPA2";
        case 4: return "WPA_WPA2";
        case 5: return "UNKNOWN";
        case 6: return "WPA3";
        case 7: return "WPA2_WPA3";
        default: return "UNKNOWN";
    }
}

inline const char* ErrorName(Error error) {
    switch (error) {
        case Error::InvalidRequest: return "INVALID_REQUEST";
        case Error::DeviceBusy: return "DEVICE_BUSY";
        case Error::ScanFailed: return "SCAN_FAILED";
        case Error::ScanTimeout: return "SCAN_TIMEOUT";
        case Error::NoMemory: return "NO_MEMORY";
        case Error::ScanExpired: return "SCAN_EXPIRED";
        case Error::IndexOutOfRange: return "INDEX_OUT_OF_RANGE";
        default: return "NONE";
    }
}

inline std::string Base64(const std::string& bytes) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < bytes.size()) {
        uint32_t n = (static_cast<unsigned char>(bytes[i]) << 16) |
                     (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                     static_cast<unsigned char>(bytes[i + 2]);
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(table[(n >> 6) & 63]);
        out.push_back(table[n & 63]);
        i += 3;
    }
    if (i < bytes.size()) {
        uint32_t n = static_cast<unsigned char>(bytes[i]) << 16;
        bool second = i + 1 < bytes.size();
        if (second) n |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(second ? table[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

inline int Frequency(int channel) {
    return channel == 14 ? 2484 : (channel >= 1 && channel <= 13 ? 2407 + channel * 5 : 0);
}

inline std::string FailedReply(const std::string& scan_id, Error error) {
    std::string body = "{\"status\":\"FAILED\"";
    if (ValidScanId(scan_id)) body += ",\"scanId\":\"" + scan_id + "\"";
    body += ",\"code\":\"";
    body += ErrorName(error);
    body += "\"}";
    return body.size() <= kMaxPayload ? body : std::string("{\"status\":\"FAILED\",\"code\":\"INVALID_REQUEST\"}");
}

inline void AddAccessPoint(std::vector<AccessPoint>& rows, const AccessPoint& ap, bool& truncated) {
    if (ap.ssid.empty() || ap.ssid.size() > 32 || !Frequency(ap.channel)) return;
    auto found = std::find_if(rows.begin(), rows.end(), [&](const AccessPoint& old) { return old.ssid == ap.ssid; });
    if (found != rows.end()) {
        if (ap.has_rssi && (!found->has_rssi || ap.rssi > found->rssi)) *found = ap;
    } else {
        rows.push_back(ap);
    }
    std::sort(rows.begin(), rows.end(), [](const AccessPoint& a, const AccessPoint& b) {
        const int ra = a.has_rssi ? a.rssi : -999;
        const int rb = b.has_rssi ? b.rssi : -999;
        return ra == rb ? a.ssid < b.ssid : ra > rb;
    });
    if (rows.size() > kMaxNetworks) {
        rows.resize(kMaxNetworks);
        truncated = true;
    }
}

// Unified V1.0 reply builder for both START and GET.
inline std::string Reply(const std::string& scan_id, State state,
                         const std::vector<AccessPoint>& rows, int index,
                         Error error = Error::None) {
    if (error != Error::None) return FailedReply(scan_id, error);
    if (!ValidScanId(scan_id)) return FailedReply(scan_id, Error::InvalidRequest);
    std::string body = "{\"status\":\"";
    if (state == State::Scanning) {
        body += "SCANNING\",\"scanId\":\"" + scan_id + "\"}";
        return body.size() <= kMaxPayload ? body : FailedReply(scan_id, Error::InvalidRequest);
    }
    if (state != State::Ready) return FailedReply(scan_id, Error::ScanFailed);
    const int total = static_cast<int>(rows.size());
    if (index < 0 || (total > 0 && index >= total) || (total == 0 && index != 0))
        return FailedReply(scan_id, Error::IndexOutOfRange);
    body += "READY\",\"scanId\":\"" + scan_id + "\",\"index\":" + std::to_string(index) +
            ",\"total\":" + std::to_string(total);
    if (total == 0) {
        body += ",\"ap\":null}";
    } else {
        const auto& ap = rows[static_cast<size_t>(index)];
        body += ",\"ap\":{\"ssidBase64\":\"" + Base64(ap.ssid) + "\",\"rssi\":";
        if (ap.has_rssi) body += std::to_string(ap.rssi);
        else body += "null";
        body += ",\"channel\":" + std::to_string(ap.channel) +
                ",\"security\":\"" + SecurityName(ap.auth) + "\"}}";
    }
    return body.size() <= kMaxPayload ? body : FailedReply(scan_id, Error::InvalidRequest);
}
}  // namespace ygsoul::wifi_scan
