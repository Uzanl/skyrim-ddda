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

#include <cstdio>
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

// Hits called from the hit function (+0x36E27D, +0x36E31B) also keep a copy of its hit
// record (the caller's ebp: damage at +0x7C) and registers, to find the attacker.
constexpr uintptr_t kHitCallers[2] = {0x36E27D, 0x36E31B};
constexpr size_t kRecBytes = 0x300;
// The attacker is [rec+0x50] (verified in game 2026-10-10): a character for melee, a shell
// (uShl*) for arrows and spells. A copy of that object is kept too, to find the shooter.
constexpr unsigned kRecAttacker = 0x50 / 4;
constexpr size_t kSrcBytes = 0x800;
enum Reg { kEdi, kEsi, kEbp, kEsp, kEbx, kEdx, kEcx, kEax, kRegs };

struct Hit {
    volatile LONG seq;  // set last; 0 = empty
    uint32_t vital, caller, arg2;
    float damage;
    uint32_t regs[kRegs];
    bool hasRec, hasSrc;
    uint32_t rec[kRecBytes / 4];
    uint32_t src[kSrcBytes / 4];
};
constexpr LONG kRing = 128;
Hit g_ring[kRing];
volatile LONG g_written = 0;
LONG g_read = 0;

bool CopyRec(uint32_t from, uint32_t* to, size_t n) {
    if (from < 0x10000) return false;
    __try {
        memcpy(to, reinterpret_cast<const void*>(from), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Runs on game threads inside the hit code. frame = pushad's block (edi first), then the
// return address, the damage and arg2. Reads game memory only under SEH (the hit record).
void __stdcall Record(const uint32_t* frame) {
    LONG n = InterlockedIncrement(&g_written);
    Hit& h = g_ring[(n - 1) % kRing];
    memcpy(h.regs, frame, sizeof(h.regs));
    h.vital = frame[kEax];
    h.caller = frame[8];
    memcpy(&h.damage, &frame[9], 4);
    h.arg2 = frame[10];
    h.hasRec = h.hasSrc = false;
    for (uintptr_t c : kHitCallers)
        if (h.caller == g_base + c) h.hasRec = CopyRec(frame[kEbp], h.rec, kRecBytes);
    if (h.hasRec) h.hasSrc = CopyRec(h.rec[kRecAttacker], h.src, kSrcBytes);
    InterlockedExchange(&h.seq, n);
}

// On entry: eax = vital block, [esp] = return address, [esp+4] = damage, [esp+8] = arg2.
__declspec(naked) void Stub() {
    __asm {
        pushad
        push esp
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

bool IsChar(const char* c) {
    return !strcmp(c, "uPlayer") || !strcmp(c, "uCmc") || !strncmp(c, "uEm", 3) || strstr(c, "Enemy");
}

// Characters seen in hits (victims, melee attackers), to recognise pointers into them.
constexpr size_t kCharSize = 0x5930;  // pawn object stride (docs/ddda-memory.md)
constexpr unsigned kDeepBytes = 0x100;
uint32_t g_chars[32];
unsigned g_charCount = 0;

void Remember(uint32_t c) {
    for (unsigned i = 0; i < g_charCount && i < 32; ++i)
        if (g_chars[i] == c) return;
    g_chars[g_charCount++ % 32] = c;
}

volatile uint32_t g_party[4];
const char* const kRoleNames[4] = {"Arisen", "main pawn", "hired 1", "hired 2"};

// Party role whose object holds v (at or inside it), or -1.
int PartyRole(uint32_t v) {
    for (int r = 0; r < 4; ++r)
        if (g_party[r] && v >= g_party[r] && v < g_party[r] + kCharSize) return r;
    return -1;
}

uint32_t KnownCharHolding(uint32_t v) {
    for (uint32_t c : g_party)
        if (c && v > c && v < c + kCharSize) return c;
    for (unsigned i = 0; i < g_charCount && i < 32; ++i)
        if (v > g_chars[i] && v < g_chars[i] + kCharSize) return g_chars[i];
    return 0;
}

}  // namespace

void NoteParty(int role, uint32_t obj) {
    if (role >= 0 && role < 4) g_party[role] = obj;
}

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
        // Characters the registers and the hit record point to (attacker candidates).
        char line[1024];
        size_t len = 0;
        auto add = [&](const char* where, unsigned off, uint32_t v) {
            if (v < 0x01000000 || (v >= g_base && v < g_base + 0x1800000)) return;
            char c[64];
            ClassName(v, c, sizeof(c));
            if (c[0] != 'u') return;
            int k = where[strlen(where) - 1] == '+'
                        ? sprintf_s(line + len, sizeof(line) - len, " %s%X=%s:%08X", where, off, c, v)
                        : sprintf_s(line + len, sizeof(line) - len, " %s=%s:%08X", where, c, v);
            if (k > 0) len += k;
        };
        static const char* kRegNames[kRegs] = {"edi", "esi", "ebp", "esp", "ebx", "edx", "ecx", "eax"};
        line[0] = 0;
        for (int r = 0; r < kRegs; ++r)
            if (r != kEsp && len < sizeof(line) - 80) add(kRegNames[r], 0, h.regs[r]);
        if (h.hasRec)
            for (unsigned i = 0; i < kRecBytes / 4 && len < sizeof(line) - 80; ++i) add("rec+", i * 4, h.rec[i]);
        if (len) g_log("  refs:%s", line);
        if (hasOwner && IsChar(cls)) Remember(owner);
        char src[64] = "?";
        if (h.hasRec) ClassName(h.rec[kRecAttacker], src, sizeof(src));
        if (IsChar(src)) Remember(h.rec[kRecAttacker]);
        // A shell (arrow, spell): characters its copy points to (shooter candidates), directly,
        // into a known character (party and anyone seen in a hit) or one pointer further on.
        // 2026-10-10: no direct pointer in the first 0x800 bytes of 40 shells; the party's
        // objects (from the move() hooks) now count as known too.
        if (h.hasSrc && strncmp(src, "uShl", 4) == 0) {
            len = 0;
            line[0] = 0;
            for (unsigned i = 0; i < kSrcBytes / 4 && len < sizeof(line) - 80; ++i) {
                uint32_t v = h.src[i];
                if (v < 0x01000000 || (v >= g_base && v < g_base + 0x1800000)) continue;
                char c[64];
                ClassName(v, c, sizeof(c));
                int k = 0;
                if (int r = PartyRole(v); r >= 0) {
                    k = sprintf_s(line + len, sizeof(line) - len, " shl+%X=%s+%X", i * 4, kRoleNames[r],
                                  v - g_party[r]);
                } else if (IsChar(c)) {
                    k = sprintf_s(line + len, sizeof(line) - len, " shl+%X=%s:%08X", i * 4, c, v);
                } else if (uint32_t base = KnownCharHolding(v)) {
                    ClassName(base, c, sizeof(c));
                    k = sprintf_s(line + len, sizeof(line) - len, " shl+%X=&%s:%08X+%X", i * 4, c, base, v - base);
                } else {
                    uint32_t next[kDeepBytes / 4];
                    if (!Read(v, next, sizeof(next))) continue;
                    for (unsigned j = 0; j < kDeepBytes / 4; ++j) {
                        uint32_t w = next[j];
                        if (w < 0x01000000 || (w >= g_base && w < g_base + 0x1800000)) continue;
                        char d[64];
                        ClassName(v, c, sizeof(c));
                        if (int pr = PartyRole(w); pr >= 0) {
                            k = sprintf_s(line + len, sizeof(line) - len, " shl+%X->%s:%08X+%X=%s+%X", i * 4, c,
                                          v, j * 4, kRoleNames[pr], w - g_party[pr]);
                            break;
                        }
                        ClassName(w, d, sizeof(d));
                        if (!IsChar(d)) continue;
                        k = sprintf_s(line + len, sizeof(line) - len, " shl+%X->%s:%08X+%X=%s:%08X", i * 4, c,
                                      v, j * 4, d, w);
                        break;
                    }
                }
                if (k > 0) len += k;
            }
            g_log("  shooter:%s", len ? line : " (no character found)");
        }
    }
}

}  // namespace damagelog
