// Host-side helper for YGSoul Device Auth v2 canonical bytes.
// Prints lowercase hex of the Python reference activate/mqtt message.

#include "ydp_device_auth.h"

#include <cstdio>
#include <string>

int main() {
    const std::string message = ygsoul::ydp::BuildDeviceAuthSigningMessage(
        "test-device-001", "SAMPLE", 1, "1.0.0", "01010101010101010101010101010101", "1700000000",
        "0202020202020202020202020202020202020202020202020202020202020202", "activate", "mqtt");
    if (message.empty() || message.back() == '\n') {
        return 1;
    }
    const std::string hex =
        ygsoul::ydp::ToLowerHex(reinterpret_cast<const uint8_t*>(message.data()), message.size());
    std::puts(hex.c_str());
    const std::string refresh = ygsoul::ydp::BuildDeviceAuthSigningMessage(
        "test-device-001", "SAMPLE", 1, "1.0.0", "01010101010101010101010101010101", "1700000000",
        "0202020202020202020202020202020202020202020202020202020202020202", "refresh", "mqtt");
    const std::string websocket = ygsoul::ydp::BuildDeviceAuthSigningMessage(
        "test-device-001", "SAMPLE", 1, "1.0.0", "01010101010101010101010101010101", "1700000000",
        "0202020202020202020202020202020202020202020202020202020202020202", "activate",
        "websocket");
    if (message == refresh || message == websocket) {
        return 2;
    }
    return 0;
}
