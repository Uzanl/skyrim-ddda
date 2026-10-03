// dinput8.dll proxy for DDDA (32-bit). Forwards DirectInput8Create to the
// system DLL, hooks the per-frame move() of the Arisen and party pawns to learn
// their objects, and publishes their state to shared memory (bridge_shared.h).
// Game memory is only read, except the hooked vtable slots and, while an
// override is active, the main camera's pose (CameraCmd in bridge_shared.h) and
// the parts masks that hide the Arisen and the models it carries.
// Offsets and their validation live in docs/ddda-memory.md.
#include <windows.h>

#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <share.h>

#include "../common/bridge_shared.h"
#include "frame_capture.h"
#include "frame_trace.h"
#include "isolate.h"
#include "relight.h"

#if !defined(_M_IX86)
#error "DDDA is 32-bit; build this DLL for x86."
#endif

#pragma comment(linker, "/EXPORT:DirectInput8Create=_Proxy_DirectInput8Create@20")

namespace {

// Image-relative (DDDA.exe loads at 0x400000, no ASLR).
constexpr uintptr_t kPlayerVtable = 0x11E90D0;
constexpr uintptr_t kPawnVtable = 0x11E8468;
constexpr uintptr_t kPlayerMove = 0x755F20;  // expected original slot 8 values
constexpr uintptr_t kPawnMove = 0x753650;
constexpr uintptr_t kMoveSlot = 8;
constexpr uintptr_t kCamRoot = 0x14D1578;
constexpr uintptr_t kCamOff = 0xDF0;

// Main game camera (uCameraBase subclass). Its slot 9 runs once per frame and
// computes the pose; the hook replaces the pose right after.
constexpr uintptr_t kCamVtable = 0x119AC90;
constexpr uintptr_t kCamUpdate = 0x3D7180;  // expected original slot 9
constexpr uintptr_t kCamUpdateSlot = 9;
constexpr uintptr_t kCamFov = 0x3C;     // vertical FOV, degrees
constexpr uintptr_t kCamPos = 0x40;     // mCameraPos
constexpr uintptr_t kCamUp = 0x50;
constexpr uintptr_t kCamTarget = 0x60;  // mTargetPos

// Models the Arisen carries; their move() (slot 8) runs once per frame.
constexpr uintptr_t kWeaponVtable = 0x1202730;   // uWeaponPl
constexpr uintptr_t kWeaponMove = 0x81B0D0;
constexpr uintptr_t kLanternVtable = 0x1203DD8;  // uOmObj10410
constexpr uintptr_t kLanternMove = 0x826E40;
constexpr uintptr_t kOwner = 0x30;  // carried model -> owning character
// Open-world tile streaming (docs/ddda-memory.md): uStageSplitCtrl's "Player Now"
// tile. Positions are tile-local; global = local + ((N - 50), 0, (M - 50)) * 10000.
constexpr uintptr_t kSplitVtable = 0x120CD58;  // uStageSplitCtrl
constexpr uintptr_t kSplitMove = 0x85CA60;     // expected original slot 8
constexpr uintptr_t kSplitN = 0x40;            // int32, x
constexpr uintptr_t kSplitM = 0x44;            // int32, z
constexpr float kTile = 10000.0f;
// uModel parts display mask: one bit per mesh part. Zero hides the model; the
// game keeps whatever is written there.
constexpr uintptr_t kPartsMask = 0x110;
constexpr int kPartsMaskWords = 16;

// Character fields.
constexpr uintptr_t kPos = 0x40;
constexpr uintptr_t kStatus = 0x4BC;
constexpr uintptr_t kHp = 0x1D8;  // HP max follows at +4
constexpr uintptr_t kRecord = 0x3DEC;
// pawn record = Arisen record + kFirstPawnRecord + kPawnRecordSize * slot
constexpr uintptr_t kFirstPawnRecord = 0x7F0;
constexpr uintptr_t kPawnRecordSize = 0x1660;

constexpr DWORD kPollMs = 8;
constexpr ULONGLONG kShowDelayMs = 500;  // override off this long before the Arisen reappears

HMODULE g_self = nullptr;
wchar_t g_folder[MAX_PATH] = L".";
uintptr_t g_base = 0;
volatile LONG g_stop = 0;
bridge::State* g_state = nullptr;
FILE* g_log = nullptr;
LARGE_INTEGER g_qpcFreq;

using MoveFn = void(__thiscall*)(void*);
MoveFn g_origPlayerMove = nullptr;
MoveFn g_origPawnMove = nullptr;
MoveFn g_origCamUpdate = nullptr;
MoveFn g_origWeaponMove = nullptr;
MoveFn g_origLanternMove = nullptr;
MoveFn g_origSplitMove = nullptr;
volatile LONG g_splitObj = 0;  // uStageSplitCtrl, captured by its move()
volatile LONG g_arisenObj = 0;

// Camera override, filled by the bridge thread from CameraCmd, applied by the
// camera hook on the game thread.
struct CamOverride {
    bool active;
    bool moveArisen;
    float body[3];
    float fovY;
    float pos[3], target[3], up[3];
    float shiftX, shiftZ;  // the treadmill shift Skyrim used
    bool global;           // kGlobalCoords: terrain mode, positions are open-world global
    bool hasPose;          // skyPose holds the Skyrim camera the command came from
    float skyPose[bridge::kSkyPoseFloats];
};
SRWLOCK g_camLock = SRWLOCK_INIT;
CamOverride g_camOverride;

void Log(const char* fmt, ...);

// --- Treadmill shift (bridge_shared.h, Shift) ----------------------------------------
// While linked, the party stays within kTreadmillRadius of where the link started
// in DDDA, inside the area DDDA's pawns can navigate (away from it they could not
// path to the Arisen, and DDDA warped them onto it every frame). When the Arisen
// goes further, the whole party moves back and the shift grows by that move;
// Skyrim's conversions add the shift, so nothing moves on Skyrim's side.
constexpr float kTreadmillRadius = 3000.0f;  // cm; each move confuses the pawns (their AI keeps absolute targets), so keep moves rare
SRWLOCK g_shiftLock = SRWLOCK_INIT;
float g_shiftNow[2];
bridge::Shift* g_shiftOut = nullptr;

void GetShift(float* out) {
    AcquireSRWLockShared(&g_shiftLock);
    out[0] = g_shiftNow[0];
    out[1] = g_shiftNow[1];
    ReleaseSRWLockShared(&g_shiftLock);
}

void SetShift(float x, float z) {
    AcquireSRWLockExclusive(&g_shiftLock);
    g_shiftNow[0] = x;
    g_shiftNow[1] = z;
    if (bridge::Shift* o = g_shiftOut) {
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&o->seq));  // odd: writing
        o->dx = x;
        o->dz = z;
        o->generation++;
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&o->seq));  // even: done
    }
    ReleaseSRWLockExclusive(&g_shiftLock);
}

void OpenShiftMapping() {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(bridge::Shift),
                                  bridge::kShiftMappingName);
    auto* v = h ? static_cast<bridge::Shift*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bridge::Shift)))
                : nullptr;
    if (!v) {
        Log("shift mapping failed: %lu", GetLastError());
        return;
    }
    v->magic = bridge::kShiftMagic;
    v->version = bridge::kShiftVersion;
    g_shiftOut = v;
    SetShift(0.0f, 0.0f);
}

// Brings a camera command computed with an older shift up to date.
void AlignToShift(CamOverride& o) {
    if (o.global) return;  // terrain mode has no treadmill
    float sh[2];
    GetShift(sh);
    float dx = sh[0] - o.shiftX, dz = sh[1] - o.shiftZ;
    if (dx == 0.0f && dz == 0.0f) return;
    o.pos[0] += dx;
    o.pos[2] += dz;
    o.target[0] += dx;
    o.target[2] += dz;
    o.body[0] += dx;
    o.body[2] += dz;
    o.shiftX = sh[0];
    o.shiftZ = sh[1];
}
volatile LONG g_camApplied = 0;

