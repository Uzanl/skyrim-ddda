// Spawn test: see spawn.h.
//
// +0x3613D0 (static 2026-10-10): eax = an object whose +4 is the enemy's DTI, stack: the
// cLayoutSetEnemy (cGroupParam at +0x74), a flag byte (0 = enemy path), the record holder
// (+4 copied into the unit's +0x20F0, +8 the placement record, a cSetInfoEnemy*); ret 0xC;
// returns the new unit in eax (0 on failure). The record's mPosition (+0x30) is global.
// An enemy's archive (rArchive "rom\enemy\emNNNN": model, motion, sounds) is released when
// the party leaves its area; a wolf spawned after the link then stopped DDDA with "Failed
// open file ... e0200.bmse" (2026-10-10). So, in a session, every enemy archive found in
// sResource's table ([0x18D0AA0] + 0x40D8, 16384 resource pointers, read live) gets one
// extra reference (cResource mRefCount +0x48, under sResource's lock at +4, as its release
// +0x9BA940 does) and stays loaded.
//
// Its first 9 bytes are whole instructions (push ebp; mov ebp, esp; and esp, -16;
// sub esp, 0x34), so they become a jmp to Stub, which records the call and runs them.
//
// State watch: a spawned wolf with its archive loaded was destroyed 0.5 s after creation on
// the generated ground (2026-10-10). The kill request (state 3 in the unit's +4, written
// inline in ~80 places) is found with a hardware write watchpoint: right after a spawn the
// bridge thread sets DR0 on the unit's +4 in every other thread for kWatchMs; a vectored
// handler records each write (instruction after it, new value, the stack) and continues.
#include "spawn.h"

#include <windows.h>
#include <tlhelp32.h>

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

constexpr uintptr_t kResourceMgr = 0x14D0AA0;  // image-relative [sResource]
constexpr uintptr_t kResTable = 0x40D8, kResSlots = 16384, kResLock = 4;
constexpr uintptr_t kArchiveVt = 0x102E2CC;  // rArchive, image-relative
constexpr uintptr_t kResPath = 0x08, kResRef = 0x48;
constexpr int kMaxPins = 12;

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

// --- state watch ---
constexpr ULONGLONG kWatchMs = 3000;
constexpr int kMaxHits = 16;
constexpr size_t kHitStack = 64;
struct WatchHit {
    uint32_t eip, value, tid;
    uint32_t stack[kHitStack];
};
WatchHit g_hits[kMaxHits];
volatile LONG g_hitCount = 0;
volatile uintptr_t g_watchAddr = 0;
ULONGLONG g_watchSince = 0;

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = e->ContextRecord;
    if (!(c->Dr6 & 1) || !g_watchAddr) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 &= ~0xFu;
    LONG n = InterlockedIncrement(&g_hitCount);
    if (n <= kMaxHits) {
        WatchHit& h = g_hits[n - 1];
        h.eip = c->Eip;
        h.tid = GetCurrentThreadId();
        h.value = 0;
        Read(g_watchAddr, &h.value, 4);
        memset(h.stack, 0, sizeof(h.stack));
        Read(c->Esp, h.stack, sizeof(h.stack));
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// DR0 = addr, 4-byte write watch (on) or cleared (off), in every thread but the caller's.
int ArmAll(uintptr_t addr, bool on) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    THREADENTRY32 te = {sizeof(te)};
    int armed = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &ctx)) {
                ctx.Dr7 &= ~0x000F0003u;
                if (on) {
                    ctx.Dr0 = addr;
                    ctx.Dr7 |= 1u | (1u << 16) | (3u << 18);  // L0, write, 4 bytes
                } else {
                    ctx.Dr0 = 0;
                }
                if (SetThreadContext(t, &ctx)) ++armed;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return armed;
}

