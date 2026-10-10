// Hijacked enemy: see hijack.h.
//
// The target comes from sAISensorTarget ([0x18D9274]): live array count +0x38, items +0x44;
// unit entries have flags +0x04 (bit 1 active), group +0x44 (3 = enemies) and the owner
// unit at +0x58 (read live 2026-10-10). The owner's class vtable gets its move() slot (8)
// replaced, after checking that the vtable lies in the image's data and the slot in its
// code; the hook calls the original, then pins the target. Other objects of that class
// pass straight through. Positions are tile-local, the same frame as the party's.
#include "hijack.h"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace hijack {
namespace {

using MoveFn = void(__thiscall*)(void*);

constexpr uintptr_t kSensor = 0x14D9274;  // image-relative [sAISensorTarget]
constexpr uintptr_t kLiveCount = 0x38, kLiveItems = 0x44;
constexpr uintptr_t kEntryFlags = 0x04, kEntryGroup = 0x44, kEntryOwner = 0x58;
constexpr uint32_t kEnemyGroup = 3, kActive = 2;
constexpr uintptr_t kPos = 0x40;
constexpr uintptr_t kMoveSlot = 8;
constexpr uintptr_t kCamRoot = 0x14D1578, kCamOff = 0xDF0;  // camera position (ddda-memory.md)
// The scenery adjust (ddda-memory.md "cScrAdjust"): found in the target by its vtable,
// since enemies may keep it at another offset than the party's +0x19C0.
constexpr uintptr_t kScrAdjustVt = 0x119704C;
constexpr uintptr_t kAdjCopies[3] = {0x120, 0x130, 0x140}, kAdjVelocity = 0x150;
constexpr float kAhead = 600.0f;      // 6 m in front of the Arisen
constexpr float kMaxPick = 1.0e7f;     // any loaded enemy (2026-10-10: none within 500 m of
                                       // the linked party; one taken at the save spot must
                                       // follow the party when the link moves it far away)
constexpr float kReanchor = 300.0f;    // the spot follows the Arisen once it is 3 m away
constexpr float kRelift = 2000.0f;     // after a 20 m move the height is set again

LogFn g_log = nullptr;
uintptr_t g_base = 0;
volatile LONG g_on = 0;
volatile uintptr_t g_target = 0, g_targetVt = 0, g_adjust = 0;
volatile LONG g_placeY = 0;  // first frame: also set the height (Arisen + 50 cm)
float g_spot[3], g_anchor[3];
volatile LONG g_pins = 0;

struct Patched {
    uintptr_t vt;
    MoveFn orig;
};
Patched g_patched[4];
volatile LONG g_patchedCount = 0;

bool Read(uintptr_t a, void* out, size_t n) {
    if (a < 0x10000) return false;
    __try {
        memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint32_t U32(uintptr_t a) {
    uint32_t v = 0;
    Read(a, &v, 4);
    return v;
}
bool Sane(const float* p) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(p[i]) || std::fabs(p[i]) > 2.0e6f) return false;
    return true;
}

void Pin(uintptr_t self) {
    __try {
        auto* p = reinterpret_cast<volatile float*>(self + kPos);
        p[0] = g_spot[0];
        p[2] = g_spot[2];
        if (InterlockedExchange(&g_placeY, 0)) p[1] = g_spot[1];
        if (uintptr_t adj = g_adjust) {
            for (uintptr_t off : kAdjCopies) {
                auto* v = reinterpret_cast<volatile float*>(adj + off);
                v[0] = p[0];
                v[1] = p[1];
                v[2] = p[2];
            }
            auto* vel = reinterpret_cast<volatile float*>(adj + kAdjVelocity);
            vel[0] = vel[2] = 0.0f;  // horizontal only: gravity keeps it on the ground
        }
        InterlockedIncrement(&g_pins);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_target = 0;
    }
}

void __fastcall MoveHook(void* self, void* /*edx*/) {
    uintptr_t vt = *reinterpret_cast<uintptr_t*>(self);
    MoveFn orig = nullptr;
    for (LONG i = 0; i < g_patchedCount; ++i)
        if (g_patched[i].vt == vt) orig = g_patched[i].orig;
    if (orig) orig(self);
    uintptr_t s = reinterpret_cast<uintptr_t>(self);
    if (g_on && s == g_target && vt == g_targetVt) Pin(s);
}

bool PatchClass(uintptr_t vt) {
    for (LONG i = 0; i < g_patchedCount; ++i)
        if (g_patched[i].vt == vt) return true;
    if (g_patchedCount >= 4) return false;
    // vtable in the image's data, slot 8 in its code (both checked; DDDA has no ASLR).
    if (vt < g_base + 0x1000000 || vt >= g_base + 0x1400000) return false;
    auto* slot = reinterpret_cast<uintptr_t*>(vt + kMoveSlot * 4);
    uint32_t fn = U32(reinterpret_cast<uintptr_t>(slot));
    if (fn < g_base + 0x1000 || fn >= g_base + 0x1000000 || fn == reinterpret_cast<uintptr_t>(&MoveHook))
        return false;
    DWORD old;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return false;
    g_patched[g_patchedCount].vt = vt;
    g_patched[g_patchedCount].orig = reinterpret_cast<MoveFn>(fn);
    InterlockedIncrement(&g_patchedCount);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(slot), static_cast<LONG>(reinterpret_cast<uintptr_t>(&MoveHook)));
    VirtualProtect(slot, 4, old, &old);
    g_log("hijack: move() of vtable %08X hooked (was %08X)", static_cast<unsigned>(vt), fn);
    return true;
}