// Written by move() hooks on game worker threads, read by the bridge thread.
struct Captured {
    bool seen;
    bool hpValid;
    LONGLONG qpc;
    float pos[3];
    float hp, hpMax;
};
SRWLOCK g_lock = SRWLOCK_INIT;
Captured g_captured[bridge::kRoleCount];
volatile LONG g_arisenRecord = 0;
volatile LONG g_rejects = 0;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// SEH-guarded reads; kept free of C++ objects so __try is allowed.
bool ReadU32(uintptr_t addr, uint32_t* out) {
    if (addr < 0x10000) return false;
    __try {
        *out = *reinterpret_cast<const volatile uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadFloats(uintptr_t addr, float* out, int n) {
    if (addr < 0x10000) return false;
    __try {
        for (int i = 0; i < n; ++i) out[i] = reinterpret_cast<const volatile float*>(addr)[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SanePos(const float* p) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(p[i]) || std::fabs(p[i]) > 1e7f) return false;
    if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f) return false;
    // (0, 200, -700) is the placeholder DDDA holds during area loads.
    return !(p[0] == 0.0f && p[1] == 200.0f && p[2] == -700.0f);
}

bool SaneHp(const float* v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && v[1] >= 1.0f && v[1] < 1e6f && v[0] >= 0.0f &&
           v[0] <= v[1] * 1.01f;
}

// Returns the role for a character, or -1 if it can't be placed.
int RoleOf(uintptr_t self, bool isPlayer) {
    uint32_t record;
    if (!ReadU32(self + kRecord, &record) || record < 0x10000) return -1;
    if (isPlayer) {
        InterlockedExchange(&g_arisenRecord, static_cast<LONG>(record));
        return bridge::kArisen;
    }
    uint32_t arisen = static_cast<uint32_t>(g_arisenRecord);
    if (!arisen || record < arisen + kFirstPawnRecord) return -1;
    uint32_t delta = record - arisen - kFirstPawnRecord;
    if (delta % kPawnRecordSize != 0) return -1;
    uint32_t slot = delta / kPawnRecordSize;
    return slot <= 2 ? static_cast<int>(bridge::kMainPawn + slot) : -1;
}

void Capture(void* obj, bool isPlayer) {
    uintptr_t self = reinterpret_cast<uintptr_t>(obj);
    int role = RoleOf(self, isPlayer);
    float pos[3];
    if (role < 0 || !ReadFloats(self + kPos, pos, 3) || !SanePos(pos)) {
        InterlockedIncrement(&g_rejects);
        return;
    }
    if (role != bridge::kArisen) isolate::NotePawn(self);
    uint32_t status;
    float hp[2] = {};
    bool hpValid = ReadU32(self + kStatus, &status) && ReadFloats(status + kHp, hp, 2) && SaneHp(hp);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);

    AcquireSRWLockExclusive(&g_lock);
    Captured& c = g_captured[role];
    c.seen = true;
    c.qpc = q.QuadPart;
    c.pos[0] = pos[0];
    c.pos[1] = pos[1];
    c.pos[2] = pos[2];
    c.hpValid = hpValid;
    if (hpValid) {
        c.hp = hp[0];
        c.hpMax = hp[1];
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// Capture must never take down the game, whatever memory it meets.
void SafeCapture(void* obj, bool isPlayer) {
    __try {
        Capture(obj, isPlayer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_rejects);
    }
}

// --- Hide the Arisen while Skyrim drives the camera ----------------------------
// The player is the Dragonborn, so the Arisen's body and the models it carries
// are hidden through their parts masks and restored when the override ends.
// Masks are only touched from the object's own move(), so the object is alive.
// An entry is never dropped just because its object stopped moving (the world
// can be paused for a long time); a saved mask is only written back while the
// mask is still all zero, so an object reallocated at the same address keeps
// its own mask.
struct Hidden {
    uintptr_t obj;  // 0 = free
    uint32_t vtable;
    uint32_t saved[kPartsMaskWords];
    ULONGLONG lastSeen;
};
constexpr int kMaxHidden = 8;
constexpr ULONGLONG kHiddenEvictMs = 10000;  // a full table may reuse entries this old
SRWLOCK g_hideLock = SRWLOCK_INIT;
Hidden g_hidden[kMaxHidden];
volatile LONG g_hideWanted = 0;
volatile LONG g_hiddenCount = 0;

bool ReadMask(uintptr_t obj, uint32_t* out) {
    __try {
        for (int i = 0; i < kPartsMaskWords; ++i)
            out[i] = reinterpret_cast<const volatile uint32_t*>(obj + kPartsMask)[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteMask(uintptr_t obj, const uint32_t* in) {
    __try {
        for (int i = 0; i < kPartsMaskWords; ++i) reinterpret_cast<volatile uint32_t*>(obj + kPartsMask)[i] = in[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void HideTick(uintptr_t obj) {
    bool wanted = g_hideWanted != 0;
    if (!wanted && g_hiddenCount == 0) return;
    uint32_t vt;
    if (!ReadU32(obj, &vt)) return;
    ULONGLONG now = GetTickCount64();
    static const uint32_t kZero[kPartsMaskWords] = {};
    AcquireSRWLockExclusive(&g_hideLock);
    Hidden* e = nullptr;
    Hidden* free = nullptr;
    Hidden* oldest = nullptr;
    for (Hidden& h : g_hidden) {
        if (h.obj == obj && h.vtable == vt) e = &h;
        else if (!h.obj && !free) free = &h;
        else if (h.obj && (!oldest || h.lastSeen < oldest->lastSeen)) oldest = &h;
    }
    if (!free && oldest && now - oldest->lastSeen > kHiddenEvictMs) {  // long gone; never written again
        oldest->obj = 0;
        InterlockedDecrement(&g_hiddenCount);
        free = oldest;
    }
    uint32_t cur[kPartsMaskWords];
    if (wanted && ReadMask(obj, cur)) {
        if (!e && free) {
            e = free;
            e->obj = obj;
            e->vtable = vt;
            memcpy(e->saved, cur, sizeof(cur));
            InterlockedIncrement(&g_hiddenCount);
        } else if (e && memcmp(cur, kZero, sizeof(cur)) != 0) {
            memcpy(e->saved, cur, sizeof(cur));  // the game changed it (new equipment)
        }
        if (e) {
            e->lastSeen = now;
            WriteMask(obj, kZero);
        }
    } else if (!wanted && e) {
        if (ReadMask(obj, cur) && memcmp(cur, kZero, sizeof(cur)) == 0) WriteMask(obj, e->saved);
        e->obj = 0;
        InterlockedDecrement(&g_hiddenCount);
    }
    ReleaseSRWLockExclusive(&g_hideLock);
}

void SafeHideTick(void* obj) {
    __try {
        HideTick(reinterpret_cast<uintptr_t>(obj));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_rejects);
    }
}

void HideIfCarriedByArisen(void* obj) {
    uint32_t owner;
    if (!ReadU32(reinterpret_cast<uintptr_t>(obj) + kOwner, &owner)) return;
    if (g_arisenObj && owner == static_cast<uint32_t>(g_arisenObj)) SafeHideTick(obj);
    else if (isolate::IsPawn(owner)) isolate::NoteCarried(reinterpret_cast<uintptr_t>(obj));
}

// Places the Arisen where Skyrim's player stands. Only x and z are written; the
// game keeps the height on its own ground on the next move().
volatile LONG g_arisenMoves = 0;
volatile LONG g_arisenRescues = 0;

// Fall guard. Skyrim's player can walk to places that map outside DD's terrain;
// placed there, the Arisen falls forever (the party followed it down to y = -950000
// and DDDA crashed).
// - A position counts as grounded only after kSteadyFrames frames in a row with
//   less than kSteadyPerFrame of vertical change. (The first version trusted any
//   frame dropping less than 25 cm; the start of a fall is that slow, so the saved
//   "ground" was in mid-air and the Arisen was put back there 5 times a second.)
// - After falling kFallLimit below the last grounded position, the Arisen goes
//   back there and waits at the edge: it follows again only once the target comes
//   kResumeCloser closer to that position than it was when it led off the edge.
constexpr float kSteadyPerFrame = 4.0f;  // cm
constexpr int kSteadyFrames = 30;
constexpr float kFallLimit = 300.0f;     // cm
constexpr float kResumeCloser = 150.0f;  // cm
constexpr ULONGLONG kFollowPauseMs = 2000;
float g_lastGood[3];
bool g_haveLastGood = false;
float g_prevY = 0.0f;
int g_steady = 0;
ULONGLONG g_followPausedUntil = 0;
bool g_waitAtEdge = false;
float g_edgeDist = 0.0f;

float DistXZ(const float* a, const float* b) {
    float dx = a[0] - b[0], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}

// --- Free the party from DDDA's world while linked ----------------------------------
// Every character embeds a cScrAdjust at +0x19C0 that keeps it on DDDA's ground and
// out of its walls. With its mIsSleep (+0x1AC) set, DDDA accepts any position we
// write, height included (docs/ddda-memory.md). While linked:
// - the pawns' adjust sleeps: each keeps DDDA's horizontal movement (its AI) but
//   takes the height of Skyrim's ground under it (Ground mapping, from DDDAGhosts);
// - the Arisen's adjust also sleeps, and it stays at the height where the link
//   started, within kTreadmillRadius of that spot (treadmill), so it is always on
//   or near DDDA's navigable surface there and never falls. Two earlier versions
//   failed: taking Skyrim's height put it 1.7 m under DDDA's ground (the pawns
//   could not path to it and DDDA warped them onto it every frame), and keeping
//   DDDA physics let a treadmill move drop it 22 m from a rock; the fall guard
//   then put it back on a ledge 50 times and the fall damage killed it.
// When the link ends, everyone is put back at the Arisen's position from when the
// link started (DDDA ground), then the adjust wakes up.
// Pawn offsets around the Arisen (x, z cm): the pin experiment and the safe return.
const float kFormation[bridge::kRoleCount][2] = {{0, 0}, {-150, -150}, {150, -150}, {0, -250}};
constexpr uintptr_t kScrAdjust = 0x19C0;
constexpr uintptr_t kScrSleep = kScrAdjust + 0x1AC;
volatile LONG g_freeParty = 0;  // set by the bridge thread while linked
volatile LONG g_terrainLinked = 0;  // linked in terrain mode: none of the freeing/treadmill below applies
// Moving the party for Skyrim (2026-10-01, robust version). Rules learned the hard way:
// - The Arisen keeps DDDA physics and stands on DDDA's navigable ground at all
//   times. When it was buried, floating or on a rock top, the pawns could not path
//   to it and DDDA glued them onto it every frame (a 12 m jump alone does not).
// - Treadmill moves only go to "home": the first steady, grounded position after
//   linking. Twice, moves to unverified spots ended in fatal falls.
// - Nobody's physics is switched off (pawns freed from DDDA's ground knelt).
// Set false for safe mode: the party is never moved for Skyrim.
// Runtime switch (ddda_experiment.txt line "follow on"); off = safe mode. Safe mode is
// the default after a home-fall loop on 2026-10-01 23:29, until the follow passes
// the player-simulator harness (tools/follow_sim).
volatile LONG g_moveParty = 0;
#define kMoveParty (g_moveParty != 0)

struct Freed {
    uintptr_t obj;  // 0 = free slot
    uint32_t vtable;
    uint8_t savedSleep;
    int role;
    float shiftApplied[2];  // pawns: the treadmill shift their position already includes
    uint32_t footIK[4];     // pawns: their uCnsIK constraints, whose ground fit is skipped
    float savedGroundDist[4];
};

// Pawn IK: char+0x2E74 -> cPlIKCtrl (vtable 0x15E64A4) holding four uCnsIK at +0x0C.
// uCnsIK vtable slot 36 fits a foot onto DDDA's ground (it moves the effector's Y to
// the ground hit). With a pawn freed at Skyrim's height, DDDA's ground is elsewhere
// and this bent its knees (it knelt whenever it stood above DDDA's ground), so the
// fit is skipped for the party's constraints: the feet stay where the animation
// puts them.
constexpr uintptr_t kIKCtrl = 0x2E74;
constexpr uint32_t kIKCtrlVtable = 0x11E64A4;  // image-relative
constexpr uintptr_t kIKCtrlCns = 0x0C;
constexpr uintptr_t kCnsIKVtable = 0x1030928;  // image-relative
constexpr uintptr_t kCnsIKGroundFit = 0xA59C70;
constexpr uintptr_t kCnsIKGroundFitSlot = 36;
// The slot-36 hook never fired (0 skips): the fit that bent the knees runs in
// uCnsIK::uCnsJoint slot 24. Its search range is uCnsIK.mGroundDistance (+0xC78,
// 200 cm); at 0 the IK finds no ground, so a freed pawn's range is set to 0 and
// restored when it is returned. Set live during the 2026-10-01 test, where the
// game kept the value; whether it fixes the kneeling is not confirmed yet.
constexpr uintptr_t kCnsIKGroundDistance = 0xC78;
SRWLOCK g_freeLock = SRWLOCK_INIT;
Freed g_freed[8];
float g_safePos[3];  // the Arisen's DDDA position when the link started
bool g_haveSafePos = false;

// Skyrim ground per role (bridge thread fills it from the Ground mapping).
SRWLOCK g_groundLock = SRWLOCK_INIT;
uint32_t g_groundValid = 0;
float g_groundY[bridge::kRoleCount];

bool ReadU8(uintptr_t addr, uint8_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uint8_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Runs from the character's own move(), so the object is alive. `role` < 0: unknown.
// Pawns are freed from DDDA's ground while linked (adjust asleep, foot-IK ground
// search 0) so they can stand at Skyrim's height; the Arisen never is (it must
// stay reachable on DDDA's navmesh). On unlink, pawns are put around the Arisen
// (always on verified DDDA ground) before their adjust wakes up.
void FreeTickUnsafe(uintptr_t self, int role) {
    bool want = kMoveParty && g_freeParty != 0 && !g_terrainLinked;
    uint32_t vt;
    if (!ReadU32(self, &vt)) return;
    AcquireSRWLockExclusive(&g_freeLock);
    Freed* e = nullptr;
    Freed* slot = nullptr;
    for (Freed& f : g_freed) {
        if (f.obj == self && f.vtable == vt) e = &f;
        else if (!f.obj && !slot) slot = &f;
    }
    auto* pos = reinterpret_cast<volatile float*>(self + kPos);
    auto* sleep = reinterpret_cast<volatile uint8_t*>(self + kScrSleep);
    if (want && !e && slot && role >= 0) {
        e = slot;
        e->obj = self;
        e->vtable = vt;
        e->role = role;
        GetShift(e->shiftApplied);
        memset(e->footIK, 0, sizeof(e->footIK));
        if (role != bridge::kArisen) {
            ReadU8(self + kScrSleep, &e->savedSleep);
            uint32_t ik, ikvt;
            if (ReadU32(self + kIKCtrl, &ik) && ReadU32(ik, &ikvt) && ikvt == g_base + kIKCtrlVtable)
                for (int k = 0; k < 4; ++k) {
                    uint32_t cvt;
                    if (!ReadU32(ik + kIKCtrlCns + 4 * k, &e->footIK[k]) || !ReadU32(e->footIK[k], &cvt) ||
                        cvt != g_base + kCnsIKVtable) {
                        e->footIK[k] = 0;
                        continue;
                    }
                    auto* gd = reinterpret_cast<volatile float*>(e->footIK[k] + kCnsIKGroundDistance);
                    e->savedGroundDist[k] = *gd;
                    *gd = 0.0f;
                }
            Log("freed pawn %08X from DDDA's ground", static_cast<unsigned>(self));
        }
    }
    if (want && e) {
        if (e->role != bridge::kArisen) *sleep = 1;
    } else if (!want && e) {
        if (e->role == bridge::kArisen) {
            SetShift(0.0f, 0.0f);  // the next link starts from the party's real place
        } else {
            float arisen[3];
            if (g_arisenObj && ReadFloats(static_cast<uintptr_t>(g_arisenObj) + kPos, arisen, 3) && SanePos(arisen)) {
                const float* off = kFormation[e->role];
                pos[0] = arisen[0] + off[0];
                pos[1] = arisen[1] + 50.0f;
                pos[2] = arisen[2] + off[1];
            }
            *sleep = e->savedSleep;
            for (int k = 0; k < 4; ++k)
                if (e->footIK[k])
                    *reinterpret_cast<volatile float*>(e->footIK[k] + kCnsIKGroundDistance) = e->savedGroundDist[k];
            Log("returned pawn %08X to DDDA's ground", static_cast<unsigned>(self));
        }
        e->obj = 0;
    }
    ReleaseSRWLockExclusive(&g_freeLock);
}

using GroundFitFn = void(__thiscall*)(void*, void*, void*);
GroundFitFn g_origGroundFit = nullptr;
volatile LONG g_footFitsSkipped = 0;

bool IsPartyFootIK(uint32_t cns) {
    if (!g_freeParty) return false;
    bool found = false;
    AcquireSRWLockShared(&g_freeLock);
    for (const Freed& f : g_freed)
        if (f.obj && f.role != bridge::kArisen)
            for (uint32_t c : f.footIK)
                if (c == cns) found = true;
    ReleaseSRWLockShared(&g_freeLock);
    return found;
}

void __fastcall GroundFitHook(void* self, void* /*edx*/, void* effector, void* hitCache) {
    if (IsPartyFootIK(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)))) {
        InterlockedIncrement(&g_footFitsSkipped);
        return;
    }
    g_origGroundFit(self, effector, hitCache);
}

void SafeFreeTick(void* self, int role) {
    __try {
        FreeTickUnsafe(reinterpret_cast<uintptr_t>(self), role);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_rejects);
    }
}

// Pawns: carried along by treadmill moves, then put at Skyrim's ground height,
// after DDDA moved them horizontally.
volatile float g_landY = 0.0f;  // treadmill moves drop the party slightly above home's ground

void GroundTickUnsafe(uintptr_t self, int role) {
    if (!kMoveParty || !g_freeParty || g_terrainLinked || role <= bridge::kArisen) return;
    float sh[2];
    GetShift(sh);
    auto* p = reinterpret_cast<volatile float*>(self + kPos);
    AcquireSRWLockExclusive(&g_freeLock);
    for (Freed& f : g_freed) {
        if (f.obj != self) continue;
        float dx = sh[0] - f.shiftApplied[0], dz = sh[1] - f.shiftApplied[1];
        if (dx != 0.0f || dz != 0.0f) {  // treadmill move: keep the formation
            p[0] += dx;
            p[1] = g_landY;
            p[2] += dz;
            f.shiftApplied[0] = sh[0];
            f.shiftApplied[1] = sh[1];
        }
    }
    ReleaseSRWLockExclusive(&g_freeLock);
    AcquireSRWLockShared(&g_groundLock);  // Skyrim's ground under the pawn
    bool ok = (g_groundValid >> role) & 1;
    float y = g_groundY[role];
    ReleaseSRWLockShared(&g_groundLock);
    if (ok) p[1] = y;
}

void SafeGroundTick(void* self, int role) {
    __try {
        GroundTickUnsafe(reinterpret_cast<uintptr_t>(self), role);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_rejects);
    }
}

float g_home[3];
bool g_haveHome = false;
ULONGLONG g_lastRescue = 0;
int g_quickRescues = 0;  // rescues less than 10 s apart, in a row
volatile LONG g_followSuspended = 0;

void FollowTickUnsafe(uintptr_t self, const CamOverride& o) {
    if (!kMoveParty) return;
    auto* p = reinterpret_cast<volatile float*>(self + kPos);
    float y = p[1];
    ULONGLONG now = GetTickCount64();
    if (g_followSuspended) return;
    if (g_haveLastGood && y < g_lastGood[1] - kFallLimit) {
        // Falling: back to the last grounded spot, or home after a second rescue
        // within 10 s (a ledge that keeps dropping it). A third quick one means
        // neither spot holds (23:29: home itself dropped it 4 times a second), so
        // following is suspended until the next link; DDDA's own physics lands it.
        bool quick = now - g_lastRescue < 10000;
        g_quickRescues = quick ? g_quickRescues + 1 : 1;
        if (g_quickRescues >= 3) {
            InterlockedExchange(&g_followSuspended, 1);
            Log("Arisen keeps falling (y %.0f); following SUSPENDED until the next link", y);
            return;
        }
        const float* to = (g_haveHome && quick) ? g_home : g_lastGood;
        p[0] = to[0];
        p[1] = to[1] + 50.0f;
        p[2] = to[2];
        g_prevY = to[1];
        g_steady = 0;
        g_lastRescue = now;
        g_followPausedUntil = now + kFollowPauseMs;
        g_waitAtEdge = true;
        g_edgeDist = DistXZ(o.body, to);
        if (InterlockedIncrement(&g_arisenRescues) <= 50)
            Log("Arisen was falling (y %.0f); put back at (%.0f, %.0f, %.0f)%s, waiting at the edge", y, to[0], to[1],
                to[2], to == g_home ? " [home]" : "");
        return;
    }
    if (std::fabs(y - g_prevY) < kSteadyPerFrame) {
        if (++g_steady >= kSteadyFrames) {
            g_lastGood[0] = p[0];
            g_lastGood[1] = y;
            g_lastGood[2] = p[2];
            g_haveLastGood = true;
        }
    } else {
        g_steady = 0;
    }
    g_prevY = y;
    if (!g_haveLastGood || now < g_followPausedUntil) return;  // follow only from known ground
    if (!g_haveHome) {
        memcpy(g_home, g_lastGood, sizeof(g_home));
        g_haveHome = true;
        Log("home (treadmill centre): (%.0f, %.0f, %.0f)", g_home[0], g_home[1], g_home[2]);
    }
    if (g_waitAtEdge) {
        if (DistXZ(o.body, g_lastGood) > g_edgeDist - kResumeCloser) return;
        g_waitAtEdge = false;
        Log("Arisen follows again");
    }
    float body[3] = {o.body[0], o.body[1], o.body[2]};
    if (DistXZ(body, g_home) > kTreadmillRadius) {
        // Back to home, which is verified ground; the pawns move along (GroundTick).
        float sh[2];
        GetShift(sh);
        float dx = g_home[0] - body[0], dz = g_home[2] - body[2];
        g_landY = g_home[1] + 50.0f;
        SetShift(sh[0] + dx, sh[1] + dz);
        p[0] = g_home[0];
        p[1] = g_home[1] + 50.0f;
        p[2] = g_home[2];
        memcpy(g_lastGood, g_home, sizeof(g_lastGood));
        g_prevY = g_home[1];
        g_steady = 0;
        static volatile LONG recentres = 0;
        if (InterlockedIncrement(&recentres) <= 100)
            Log("treadmill: party moved back by (%.0f, %.0f); shift now (%.0f, %.0f)", dx, dz, sh[0] + dx,
                sh[1] + dz);
        InterlockedIncrement(&g_arisenMoves);
        return;
    }
    p[0] = body[0];
    p[2] = body[2];
    InterlockedIncrement(&g_arisenMoves);
}

// --- Terrain mode (docs/terrain-proxy.md) -----------------------------------------
// DDDA's open-world tiles are replaced by collision and waypoint graphs generated from
// Skyrim's terrain, so the party can stay on DDDA's own physics and AI: the Arisen
// gets only its x/z from Skyrim, converted from global to DDDA's tile-local space.
//
// Tile origin. When the Arisen crosses a tile edge, its local coordinate wraps by
// 10000 one frame BEFORE uStageSplitCtrl's N/M follow (tools/recon/tilewatch.py), so
// the wrap itself is what moves the origin; N/M only seed it and correct it if the
// two disagree for kTileResyncFrames frames (area loads, teleports).
constexpr int kTileResyncFrames = 60;
constexpr ULONGLONG kResyncQuietMs = 2000;  // no resync this soon after a wrap or a protection
ULONGLONG g_lastWrapTick = 0;
bool Protecting();
ULONGLONG ProtectStartedTick();
constexpr float kWrapSlack = 2000.0f;  // a wrap is a multiple of kTile, give or take one frame of motion
volatile LONG g_orgN = 0, g_orgM = 0, g_orgValid = 0;
float g_lastLocal[3];
bool g_haveLastLocal = false;
int g_tileDisagree = 0;

bool ReadSplitTile(int* n, int* m) {
    uint32_t obj = static_cast<uint32_t>(g_splitObj), vt;
    if (!obj || !ReadU32(obj, &vt) || vt != g_base + kSplitVtable) return false;
    uint32_t a, b;
    if (!ReadU32(obj + kSplitN, &a) || !ReadU32(obj + kSplitM, &b)) return false;
    *n = static_cast<int>(a);
    *m = static_cast<int>(b);
    return *n > 0 && *n < 200 && *m > 0 && *m < 200;
}

void SetOrigin(int n, int m, const char* why) {
    if (g_orgValid && g_orgN == n && g_orgM == m) return;
    static volatile LONG logs = 0;
    if (InterlockedIncrement(&logs) <= 200)
        Log("tile origin %dm%dn -> %dm%dn (%s)", static_cast<int>(g_orgM), static_cast<int>(g_orgN), m, n, why);
    InterlockedExchange(&g_orgN, n);
    InterlockedExchange(&g_orgM, m);
    InterlockedExchange(&g_orgValid, 1);
}

// Runs in the Arisen's move hook after DDDA moved it, before anything is written.
void TileTickUnsafe(uintptr_t self) {
    float p[3];
    if (!ReadFloats(self + kPos, p, 3) || !SanePos(p)) return;
    int sn, sm;
    bool split = ReadSplitTile(&sn, &sm);
    if (!g_orgValid) {
        if (split) SetOrigin(sn, sm, "uStageSplitCtrl");
    } else if (g_haveLastLocal) {
        // A wrap moves the coordinate by a whole number of tiles: one when walking,
        // several after the link placed the Arisen far away (2026-10-02: a 2-tile
        // move was missed and the origin was wrong for a second, until the resync).
        int n = g_orgN, m = g_orgM;
        float dx = p[0] - g_lastLocal[0], dz = p[2] - g_lastLocal[2];
        int kx = static_cast<int>(std::lround(dx / kTile)), kz = static_cast<int>(std::lround(dz / kTile));
        if (kx && std::fabs(dx - kx * kTile) < kWrapSlack) n -= kx;
        if (kz && std::fabs(dz - kz * kTile) < kWrapSlack) m -= kz;
        if (n != g_orgN || m != g_orgM) {
            SetOrigin(n, m, "coordinate wrap");
            g_lastWrapTick = GetTickCount64();
        }
    }
    // N/M lag behind a wrap and can disagree for a while around leaps (2026-10-02: a
    // resync in the middle of two leaps set a wrong tile for one frame and the pawns,
    // placed in that frame, were lost); only a long, quiet disagreement resyncs.
    ULONGLONG now = GetTickCount64();
    bool quiet = !Protecting() && now - g_lastWrapTick > kResyncQuietMs && now - ProtectStartedTick() > kResyncQuietMs;
    if (split && g_orgValid && quiet && (sn != g_orgN || sm != g_orgM)) {
        if (++g_tileDisagree >= kTileResyncFrames) {
            SetOrigin(sn, sm, "resync from uStageSplitCtrl");
            g_tileDisagree = 0;
        }
    } else {
        g_tileDisagree = 0;
    }
    memcpy(g_lastLocal, p, sizeof(g_lastLocal));
    g_haveLastLocal = true;
}

void SafeTileTick(void* self) {
    __try {
        TileTickUnsafe(reinterpret_cast<uintptr_t>(self));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_rejects);
    }
}

bool GlobalToLocal(float* v) {  // in place: x and z of a 3-vector
    if (!g_orgValid) return false;
    v[0] -= (static_cast<float>(g_orgN) - 50.0f) * kTile;
    v[2] -= (static_cast<float>(g_orgM) - 50.0f) * kTile;
    return true;
}

// Party moves in terrain mode. The Arisen is kinematic while linked (see
// TerrainFollowUnsafe). The pawns keep DDDA's physics; the link's PROTECTION takes them
// from the held save height up to the Arisen (their adjust sleeps for kProtectMs while
// DDDA loads the ground there, then they land). Pawns are never moved DOWN: DDDA's fall
// damage then hurt them (2026-10-02); DDDA's own warp brings far pawns safely.
// "hold" (ddda_experiment.txt, written by tools/terrain/stream.py): while not linked,
// the party's adjust sleeps, so a save made before the generated ground existed does
// not drop them into the void before Skyrim links (2026-10-02: they fell 1.6 km).
volatile LONG g_terrainHold = 0;
constexpr ULONGLONG kProtectMs = 2500;
constexpr int kTerrainMaxProtects = 5;
constexpr float kLandHeight = 30.0f;
volatile LONG g_terrainSuspended = 0;
volatile LONG64 g_protectUntil = 0;  // GetTickCount64 deadline
float g_protectGlobal[3];             // where the Arisen arrived, global (pawns are held around it)
int g_protects = 0;
ULONGLONG g_protectWindow = 0;
bool g_wasTerrainLinked = false;
ULONGLONG g_terrainLinkSeen = 0;  // last frame the terrain link was active
constexpr ULONGLONG kRelinkMs = 2000;  // a shorter gap in the commands is the same link
                                       // (2026-10-02: a 0.5 s stall of the sender re-ran the
                                       // link move, 37 m down, and hurt the Arisen)

// Objects whose scenery adjust we put to sleep, with the value to restore.
struct Slept {
    uintptr_t obj;
    uint8_t saved;
};
SRWLOCK g_sleptLock = SRWLOCK_INIT;
Slept g_slept[8];

// A teleport must also move the scenery adjust's own copies of the position and
// clear its velocity: it keeps the previous frame's position (mOldPos +0x120, mRealPos
// +0x130, mResPos +0x140) and a velocity (+0x150, y -49.5 while falling). Moving a
// character tens of metres down without this read as a fall at that speed when its
// physics resumed: pawns were knocked down and the Arisen lost 601 HP over two
// downward moves (2026-10-02). Upward moves were harmless (upward "velocity").
constexpr uintptr_t kAdjOldPos = 0x120, kAdjRealPos = 0x130, kAdjResPos = 0x140, kAdjVelocity = 0x150;

void SyncAdjust(uintptr_t obj) {
    auto* p = reinterpret_cast<volatile float*>(obj + kPos);
    const uintptr_t copies[] = {kAdjOldPos, kAdjRealPos, kAdjResPos};
    for (uintptr_t off : copies) {
        auto* v = reinterpret_cast<volatile float*>(obj + kScrAdjust + off);
        v[0] = p[0];
        v[1] = p[1];
        v[2] = p[2];
    }
    auto* vel = reinterpret_cast<volatile float*>(obj + kScrAdjust + kAdjVelocity);
    vel[0] = vel[1] = vel[2] = 0.0f;
}

void SleepAdjust(uintptr_t obj, bool on) {
    AcquireSRWLockExclusive(&g_sleptLock);
    Slept* e = nullptr;
    Slept* slot = nullptr;
    for (Slept& x : g_slept) {
        if (x.obj == obj) e = &x;
        else if (!x.obj && !slot) slot = &x;
    }
    auto* sleep = reinterpret_cast<volatile uint8_t*>(obj + kScrSleep);
    if (on && !e && slot) {
        slot->obj = obj;
        slot->saved = *sleep;
        e = slot;
    }
    if (on && e) *sleep = 1;
    if (!on && e) {
        *sleep = e->saved;
        e->obj = 0;
    }
    ReleaseSRWLockExclusive(&g_sleptLock);
}

bool Protecting() {
    return static_cast<ULONGLONG>(g_protectUntil) > GetTickCount64();
}

ULONGLONG g_protectStarted = 0;
ULONGLONG ProtectStartedTick() {
    return g_protectStarted;
}

void StartProtect(const float* globalTarget, const char* why, bool counts) {
    ULONGLONG now = GetTickCount64();
    if (counts) {
        if (now - g_protectWindow > 10000) {
            g_protectWindow = now;
            g_protects = 0;
        }
        if (++g_protects > kTerrainMaxProtects) {
            InterlockedExchange(&g_terrainSuspended, 1);
            Log("terrain: the party keeps needing to be moved; following SUSPENDED until the next link");
            return;
        }
    }
    memcpy(g_protectGlobal, globalTarget, sizeof(g_protectGlobal));
    g_protectStarted = now;
    InterlockedExchange64(&g_protectUntil, static_cast<LONG64>(now + kProtectMs));
    Log("terrain: party protected and moved to global (%.0f, %.0f, %.0f): %s", globalTarget[0], globalTarget[1],
        globalTarget[2], why);
}

// While linked the Arisen is KINEMATIC: its scenery adjust sleeps and it is written
// at Skyrim's feet every frame (x, y, z, plus the adjust's copies and velocity). DDDA's
// ground now matches Skyrim's, so it stands on it, and it can never fall: with its own
// physics it was pushed down steep Skyrim slopes and died of fall damage twice
// (2026-10-02, 601 HP lost in a 1 km simulated walk). The pawns keep DDDA's physics and
// AI and follow it. The link's protection still brings the pawns up from the held save
// height. Known limit: on a Skyrim roof the Arisen floats above DDDA's ground there.
constexpr float kArisenLift = 2.0f;  // cm above Skyrim's feet
// A target that jumps this far in one frame is a leap (the streamer moved the mapping to
// another part of DDDA's map, or Skyrim teleported): the pawns are protected and brought
// along when that is upward (tools/terrain/stream.py makes its leaps go up).
constexpr float kLeapDistance = 3000.0f;

void TerrainFollowUnsafe(uintptr_t self, const CamOverride& o) {
    if (g_terrainSuspended) return;
    float t[3] = {o.body[0], o.body[1], o.body[2]};
    if (!GlobalToLocal(t)) return;
    auto* p = reinterpret_cast<volatile float*>(self + kPos);
    ULONGLONG nowLink = GetTickCount64();
    if (g_wasTerrainLinked && nowLink - g_terrainLinkSeen > kRelinkMs) g_wasTerrainLinked = false;
    g_terrainLinkSeen = nowLink;
    float jx = t[0] - p[0], jz = t[2] - p[2];
    if (!g_wasTerrainLinked) {
        g_wasTerrainLinked = true;
        StartProtect(o.body, "link", false);
    } else if (jx * jx + jz * jz > kLeapDistance * kLeapDistance) {
        StartProtect(o.body, "leap", true);
    }
    // The pawns are held where the Arisen arrived, not where it walks next: following it
    // each frame dragged them along "like a magnet" for kProtectMs whenever the player
    // walked on right after a link (an interior's arena, 2026-10-03).
    SleepAdjust(self, true);
    p[0] = t[0];
    p[1] = t[1] + kArisenLift;
    p[2] = t[2];
    SyncAdjust(self);
    memcpy(g_lastLocal, t, sizeof(g_lastLocal));  // the wrap check compares against what was written
    g_lastLocal[1] = t[1] + kArisenLift;
    InterlockedIncrement(&g_arisenMoves);
}

// Pawns during a protection: held right next to the Arisen's target (half a metre, so
// they stand at its height even on a slope; DDDA spreads them out after), asleep.
const float kProtectSpot[bridge::kRoleCount][2] = {{0, 0}, {-50, -30}, {50, -30}, {0, -60}};

// A pawn is only taken DOWN to the target by DDDA itself: DDDA's fall damage compares
// the last ground with the landing, so a pawn put 34 m lower took a 34 m fall when its
// adjust woke up and all three were knocked down, then returned to the Rift
// (2026-10-02). Moving one up is safe (the link lifts them 506 m), and DDDA's own warp
// brings a pawn that is far from the Arisen without damage (leap_test.py, 771 m).
constexpr float kTakeDownSlack = 100.0f;  // cm a pawn may be above the target and still be taken

void PawnProtectUnsafe(uintptr_t self, int role) {
    if (role <= bridge::kArisen) return;
    bool protect = g_terrainLinked && Protecting();
    bool hold = !g_terrainLinked && g_terrainHold;
    bool take = false;
    if (protect) {
        float t[3];
        memcpy(t, g_protectGlobal, sizeof(t));
        if (!GlobalToLocal(t)) return;
        auto* p = reinterpret_cast<volatile float*>(self + kPos);
        // Pawns are only taken UP (the link lifts them from the held save height; streamer
        // leaps go up). A pawn above the target is left to DDDA's own warp. (Taking them
        // down, or after DDDA had warped them, cost a fall before SyncAdjust existed.)
        float dx = p[0] - t[0], dz = p[2] - t[2];
        bool placed = dx * dx + dz * dz < 300.0f * 300.0f && std::fabs(p[1] - t[1]) < 300.0f;
        take = placed || p[1] <= t[1] + kTakeDownSlack;  // link and leaps: only upwards
        if (take) {
            p[0] = t[0] + kProtectSpot[role][0];
            p[1] = t[1] + kLandHeight;
            p[2] = t[2] + kProtectSpot[role][1];
            SyncAdjust(self);
        }
    }
    SleepAdjust(self, take || hold);
}

void SafePawnProtect(void* self, int role) {
    __try {
        PawnProtectUnsafe(reinterpret_cast<uintptr_t>(self), role);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_rejects);
    }
}

void FollowTick(uintptr_t self) {
    AcquireSRWLockShared(&g_camLock);
    CamOverride o = g_camOverride;
    ReleaseSRWLockShared(&g_camLock);
    AlignToShift(o);
    if (o.active && o.moveArisen && o.global) {
        __try {
            TerrainFollowUnsafe(self, o);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&g_rejects);
        }
        return;
    }
    if (GetTickCount64() - g_terrainLinkSeen > kRelinkMs) {
        InterlockedExchange(&g_terrainSuspended, 0);  // the next terrain link starts fresh
        g_wasTerrainLinked = false;
    }
    if (Protecting()) InterlockedExchange64(&g_protectUntil, 0);
    __try {
        SleepAdjust(self, g_terrainHold != 0);  // hold while unlinked; else wake (link ended mid-protection)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_rejects);
    }
    if (!o.active || !o.moveArisen) {
        g_haveLastGood = false;  // DDDA moves the Arisen itself (area loads, its own controls)
        g_haveHome = false;
        g_quickRescues = 0;
        InterlockedExchange(&g_followSuspended, 0);
        g_waitAtEdge = false;
        g_steady = 0;
        return;
    }
    __try {
        FollowTickUnsafe(self, o);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_rejects);
    }
}

void __fastcall PlayerMoveHook(void* self, void* /*edx*/) {
    InterlockedExchange(&g_arisenObj, static_cast<LONG>(reinterpret_cast<uintptr_t>(self)));
    SafeCapture(self, true);
    SafeFreeTick(self, bridge::kArisen);
    g_origPlayerMove(self);
    SafeTileTick(self);
    SafeHideTick(self);
    FollowTick(reinterpret_cast<uintptr_t>(self));
}

void __fastcall SplitMoveHook(void* self, void* /*edx*/) {
    InterlockedExchange(&g_splitObj, static_cast<LONG>(reinterpret_cast<uintptr_t>(self)));
    g_origSplitMove(self);
}

void __fastcall WeaponMoveHook(void* self, void* /*edx*/) {
    g_origWeaponMove(self);
    HideIfCarriedByArisen(self);
}

void __fastcall LanternMoveHook(void* self, void* /*edx*/) {
    g_origLanternMove(self);
    HideIfCarriedByArisen(self);
}

// --- Experiment: pin the pawns (docs/ghosts.md, "Neutralising DDDA's world") -------
// `ddda_experiment.txt` next to the DLL, read every second by the bridge thread:
//   pin <dy>   hold each pawn in a fixed formation around the Arisen, dy cm above
//              the Arisen's feet, by writing its position after its move()
//   off        (or no file) normal play
// Every second the log shows, per pawn, how far DDDA moved it during its own
// move() away from where it was pinned (a growing negative dy means falling).
volatile LONG g_pinOn = 0;
volatile LONG g_pinDyCm = 0;
struct PinStat {
    float lastPin[3];
    bool pinned;
    float drift[3];  // DDDA's own move() result minus the previous pin
};
PinStat g_pinStat[bridge::kRoleCount];

void PinTickUnsafe(uintptr_t self) {
    if (!g_pinOn || !g_arisenObj) return;
    int role = RoleOf(self, false);
    if (role <= bridge::kArisen) return;
    float arisen[3];
    if (!ReadFloats(static_cast<uintptr_t>(g_arisenObj) + kPos, arisen, 3) || !SanePos(arisen)) return;
    auto* p = reinterpret_cast<volatile float*>(self + kPos);
    PinStat& st = g_pinStat[role];
    if (st.pinned)
        for (int i = 0; i < 3; ++i) st.drift[i] = p[i] - st.lastPin[i];
    float pin[3] = {arisen[0] + kFormation[role][0], arisen[1] + static_cast<float>(g_pinDyCm),
                    arisen[2] + kFormation[role][1]};
    for (int i = 0; i < 3; ++i) {
        p[i] = pin[i];
        st.lastPin[i] = pin[i];
    }
    st.pinned = true;
}

void SafePinTick(void* self) {
    __try {
        PinTickUnsafe(reinterpret_cast<uintptr_t>(self));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_rejects);
    }
}

