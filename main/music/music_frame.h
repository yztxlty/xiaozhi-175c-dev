#pragma once

#include <cstddef>
#include <cstdint>

struct MusicFrameView {
    uint32_t epoch;
    uint32_t sequence;
    uint64_t pts_ms;
    const uint8_t* payload;
    size_t payload_size;
    bool end;
};

bool ParseMusicFrame(const uint8_t* data, size_t size, uint32_t minimum_epoch,
                     MusicFrameView& frame);
