#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace StreamV2 {

constexpr uint16_t kPort = 8888;
constexpr size_t kHeaderSize = 16;
constexpr size_t kPayloadSize = 1440;
constexpr size_t kDatagramSize = kHeaderSize + kPayloadSize;
constexpr size_t kFeedbackSize = 64;
constexpr uint8_t kModeRgb332 = 0;
constexpr uint8_t kModeRgb565 = 1;
constexpr uint8_t kRgb332Chunks = 40;
constexpr uint8_t kRgb565Chunks = 80;
constexpr uint32_t kNoFrame = 0xFFFFFFFFu;

constexpr uint8_t kDataMagic[4] = {'S', 'S', 'D', '2'};
constexpr uint8_t kFeedbackMagic[4] = {'S', 'S', 'F', '2'};

inline uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] << 8) | data[1];
}

inline uint32_t readU32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24)
        | (static_cast<uint32_t>(data[1]) << 16)
        | (static_cast<uint32_t>(data[2]) << 8)
        | static_cast<uint32_t>(data[3]);
}

inline void writeU16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

inline void writeU32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

inline bool newerFrame(uint32_t candidate, uint32_t reference) {
    return static_cast<int32_t>(candidate - reference) > 0;
}

constexpr uint16_t rgb332ToRgb444(uint8_t value) {
    return static_cast<uint16_t>(
        ((((value >> 5) & 0x07) << 1) | (((value >> 5) & 0x07) >> 2)) << 8
        | ((((value >> 2) & 0x07) << 1) | (((value >> 2) & 0x07) >> 2)) << 4
        | (((value & 0x03) << 2) | (value & 0x03)));
}

constexpr uint32_t packRgb444Pair(uint16_t first, uint16_t second) {
    return (static_cast<uint32_t>(first) << 12) | second;
}

static_assert(rgb332ToRgb444(0xE0) == 0xF00, "RGB332 red conversion");
static_assert(rgb332ToRgb444(0x1C) == 0x0F0, "RGB332 green conversion");
static_assert(rgb332ToRgb444(0x03) == 0x00F, "RGB332 blue conversion");
static_assert(packRgb444Pair(0xF00, 0x0F0) == 0xF000F0, "RGB444 pair order");

struct DataHeader {
    uint32_t sessionId;
    uint32_t frameId;
    uint16_t payloadLength;
    uint8_t mode;
    uint8_t chunkIndex;
};

inline bool parseDataHeader(const uint8_t* bytes, DataHeader& header) {
    if (memcmp(bytes, kDataMagic, sizeof(kDataMagic)) != 0) {
        return false;
    }
    header.sessionId = readU32(bytes + 4);
    header.frameId = readU32(bytes + 8);
    header.mode = bytes[12];
    header.chunkIndex = bytes[13];
    header.payloadLength = readU16(bytes + 14);
    if (header.payloadLength != kPayloadSize) {
        return false;
    }
    if (header.mode == kModeRgb332) {
        return header.chunkIndex < kRgb332Chunks;
    }
    if (header.mode == kModeRgb565) {
        return header.chunkIndex < kRgb565Chunks;
    }
    return false;
}

inline uint8_t chunksForMode(uint8_t mode) {
    return mode == kModeRgb332 ? kRgb332Chunks : kRgb565Chunks;
}

}  // namespace StreamV2