int SafeRoleOf(void* self) {
    __try {
        return RoleOf(reinterpret_cast<uintptr_t>(self), false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Camera-occlusion fade (2026-10-02, tools/recon/fadewatch.py + hwbp.py): DDDA fades a
// character out when it stands between the camera and the player: the pawn routine
// (+0x48C414) writes char+0x2514 = char+0x2510 x a camera factor, and the character
// draw (+0x76A070) loads it (+0x76A07B) into uModel mTransparency (+0x158) and sets a
// transparency flag when it is below 1. With Skyrim's camera the Arisen is hidden
// where the Dragonborn stands, so pawns kept dissolving in front of the camera.
// Resetting +0x2514 after move() did not hold (the fade is also computed later in the
// frame), so while linked the draw's load is patched to read a constant 1.0 instead:
//   movss xmm0, [esi+0x2514]  F3 0F 10 86 14 25 00 00
//   movss xmm0, [&kOpaque]    F3 0F 10 05 <address>      (same 8 bytes)
constexpr uintptr_t kDrawFadeLoad = 0x76A07B;
const uint8_t kDrawFadeOriginal[8] = {0xF3, 0x0F, 0x10, 0x86, 0x14, 0x25, 0x00, 0x00};
alignas(16) const float kOpaque = 1.0f;
bool g_fadePatched = false;  // bridge thread only

void SetFadePatch(bool on) {
    if (on == g_fadePatched) return;
    auto* code = reinterpret_cast<volatile LONG64*>(g_base + kDrawFadeLoad);
    uint8_t patched[8] = {0xF3, 0x0F, 0x10, 0x05};
    uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&kOpaque));
    memcpy(patched + 4, &addr, 4);
    LONG64 from, to;
    memcpy(&from, on ? kDrawFadeOriginal : patched, 8);
    memcpy(&to, on ? patched : kDrawFadeOriginal, 8);
    DWORD old;
    if (!VirtualProtect(const_cast<LONG64*>(code), 8, PAGE_EXECUTE_READWRITE, &old)) {
        Log("camera fade patch: VirtualProtect failed: %lu", GetLastError());
        return;
    }
    LONG64 seen = InterlockedCompareExchange64(code, to, from);
    VirtualProtect(const_cast<LONG64*>(code), 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), const_cast<LONG64*>(code), 8);
    if (seen != from) {
        Log("camera fade patch: unexpected code at %08X (different game build?); not patched",
            static_cast<unsigned>(g_base + kDrawFadeLoad));
        return;
    }
    g_fadePatched = on;
    Log("camera fade %s", on ? "OFF (pawns stay opaque while linked)" : "restored");
}

