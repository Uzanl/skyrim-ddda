// Spawn log: see spawn_log.h.
//
// uEnemy's constructor (+0x6A7B60, static 2026-10-10) takes the new object in edi (custom
// convention): it calls the base constructor (+0x44A100), then writes uEnemy's vtable
// (0x15DF2A8) into [edi]. Over 30 enemy constructors call it. Its first 9 bytes are whole
// instructions (push ebp; mov ebp, esp; and esp, -16; sub esp, 0x38), so they become a jmp
// to Stub, which records the call and runs them before going on at +9.
//
// uEnemy's destructor (+0x6A8730, thiscall, this in ecx) is hooked the same way, its first
// 10 bytes (push ebx; push esi; mov esi, ecx; mov ecx, [esi+0x5FF4]) replayed: the class and
// position are read in the hook (the object is freed right after), the callers later.
// 2026-10-10: in a bridge session every enemy was destroyed within ~30 s without moving.
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
constexpr uintptr_t kEnemyDtor = 0x6A8730;
const uint8_t kDtorEntry[10] = {0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0xF4, 0x5F, 0x00, 0x00};
constexpr uintptr_t kPos = 0x40;
constexpr size_t kStackDwords = 256;  // 1 KB of the caller's stack

LogFn g_log = nullptr;
uintptr_t g_base = 0;
uintptr_t g_resume = 0, g_dtorResume = 0;

struct Spawn {
    volatile LONG seq;
    bool destroyed;     // from the destructor: cls and pos read in the hook
    char cls[32];
    float pos[3];
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
void ClassName(uint32_t obj, char* out, size_t cap);

void Fill(Spawn& s, const uint32_t* frame, uint32_t obj, bool destroyed) {
    s.obj = obj;
    s.destroyed = destroyed;
    if (destroyed) {
        ClassName(obj, s.cls, sizeof(s.cls));
        if (!Read(obj + kPos, s.pos, sizeof(s.pos))) memset(s.pos, 0, sizeof(s.pos));
    }
    // In 64-byte pieces up to the first unreadable one: 95 of 113 whole 1 KB copies failed
    // (2026-10-10), probably at the end of the thread's stack.
    memset(s.stack, 0, sizeof(s.stack));
    const auto* src = reinterpret_cast<const uint8_t*>(frame + 8);
    auto* dst = reinterpret_cast<uint8_t*>(s.stack);
    for (size_t off = 0; off < sizeof(s.stack); off += 64)
        if (!Read(reinterpret_cast<uintptr_t>(src + off), dst + off, 64)) break;
    s.tick = GetTickCount64();
}

void __stdcall Record(const uint32_t* frame) {
    LONG n = InterlockedIncrement(&g_written);
    Spawn& s = g_ring[(n - 1) % kRing];
    Fill(s, frame, frame[0], false);  // edi
    InterlockedExchange(&s.seq, n);
}

void __stdcall RecordDtor(const uint32_t* frame) {
    LONG n = InterlockedIncrement(&g_written);
    Spawn& s = g_ring[(n - 1) % kRing];
    Fill(s, frame, frame[6], true);  // ecx
    InterlockedExchange(&s.seq, n);
}

__declspec(naked) void DtorStub() {
    __asm {
        pushad
        push esp
        call RecordDtor
        popad
        push ebx
        push esi
        mov esi, ecx
        mov ecx, dword ptr [esi + 0x5FF4]
        jmp dword ptr [g_dtorResume]
    }
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

// Replaces the first `len` (9..16) bytes at base+at with a jmp to stub, after checking them.
bool Patch(uintptr_t at, const uint8_t* expect, size_t len, void* stub, const char* what) {
    uint8_t code[16];
    if (!Read(g_base + at, code, len) || memcmp(code, expect, len) != 0) {
        g_log("spawn log: unexpected code at %08X (different game build?); %s not hooked",
              static_cast<unsigned>(g_base + at), what);
        return false;
    }
    uint8_t patch[16];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) - (g_base + at + 5));
    memcpy(patch + 1, &rel, 4);
    auto* p = reinterpret_cast<uint8_t*>(g_base + at);
    DWORD old;
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old)) {
        g_log("spawn log: VirtualProtect failed: %lu", GetLastError());
        return false;
    }
    // The jmp's first 8 bytes go in one atomic write; the rest (nops, never run) after it.
    LONG64 from, to;
    memcpy(&from, code, 8);
    memcpy(&to, patch, 8);
    LONG64 seen = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(p), to, from);
    if (seen == from) memcpy(p + 8, patch + 8, len - 8);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    g_log(seen == from ? "spawn log: %s hooked (log-only)" : "spawn log: code changed while patching; %s not hooked",
          what);
    return seen == from;
}

void Install(LogFn log, uintptr_t base) {
    g_log = log;
    g_base = base;
    g_resume = base + kEnemyCtor + sizeof(kEntry);
    g_dtorResume = base + kEnemyDtor + sizeof(kDtorEntry);
    Patch(kEnemyCtor, kEntry, sizeof(kEntry), reinterpret_cast<void*>(&Stub), "uEnemy constructor");
    Patch(kEnemyDtor, kDtorEntry, sizeof(kDtorEntry), reinterpret_cast<void*>(&DtorStub), "uEnemy destructor");
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
        if (!s.destroyed && now - s.tick < 500) break;  // let the constructors finish
        if (s.destroyed) {
            g_log("destroy: %s %08X pos (%.0f, %.0f, %.0f)", s.cls, s.obj, s.pos[0], s.pos[1], s.pos[2]);
        } else {
            char cls[64];
            ClassName(s.obj, cls, sizeof(cls));
            float pos[3] = {};
            Read(s.obj + kPos, pos, sizeof(pos));
            g_log("spawn: %s %08X pos (%.0f, %.0f, %.0f)", cls, s.obj, pos[0], pos[1], pos[2]);
        }
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