bool AfterCall(uint32_t v) {
    if (v < g_base + 0x1000 || v >= g_base + 0x1000000) return false;
    uint8_t b[7];
    if (!Read(v - 7, b, 7)) return false;
    return b[2] == 0xE8 || (b[5] == 0xFF && (b[6] & 0x38) == 0x10) || (b[4] == 0xFF && (b[5] & 0x38) == 0x10) ||
           (b[1] == 0xFF && (b[2] & 0x38) == 0x10);
}

void PollWatch(ULONGLONG now) {
    if (!g_watchAddr || now - g_watchSince < kWatchMs) return;
    int n = ArmAll(0, false);
    uintptr_t addr = g_watchAddr;
    g_watchAddr = 0;
    LONG hits = g_hitCount;
    g_log("spawn: state watch on %08X done (%d threads cleared): %ld writes", static_cast<unsigned>(addr), n, hits);
    for (LONG i = 0; i < hits && i < kMaxHits; ++i) {
        const WatchHit& h = g_hits[i];
        char line[700];
        size_t len = 0;
        line[0] = 0;
        for (size_t k = 0; k < kHitStack && len < sizeof(line) - 16; ++k)
            if (AfterCall(h.stack[k])) {
                int w = sprintf_s(line + len, sizeof(line) - len, " +%X", h.stack[k] - static_cast<uint32_t>(g_base));
                if (w > 0) len += w;
            }
        g_log("  write %ld: after +%X, state now %08X (low bits %u), thread %u; callers:%s", i + 1,
              h.eip - static_cast<uint32_t>(g_base), h.value, h.value & 7, h.tid, line);
    }
}

uint32_t g_pinned[kMaxPins];
int g_pinCount = 0;

// Bridge thread, every 2 s in a session.
void PinEnemyArchives() {
    uint32_t mgr = U32(g_base + kResourceMgr);
    if (!mgr || g_pinCount >= kMaxPins) return;
    static uint32_t table[kResSlots];
    if (!Read(mgr + kResTable, table, sizeof(table))) return;
    for (uint32_t r : table) {
        if (!r || U32(r) != g_base + kArchiveVt) continue;
        char path[24] = {};
        if (!Read(r + kResPath, path, sizeof(path) - 1) || strncmp(path, "rom\\enemy\\em", 12) != 0) continue;
        bool known = false;
        for (int i = 0; i < g_pinCount; ++i) known |= g_pinned[i] == r;
        if (known) continue;
        auto* lock = reinterpret_cast<CRITICAL_SECTION*>(mgr + kResLock);
        LONG ref = 0;
        __try {
            EnterCriticalSection(lock);
            ref = ++*reinterpret_cast<volatile LONG*>(r + kResRef);
            LeaveCriticalSection(lock);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_log("spawn: pinning %s faulted", path);
            continue;
        }
        g_pinned[g_pinCount++] = r;
        g_log("spawn: pinned enemy archive %s (%08X), references now %ld", path, r, ref);
        if (g_pinCount >= kMaxPins) break;
    }
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
    AddVectoredExceptionHandler(1, &WatchHandler);
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
    static ULONGLONG lastPin = 0;
    ULONGLONG now = GetTickCount64();
    if (g_session && now - lastPin >= 2000) {
        lastPin = now;
        PinEnemyArchives();
    }
    PollWatch(now);
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
        if (r.unit && !g_watchAddr) {
            g_hitCount = 0;
            g_watchAddr = r.unit + 4;
            g_watchSince = now;
            int n = ArmAll(r.unit + 4, true);
            g_log("spawn: state watch armed on %08X in %d threads for %llu ms", static_cast<unsigned>(r.unit + 4), n,
                  kWatchMs);
        }
        g_log("spawn: %s; unit %s %08X at (%.0f, %.0f, %.0f) from record %08X",
              r.fault ? "FAULT inside the game's creation" : (r.unit ? "created" : "returned 0"), cls, r.unit,
              r.pos[0], r.pos[1], r.pos[2], r.record);
    }
}

}  // namespace spawn