void __fastcall PawnMoveHook(void* self, void* /*edx*/) {
    SafeCapture(self, false);
    int role = SafeRoleOf(self);
    SafeFreeTick(self, role);
    g_origPawnMove(self);
    SafePawnProtect(self, role);
    SafePinTick(self);
    SafeGroundTick(self, role);
}

void PollExperiment(const wchar_t* folder) {
    static ULONGLONG last = 0, lastReport = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\ddda_experiment.txt", folder);
    FILE* f = nullptr;
    char line[64] = {};
    // Any line "posedelay N" (frame_capture.h SetPoseDelay). Default 1: DDDA renders each
    // frame with the camera of the previous update (exactly one update per frame, measured
    // 2026-10-02), and 1 removed the "drag" in the user's A/B test.
    int poseDelay = 1;
    bool holdLine = false;  // any line "hold": see g_terrainHold
    // Lighting sync (relight.h): "nolight" turns it off, "nofog" keeps DDDA's fog,
    // "lightscale X" multiplies the transferred light (calibration), "lightmask HEX"
    // picks the DDDA light groups that get Skyrim's values.
    bool lightOn = true, fogOn = true;
    float lightScale = 1.0f;
    unsigned lightMask = ~0x10u;
    bool learnPaused = false;  // "nolearn": see isolate::SetLearnPaused
    if (_wfopen_s(&f, path, L"r") == 0 && f) {
        if (!fgets(line, sizeof(line), f)) line[0] = 0;
        char more[64];
        auto parse = [&](const char* l) {
            if (strncmp(l, "posedelay latch", 15) == 0) poseDelay = capture::kPoseLatch;
            else sscanf_s(l, "posedelay %d", &poseDelay);
            if (strncmp(l, "hold", 4) == 0) holdLine = true;
            if (strncmp(l, "nolight", 7) == 0) lightOn = false;
            if (strncmp(l, "nofog", 5) == 0) fogOn = false;
            sscanf_s(l, "lightscale %f", &lightScale);
            sscanf_s(l, "lightmask %x", &lightMask);
            if (strncmp(l, "nolearn", 7) == 0) learnPaused = true;
        };
        parse(line);
        while (fgets(more, sizeof(more), f)) parse(more);
        fclose(f);
    }
    relight::SetOptions(lightOn, fogOn, lightScale, lightMask);
    isolate::SetLearnPaused(learnPaused);
    static int lastPoseDelay = INT_MIN;  // not kPoseLatch (-1): that is a valid setting
    if (poseDelay != lastPoseDelay) {
        lastPoseDelay = poseDelay;
        capture::SetPoseDelay(poseDelay);
        if (poseDelay == capture::kPoseLatch) Log("frame pose: newest at the previous Present (latch)");
        else Log("frame pose delay: %d camera updates", poseDelay);
    }
    bool follow = strncmp(line, "follow on", 9) == 0;
    bool hold = holdLine;
    if (hold != (g_terrainHold != 0)) {
        InterlockedExchange(&g_terrainHold, hold ? 1 : 0);
        Log("terrain hold %s", hold ? "ON (party held until Skyrim links)" : "OFF");
    }
    if (follow != (g_moveParty != 0)) {
        InterlockedExchange(&g_moveParty, follow ? 1 : 0);
        Log("party following %s", follow ? "ON" : "OFF (safe mode)");
    }
    int dy = 0;
    bool on = sscanf_s(line, "pin %d", &dy) == 1;
    if (on != (g_pinOn != 0) || dy != g_pinDyCm) {
        InterlockedExchange(&g_pinDyCm, dy);
        InterlockedExchange(&g_pinOn, on ? 1 : 0);
        for (PinStat& st : g_pinStat) st.pinned = false;
        Log("experiment: %s", on ? line : "off");
    }
    if (on && now - lastReport >= 1000) {
        lastReport = now;
        const PinStat* p = g_pinStat;
        Log("pin drift per frame (x,y,z cm): main (%.1f,%.1f,%.1f) h1 (%.1f,%.1f,%.1f) h2 (%.1f,%.1f,%.1f)",
            p[1].drift[0], p[1].drift[1], p[1].drift[2], p[2].drift[0], p[2].drift[1], p[2].drift[2], p[3].drift[0],
            p[3].drift[1], p[3].drift[2]);
    }
}

