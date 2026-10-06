// Damage log: see damage_log.h.
//
// ApplyDamage (+0x376F50, docs/ddda-memory.md "Code addresses") takes HP off any
// character: eax = its vital block (HP at +8, max at +0xC, owning character at +0x1B4),
// then on the stack the damage (float, positive) and a second argument; ret 8. Its first
// 8 bytes are whole instructions (push ecx; push edi; mov edi, eax;
// test byte [edi+0x30], 1), so they become a jmp to Stub, which records the hit, runs
// them and goes on at +8 (the jne that follows uses the test's flags).
//
// Tried first (2026-10-06): the three DamageStat call sites from ddda-dinput8 never fired
// in game (the Arisen's spells, a pawn's melee and arrows), and +0x488DA0, caught by a
// write watchpoint on a pawn's HP, is the per-frame pawn routine, not damage.
#include "damage_log.h"

#include <windows.h>

#include <cstring>

namespace damagelog {
namespace {

constexpr uintptr_t kApplyDamage = 0x376F50;  // image-relative
const uint8_t kEntry[8] = {0x51, 0x57, 0x8B, 0xF8, 0xF6, 0x47, 0x30, 0x01};
constexpr uintptr_t kVitalHp = 8, kVitalOwner = 0x1B4;
constexpr uintptr_t kPos = 0x40;  // character position, as in dllmain.cpp

LogFn g_log = nullptr;
uintptr_t g_base = 0;
uintptr_t g_resume = 0;  // ApplyDamage + 8

struct Hit {
    volatile LONG seq;  // set last; 0 = empty
    uint32_t vital, caller, arg2;
    float damage;
};
constexpr LONG kRing = 128;
Hit g_ring[kRing];
volatile LONG g_written = 0;
LONG g_read = 0;

// Runs on game threads inside the hit code: stores only, no reads of game memory.
void __stdcall Record(uint32_t vital, uint32_t damageBits, uint32_t arg2, uint32_t caller) {
    LONG n = InterlockedIncrement(&g_written);
    Hit& h = g_ring[(n - 1) % kRing];
    h.vital = vital;
    h.caller = caller;
    h.arg2 = arg2;
    memcpy(&h.damage, &damageBits, 4);
    InterlockedExchange(&h.seq, n);
}

// On entry: eax = vital block, [esp] = return address, [esp+4] = damage, [esp+8] = arg2.
// After pushad: eax at +28, return at +32, damage at +36, arg2 at +40; each push adds 4.
__declspec(naked) void Stub() {
    __asm {
        pushad
        push dword ptr [esp + 32]
        push dword ptr [esp + 44]
        push dword ptr [esp + 44]
        push dword ptr [esp + 40]
        call Record
        popad
        push ecx
        push edi
        mov edi, eax
        test byte ptr [edi + 0x30], 1
        jmp dword ptr [g_resume]
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
    g_resume = base + kApplyDamage + 8;
    auto* code = reinterpret_cast<volatile LONG64*>(base + kApplyDamage);
    LONG64 from;
    if (!Read(base + kApplyDamage, &from, 8) || memcmp(&from, kEntry, 8) != 0) {
        g_log("damage log: unexpected code at %08X (different game build?); not hooked",
              static_cast<unsigned>(base + kApplyDamage));
        return;
    }
    uint8_t bytes[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&Stub) - (base + kApplyDamage + 5));
    memcpy(bytes + 1, &rel, 4);
    LONG64 to;
    memcpy(&to, bytes, 8);
    DWORD old;
    if (!VirtualProtect(const_cast<LONG64*>(code), 8, PAGE_EXECUTE_READWRITE, &old)) {
        g_log("damage log: VirtualProtect failed: %lu", GetLastError());
        return;
    }
    LONG64 seen = InterlockedCompareExchange64(code, to, from);
    VirtualProtect(const_cast<LONG64*>(code), 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), const_cast<LONG64*>(code), 8);
    g_log(seen == from ? "damage log: ApplyDamage hooked (log-only)"
                       : "damage log: code changed while patching; not hooked");
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
        uint32_t owner = 0;
        float hp[2] = {}, pos[3] = {};
        bool hasHp = Read(h.vital + kVitalHp, hp, sizeof(hp));
        bool hasOwner = Read(h.vital + kVitalOwner, &owner, 4) && owner;
        bool hasPos = hasOwner && Read(owner + kPos, pos, sizeof(pos));
        char cls[64] = "?";
        if (hasOwner) ClassName(owner, cls, sizeof(cls));
        g_log("damage: %s %08X takes %.1f (arg2 %08X, from +%X) hp now %.0f/%.0f%s pos (%.0f, %.0f, %.0f)%s "
              "vital %08X",
              cls, owner, h.damage, h.arg2, h.caller - static_cast<uint32_t>(g_base), hp[0], hp[1],
              hasHp ? "" : " (no hp)", pos[0], pos[1], pos[2], hasPos ? "" : " (no pos)", h.vital);
    }
}

}  // namespace damagelog
