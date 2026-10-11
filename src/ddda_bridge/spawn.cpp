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

#include "../common/bridge_shared.h"
#include "damage_log.h"

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
volatile LONG g_session = 0;
// The bridge's live tile origin (dllmain.cpp g_orgN/g_orgM/g_orgValid): a copy refreshed once a
// second put the stand-ins 100 m off for a moment after the Arisen crossed a tile (2026-10-10).
const volatile LONG* g_orgNp = nullptr;
const volatile LONG* g_orgMp = nullptr;
const volatile LONG* g_orgValidp = nullptr;
#define g_tileN (*g_orgNp)
#define g_tileM (*g_orgMp)
#define g_originValid (g_orgValidp && *g_orgValidp)
int g_lastN = -1;

// Results for the log (written by the Arisen's thread).
struct Result {
    uint32_t unit, record;
    float pos[3];
    bool fault;
    uint32_t faultAt;  // exception address when fault
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

// The spot 4 m in front of the Arisen (camera -> Arisen, horizontally), global coordinates.
bool SpotGlobal(uintptr_t arisen, float* g) {
    float a[3], cam[3];
    if (!g_originValid || !Read(arisen + kPos, a, sizeof(a)) || !Sane(a)) return false;
    float fx = 0, fz = 1;
    if (Read(U32(g_base + kCamRoot) + kCamOff, cam, sizeof(cam)) && Sane(cam)) {
        float dx = a[0] - cam[0], dz = a[2] - cam[2], l = std::hypot(dx, dz);
        if (l > 1.0f) fx = dx / l, fz = dz / l;
    }
    g[0] = a[0] + fx * kAhead + (static_cast<float>(g_tileN) - 50.0f) * kTile;
    g[1] = a[1] + 50.0f;
    g[2] = a[2] + fz * kAhead + (static_cast<float>(g_tileM) - 50.0f) * kTile;
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
    float g[3];
    r.why = !have ? "no real enemy creation seen yet" : !SpotGlobal(arisen, g) ? "no spot (tile or Arisen unknown)" : nullptr;
    if (r.why) {
        InterlockedIncrement(&g_resultCount);
        return;
    }
    Slot& s = g_slots[idx];
    memcpy(s.holder, c.holderCopy, kHolderBytes);
    memcpy(s.record, c.recordCopy, kRecBytes);
    uint32_t rec = reinterpret_cast<uint32_t>(s.record);
    memcpy(s.holder + 8, &rec, 4);
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

// --- synthetic spawn ("spawnkind ID", docs/ddda-memory.md "Loading any enemy") ---
// Nothing copied from a real creation: the kind row comes from the static table, the
// archive is requested by ID (and pinned), the record (cSetInfoEnemy), the layout
// (cLayoutSetEnemy) and its cGroupParam are new default instances (MtDTI vtable slot 1 =
// newInstance, no arguments), and the record gets the spot as its mPosition.
constexpr uintptr_t kKindTable = 0x110F730;  // image-relative; 112 rows of 0x10
constexpr int kKinds = 112;
constexpr uintptr_t kArcMgr = 0x14D9280, kRequestArc = 0x188C0;
constexpr uintptr_t kRecordDti = 0x1593650, kLayoutDti = 0x1592B98;  // cSetInfoEnemy, cLayoutSetEnemy
// +0x361970 reads the layout's cGroupParam (+0x74) without a null check (first synthetic
// spawn, 2026-10-10: FAULT); a default one (+0x138 = 0, +0x60 = 0) makes it do nothing.
constexpr uintptr_t kGroupDti = 0x15A8804, kLayoutGroup = 0x74;
constexpr uintptr_t kResState = 0x50;  // bit 0 loaded, bit 6 failed
constexpr ULONGLONG kArcTimeoutMs = 20000;
using RequestFn = int(__stdcall*)(uint32_t mgr, uint32_t id, uint32_t* res, uint32_t prio, uint32_t flag);

volatile LONG g_kind = 0;

// Archives requested by ID (one entry per kind; pinned once loaded). Arisen's thread only.
struct Arc {
    LONG kind;
    uint32_t res;
    int handle;
    bool pinned;
    ULONGLONG since;
};
Arc g_arcs[8] = {};

uint32_t g_faultAt = 0;
int FaultFilter(EXCEPTION_POINTERS* e) {
    g_faultAt = reinterpret_cast<uint32_t>(e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

uint32_t KindRow(LONG id) {
    for (int i = 0; i < kKinds; ++i) {
        uint32_t row = static_cast<uint32_t>(g_base + kKindTable + i * 0x10);
        if (U32(row + 8) == static_cast<uint32_t>(id)) return row;
    }
    return 0;
}

uint32_t NewInstance(uint32_t dti) {
    uint32_t fn = U32(U32(dti) + 4), obj = 0;
    if (fn < g_base + 0x1000 || fn >= g_base + 0x1000000) return 0;
    __asm {
        mov ecx, dti
        call fn
        mov obj, eax
    }
    return obj;
}

// 1 = the kind's archive is loaded (and pinned), 0 = still loading, -1 = failed (why set).
int EnsureArchive(LONG kind, const char** why) {
    ULONGLONG now = GetTickCount64();
    Arc* a = nullptr;
    for (Arc& x : g_arcs)
        if (x.kind == kind) a = &x;
    if (!a) {
        for (Arc& x : g_arcs)
            if (!x.kind && !a) a = &x;
        if (!a) {
            *why = "kind: too many kinds";
            return -1;
        }
        *a = {};
        a->kind = kind;
        a->handle = -1;
        a->since = now;
    }
    if (!a->res && a->handle < 0) {
        int handle = -1;
        uint32_t* slot = &a->res;
        uint32_t mgr = U32(g_base + kArcMgr);
        auto request = reinterpret_cast<RequestFn>(g_base + kRequestArc);
        __try {
            handle = request(mgr, static_cast<uint32_t>(kind), slot, 2, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            a->kind = 0;
            *why = "kind: FAULT requesting the archive";
            return -1;
        }
        a->handle = handle;
        if (handle < 0 && !a->res) {
            a->kind = 0;
            *why = "kind: the archive request returned nothing";
            return -1;
        }
    }
    uint32_t st = a->res ? U32(a->res + kResState) : 0;
    if (st & 0x40) {
        a->kind = 0;  // retry from scratch next time
        *why = "kind: the archive failed to load";
        return -1;
    }
    if (!(st & 1)) {
        if (now - a->since <= kArcTimeoutMs) return 0;
        a->kind = 0;
        *why = "kind: archive not loaded after 20 s";
        return -1;
    }
    if (!a->pinned) {
        uint32_t mgr = U32(g_base + kResourceMgr);
        auto* lock = reinterpret_cast<CRITICAL_SECTION*>(mgr + kResLock);
        auto* ref = reinterpret_cast<volatile LONG*>(a->res + kResRef);
        __try {
            EnterCriticalSection(lock);
            ++*ref;
            LeaveCriticalSection(lock);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *why = "kind: FAULT pinning the archive";
            return -1;
        }
        a->pinned = true;
    }
    return 1;
}

// Creates one enemy of `kind` (archive loaded) at global g. Fills r (unit, fault).
void CreateAt(LONG kind, const float* g, Result& r) {
    r = {};
    uint32_t row = KindRow(kind);
    if (!row) {
        r.why = "kind: no row with that archive ID in the kind table";
        return;
    }
    uint32_t rec = 0, lay = 0, grp = 0;
    __try {
        rec = NewInstance(static_cast<uint32_t>(g_base + kRecordDti));
        lay = NewInstance(static_cast<uint32_t>(g_base + kLayoutDti));
        grp = NewInstance(static_cast<uint32_t>(g_base + kGroupDti));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r.why = "kind: FAULT creating the record, layout or group";
        return;
    }
    if (!rec || !lay || !grp) {
        r.why = "kind: newInstance returned 0";
        return;
    }
    memcpy(reinterpret_cast<void*>(lay + kLayoutGroup), &grp, 4);
    // The holder must outlive the unit (its +4 is copied into the unit): never freed.
    auto* holder = new uint8_t[kHolderBytes]();
    memcpy(holder + 8, &rec, 4);
    __try {
        memcpy(reinterpret_cast<void*>(rec + kRecPos), g, 3 * sizeof(float));
        r.unit = CallCreate(row, lay, reinterpret_cast<uint32_t>(holder));
    } __except (FaultFilter(GetExceptionInformation())) {
        r.fault = true;
        r.faultAt = g_faultAt;
    }
    r.record = rec;
    if (r.unit) Read(r.unit + kPos, r.pos, sizeof(r.pos));
}

// Arisen's thread. Returns true when this request is finished (created or given up).
bool SyntheticTick(uintptr_t arisen) {
    Result& r = g_results[g_resultCount % kResults];
    const char* why = nullptr;
    int st = EnsureArchive(g_kind, &why);
    if (st == 0) return false;
    float g[3];
    if (st < 0) {
        r = {};
        r.why = why;
    } else if (!SpotGlobal(arisen, g)) {
        r = {};
        r.why = "no spot (tile or Arisen unknown)";
    } else {
        CreateAt(g_kind, g, r);
    }
    InterlockedIncrement(&g_resultCount);
    return true;
}

// --- stand-ins for Skyrim NPCs ("standins", docs/ddda-memory.md "Stand-ins") ---
// One DDDA enemy per hostile Skyrim NPC that DDDAGhosts publishes (bridge::Npcs): a human
// (he00_00) for a person, a wolf (em0200) for anything else. Its AI stays on; after each of
// its move()s it is put back at the NPC's spot (x/z, height on the first placement), so the
// pawns fight it where the NPC is in Skyrim. It takes no damage (ApplyDamage is zeroed for
// it, the hit reaction still plays); the damage is summed for the NPC (step 2, not done).
// When the NPC is gone for 3 s, the stand-in is killed with a lethal hit through
// ApplyDamage, so it dies the normal way. (2026-10-10: the kill request written directly,
// state 3 in +4, deleted a living enemy the pawns still had in their target lists, and a
// pawn's AI crashed on it at +0x65F1A two seconds later.)
constexpr LONG kHumanKind = 0x6E5, kCreatureKind = 0x49;
// The NPC flickered in and out of the published list (2026-10-10): 3 s before removing.
constexpr ULONGLONG kNpcGoneMs = 3000;
constexpr uintptr_t kStatusPtr = 0x4BC, kStatusToVital = 0x1D0, kVitalOwnerOff = 0x1B4, kVitalHpOff = 8,
                    kVitalMax = 0xC;
// Enemies: [unit+0x1D18] is a list of their vital blocks (read live 2026-10-10 on a wolf
// stand-in: [+0] and [+4], both owned by the unit, 800 max HP; wolves take each hit twice).
constexpr uintptr_t kVitalList = 0x1D18;
constexpr int kMaxStandins = static_cast<int>(bridge::kMaxNpcs);
constexpr uintptr_t kAdjCopies[3] = {0x120, 0x130, 0x140}, kAdjVelocity = 0x150;
constexpr uintptr_t kScrAdjustVt = 0x119704C;  // cScrAdjust, image-relative

struct Standin {
    volatile uint32_t unit;  // 0 = free
    uint32_t formId, vt, adj;
    volatile LONG placeY;    // set the height at the next pin
    float target[3];         // tile-local
    ULONGLONG lastSeen;
    volatile LONG damage;    // summed damage taken, x10 (fixed point)
    LONG kind;
    volatile uint32_t vital; // its vital block, seen in ApplyDamage
    volatile LONG removing;  // the lethal hit must not be zeroed
};
Standin g_si[kMaxStandins] = {};
volatile LONG g_standinsOn = 0;
const bridge::Npcs* g_npcView = nullptr;

struct StandinEvent {
    char what[24];
    uint32_t formId, unit;
    float pos[3];
    const char* why;
};
constexpr LONG kEvents = 32;
StandinEvent g_events[kEvents];
volatile LONG g_eventCount = 0;
LONG g_eventsLogged = 0;

void Event(const char* what, const Standin& s, const char* why = nullptr) {
    StandinEvent& e = g_events[g_eventCount % kEvents];
    strcpy_s(e.what, what);
    e.formId = s.formId;
    e.unit = s.unit;
    memcpy(e.pos, s.target, sizeof(e.pos));
    e.why = why;
    InterlockedIncrement(&g_eventCount);
}

Standin* StandinOf(uint32_t unit) {
    if (!unit) return nullptr;
    for (Standin& s : g_si)
        if (s.unit == unit) return &s;
    return nullptr;
}

// Each stand-in class's move() is wrapped once (vtable in the image's data, slot 8 in its
// code), as hijack.cpp does.
using MoveFnT = void(__thiscall*)(void*);
struct PatchedVt {
    uint32_t vt;
    MoveFnT orig;
};
PatchedVt g_moveVts[4];
volatile LONG g_moveVtCount = 0;

void PinStandin(Standin& s, uintptr_t self) {
    __try {
        auto* p = reinterpret_cast<volatile float*>(self + kPos);
        p[0] = s.target[0];
        p[2] = s.target[2];
        if (InterlockedExchange(&s.placeY, 0)) p[1] = s.target[1];
        if (uintptr_t adj = s.adj) {
            for (uintptr_t off : kAdjCopies) {
                auto* v = reinterpret_cast<volatile float*>(adj + off);
                v[0] = p[0];
                v[1] = p[1];
                v[2] = p[2];
            }
            auto* vel = reinterpret_cast<volatile float*>(adj + kAdjVelocity);
            vel[0] = vel[2] = 0.0f;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall StandinMove(void* self, void* /*edx*/) {
    uint32_t vt = *reinterpret_cast<uint32_t*>(self);
    MoveFnT orig = nullptr;
    for (LONG i = 0; i < g_moveVtCount; ++i)
        if (g_moveVts[i].vt == vt) orig = g_moveVts[i].orig;
    if (orig) orig(self);
    Standin* s = StandinOf(reinterpret_cast<uint32_t>(self));
    if (s && g_standinsOn && s->vt == vt) PinStandin(*s, reinterpret_cast<uintptr_t>(self));
}

bool WrapMove(uint32_t vt) {
    for (LONG i = 0; i < g_moveVtCount; ++i)
        if (g_moveVts[i].vt == vt) return true;
    if (g_moveVtCount >= 4 || vt < g_base + 0x1000000 || vt >= g_base + 0x1400000) return false;
    auto* slot = reinterpret_cast<uint32_t*>(vt + 8 * 4);
    uint32_t fn = U32(reinterpret_cast<uintptr_t>(slot));
    if (fn < g_base + 0x1000 || fn >= g_base + 0x1000000) return false;
    DWORD old;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return false;
    g_moveVts[g_moveVtCount].vt = vt;
    g_moveVts[g_moveVtCount].orig = reinterpret_cast<MoveFnT>(fn);
    InterlockedIncrement(&g_moveVtCount);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(slot), static_cast<LONG>(reinterpret_cast<uintptr_t>(&StandinMove)));
    VirtualProtect(slot, 4, old, &old);
    return true;
}

void KillUnit(uint32_t unit) {
    __try {
        auto* st = reinterpret_cast<volatile LONG*>(unit + 4);
        LONG v = *st;
        InterlockedCompareExchange(st, (v & ~4) | 3, v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool ReadNpcs(bridge::Npcs* out) {
    if (!g_npcView) {
        HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, bridge::kNpcsMappingName);
        if (!h) return false;
        g_npcView = static_cast<const bridge::Npcs*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(bridge::Npcs)));
        CloseHandle(h);
        if (!g_npcView) return false;
    }
    if (g_npcView->magic != bridge::kNpcsMagic) return false;
    for (int tries = 0; tries < 100; ++tries) {
        uint32_t a = g_npcView->seq;
        if (a & 1) continue;
        MemoryBarrier();
        memcpy(out, const_cast<const bridge::Npcs*>(g_npcView), sizeof(*out));
        MemoryBarrier();
        if (g_npcView->seq == a) return GetTickCount() - out->writerTick <= bridge::kNpcsStaleMs;
    }
    return false;
}

// The stand-in's living vital blocks (owner field +0x1B4 = the unit, HP > 0): the one seen
// in a hit, the enemy list at [unit+0x1D18], and the party's layout ([unit+0x4BC] + 0x1D0).
int VitalsOf(const Standin& s, uint32_t* out, int cap) {
    uint32_t cand[10] = {s.vital};
    int n = 1;
    if (uint32_t list = U32(s.unit + kVitalList))
        for (uint32_t k = 0; k < 8; ++k) cand[n++] = U32(list + k * 4);
    if (uint32_t st = U32(s.unit + kStatusPtr)) cand[n++] = st + kStatusToVital;
    int found = 0;
    for (int i = 0; i < n && found < cap; ++i) {
        uint32_t v = cand[i];
        float hp = 0;
        if (!v || U32(v + kVitalOwnerOff) != s.unit || !Read(v + kVitalHpOff, &hp, 4) || !(hp > 0)) continue;
        bool dup = false;
        for (int j = 0; j < found; ++j) dup |= out[j] == v;
        if (!dup) out[found++] = v;
    }
    return found;
}

void RemoveStandin(Standin& s, const char* why) {
    if (s.unit && U32(s.unit) == s.vt) {
        uint32_t vitals[4];
        int n = VitalsOf(s, vitals, 4);
        if (!n) {
            Event("kept", s, "no vital block to kill it with (left alive)");
            s.lastSeen = GetTickCount64();  // try again later
            return;
        }
        InterlockedExchange(&s.removing, 1);
        for (int i = 0; i < n; ++i) {
            float max = 0;
            Read(vitals[i] + kVitalMax, &max, 4);
            __try {
                damagelog::Apply(vitals[i], (max > 0 && max < 1.0e6f ? max : 10000.0f) * 10.0f);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Event("removed", s, "FAULT in the lethal hit");
            }
        }
    }
    Event("removed", s, why);
    s.unit = 0;
}

// Arisen's thread, every frame while "standins" is on in a session.
void StandinTick() {
    if (!g_orgNp || !g_orgMp) return;
    ULONGLONG now = GetTickCount64();
    bridge::Npcs npcs;
    if (!ReadNpcs(&npcs)) npcs.count = 0;
    if (npcs.count > bridge::kMaxNpcs) npcs.count = 0;
    const float ox = (static_cast<float>(g_tileN) - 50.0f) * kTile, oz = (static_cast<float>(g_tileM) - 50.0f) * kTile;
    bool seen[bridge::kMaxNpcs] = {};
    for (Standin& s : g_si) {
        if (!s.unit) continue;
        if (U32(s.unit) != s.vt) {  // destroyed by DDDA: made again next frame
            Event("lost", s, "destroyed by DDDA");
            s.unit = 0;
            continue;
        }
        bool found = false;
        for (uint32_t i = 0; i < npcs.count; ++i)
            if (npcs.npcs[i].formId == s.formId) {
                const float* g = npcs.npcs[i].pos;
                s.target[0] = g[0] - ox;
                s.target[1] = g[1];
                s.target[2] = g[2] - oz;
                s.lastSeen = now;
                seen[i] = found = true;
            }
        if (!found && now - s.lastSeen > kNpcGoneMs) RemoveStandin(s, "the NPC is gone");
    }
    if (!g_originValid) return;
    // At most one new stand-in per frame.
    for (uint32_t i = 0; i < npcs.count; ++i) {
        if (seen[i]) continue;
        Standin* free = nullptr;
        for (Standin& s : g_si)
            if (!s.unit && !free) free = &s;
        if (!free) return;
        const bridge::Npc& n = npcs.npcs[i];
        LONG kind = (n.flags & bridge::kNpcHuman) ? kHumanKind : kCreatureKind;
        const char* why = nullptr;
        int st = EnsureArchive(kind, &why);
        if (st == 0) return;  // loading: try again next frame
        Standin s = {};
        s.formId = n.formId;
        s.kind = kind;
        s.target[0] = n.pos[0] - ox;
        s.target[1] = n.pos[1];
        s.target[2] = n.pos[2] - oz;
        if (st < 0) {
            Event("not made", s, why);
            return;
        }
        Result r;
        CreateAt(kind, n.pos, r);
        if (!r.unit) {
            Event("not made", s, r.fault ? "FAULT in the creation" : r.why);
            return;
        }
        s.vt = U32(r.unit);
        for (uintptr_t off = 0; off < 0x6000; off += 4)
            if (U32(r.unit + off) == g_base + kScrAdjustVt) {
                s.adj = r.unit + static_cast<uint32_t>(off);
                break;
            }
        s.placeY = 1;
        s.lastSeen = now;
        if (!WrapMove(s.vt)) {
            KillUnit(r.unit);
            s.unit = r.unit;
            Event("not made", s, "could not wrap its move()");
            return;
        }
        *free = s;
        free->unit = r.unit;  // last: the move hook and the damage filter see it from now on
        Event("made", *free);
        return;
    }
}

// damage_log's filter (game threads, inside ApplyDamage): a stand-in's damage is zeroed.
bool StandinDamage(uint32_t owner, uint32_t vital, float damage) {
    Standin* s = StandinOf(owner);
    if (!s || !g_standinsOn || s->removing) return false;
    s->vital = vital;
    InterlockedExchangeAdd(&s->damage, static_cast<LONG>(damage * 10.0f));
    return true;
}

}  // namespace

void SetKind(int archiveId) {
    if (archiveId == g_kind) return;
    InterlockedExchange(&g_kind, archiveId);
    if (archiveId) g_log("spawn: kind = archive %X (synthetic spawns, nothing copied)", archiveId);
    else g_log("spawn: kind off (spawns copy the last real creation)");
}

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

void SetTileOrigin(const volatile LONG* n, const volatile LONG* m, const volatile LONG* valid) {
    g_orgNp = n;
    g_orgMp = m;
    g_orgValidp = valid;
}

void SetRequest(int n, bool session, bool originValid, int tileN, int tileM) {
    InterlockedExchange(&g_session, session ? 1 : 0);
    (void)originValid;
    (void)tileN;
    (void)tileM;
    if (n != g_lastN) {
        if (g_lastN >= 0 && n >= 0 && session) {
            InterlockedIncrement(&g_pending);
            g_log("spawn: request %d (one enemy 4 m in front of the Arisen)", n);
        }
        g_lastN = n;
    }
}

void SetStandins(bool on) {
    on = on && g_session;
    if (on == (g_standinsOn != 0)) return;
    InterlockedExchange(&g_standinsOn, on ? 1 : 0);
    if (on) damagelog::SetFilter(&StandinDamage);
    g_log("standins %s", on ? "ON (one DDDA enemy at each hostile Skyrim NPC)" : "OFF");
}

void ArisenTick(uintptr_t arisen) {
    if (g_session) {
        if (g_standinsOn) {
            StandinTick();
        } else {
            for (Standin& s : g_si)
                if (s.unit) RemoveStandin(s, "standins off");
        }
    }
    if (!g_session || !g_pending) return;
    if (g_kind) {
        if (SyntheticTick(arisen)) InterlockedDecrement(&g_pending);
        return;
    }
    InterlockedDecrement(&g_pending);
    RunSpawn(arisen);
}

void Poll() {
    static ULONGLONG lastPin = 0, lastStatus = 0;
    ULONGLONG now = GetTickCount64();
    for (; g_eventsLogged < g_eventCount; ++g_eventsLogged) {
        const StandinEvent& e = g_events[g_eventsLogged % kEvents];
        g_log("standin %s: NPC %08X unit %08X at local (%.0f, %.0f, %.0f)%s%s", e.what, e.formId, e.unit, e.pos[0],
              e.pos[1], e.pos[2], e.why ? ": " : "", e.why ? e.why : "");
    }
    if (g_standinsOn && now - lastStatus >= 5000) {
        lastStatus = now;
        char line[600];
        size_t len = 0;
        line[0] = 0;
        int n = 0;
        for (const Standin& s : g_si) {
            if (!s.unit) continue;
            ++n;
            float p[3] = {};
            Read(s.unit + kPos, p, sizeof(p));
            int k = sprintf_s(line + len, sizeof(line) - len, " %08X@(%.0f,%.0f,%.0f) dmg %.0f", s.formId, p[0], p[1],
                              p[2], s.damage / 10.0f);
            if (k > 0) len += k;
            if (len > sizeof(line) - 80) break;
        }
        g_log("standins: %d%s", n, line);
    }
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
        g_log("spawn: %s; unit %s %08X at (%.0f, %.0f, %.0f) from record %08X%s",
              r.fault ? "FAULT inside the game's creation" : (r.unit ? "created" : "returned 0"), cls, r.unit,
              r.pos[0], r.pos[1], r.pos[2], r.record, "");
        if (r.fault && r.faultAt) g_log("spawn: the fault was at +%X", r.faultAt - static_cast<uint32_t>(g_base));
    }
}

}  // namespace spawn