bool WriteCamera(uintptr_t self, const CamOverride& o) {
    __try {
        auto* f = reinterpret_cast<volatile float*>(self);
        for (int i = 0; i < 3; ++i) {
            f[(kCamPos >> 2) + i] = o.pos[i];
            f[(kCamTarget >> 2) + i] = o.target[i];
            f[(kCamUp >> 2) + i] = o.up[i];
        }
        if (o.fovY > 0.0f) f[kCamFov >> 2] = o.fovY;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SnapshotCameraCmd(bridge::CameraCmd* out);
bool SaneDir(const float* v);
bool HasSkyPose(const float* p);

// The camera command is re-read here, every frame, instead of using the bridge
// thread's copy (polled every kPollMs): that copy can be 8 ms older. The thread
// still decides whether an override is active (staleness, sanity).
void FreshenFromCommand(CamOverride& o) {
    bridge::CameraCmd s;
    if (!o.active || !SnapshotCameraCmd(&s) || !(s.flags & bridge::kCamOverride)) return;
    float d[3] = {s.target[0] - s.pos[0], s.target[1] - s.pos[1], s.target[2] - s.pos[2]};
    if (!SanePos(s.pos) || !SanePos(s.target) || !SaneDir(d) || !SaneDir(s.up) ||
        ((s.flags & bridge::kGlobalCoords) != 0) != o.global)
        return;
    memcpy(o.pos, s.pos, sizeof(o.pos));
    memcpy(o.target, s.target, sizeof(o.target));
    memcpy(o.up, s.up, sizeof(o.up));
    if (s.fovY > 0.0f && s.fovY < 170.0f) o.fovY = s.fovY;
    if (SanePos(s.body)) memcpy(o.body, s.body, sizeof(o.body));
    o.shiftX = s.shiftX;
    o.shiftZ = s.shiftZ;
    o.hasPose = HasSkyPose(s.skyPose);
    memcpy(o.skyPose, s.skyPose, sizeof(o.skyPose));
}

void __fastcall CamUpdateHook(void* self, void* /*edx*/) {
    g_origCamUpdate(self);
    AcquireSRWLockShared(&g_camLock);
    CamOverride o = g_camOverride;
    ReleaseSRWLockShared(&g_camLock);
    FreshenFromCommand(o);
    AlignToShift(o);
    // The camera height comes from Skyrim's terrain, the Arisen's from DD's. Keep
    // the camera at Skyrim's height above the player, measured from the Arisen's
    // real feet, so it never sinks into DD's ground around the party.
    // With the party freed, pawns stand at Skyrim heights, so the camera keeps
    // Skyrim's height too (DDDA's scenery is not drawn, so being inside DDDA's
    // hills does not matter).
    // Terrain mode: both games have the same ground, so the camera keeps Skyrim's
    // height as is. Measuring it from the Arisen's feet made the pawns rise with the
    // Dragonborn on every jump (the Arisen stays on the ground, so the camera was
    // pulled back down while Skyrim's went up).
    if (o.active && o.global && !(GlobalToLocal(o.pos) && GlobalToLocal(o.target) && GlobalToLocal(o.body))) {
        capture::SetRenderPose(nullptr);
        return;  // tile origin not known yet: DDDA keeps its own camera this frame
    }
    float feetY;
    if (o.active && o.moveArisen && !o.global && !(kMoveParty && g_freeParty) && g_arisenObj &&
        ReadFloats(static_cast<uintptr_t>(g_arisenObj) + kPos + 4, &feetY, 1) && std::isfinite(feetY)) {
        float dy = feetY - o.body[1];
        o.pos[1] += dy;
        o.target[1] += dy;
    }
    bool applied = o.active && WriteCamera(reinterpret_cast<uintptr_t>(self), o);
    if (applied) InterlockedIncrement(&g_camApplied);
    capture::SetRenderPose(applied && o.hasPose ? o.skyPose : nullptr);
}

bool PatchSlot(uintptr_t vtable, uintptr_t slotIndex, uintptr_t expected, void* hook, MoveFn* orig) {
    auto* slot = reinterpret_cast<uintptr_t*>(g_base + vtable + slotIndex * sizeof(uintptr_t));
    uint32_t current;
    if (!ReadU32(reinterpret_cast<uintptr_t>(slot), &current) || current != g_base + expected) {
        Log("vtable %08X slot %u holds %08X, expected %08X; not hooking", static_cast<unsigned>(g_base + vtable),
            static_cast<unsigned>(slotIndex), current, static_cast<unsigned>(g_base + expected));
        return false;
    }
    DWORD old;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) {
        Log("VirtualProtect failed: %lu", GetLastError());
        return false;
    }
    *orig = reinterpret_cast<MoveFn>(current);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(slot), static_cast<LONG>(reinterpret_cast<uintptr_t>(hook)));
    VirtualProtect(slot, sizeof(*slot), old, &old);
    return true;
}

