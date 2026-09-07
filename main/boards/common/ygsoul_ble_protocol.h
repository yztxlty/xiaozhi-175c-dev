#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ygsoul::ble {

constexpr uint16_t kServiceUuid = 0x1910;
constexpr uint16_t kNotifyCharacteristicUuid = 0x2b10;
constexpr uint16_t kWriteCharacteristicUuid = 0x2b11;
constexpr uint8_t kQueryDevInfo = 0x01;
constexpr uint8_t kDevInfoRsp = 0x02;
constexpr uint8_t kWifiConfig = 0x03;
constexpr uint8_t kNetcfgResult = 0x04;
constexpr size_t kMaxPayloadSize = 238;

struct Frame {
    uint8_t command;
    std::string payload;
};

// 仅保存会话 UUID，不保留配网令牌；跨 BLE/Wi-Fi/主线程读写均复制后解锁。
class PairingReceipt {
public:
    enum class Result { Ignored, Matched, Mismatch };
    void Begin(const std::string& token, const std::string& ssid);
    void StartWaiting();
    Result Complete(const std::string& ssid);
    bool CancelPending();
    std::string SessionId() const;

private:
    mutable std::mutex mutex_;
    bool waiting_ = false;
    std::string target_ssid_;
    std::string pending_session_id_;
    std::string completed_session_id_;
};

uint16_t Crc16(const uint8_t* data, size_t length);
std::vector<uint8_t> EncodeBleFrame(uint8_t command, const std::string& payload);

class BleFrameParser {
public:
    void Feed(const uint8_t* data, size_t length, std::vector<Frame>& frames);
    void Reset();

private:
    std::vector<uint8_t> buffer_;
};

}  // namespace ygsoul::ble
