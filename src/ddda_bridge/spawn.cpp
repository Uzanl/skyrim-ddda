// Spawn test: see spawn.h.
//
// +0x3613D0 (static 2026-10-10): eax = an object whose +4 is the enemy's DTI, stack: the
// cLayoutSetEnemy (cGroupParam at +0x74), a flag byte (0 = enemy path), the record holder
// (+4 copied into the unit's +0x20F0, +8 the placement record, a cSetInfoEnemy*); ret 0xC;
// returns the new unit in eax (0 on failure). The record's mPosition (+0x30) is global.
// Its first 9 bytes are whole instructions (push ebp; mov ebp, esp; and esp, -16;
// sub esp, 0x34), so they become a jmp to Stub, which records the call and runs them.
#include "spawn.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace spawn {
namespace {

constexpr uintptr_t kCreate = 0x3613D0;  // image-relative
const uint8_t kEntry[9] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x34};
constexpr uintptr_t kPos = 0x40;
constexpr uintptr_t kRecPos = 0x30;
constexpr size_t kRecBytes = 0x200, kHolderBytes = 0x20;
constexpr uintptr_t kCamRoot = 0x14D1578, kCamOff = 0xDF0;  // camera position (ddda-memory.md)
constexpr float kAhead = 400.0f, kTile = 10000.0f;

LogFn g_log = nullptr;
uintptr_t g_base = 0;
uintptr_t g_resume = 0;

// The last real call (written by game threads under the SRW lock).
struct Capture {
    uint32_t obj, layout, flag, holder, record;
    uint8_t holderCopy[kHolderBytes];
    uint8_t recordCopy[kRecBytes];
};
SRWLOCK g_lock = SRWLOCK_INIT;
Capture g_last;
bool g_haveLast = false;
volatile LONG g_captures = 0;

// Our records and holders must outlive the spawned units: one slot each, never reused.
constexpr int kSlots = 16;
struct Slot {
    alignas(16) uint8_t holder[kHolderBytes];
    alignas(16) uint8_t record[kRecBytes];
};
Slot g_slots[kSlots];
volatile LONG g_used = 0;

volatile LONG g_pending = 0;  // spawns requested, not run yet
volatile LONG g_session = 0, g_originValid = 0, g_tileN = 0, g_tileM = 0;
int g_lastN = -1;

// Results for the log (written by the Arisen's thread).
struct Result {
    uint32_t unit, record;
    float pos[3];
    bool fault;
    const char* why;  // set when the spawn did not run
};
constexpr LONG kResults = 32;  // ring, indexed by g_resultCount
Result g_results[kResults];
volatile LONG g_resultCount = 0;
LONG g_resultsLogged = 0;
LONG g_capturesLogged = 0;

bool Read(uintptr_t addr, void* out, size_t n) {
    if (addr < 0x10000) return false;
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), n);
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

// frame: pushad block (eax at +7), then the return address, layout, flag, holder.
void __stdcall Record(const uint32_t* frame) {
    Capture c = {};
    c.obj = frame[7];
    c.layout = frame[9];
    c.flag = frame[10];
    c.holder = frame[11];
    if (c.flag & 0xFF) return;  // the other (non-enemy) path
    if (!Read(c.holder, c.holderCopy, kHolderBytes)) return;
    memcpy(&c.record, c.holderCopy + 8, 4);
    if (!Read(c.record, c.recordCopy, kRecBytes)) return;
    AcquireSRWLockExclusive(&g_lock);
    g_last = c;
    g_haveLast = true;
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedIncrement(&g_captures);
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
        sub esp, 0x34
        jmp dword ptr [g_resume]
    }
}

// Calls the original (past our jmp: the stub's replayed entry) with eax = obj.
uint32_t CallCreate(uint32_t obj, uint32_t layout, uint32_t holder) {
    uint32_t fn = reinterpret_cast<uint32_t>(&Stub);  // runs Record too (harmless)
    uint32_t unit = 0;
    __asm {
        push holder
        push 0
        push layout
        mov eax, obj
        call fn
        mov unit, eax
    }
    return unit;
}

void ClassName(uint32_t obj, char* out, size_t cap) {
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

bool Sane(const float* p) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(p[i]) || std::fabs(p[i]) > 2.0e6f) return false;
    return true;
}

