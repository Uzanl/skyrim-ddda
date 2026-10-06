// Damage log: see damage_log.h. Sites from ddda-dinput8's DamageLog.cpp, matched in our
// DDDA.exe (2026-10-06). Each site is
//   push ecx; movss [esp], xmm1; call DamageStat; mov reg, [target]; ...; call [vt+0x1D4]
// DamageStat (+0x4B710) only adds the damage to a global statistic (sMain+0xB88AC); the
// call is a convenient point inside the hit code where [esp] is the damage and the target
// is in a register. We redirect the call's rel32 to a stub that records (site, target,
// damage) and jumps on to DamageStat.
#include "damage_log.h"

#include <windows.h>

#include <cstring>

namespace damagelog {
namespace {

constexpr uintptr_t kDamageStat = 0x4B710;  // image-relative
constexpr uintptr_t kSites[3] = {0x6AAF78, 0x7AA3E8, 0x7B7245};  // the call instructions
const uint8_t kSitePrefix[6] = {0x51, 0xF3, 0x0F, 0x11, 0x0C, 0x24};  // push ecx; movss [esp], xmm1
constexpr uintptr_t kPos = 0x40, kStatus = 0x4BC, kHp = 0x1D8;  // as in dllmain.cpp

LogFn g_log = nullptr;
uintptr_t g_base = 0;
uintptr_t g_target = 0;  // absolute DamageStat, the stubs' jump target

struct Hit {
    volatile LONG seq;  // set last; 0 = empty
    uint32_t site, obj;
    float damage;
};
constexpr LONG kRing = 64;
Hit g_ring[kRing];
volatile LONG g_written = 0;
LONG g_read = 0;

// Runs on the game thread inside the hit code: stores only, no reads of game memory.
void __stdcall Record(uint32_t site, uint32_t obj, uint32_t damageBits) {
    LONG n = InterlockedIncrement(&g_written);
    Hit& h = g_ring[(n - 1) % kRing];
    h.site = site;
    h.obj = obj;
    memcpy(&h.damage, &damageBits, 4);
    InterlockedExchange(&h.seq, n);
}

// On entry: [esp] = return address, [esp+4] = damage. After pushad, +32.
__declspec(naked) void Stub0() {
    __asm {
        pushad
        push dword ptr [esp + 36]
        push ebx
        push 0
        call Record
        popad
        jmp dword ptr [g_target]
    }
}
__declspec(naked) void Stub1() {
    __asm {
        pushad
        push dword ptr [esp + 36]
        push esi
        push 1
        call Record
        popad
        jmp dword ptr [g_target]
    }
}
__declspec(naked) void Stub2() {
    __asm {
        pushad
        push dword ptr [esp + 36]
        push esi
        push 2
        call Record
        popad
        jmp dword ptr [g_target]
    }
}

bool Read(uintptr_t addr, void* out, size_t n) {
    if (addr < 0x10000) return false;
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// MT class name: vtable slot 4 is `mov eax, DTI; ret`, the name pointer is at DTI+4.
void ClassName(uint32_t obj, char* out, size_t cap) {
    strcpy_s(out, cap, "?");
    uint32_t vt, fn, dti, name;
    uint8_t code[6];
    if (!Read(obj, &vt, 4) || !Read(vt + 16, &fn, 4) || !Read(fn, code, 6) || code[0] != 0xB8 ||
        code[5] != 0xC3)
        return;
    memcpy(&dti, code + 1, 4);
    char buf[64] = {};
    if (!Read(dti + 4, &name, 4) || !Read(name, buf, sizeof(buf) - 1)) return;
    buf[sizeof(buf) - 1] = 0;
    strcpy_s(out, cap, buf);
}

}  // namespace

void Install(LogFn log, uintptr_t base) {
    g_log = log;
    g_base = base;
    g_target = base + kDamageStat;
    void* stubs[3] = {reinterpret_cast<void*>(&Stub0), reinterpret_cast<void*>(&Stub1),
                      reinterpret_cast<void*>(&Stub2)};
    int ok = 0;
    for (int i = 0; i < 3; ++i) {
        uint8_t* call = reinterpret_cast<uint8_t*>(base + kSites[i]);
        uint8_t code[11];
        if (!Read(reinterpret_cast<uintptr_t>(call) - 6, code, sizeof(code)) ||
            memcmp(code, kSitePrefix, 6) != 0 || code[6] != 0xE8) {
            g_log("damage log: site %d at %08X has unexpected code; skipped", i,
                  static_cast<unsigned>(base + kSites[i]));
            continue;
        }
        int32_t rel;
        memcpy(&rel, code + 7, 4);
        if (reinterpret_cast<uintptr_t>(call) + 5 + rel != g_target) {
            g_log("damage log: site %d calls %08X, expected %08X; skipped", i,
                  static_cast<unsigned>(reinterpret_cast<uintptr_t>(call) + 5 + rel),
                  static_cast<unsigned>(g_target));
            continue;
        }
        int32_t newRel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stubs[i]) -
                                              (reinterpret_cast<uintptr_t>(call) + 5));
        DWORD old;
        if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
            g_log("damage log: VirtualProtect failed: %lu", GetLastError());
            continue;
        }
        InterlockedExchange(reinterpret_cast<volatile LONG*>(call + 1), newRel);
        VirtualProtect(call + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), call + 1, 4);
        ++ok;
    }
    g_log("damage log: %d of 3 sites hooked (log-only)", ok);
}

void Poll() {
    LONG written = g_written;
    if (written - g_read > kRing) {
        g_log("damage log: %ld hits not logged (too many at once)", written - g_read - kRing);
        g_read = written - kRing;
    }
    for (; g_read < written; ++g_read) {
        const Hit& h = g_ring[g_read % kRing];
        if (h.seq != g_read + 1) break;  // not finished writing yet
        char cls[64];
        ClassName(h.obj, cls, sizeof(cls));
        uint32_t vt = 0, status = 0;
        float pos[3] = {}, hp[2] = {};
        Read(h.obj, &vt, 4);
        bool hasPos = Read(h.obj + kPos, pos, sizeof(pos));
        bool hasHp = Read(h.obj + kStatus, &status, 4) && Read(status + kHp, hp, sizeof(hp));
        g_log("damage: site %u target %08X %s (vt +%X) dmg %.1f hp %.0f/%.0f%s pos (%.0f, %.0f, %.0f)%s",
              h.site, h.obj, cls, vt ? static_cast<unsigned>(vt - g_base) : 0u, h.damage, hp[0], hp[1],
              hasHp ? "" : " (no hp)", pos[0], pos[1], pos[2], hasPos ? "" : " (no pos)");
    }
}

}  // namespace damagelog