void ClassName(uintptr_t obj, char* out, size_t cap) {
    strcpy_s(out, cap, "?");
    uint32_t vt = U32(obj), fn = U32(vt + 16), name;
    uint8_t code[6];
    if (!Read(fn, code, 6) || code[0] != 0xB8 || code[5] != 0xC3) return;
    uint32_t dti;
    memcpy(&dti, code + 1, 4);
    char buf[64] = {};
    if (!Read(dti + 4, &name, 4) || !Read(name, buf, sizeof(buf) - 1)) return;
    strcpy_s(out, cap, buf);
}

// Nearest active enemy entry's owner, or 0.
uintptr_t PickEnemy(const float* arisen, float* dist) {
    uintptr_t mgr = U32(g_base + kSensor);
    uint32_t n = U32(mgr + kLiveCount), items = U32(mgr + kLiveItems);
    if (!mgr || !items || n > 4096) return 0;
    uintptr_t best = 0;
    float bestD = kMaxPick;
    for (uint32_t i = 0; i < n; ++i) {
        uintptr_t e = U32(items + i * 4);
        if (U32(e + kEntryGroup) != kEnemyGroup || !(U32(e + kEntryFlags) & kActive)) continue;
        uintptr_t u = U32(e + kEntryOwner);
        float p[3];
        if (!Read(u + kPos, p, sizeof(p)) || !Sane(p)) continue;
        float d = std::hypot(p[0] - arisen[0], p[2] - arisen[2]);
        if (d < bestD) bestD = d, best = u;
    }
    *dist = bestD;
    return best;
}

bool StillEnemy(uintptr_t u) {
    uintptr_t mgr = U32(g_base + kSensor);
    uint32_t n = U32(mgr + kLiveCount), items = U32(mgr + kLiveItems);
    for (uint32_t i = 0; i < n && i < 4096; ++i) {
        uintptr_t e = U32(items + i * 4);
        if (U32(e + kEntryOwner) == u)
            return U32(e + kEntryGroup) == kEnemyGroup && (U32(e + kEntryFlags) & kActive);
    }
    return false;
}

// The spot 6 m in front of the Arisen, horizontally from the camera to the Arisen.
void PlaceSpot(const float* arisen) {
    float fx = 0, fz = 1, cam[3];
    if (Read(U32(g_base + kCamRoot) + kCamOff, cam, sizeof(cam)) && Sane(cam)) {
        float dx = arisen[0] - cam[0], dz = arisen[2] - cam[2], l = std::hypot(dx, dz);
        if (l > 1.0f) fx = dx / l, fz = dz / l;
    }
    g_spot[0] = arisen[0] + fx * kAhead;
    g_spot[1] = arisen[1] + 50.0f;
    g_spot[2] = arisen[2] + fz * kAhead;
    memcpy(g_anchor, arisen, sizeof(g_anchor));
}

}  // namespace

void Init(LogFn log, uintptr_t base) {
    g_log = log;
    g_base = base;
}

void SetEnabled(bool on) {
    if (on == (g_on != 0)) return;
    InterlockedExchange(&g_on, on ? 1 : 0);
    if (!on) g_target = 0;
    g_log("hijack %s", on ? "ON (nearest enemy held 6 m in front of the Arisen)" : "OFF");
}

void Poll(const float* arisen) {
    static ULONGLONG lastPick = 0, lastReport = 0;
    if (!g_on) return;
    ULONGLONG now = GetTickCount64();
    uintptr_t t = g_target;
    if (t && (U32(t) != g_targetVt || !StillEnemy(t))) {
        g_log("hijack: target %08X gone or no longer an active enemy", static_cast<unsigned>(t));
        g_target = t = 0;
    }
    if (!t && arisen && Sane(arisen) && now - lastPick >= 2000) {
        lastPick = now;
        float d = 0;
        uintptr_t u = PickEnemy(arisen, &d);
        if (!u) {
            g_log("hijack: no active enemy loaded");
            return;
        }
        PlaceSpot(arisen);
        uintptr_t vt = U32(u), adj = 0;
        for (uintptr_t off = 0; off < 0x6000; off += 4)
            if (U32(u + off) == g_base + kScrAdjustVt) {
                adj = u + off;
                break;
            }
        if (!PatchClass(vt)) {
            g_log("hijack: could not hook vtable %08X", static_cast<unsigned>(vt));
            return;
        }
        char c[64];
        ClassName(u, c, sizeof(c));
        g_adjust = adj;
        g_targetVt = vt;
        g_placeY = 1;
        g_target = u;
        g_log("hijack: took %s %08X, %.0f m away; held at (%.0f, %.0f, %.0f); scenery adjust %s", c,
              static_cast<unsigned>(u), d / 100, g_spot[0], g_spot[1], g_spot[2],
              adj ? "found" : "not found");
    }
    if (t && arisen && Sane(arisen)) {
        float moved = std::hypot(arisen[0] - g_anchor[0], arisen[2] - g_anchor[2]);
        if (moved > kReanchor) {
            PlaceSpot(arisen);
            if (moved > kRelift) {
                g_placeY = 1;
                g_log("hijack: Arisen moved %.0f m; target follows to (%.0f, %.0f, %.0f)", moved / 100, g_spot[0],
                      g_spot[1], g_spot[2]);
            }
        }
    }
    if (t && now - lastReport >= 2000) {
        lastReport = now;
        float p[3] = {};
        Read(t + kPos, p, sizeof(p));
        g_log("hijack: target %08X at (%.0f, %.0f, %.0f), pinned %ld frames", static_cast<unsigned>(t), p[0], p[1],
              p[2], g_pins);
    }
}

}  // namespace hijack