void RunSpawn(uintptr_t arisen) {
    Capture c;
    AcquireSRWLockShared(&g_lock);
    bool have = g_haveLast;
    c = g_last;
    ReleaseSRWLockShared(&g_lock);
    LONG idx = g_used;
    if (idx >= kSlots) return;  // all slots used: nothing more is spawned
    Result& r = g_results[g_resultCount % kResults];  // only the Arisen's thread writes
    r = {};
    float a[3], cam[3];
    r.why = !have ? "no real enemy creation seen yet"
            : !g_originValid ? "tile origin unknown"
            : (!Read(arisen + kPos, a, sizeof(a)) || !Sane(a)) ? "Arisen position unreadable"
            : nullptr;
    if (r.why) {
        InterlockedIncrement(&g_resultCount);
        return;
    }
    float fx = 0, fz = 1;
    if (Read(U32(g_base + kCamRoot) + kCamOff, cam, sizeof(cam)) && Sane(cam)) {
        float dx = a[0] - cam[0], dz = a[2] - cam[2], l = std::hypot(dx, dz);
        if (l > 1.0f) fx = dx / l, fz = dz / l;
    }
    Slot& s = g_slots[idx];
    memcpy(s.holder, c.holderCopy, kHolderBytes);
    memcpy(s.record, c.recordCopy, kRecBytes);
    uint32_t rec = reinterpret_cast<uint32_t>(s.record);
    memcpy(s.holder + 8, &rec, 4);
    float g[3] = {a[0] + fx * kAhead + (static_cast<float>(g_tileN) - 50.0f) * kTile, a[1] + 50.0f,
                  a[2] + fz * kAhead + (static_cast<float>(g_tileM) - 50.0f) * kTile};
    memcpy(s.record + kRecPos, g, sizeof(g));
    r.record = c.record;
    r.unit = 0;
    r.fault = false;
    __try {
        r.unit = CallCreate(c.obj, c.layout, reinterpret_cast<uint32_t>(s.holder));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r.fault = true;
    }
    if (r.unit) Read(r.unit + kPos, r.pos, sizeof(r.pos));
    InterlockedIncrement(&g_used);
    InterlockedIncrement(&g_resultCount);
}

}  // namespace

void Install(LogFn log, uintptr_t base) {
    g_log = log;
    g_base = base;
    g_resume = base + kCreate + 9;
    uint8_t code[9];
    if (!Read(base + kCreate, code, 9) || memcmp(code, kEntry, 9) != 0) {
        g_log("spawn: unexpected code at %08X (different game build?); not hooked",
              static_cast<unsigned>(base + kCreate));
        return;
    }
    uint8_t patch[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&Stub) - (base + kCreate + 5));
    memcpy(patch + 1, &rel, 4);
    auto* p = reinterpret_cast<uint8_t*>(base + kCreate);
    DWORD old;
    if (!VirtualProtect(p, 9, PAGE_EXECUTE_READWRITE, &old)) {
        g_log("spawn: VirtualProtect failed: %lu", GetLastError());
        return;
    }
    LONG64 from, to;
    memcpy(&from, code, 8);
    memcpy(&to, patch, 8);
    LONG64 seen = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(p), to, from);
    if (seen == from) p[8] = 0x90;
    VirtualProtect(p, 9, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 9);
    g_log(seen == from ? "spawn: enemy creation watched (log-only until a spawn line)"
                       : "spawn: code changed while patching; not hooked");
}

void SetRequest(int n, bool session, bool originValid, int tileN, int tileM) {
    InterlockedExchange(&g_session, session ? 1 : 0);
    InterlockedExchange(&g_originValid, originValid ? 1 : 0);
    InterlockedExchange(&g_tileN, tileN);
    InterlockedExchange(&g_tileM, tileM);
    if (n != g_lastN) {
        if (g_lastN >= 0 && n >= 0 && session) {
            InterlockedIncrement(&g_pending);
            g_log("spawn: request %d (one enemy 4 m in front of the Arisen)", n);
        }
        g_lastN = n;
    }
}

void ArisenTick(uintptr_t arisen) {
    if (!g_session || !g_pending) return;
    InterlockedDecrement(&g_pending);
    RunSpawn(arisen);
}

void Poll() {
    LONG caps = g_captures;
    if (caps != g_capturesLogged) {
        Capture c;
        AcquireSRWLockShared(&g_lock);
        c = g_last;
        ReleaseSRWLockShared(&g_lock);
        char cls[64];
        ClassName(c.record, cls, sizeof(cls));
        float p[3];
        memcpy(p, c.recordCopy + kRecPos, sizeof(p));
        g_log("spawn: %ld real creations seen; last %s record %08X layout %08X obj %08X at (%.0f, %.0f, %.0f)",
              caps, cls, c.record, c.layout, c.obj, p[0], p[1], p[2]);
        g_capturesLogged = caps;
    }
    for (; g_resultsLogged < g_resultCount; ++g_resultsLogged) {
        const Result& r = g_results[g_resultsLogged % kResults];
        char cls[64] = "-";
        if (r.unit) ClassName(r.unit, cls, sizeof(cls));
        if (r.why) {
            g_log("spawn: not run (%s)", r.why);
            continue;
        }
        g_log("spawn: %s; unit %s %08X at (%.0f, %.0f, %.0f) from record %08X",
              r.fault ? "FAULT inside the game's creation" : (r.unit ? "created" : "returned 0"), cls, r.unit,
              r.pos[0], r.pos[1], r.pos[2], r.record);
    }
}

}  // namespace spawn
