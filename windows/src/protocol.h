// Wire format shared with the Android app. All integers are little-endian.
// See docs/PROTOCOL.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace proto {

constexpr uint16_t kDefaultPort = 4747;
constexpr uint8_t kVersion = 1;
constexpr char kMagic[4] = {'P', 'W', 'C', 'M'};

constexpr size_t kHandshakeSize = 11;
constexpr size_t kFrameHeaderSize = 13;

constexpr uint8_t kStatusOk = 0;
constexpr uint8_t kCodecH264 = 1;

constexpr uint8_t kCameraBack = 0;
constexpr uint8_t kCameraFront = 1;

constexpr uint8_t kFrameConfig = 1;
constexpr uint8_t kFrameKey = 2;
constexpr uint8_t kFrameDelta = 3;

constexpr uint8_t kCtrlSwitchCamera = 0x10;
constexpr uint8_t kCtrlSetResolution = 0x11;
constexpr uint8_t kCtrlRequestKeyframe = 0x12;
constexpr uint8_t kCtrlBye = 0x1F;

inline void PutU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

inline uint16_t GetU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

inline uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t GetU64(const uint8_t* p) {
    return static_cast<uint64_t>(GetU32(p)) | (static_cast<uint64_t>(GetU32(p + 4)) << 32);
}

struct Hello {
    uint16_t width;
    uint16_t height;
    uint8_t fps;
    uint8_t camera;
};

inline void WriteHello(const Hello& h, uint8_t out[kHandshakeSize]) {
    std::memcpy(out, kMagic, 4);
    out[4] = kVersion;
    PutU16(out + 5, h.width);
    PutU16(out + 7, h.height);
    out[9] = h.fps;
    out[10] = h.camera;
}

struct Reply {
    uint8_t status;
    uint16_t width;
    uint16_t height;
    uint8_t fps;
    uint8_t codec;
};

inline bool ParseReply(const uint8_t in[kHandshakeSize], Reply* r) {
    if (std::memcmp(in, kMagic, 4) != 0) return false;
    r->status = in[4];
    r->width = GetU16(in + 5);
    r->height = GetU16(in + 7);
    r->fps = in[9];
    r->codec = in[10];
    return true;
}

struct FrameHeader {
    uint8_t type;
    uint64_t pts_us;
    uint32_t length;
};

inline FrameHeader ParseFrameHeader(const uint8_t in[kFrameHeaderSize]) {
    return FrameHeader{in[0], GetU64(in + 1), GetU32(in + 9)};
}

}  // namespace proto
