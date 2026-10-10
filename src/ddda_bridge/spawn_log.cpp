// Spawn log: see spawn_log.h.
//
// uEnemy's constructor (+0x6A7B60, static 2026-10-10) takes the new object in edi (custom
// convention): it calls the base constructor (+0x44A100), then writes uEnemy's vtable
// (0x15DF2A8) into [edi]. Over 30 enemy constructors call it. Its first 9 bytes are whole
// instructions (push ebp; mov ebp, esp; and esp, -16; sub esp, 0x38), so they become a jmp
// to Stub, which records the call and runs them before going on at +9.
//
// The stack copy is read later: values that point just after a call instruction in
// DDDA.exe's code are return addresses, i.e. the chain that led to the spawn.
#include "spawn_log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace spawnlog {
namespace {

constexpr uintptr_t kEnemyCtor = 0x6A7B60;  // image-relative
const uint8_t kEntry[9] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x38};
constexpr uintptr_t kPos = 0x40;
constexpr size_t kStackDwords = 256;  // 1 KB of the caller's stack

LogFn g_log = nullptr;
uintptr_t g_base = 0;
uintptr_t g_resume = 0;

struct Spawn {
    volatile LONG seq;
    uint32_t obj;
    uint32_t stack[kStackDwords];
    ULONGLONG tick;
};
constexpr LONG kRing = 64;
Spawn g_ring[kRing];
volatile LONG g_written = 0;
LONG g_read = 0;

bool Read(uintptr_t addr, void* out, size_t n) {
    if (addr < 0x10000) return false;
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// pushad block: edi first; then the return address and the caller's stack.
void __stdcall Record(const uint32_t* frame) {
    LONG n = InterlockedIncrement(&g_written);
    Spawn& s = g_ring[(n - 1) % kRing];
    s.obj = frame[0];
    if (!Read(reinterpret_cast<uintptr_t>(frame + 8), s.stack, sizeof(s.stack)))
        memset(s.stack, 0, sizeof(s.stack));
    s.tick = GetTickCount64();
    InterlockedExchange(&s.seq, n);
}

__declspec(naked) void Stub() {
    __asm {
        pushad
        push esp
        call Record
        popad
        push ebp
        mov ebp, esp
        and esp, 0xFFFFFFF0
        sub esp, 0x38
        jmp dword ptr [g_resume]
    }
}

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
    strcpy_s(out, cap, buf);
}

// v is a return address if the bytes before it are a call (E8 rel32, FF /2 forms).
bool AfterCall(uint32_t v) {
    if (v < g_base + 0x1000 || v >= g_base + 0x1000000) return false;
    uint8_t b[7];
    if (!Read(v - 7, b, 7)) return false;
    return b[2] == 0xE8 || (b[5] == 0xFF && (b[6] & 0x38) == 0x10) || (b[4] == 0xFF && (b[5] & 0x38) == 0x10) ||
           (b[1] == 0xFF && (b[2] & 0x38) == 0x10);
}

}  // namespace

void Install(LogFn log, uintptr_t base) {
    g_log = log;
    g_base = base;
    g_resume = base + kEnemyCtor + 9;
    uint8_t code[9];
    if (!Read(base + kEnemyCtor, code, 9) || memcmp(code, kEntry, 9) != 0) {
        g_log("spawn log: unexpected code at %08X (different game build?); not hooked",
              static_cast<unsigned>(base + kEnemyCtor));
        return;
    }
    uint8_t patch[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&Stub) - (base + kEnemyCtor + 5));
    memcpy(patch + 1, &rel, 4);
    auto* p = reinterpret_cast<uint8_t*>(base + kEnemyCtor);
    DWORD old;
    if (!VirtualProtect(p, 9, PAGE_EXECUTE_READWRITE, &old)) {
        g_log("spawn log: VirtualProtect failed: %lu", GetLastError());
        return;
    }
    // The jmp's first 8 bytes go in one atomic write; the 9th byte (a nop) after it.
    LONG64 from, to;
    memcpy(&from, code, 8);
    memcpy(&to, patch, 8);
    LONG64 seen = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(p), to, from);
    if (seen == from) p[8] = 0x90;
    VirtualProtect(p, 9, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 9);
    g_log(seen == from ? "spawn log: uEnemy constructor hooked (log-only)"
                       : "spawn log: code changed while patching; not hooked");
}

void Poll() {
    LONG written = g_written;
    if (written - g_read > kRing) {
        g_log("spawn log: %ld spawns not logged (too many at once)", written - g_read - kRing);
        g_read = written - kRing;
    }
    ULONGLONG now = GetTickCount64();
    for (; g_read < written; ++g_read) {
        const Spawn& s = g_ring[g_read % kRing];
        if (s.seq != g_read + 1) break;
        if (now - s.tick < 500) break;  // let the constructors finish (final class, position)
        char cls[64];
        ClassName(s.obj, cls, sizeof(cls));
        float pos[3] = {};
        Read(s.obj + kPos, pos, sizeof(pos));
        g_log("spawn: %s %08X pos (%.0f, %.0f, %.0f)", cls, s.obj, pos[0], pos[1], pos[2]);
        char line[1024];
        size_t len = 0;
        line[0] = 0;
        for (size_t i = 0; i < kStackDwords && len < sizeof(line) - 24; ++i)
            if (AfterCall(s.stack[i])) {
                int k = sprintf_s(line + len, sizeof(line) - len, " +%X", s.stack[i] - static_cast<uint32_t>(g_base));
                if (k > 0) len += k;
            }
        g_log("  callers:%s", len ? line : " (none found)");
    }
}

}  // namespace spawnlog