bool InstallHooks() {
    // Both slots are validated before either is written, so a different game
    // build is left untouched.
    uint32_t p, q;
    if (!ReadU32(g_base + kPlayerVtable + kMoveSlot * 4, &p) || p != g_base + kPlayerMove ||
        !ReadU32(g_base + kPawnVtable + kMoveSlot * 4, &q) || q != g_base + kPawnMove) {
        Log("unexpected vtable contents (different game build?); hooks not installed");
        return false;
    }
    if (!PatchSlot(kPlayerVtable, kMoveSlot, kPlayerMove, reinterpret_cast<void*>(&PlayerMoveHook),
                   &g_origPlayerMove))
        return false;
    if (!PatchSlot(kPawnVtable, kMoveSlot, kPawnMove, reinterpret_cast<void*>(&PawnMoveHook), &g_origPawnMove))
        return false;
    Log("move() hooks installed");
    // Independent of the party hooks: without it, camera commands are ignored.
    if (PatchSlot(kCamVtable, kCamUpdateSlot, kCamUpdate, reinterpret_cast<void*>(&CamUpdateHook),
                  &g_origCamUpdate))
        Log("camera hook installed");
    if (PatchSlot(kCnsIKVtable, kCnsIKGroundFitSlot, kCnsIKGroundFit, reinterpret_cast<void*>(&GroundFitHook),
                  reinterpret_cast<MoveFn*>(&g_origGroundFit)))
        Log("foot IK ground-fit hook installed");
    if (PatchSlot(kSplitVtable, kMoveSlot, kSplitMove, reinterpret_cast<void*>(&SplitMoveHook), &g_origSplitMove))
        Log("tile hook (uStageSplitCtrl) installed");
    bool weapon = PatchSlot(kWeaponVtable, kMoveSlot, kWeaponMove, reinterpret_cast<void*>(&WeaponMoveHook),
                            &g_origWeaponMove);
    bool lantern = PatchSlot(kLanternVtable, kMoveSlot, kLanternMove, reinterpret_cast<void*>(&LanternMoveHook),
                             &g_origLanternMove);
    Log("hide hooks: weapon %s, lantern %s", weapon ? "ok" : "MISSING", lantern ? "ok" : "MISSING");
    return true;
}

// --- Keep running in the background ---------------------------------------
// DDDA imports no focus queries (GetForegroundWindow etc.); it pauses on window
// messages. Deactivation messages are routed to DefWindowProc instead of the
// game's procedure, so Windows behaves normally but the game never learns it
// lost focus.
HWND g_wnd = nullptr;
WNDPROC g_origWndProc = nullptr;
bool g_wndUnicode = false;
volatile LONG g_swallowed[4];  // WM_ACTIVATEAPP, WM_ACTIVATE, WM_KILLFOCUS, WM_NCACTIVATE

bool IsDeactivation(UINT msg, WPARAM wp);

LRESULT CALLBACK BridgeWndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    int which = -1;
    if (msg == WM_ACTIVATEAPP && !wp) which = 0;
    else if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) which = 1;
    else if (msg == WM_KILLFOCUS) which = 2;
    else if (msg == WM_NCACTIVATE && !wp) which = 3;
    if (which >= 0) {
        InterlockedIncrement(&g_swallowed[which]);
        return g_wndUnicode ? DefWindowProcW(wnd, msg, wp, lp) : DefWindowProcA(wnd, msg, wp, lp);
    }
    return g_wndUnicode ? CallWindowProcW(g_origWndProc, wnd, msg, wp, lp)
                        : CallWindowProcA(g_origWndProc, wnd, msg, wp, lp);
}

BOOL CALLBACK FindGameWindow(HWND wnd, LPARAM out) {
    DWORD pid;
    GetWindowThreadProcessId(wnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(wnd) && !GetWindow(wnd, GW_OWNER)) {
        *reinterpret_cast<HWND*>(out) = wnd;
        return FALSE;
    }
    return TRUE;
}

void TrySubclassWindow() {
    HWND wnd = nullptr;
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&wnd));
    if (!wnd) return;
    g_wndUnicode = IsWindowUnicode(wnd) != FALSE;
    LONG_PTR prev = g_wndUnicode
                        ? SetWindowLongPtrW(wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&BridgeWndProc))
                        : SetWindowLongPtrA(wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&BridgeWndProc));
    if (!prev) {
        Log("subclassing window %p failed: %lu", wnd, GetLastError());
        return;
    }
    g_origWndProc = reinterpret_cast<WNDPROC>(prev);
    g_wnd = wnd;
    char title[128] = {};
    GetWindowTextA(wnd, title, sizeof(title));
    Log("background mode: subclassed window %p \"%s\" (%s)", wnd, title, g_wndUnicode ? "W" : "A");
}

bool IsDeactivation(UINT msg, WPARAM wp) {
    return (msg == WM_ACTIVATEAPP && !wp) || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) ||
           msg == WM_KILLFOCUS || (msg == WM_NCACTIVATE && !wp);
}

// The game installs Windows hooks (SetWindowsHookExA). A WH_CALLWNDPROC hook sees
// sent messages before the window procedure, so deactivation is also hidden there.
using SetWindowsHookExA_t = HHOOK(WINAPI*)(int, HOOKPROC, HINSTANCE, DWORD);
SetWindowsHookExA_t g_origSetWindowsHookExA = nullptr;
HOOKPROC g_gameCallWndProc = nullptr;
HOOKPROC g_gameCallWndProcRet = nullptr;
volatile LONG g_hookSwallowed = 0;

LRESULT CALLBACK FilterCallWndProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* m = reinterpret_cast<const CWPSTRUCT*>(lp);
        if (IsDeactivation(m->message, m->wParam)) {
            InterlockedIncrement(&g_hookSwallowed);
            return CallNextHookEx(nullptr, code, wp, lp);
        }
    }
    return g_gameCallWndProc(code, wp, lp);
}

LRESULT CALLBACK FilterCallWndProcRet(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* m = reinterpret_cast<const CWPRETSTRUCT*>(lp);
        if (IsDeactivation(m->message, m->wParam)) {
            InterlockedIncrement(&g_hookSwallowed);
            return CallNextHookEx(nullptr, code, wp, lp);
        }
    }
    return g_gameCallWndProcRet(code, wp, lp);
}

// The game's WH_KEYBOARD_LL hook sees every key in the system, whatever window
// has focus; it must not see keys typed into Skyrim.
HOOKPROC g_gameKeyboardLL = nullptr;
volatile LONG g_keysBlocked = 0;
bool GameInForeground();

LRESULT CALLBACK FilterKeyboardLL(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && !GameInForeground()) {
        InterlockedIncrement(&g_keysBlocked);
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    return g_gameKeyboardLL(code, wp, lp);
}

// GetAsyncKeyState / GetKeyState read the physical keyboard regardless of focus.
using GetKeyStateFn = SHORT(WINAPI*)(int);
GetKeyStateFn g_origGetAsyncKeyState = nullptr;
GetKeyStateFn g_origGetKeyState = nullptr;

SHORT WINAPI HookGetAsyncKeyState(int key) {
    if (!GameInForeground()) return 0;
    return g_origGetAsyncKeyState(key);
}

SHORT WINAPI HookGetKeyState(int key) {
    if (!GameInForeground()) return 0;
    return g_origGetKeyState(key);
}

HHOOK WINAPI HookSetWindowsHookExA(int id, HOOKPROC proc, HINSTANCE mod, DWORD tid) {
    HOOKPROC use = proc;
    if (id == WH_KEYBOARD_LL && !g_gameKeyboardLL) {
        g_gameKeyboardLL = proc;
        use = FilterKeyboardLL;
    } else if (id == WH_CALLWNDPROC && !g_gameCallWndProc) {
        g_gameCallWndProc = proc;
        use = FilterCallWndProc;
    } else if (id == WH_CALLWNDPROCRET && !g_gameCallWndProcRet) {
        g_gameCallWndProcRet = proc;
        use = FilterCallWndProcRet;
    }
    HHOOK h = g_origSetWindowsHookExA(id, use, mod, tid);
    Log("game SetWindowsHookExA(id %d, proc %p, tid %lu) = %p%s", id, proc, tid, h,
        use != proc ? " [filtered]" : "");
    return h;
}

// Replaces an import of DDDA.exe; returns the original function or nullptr.
void* PatchImport(const char* dll, const char* func, void* hook) {
    auto* base = reinterpret_cast<BYTE*>(g_base);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char*>(base + imp->Name), dll) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(byName->Name), func) != 0) continue;
            DWORD old;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
            void* orig = reinterpret_cast<void*>(iat->u1.Function);
            iat->u1.Function = reinterpret_cast<uintptr_t>(hook);
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            return orig;
        }
    }
    return nullptr;
}

