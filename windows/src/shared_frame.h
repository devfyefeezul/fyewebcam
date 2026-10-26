// Shared memory between fyewebcam.exe (writer) and the virtual camera DLL (reader).
//
// The DLL runs inside the Windows Camera Frame Server service, so it creates the mapping in the
// Global namespace with a DACL that lets interactive users write to it. The app opens it when it
// exists. Frames are BGRA, top-down; a sequence counter (odd while writing) keeps reads consistent.
#pragma once

#include <cstdint>

namespace shared_frame {

constexpr wchar_t kMappingName[] = L"Global\\fyewebcam_vcam_frame";
constexpr uint32_t kMagic = 0x4D435946;  // "FYCM"
constexpr uint32_t kMaxPixels = 1920 * 1080;
constexpr uint32_t kHeaderSize = 64;
constexpr uint32_t kMappingSize = kHeaderSize + kMaxPixels * 4;

// CLSID of the virtual camera media source, {58E7AF15-C40C-437E-9DEA-FDD8E15057D6}.
constexpr wchar_t kSourceClsidString[] = L"{58E7AF15-C40C-437E-9DEA-FDD8E15057D6}";

struct Header {
    volatile uint32_t magic;
    volatile uint32_t sequence;  // odd while the writer is copying a frame
    volatile uint32_t width;
    volatile uint32_t height;
    volatile int64_t write_time;  // GetTickCount64() of the last frame, to detect a stopped app
};
static_assert(sizeof(Header) <= kHeaderSize, "header too large");

inline uint8_t* Pixels(void* base) { return static_cast<uint8_t*>(base) + kHeaderSize; }

}  // namespace shared_frame
