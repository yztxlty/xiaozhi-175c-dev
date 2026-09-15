#include "music_frame.h"

#include <cstring>

namespace {
constexpr size_t kHeaderSize = 28;

uint16_t Read16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] << 8 | p[1]);
}

uint32_t Read32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 |
           static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | p[3];
}

uint64_t Read64(const uint8_t* p) {
    return static_cast<uint64_t>(Read32(p)) << 32 | Read32(p + 4);
}
}  // namespace

bool ParseMusicFrame(const uint8_t* data, size_t size, uint32_t minimum_epoch,
                     MusicFrameView& frame) {
    if (data == nullptr || size < kHeaderSize || std::memcmp(data, "YGM1", 4) != 0 ||
        data[4] != 1 || data[5] != 1) {
        return false;
    }
    const uint32_t payload_size = Read32(data + 24);
    const uint32_t epoch = Read32(data + 8);
    if (epoch < minimum_epoch || size != kHeaderSize + payload_size) {
        return false;
    }
    frame = {
        .epoch = epoch,
        .sequence = Read32(data + 12),
        .pts_ms = Read64(data + 16),
        .payload = data + kHeaderSize,
        .payload_size = payload_size,
        .end = (Read16(data + 6) & 1) != 0,
    };
    return true;
}