// DirectInput: devices are switched to background mode so input never "goes
// away" when the window loses focus. While the game is not in the foreground,
// keyboard and mouse reads return nothing pressed, so typing in Skyrim never
// reaches DDDA. COM methods are patched on the shared vtables in dinput8.dll.
constexpr int kDiCreateDevice = 3;
constexpr int kDevGetDeviceState = 9;
constexpr int kDevGetDeviceData = 10;
constexpr int kDevSetCooperativeLevel = 13;
constexpr DWORD kDisclExclusive = 0x1, kDisclNonExclusive = 0x2, kDisclForeground = 0x4, kDisclBackground = 0x8,
                kDisclNoWinKey = 0x10;
constexpr DWORD kKeyboardStateSize = 256, kMouseStateSize = 16, kMouseState2Size = 20;

using CreateDeviceFn = HRESULT(__stdcall*)(void*, REFGUID, void**, void*);
using GetDeviceStateFn = HRESULT(__stdcall*)(void*, DWORD, void*);
using GetDeviceDataFn = HRESULT(__stdcall*)(void*, DWORD, void*, DWORD*, DWORD);
using SetCooperativeLevelFn = HRESULT(__stdcall*)(void*, HWND, DWORD);
CreateDeviceFn g_origCreateDevice = nullptr;
GetDeviceStateFn g_origGetDeviceState = nullptr;
GetDeviceDataFn g_origGetDeviceData = nullptr;
SetCooperativeLevelFn g_origSetCooperativeLevel = nullptr;
volatile LONG g_diPatchedDevice = 0;

bool GameInForeground() {
    return !g_wnd || GetForegroundWindow() == g_wnd;
}

void PatchVtableEntry(void** vtable, int index, void* hook, void** orig) {
    DWORD old;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
    *orig = vtable[index];
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&vtable[index]), reinterpret_cast<LONG>(hook));
    VirtualProtect(&vtable[index], sizeof(void*), old, &old);
}

HRESULT __stdcall HookSetCooperativeLevel(void* dev, HWND wnd, DWORD flags) {
    DWORD patched = (flags & ~(kDisclExclusive | kDisclForeground | kDisclNoWinKey)) | kDisclNonExclusive |
                    kDisclBackground;
    HRESULT hr = g_origSetCooperativeLevel(dev, wnd, patched);
    Log("dinput SetCooperativeLevel(dev %p, flags %lX -> %lX) = %08lX", dev, flags, patched, hr);
    return hr;
}

HRESULT __stdcall HookGetDeviceState(void* dev, DWORD size, void* data) {
    HRESULT hr = g_origGetDeviceState(dev, size, data);
    if (SUCCEEDED(hr) && data && !GameInForeground() &&
        (size == kKeyboardStateSize || size == kMouseStateSize || size == kMouseState2Size))
        memset(data, 0, size);
    return hr;
}

HRESULT __stdcall HookGetDeviceData(void* dev, DWORD objSize, void* items, DWORD* inOut, DWORD flags) {
    HRESULT hr = g_origGetDeviceData(dev, objSize, items, inOut, flags);
    if (SUCCEEDED(hr) && inOut && !GameInForeground()) *inOut = 0;  // drop buffered events
    return hr;
}

HRESULT __stdcall HookCreateDevice(void* di, REFGUID guid, void** dev, void* outer) {
    HRESULT hr = g_origCreateDevice(di, guid, dev, outer);
    // All dinput8 device objects share one vtable; patch it once.
    if (SUCCEEDED(hr) && dev && *dev && InterlockedCompareExchange(&g_diPatchedDevice, 1, 0) == 0) {
        void** vt = *reinterpret_cast<void***>(*dev);
        PatchVtableEntry(vt, kDevSetCooperativeLevel, reinterpret_cast<void*>(&HookSetCooperativeLevel),
                         reinterpret_cast<void**>(&g_origSetCooperativeLevel));
        PatchVtableEntry(vt, kDevGetDeviceState, reinterpret_cast<void*>(&HookGetDeviceState),
                         reinterpret_cast<void**>(&g_origGetDeviceState));
        PatchVtableEntry(vt, kDevGetDeviceData, reinterpret_cast<void*>(&HookGetDeviceData),
                         reinterpret_cast<void**>(&g_origGetDeviceData));
        Log("dinput device vtable %p patched", vt);
    }
    return hr;
}

void HookDirectInput(void* di) {
    static volatile LONG done = 0;
    if (!di || InterlockedCompareExchange(&done, 1, 0) != 0) return;
    void** vt = *reinterpret_cast<void***>(di);
    PatchVtableEntry(vt, kDiCreateDevice, reinterpret_cast<void*>(&HookCreateDevice),
                     reinterpret_cast<void**>(&g_origCreateDevice));
    Log("dinput interface vtable %p patched", vt);
}

// The pause itself: sUnit only runs move() for units whose group bits are all
// enabled in its 64-bit move mask (sUnit+D30/D34). On focus loss the game clears
// bit 0x02000000 of the high half, which every character carries. Swallowing
// focus messages stops this except on the first focus loss after launch, so
// while unfocused the bit is also forced back on.
constexpr uintptr_t kUnitRoot = 0x14D09E0;
constexpr uint32_t kUnitVtable = 0x11654E0;  // image-relative
constexpr uintptr_t kMoveMaskHigh = 0xD34;
constexpr uint32_t kFocusPauseBit = 0x02000000;
volatile LONG g_unpauses = 0;

void KeepWorldRunning() {
    if (GameInForeground()) return;
    uint32_t unit, vt, mask;
    if (!ReadU32(g_base + kUnitRoot, &unit) || !ReadU32(unit, &vt) || vt != g_base + kUnitVtable ||
        !ReadU32(unit + kMoveMaskHigh, &mask) || (mask & kFocusPauseBit))
        return;
    InterlockedOr(reinterpret_cast<volatile LONG*>(unit + kMoveMaskHigh), static_cast<LONG>(kFocusPauseBit));
    if (InterlockedIncrement(&g_unpauses) == 1) Log("world was paused while unfocused; move mask restored");
}

bool ReadCamera(float* out) {
    uint32_t obj;
    return ReadU32(g_base + kCamRoot, &obj) && ReadFloats(obj + kCamOff, out, 3) && SanePos(out);
}

bool OpenMapping() {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(bridge::State),
                                  bridge::kMappingName);
    if (!h) {
        Log("CreateFileMapping failed: %lu", GetLastError());
        return false;
    }
    void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bridge::State));
    if (!view) {
        Log("MapViewOfFile failed: %lu", GetLastError());
        CloseHandle(h);
        return false;
    }
    // The handle stays open for the life of the process so the mapping persists.
    g_state = static_cast<bridge::State*>(view);
    g_state->magic = bridge::kMagic;
    g_state->version = bridge::kVersion;
    g_state->writerPid = GetCurrentProcessId();
    return true;
}

// --- Camera commands ---------------------------------------------------------
bridge::CameraCmd* g_camCmd = nullptr;

void OpenCameraMapping() {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(bridge::CameraCmd),
                                  bridge::kCamMappingName);
    if (!h) {
        Log("camera CreateFileMapping failed: %lu", GetLastError());
        return;
    }
    g_camCmd = static_cast<bridge::CameraCmd*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(bridge::CameraCmd)));
    if (!g_camCmd) {
        Log("camera MapViewOfFile failed: %lu", GetLastError());
        CloseHandle(h);
    }
}

bool SaneDir(const float* v) {
    float len2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(v[i])) return false;
        len2 += v[i] * v[i];
    }
    return len2 > 1e-6f;
}

// Consistent copy of the camera command (seqlock). False if the writer stayed busy.
bool SnapshotCameraCmd(bridge::CameraCmd* out) {
    const bridge::CameraCmd* c = g_camCmd;
    if (!c || c->magic != bridge::kCamMagic || c->version != bridge::kCamVersion) return false;
    for (int tries = 0; tries < 1000; ++tries) {
        uint32_t a = c->seq;
        if (a & 1) {
            YieldProcessor();
            continue;
        }
        MemoryBarrier();
        memcpy(out, const_cast<const bridge::CameraCmd*>(c), sizeof(*out));
        MemoryBarrier();
        if (c->seq == a) return true;
    }
    return false;
}

bool HasSkyPose(const float* p) {
    return std::isfinite(p[14]) && p[14] > 0.01f && p[14] < 10.0f;  // frustum top
}

// Reads the command and updates g_camOverride. Returns whether an override is active.
bool PollCameraCmd() {
    static uint64_t lastUpdates = 0;
    static ULONGLONG lastAdvance = 0;
    static bool lastActive = false;
    CamOverride o = {};
    const bridge::CameraCmd* c = g_camCmd;
    if (c && c->magic == bridge::kCamMagic && c->version == bridge::kCamVersion) {
        bridge::CameraCmd s = {};
        bool ok = SnapshotCameraCmd(&s);
        if (!ok) return lastActive;  // writer busy; keep the current override this round
        ULONGLONG now = GetTickCount64();
        if (s.updates != lastUpdates) {
            lastUpdates = s.updates;
            lastAdvance = now;
        }
        float d[3] = {s.target[0] - s.pos[0], s.target[1] - s.pos[1], s.target[2] - s.pos[2]};
        if (ok && (s.flags & bridge::kCamOverride) && now - lastAdvance <= bridge::kCamStaleMs && SanePos(s.pos) &&
            SanePos(s.target) && SaneDir(d) && SaneDir(s.up) && s.fovY >= 0.0f && s.fovY < 170.0f) {
            o.active = true;
            o.moveArisen = (s.flags & bridge::kMoveArisen) && SanePos(s.body);
            o.global = (s.flags & bridge::kGlobalCoords) != 0;
            o.hasPose = HasSkyPose(s.skyPose);
            memcpy(o.skyPose, s.skyPose, sizeof(o.skyPose));
            memcpy(o.body, s.body, sizeof(o.body));
            o.shiftX = s.shiftX;
            o.shiftZ = s.shiftZ;
            o.fovY = s.fovY;
            memcpy(o.pos, s.pos, sizeof(o.pos));
            memcpy(o.target, s.target, sizeof(o.target));
            memcpy(o.up, s.up, sizeof(o.up));
        }
    }
    AcquireSRWLockExclusive(&g_camLock);
    g_camOverride = o;
    ReleaseSRWLockExclusive(&g_camLock);
    InterlockedExchange(&g_terrainLinked, o.active && o.global ? 1 : 0);
    lastActive = o.active;
    return o.active;
}

// --- Ground (DDDAGhosts -> here) --------------------------------------------------
bridge::Ground* g_groundIn = nullptr;

void OpenGroundMapping() {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(bridge::Ground),
                                  bridge::kGroundMappingName);
    if (h) g_groundIn = static_cast<bridge::Ground*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(bridge::Ground)));
    if (!g_groundIn) Log("ground mapping failed: %lu", GetLastError());
}

void PollGround() {
    static uint64_t lastUpdates = 0;
    static ULONGLONG lastAdvance = 0;
    const bridge::Ground* g = g_groundIn;
    uint32_t valid = 0;
    float y[bridge::kRoleCount] = {};
    if (g && g->magic == bridge::kGroundMagic && g->version == bridge::kGroundVersion) {
        for (int tries = 0; tries < 100; ++tries) {
            uint32_t a = g->seq;
            if (a & 1) continue;
            MemoryBarrier();
            uint64_t updates = g->updates;
            uint32_t mask = g->validMask;
            memcpy(y, const_cast<const float*>(g->groundY), sizeof(y));
            MemoryBarrier();
            if (g->seq != a) continue;
            ULONGLONG now = GetTickCount64();
            if (updates != lastUpdates) {
                lastUpdates = updates;
                lastAdvance = now;
            }
            if (now - lastAdvance <= bridge::kGroundStaleMs) valid = mask;
            break;
        }
    }
    AcquireSRWLockExclusive(&g_groundLock);
    g_groundValid = valid;
    memcpy(g_groundY, y, sizeof(y));
    ReleaseSRWLockExclusive(&g_groundLock);
}

