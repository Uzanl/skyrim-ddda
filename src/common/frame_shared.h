// Shared-memory frame transport: DDDA (32-bit writer) -> Skyrim ReShade add-on
// (64-bit reader). Fixed-width fields only, identical layout in both bitnesses.
//
// Triple buffered: the writer fills slot (latest + 1) % kSlots, then publishes it
// as `latest`. Each slot has its own seqlock, so a reader that is overtaken sees
// the sequence change and drops that copy.
//
// Each slot holds the color plane and, with kSlotHasMask, a second plane: DDDA's
// G-buffer scaled the same way. Its pixels are exactly white (FF FF FF) where
// nothing was drawn, so it doubles as the transparency mask for the pawns.
#pragma once

#include <cstddef>
#include <cstdint>

namespace frame {

inline constexpr wchar_t kMappingName[] = L"Local\\DDDA_SkyrimBridge_frame_v3";
inline constexpr uint32_t kMagic = 0x52464444;  // "DDFR"
inline constexpr uint32_t kVersion = 3;

inline constexpr uint32_t kMaxWidth = 1920;
inline constexpr uint32_t kMaxHeight = 1080;
inline constexpr uint32_t kSlots = 3;
inline constexpr uint32_t kPlaneBytes = kMaxWidth * kMaxHeight * 4;
inline constexpr uint32_t kSlotBytes = 2 * kPlaneBytes;
inline constexpr uint32_t kReaderTimeoutMs = 1000;  // writer stops capturing without a reader

enum SlotFlags : uint32_t {
    kSlotHasMask = 1u << 0,  // mask plane valid: only the party was rendered
    kSlotHasPose = 1u << 1,  // skyPose valid: the Skyrim camera this frame was rendered for
};

struct Slot {
    volatile uint32_t seq;  // odd while being written
    uint32_t width;
    uint32_t height;
    uint32_t pitch;  // bytes per row, both planes
    uint64_t frameId;
    uint64_t qpcTime;  // writer's QueryPerformanceCounter at capture
    uint32_t flags;    // SlotFlags
    uint32_t reserved[3];
    float skyPose[16];  // bridge_shared.h SkyPose layout (kSlotHasPose)
    uint32_t reserved2[4];
};

struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t writerPid;
    uint32_t format;  // kFormatBGRA8
    volatile uint32_t latest;  // slot index of the newest complete frame
    volatile uint32_t readerTick;  // reader writes GetTickCount() every frame it wants
    uint64_t frames;  // frames captured so far
    Slot slots[kSlots];
};

inline constexpr uint32_t kFormatBGRA8 = 1;  // bytes B, G, R, A (alpha undefined)
inline constexpr uint32_t kHeaderBytes = 4096;
inline constexpr uint32_t kMappingBytes = kHeaderBytes + kSlots * kSlotBytes;

inline constexpr uint32_t SlotOffset(uint32_t i) {
    return kHeaderBytes + i * kSlotBytes;
}

inline constexpr uint32_t MaskOffset(uint32_t i) {
    return SlotOffset(i) + kPlaneBytes;
}

static_assert(sizeof(Slot) == 128);
static_assert(offsetof(Header, slots) == 32);
static_assert(sizeof(Header) <= kHeaderBytes);

}  // namespace frame
