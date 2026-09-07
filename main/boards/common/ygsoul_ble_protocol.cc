#include "ygsoul_ble_protocol.h"

#include <algorithm>

namespace ygsoul::ble {

void PairingReceipt::Begin(const std::string& token, const std::string& ssid) {
    std::string session_id;
    if (token.size() > 37 && token[36] == '.') {
        bool valid = true;
        for (size_t i = 0; i < 36; ++i) {
            const char c = token[i];
            const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
            valid &= separator ? c == '-' :
                (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }
        if (valid) session_id = token.substr(0, 36);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    target_ssid_ = ssid;
    pending_session_id_ = session_id;
    completed_session_id_.clear();
    waiting_ = false;
}

void PairingReceipt::StartWaiting() {
    std::lock_guard<std::mutex> lock(mutex_);
    waiting_ = !target_ssid_.empty();
}

PairingReceipt::Result PairingReceipt::Complete(const std::string& ssid) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!waiting_) return Result::Ignored;
    waiting_ = false;
    const bool matched = ssid == target_ssid_;
    if (matched) completed_session_id_ = pending_session_id_;
    target_ssid_.clear();
    pending_session_id_.clear();
    return matched ? Result::Matched : Result::Mismatch;
}

bool PairingReceipt::CancelPending() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool was_waiting = waiting_;
    waiting_ = false;
    target_ssid_.clear();
    pending_session_id_.clear();
    return was_waiting;
}

std::string PairingReceipt::SessionId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completed_session_id_;
}

uint16_t Crc16(const uint8_t* data, size_t length) {
    uint16_t value = 0xffff;
    for (size_t index = 0; index < length; ++index) {
        value ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1) ? static_cast<uint16_t>((value >> 1) ^ 0xA001)
                                : static_cast<uint16_t>(value >> 1);
        }
    }
    return value;
}

std::vector<uint8_t> EncodeBleFrame(uint8_t command, const std::string& payload) {
    if (payload.size() > kMaxPayloadSize) return {};
    std::vector<uint8_t> frame(6 + payload.size());
    frame[0] = 0xaa;
    frame[1] = command;
    frame[2] = static_cast<uint8_t>(payload.size() >> 8);
    frame[3] = static_cast<uint8_t>(payload.size());
    std::copy(payload.begin(), payload.end(), frame.begin() + 4);
    const auto checksum = Crc16(frame.data() + 1, frame.size() - 3);
    frame[frame.size() - 2] = static_cast<uint8_t>(checksum);
    frame[frame.size() - 1] = static_cast<uint8_t>(checksum >> 8);
    return frame;
}

void BleFrameParser::Feed(const uint8_t* data, size_t length, std::vector<Frame>& frames) {
    buffer_.insert(buffer_.end(), data, data + length);
    while (true) {
        const auto header = std::find(buffer_.begin(), buffer_.end(), uint8_t{0xaa});
        if (header == buffer_.end()) {
            buffer_.clear();
            return;
        }
        buffer_.erase(buffer_.begin(), header);
        if (buffer_.size() < 4) return;
        const size_t payload_length = (static_cast<size_t>(buffer_[2]) << 8) | buffer_[3];
        if (payload_length > kMaxPayloadSize) {
            buffer_.erase(buffer_.begin());
            continue;
        }
        const size_t frame_length = payload_length + 6;
        if (buffer_.size() < frame_length) return;
        const auto checksum = static_cast<uint16_t>(buffer_[frame_length - 2])
                              | (static_cast<uint16_t>(buffer_[frame_length - 1]) << 8);
        if (checksum == Crc16(buffer_.data() + 1, frame_length - 3)) {
            frames.push_back({buffer_[1], std::string(buffer_.begin() + 4,
                                                       buffer_.begin() + 4 + payload_length)});
            buffer_.erase(buffer_.begin(), buffer_.begin() + frame_length);
        } else {
            buffer_.erase(buffer_.begin());
        }
    }
}

void BleFrameParser::Reset() {
    buffer_.clear();
}

}  // namespace ygsoul::ble