void Publish(bool hooks, const Captured* snap, LONGLONG now) {
    bridge::State* s = g_state;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&s->seq));  // odd: writing
    uint32_t flags = hooks ? bridge::kHooksActive : 0;
    float cam[3];
    if (ReadCamera(cam)) {
        flags |= bridge::kCamValid;
        s->cam[0] = cam[0];
        s->cam[1] = cam[1];
        s->cam[2] = cam[2];
    }
    s->flags = flags;
    s->tile = g_orgValid ? (static_cast<uint32_t>(g_orgM) << 16 | static_cast<uint32_t>(g_orgN)) : 0;
    for (uint32_t r = 0; r < bridge::kRoleCount; ++r) {
        bridge::Actor& a = s->actors[r];
        const Captured& c = snap[r];
        if (!c.seen) {
            a.flags = 0;
            a.msSinceSeen = 0xFFFFFFFF;
            continue;
        }
        LONGLONG ms = (now - c.qpc) * 1000 / g_qpcFreq.QuadPart;
        a.msSinceSeen = ms > 0xFFFFFFF0 ? 0xFFFFFFF0 : static_cast<uint32_t>(ms);
        a.flags = (a.msSinceSeen < bridge::kPresentMs ? bridge::kActorPresent : 0) |
                  (c.hpValid ? bridge::kActorHpValid : 0);
        a.pos[0] = c.pos[0];
        a.pos[1] = c.pos[1];
        a.pos[2] = c.pos[2];
        a.hp = c.hp;
        a.hpMax = c.hpMax;
    }
    s->qpcTime = static_cast<uint64_t>(now);
    s->updates++;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&s->seq));  // even: done
}

// The terrain streamer (tools/terrain/stream.py) starts with DDDA and dies with it, so
// playing needs no terminal. ddda_streamer.txt next to DDDA.exe: line 1 the working folder,
// line 2 the command line. No file: nothing is started. The process goes into a job that
// kills it when DDDA's handle closes (exit or crash); stream.py itself refuses to run twice.
HANDLE g_streamerJob = nullptr;

void LaunchStreamer() {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\ddda_streamer.txt", g_folder);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r, ccs=UTF-8") != 0 || !f) return;
    wchar_t dir[MAX_PATH] = {}, cmd[1024] = {};
    bool ok = fgetws(dir, MAX_PATH, f) && fgetws(cmd, 1024, f);
    fclose(f);
    if (!ok) return;
    wchar_t* lines[2] = {dir, cmd};
    for (wchar_t* s : lines)
        for (size_t n = wcslen(s); n && (s[n - 1] == L'\n' || s[n - 1] == L'\r' || s[n - 1] == L' '); --n) s[n - 1] = 0;
    g_streamerJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (g_streamerJob) SetInformationJobObject(g_streamerJob, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, dir, &si,
                        &pi)) {
        Log("streamer: could not start (%lu): %ls", GetLastError(), cmd);
        return;
    }
    if (g_streamerJob) AssignProcessToJobObject(g_streamerJob, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Log("streamer: started (pid %lu): %ls", pi.dwProcessId, cmd);
}

DWORD WINAPI BridgeThread(LPVOID) {
    QueryPerformanceFrequency(&g_qpcFreq);
    LaunchStreamer();
    if (!OpenMapping()) return 1;
    Log("bridge up: mapping %ls, base 0x%08X", bridge::kMappingName, static_cast<unsigned>(g_base));
    bool hooks = InstallHooks();
    relight::Init(&Log, g_base);
    relight::Install();
    OpenCameraMapping();
    OpenGroundMapping();
    OpenShiftMapping();
    bool camActive = false;
    ULONGLONG camOffSince = 0;

    uint32_t lastPresent = 0xFFFFFFFF;
    ULONGLONG lastStatus = 0;
    ULONGLONG lastWndTry = 0;
    while (!g_stop) {
        if (!g_wnd && GetTickCount64() - lastWndTry >= 250) {
            lastWndTry = GetTickCount64();
            TrySubclassWindow();
        }
        KeepWorldRunning();
        capture::Maintain();
        trace::Poll();
        PollGround();
        PollExperiment(g_folder);
        relight::Poll();
        Captured snap[bridge::kRoleCount];
        AcquireSRWLockShared(&g_lock);
        memcpy(snap, g_captured, sizeof(snap));
        ReleaseSRWLockShared(&g_lock);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        Publish(hooks, snap, now.QuadPart);
        if (PollCameraCmd() != camActive) {
            camActive = !camActive;
            camOffSince = GetTickCount64();
            Log("camera override %s (applied %ld frames, Arisen placed %ld times, %ld foot fits skipped so far)",
                camActive ? "ON" : "OFF", g_camApplied, g_arisenMoves, g_footFitsSkipped);
        }
        bool hide = camActive || GetTickCount64() - camOffSince < kShowDelayMs;
        if (hide != (g_hideWanted != 0)) {
            InterlockedExchange(&g_hideWanted, hide ? 1 : 0);
            isolate::SetLinked(hide);
            relight::SetLinked(hide);
            InterlockedExchange(&g_freeParty, hide ? 1 : 0);
            SetFadePatch(hide);
            Log("Arisen %s", hide ? "hidden" : "shown");
        }

        uint32_t present = 0;
        for (uint32_t r = 0; r < bridge::kRoleCount; ++r)
            if (g_state->actors[r].flags & bridge::kActorPresent) present |= 1u << r;
        // Party watch while terrain-linked: log the moment a pawn goes down or stops
        // existing, with where it was against the Arisen (2026-10-02: three pawns
        // vanished after one was knocked down; the 10 s status lines were too coarse).
        static uint32_t lastWatchPresent = 0xF;
        static float lastHp[bridge::kRoleCount];
        if (g_terrainLinked) {
            const bridge::Actor* a = g_state->actors;
            for (uint32_t r = 1; r < bridge::kRoleCount; ++r) {
                bool was = (lastWatchPresent >> r) & 1, is = (present >> r) & 1;
                bool down = (a[r].flags & bridge::kActorHpValid) && a[r].hp <= 0.0f && lastHp[r] > 0.0f;
                if ((was && !is) || down)
                    Log("party watch: pawn %u %s at (%.0f, %.0f, %.0f); Arisen at (%.0f, %.0f, %.0f); "
                        "protecting %d, tile %ldm%ldn, unpauses %ld",
                        r, down ? "KNOCKED DOWN" : "STOPPED UPDATING", a[r].pos[0], a[r].pos[1], a[r].pos[2],
                        a[0].pos[0], a[0].pos[1], a[0].pos[2], Protecting() ? 1 : 0, g_orgM, g_orgN, g_unpauses);
                lastHp[r] = (a[r].flags & bridge::kActorHpValid) ? a[r].hp : lastHp[r];
            }
        }
        lastWatchPresent = present;
        ULONGLONG t = GetTickCount64();
        if (present != lastPresent || t - lastStatus >= 10000) {
            const bridge::Actor* a = g_state->actors;
            Log("swallowed activateapp=%ld activate=%ld killfocus=%ld ncactivate=%ld hook=%ld unpauses=%ld "
                "keysBlocked=%ld",
                g_swallowed[0], g_swallowed[1], g_swallowed[2], g_swallowed[3], g_hookSwallowed, g_unpauses,
                g_keysBlocked);
            Log("present=%X rejects=%ld | A(%.0f,%.0f,%.0f %.0f/%.0f) M(%.0f,%.0f,%.0f %.0f/%.0f) "
                "H1(%.0f,%.0f,%.0f %.0f/%.0f) H2(%.0f,%.0f,%.0f %.0f/%.0f)",
                present, g_rejects, a[0].pos[0], a[0].pos[1], a[0].pos[2], a[0].hp, a[0].hpMax, a[1].pos[0],
                a[1].pos[1], a[1].pos[2], a[1].hp, a[1].hpMax, a[2].pos[0], a[2].pos[1], a[2].pos[2], a[2].hp,
                a[2].hpMax, a[3].pos[0], a[3].pos[1], a[3].pos[2], a[3].hp, a[3].hpMax);
            lastPresent = present;
            lastStatus = t;
        }
        Sleep(kPollMs);
    }
    return 0;
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash || (slash - path) + 20 >= MAX_PATH) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"ddda_bridge.log");
    // Deny writes only, so the log can be read while the game runs.
    g_log = _wfsopen(path, L"w", _SH_DENYWR);
}

using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

DirectInput8Create_t RealDirectInput8Create() {
    static DirectInput8Create_t fn = [] {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);  // SysWOW64 for a 32-bit process
        if (n == 0 || n + 14 >= MAX_PATH) return DirectInput8Create_t{};
        wcscat_s(path, L"\\dinput8.dll");
        HMODULE real = LoadLibraryW(path);
        if (!real) {
            Log("failed to load system dinput8.dll: %lu", GetLastError());
            return DirectInput8Create_t{};
        }
        return reinterpret_cast<DirectInput8Create_t>(GetProcAddress(real, "DirectInput8Create"));
    }();
    return fn;
}

}  // namespace

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out,
                                                   LPUNKNOWN outer) {
    DirectInput8Create_t real = RealDirectInput8Create();
    if (!real) return E_FAIL;
    HRESULT hr = real(inst, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out) HookDirectInput(*out);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        DisableThreadLibraryCalls(inst);
        OpenLog();
        Log("ddda_bridge loaded into pid %lu", GetCurrentProcessId());
        // Patched here, before the game's entry point runs and installs its hooks.
        g_origSetWindowsHookExA = reinterpret_cast<SetWindowsHookExA_t>(
            PatchImport("USER32.dll", "SetWindowsHookExA", reinterpret_cast<void*>(&HookSetWindowsHookExA)));
        Log("SetWindowsHookExA import %s", g_origSetWindowsHookExA ? "hooked" : "not found");
        g_origGetAsyncKeyState = reinterpret_cast<GetKeyStateFn>(
            PatchImport("USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&HookGetAsyncKeyState)));
        g_origGetKeyState = reinterpret_cast<GetKeyStateFn>(
            PatchImport("USER32.dll", "GetKeyState", reinterpret_cast<void*>(&HookGetKeyState)));
        Log("GetAsyncKeyState import %s, GetKeyState import %s", g_origGetAsyncKeyState ? "hooked" : "not found",
            g_origGetKeyState ? "hooked" : "not found");
        void* create9 = PatchImport("d3d9.dll", "Direct3DCreate9", capture::Direct3DCreate9Hook());
        capture::SetOriginal(&Log, create9);
        isolate::Init(&Log);
        {
            wchar_t dir[MAX_PATH];
            DWORD n = GetModuleFileNameW(inst, dir, MAX_PATH);
            wchar_t* slash = n && n < MAX_PATH ? wcsrchr(dir, L'\\') : nullptr;
            if (slash) {
                *slash = 0;
                trace::Init(&Log, dir);
                wcscpy_s(g_folder, dir);
            }
        }
        Log("Direct3DCreate9 import %s", create9 ? "hooked" : "not found");
        // The thread starts running once the loader lock is released.
        HANDLE t = CreateThread(nullptr, 0, BridgeThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_stop = 1;
    }
    return TRUE;
}
