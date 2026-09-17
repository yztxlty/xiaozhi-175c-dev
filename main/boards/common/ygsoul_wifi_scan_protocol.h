#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace ygsoul::wifi_scan {
constexpr uint8_t kRequest = 0x05;
constexpr uint8_t kResponse = 0x06;
constexpr size_t kMaxNetworks = 32;
constexpr size_t kMaxPayload = 238;

enum class State { Idle, Scanning, Done, Cancelled, Failed };
enum class Error { None, InvalidRequest, Busy, StaleScan, IndexOutOfRange, Driver, NoMemory, Cancelled };
struct Request {
    std::string id;
    std::string scan;
    std::string op;
    int index = -1;
};
struct AccessPoint {
    std::string ssid; // Original bytes; never trim or infer band from the name.
    std::string bssid; // Twelve lowercase hexadecimal digits.
    int rssi = -127;
    int channel = 0;
    int auth = 0; // ESP-IDF wifi_auth_mode_t, not a guessed security label.
};
inline bool HexString(const std::string& value, size_t size) {
    return value.size() == size && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
inline bool ValidId(const std::string& value) { return HexString(value, 8); }
inline std::string Hex(const std::string& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}
inline int Frequency(int channel) {
    return channel == 14 ? 2484 : (channel >= 1 && channel <= 13 ? 2407 + channel * 5 : 0);
}
inline const char* ErrorName(Error error) {
    switch (error) {
        case Error::InvalidRequest: return "INVALID_REQUEST";
        case Error::Busy: return "BUSY";
        case Error::StaleScan: return "STALE_SCAN";
        case Error::IndexOutOfRange: return "INDEX_OUT_OF_RANGE";
        case Error::Driver: return "WIFI_DRIVER_ERROR";
        case Error::NoMemory: return "NO_MEMORY";
        case Error::Cancelled: return "CANCELLED";
        default: return "NONE";
    }
}
inline const char* StateName(State state) {
    switch (state) {
        case State::Scanning: return "SCANNING";
        case State::Done: return "DONE";
        case State::Cancelled: return "CANCELLED";
        case State::Failed: return "ERROR";
        default: return "IDLE";
    }
}
inline std::string Prefix(const std::string& id, const std::string& scan) {
    return "{\"id\":\"" + (ValidId(id) ? id : "") + "\",\"scan\":\"" +
           (ValidId(scan) ? scan : "") + "\"";
}
inline std::string ErrorReply(const std::string& id, const std::string& scan, Error error) {
    return Prefix(id, scan) + ",\"state\":\"ERROR\",\"error\":\"" + ErrorName(error) + "\"}";
}
inline void AddAccessPoint(std::vector<AccessPoint>& rows, const AccessPoint& ap, bool& truncated) {
    if (ap.ssid.empty() || ap.ssid.size() > 32 || !Frequency(ap.channel) ||
        !HexString(ap.bssid, 12) || ap.rssi < -127 || ap.rssi > 0 || ap.auth < 0 || ap.auth > 255) return;
    auto found = std::find_if(rows.begin(), rows.end(), [&](const AccessPoint& old) { return old.bssid == ap.bssid; });
    if (found != rows.end()) {
        if (ap.rssi > found->rssi) *found = ap;
    } else {
        rows.push_back(ap);
    }
    std::sort(rows.begin(), rows.end(), [](const AccessPoint& a, const AccessPoint& b) {
        return a.rssi == b.rssi ? a.bssid < b.bssid : a.rssi > b.rssi;
    });
    if (rows.size() > kMaxNetworks) { rows.resize(kMaxNetworks); truncated = true; }
}
// One AP per page. Hex SSIDs keep arbitrary 32-byte names below the wire limit,
// including names whose JSON escaping would otherwise expand to 192 bytes.
inline std::string Reply(const std::string& id, const std::string& scan, State state,
                         const std::vector<AccessPoint>& rows, int index, bool truncated,
                         Error error = Error::None) {
    if (!ValidId(id) || !ValidId(scan)) return ErrorReply(id, scan, Error::InvalidRequest);
    if (error != Error::None) return ErrorReply(id, scan, error);
    if (index < -1 || (state == State::Done && index >= 0 && static_cast<size_t>(index) >= rows.size()))
        return ErrorReply(id, scan, Error::IndexOutOfRange);
    std::string result = Prefix(id, scan) + ",\"state\":\"" + StateName(state) +
        "\",\"total\":" + std::to_string(state == State::Done ? rows.size() : 0) +
        ",\"truncated\":" + (truncated ? "true" : "false");
    if (state == State::Done && index >= 0) {
        const auto& ap = rows[index];
        result += ",\"index\":" + std::to_string(index) + ",\"ap\":{\"s\":\"" + Hex(ap.ssid) +
            "\",\"b\":\"" + ap.bssid + "\",\"r\":" + std::to_string(ap.rssi) +
            ",\"c\":" + std::to_string(ap.channel) + ",\"a\":" + std::to_string(ap.auth) + "}";
    }
    result += "}";
    return result.size() <= kMaxPayload ? result : ErrorReply(id, scan, Error::InvalidRequest);
}
} // namespace ygsoul::wifi_scan
