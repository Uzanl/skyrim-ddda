// See relight.h. Static RE (tools/recon/sdti.py, sprops.py) and a live census
// (tools/recon/findclass.py "Light|Fog", 2026-10-02), DDDA 1.0 addresses with base
// 0x400000. The world is lit by the uSky* subclasses (weather-driven); the base classes
// uInfiniteLight/uHemiSphereLight exist too but in other light groups (0x2000, 0x80000)
// and do not light the pawns (v1 hooked those: no visible effect). The subclasses share
// the base draw functions:
//   uSkyInfiniteLight   vtable 0x160DA70, draw (slot 9) 0xE15C70: colour +0x50, mDir
//                       +0x230 (points TOWARDS the light: the moon at night has y > 0,
//                       the sun below the horizon y < 0)
//   uSkyHemiSphereLight vtable 0x160DB40, draw (slot 9) 0xE15550: mColor +0x50 (sky),
//                       mRevColor +0x230 (ground), mDir +0x240
//   uSkyColorFog        vtable 0x160D3E0, draw (slot 9) 0xE47F80: mColor +0x1A0,
//                       mStart +0x188, mEnd +0x18C
//   uLight              mGroup +0x34 (bit mask). Census at night: group 0x10 = sky dome
//                       lights (constant 0.1 values), 0x10C7 = sun/ambient, 0x10C5 = a
//                       vertical fill light with the ambient colour.
// sWeatherManager rewrites the values every frame (in move), so they are written in draw.
#include "relight.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../common/bridge_shared.h"

namespace relight {
namespace {

using DrawFn = void(__thiscall*)(void*);

constexpr uintptr_t kSunVtable = 0x120DA70, kSunDraw = 0xA15C70;
constexpr uintptr_t kHemiVtable = 0x120DB40, kHemiDraw = 0xA15550;
constexpr uintptr_t kFogVtable = 0x120D3E0, kFogDraw = 0xA47F80;
constexpr uintptr_t kDrawSlot = 9;

constexpr uintptr_t kGroup = 0x34;        // uLight mGroup
constexpr uintptr_t kColor = 0x50;        // uLight mColor (rgb)
constexpr uintptr_t kSunDir = 0x230;      // uInfiniteLight mDir
constexpr uintptr_t kHemiGround = 0x230;  // uHemiSphereLight mRevColor
constexpr uintptr_t kFogColor = 0x1A0, kFogStart = 0x188, kFogEnd = 0x18C;

constexpr float kSkyrimToDD = 100.0f / 70.0f;

LogFn g_log = nullptr;
uintptr_t g_base = 0;
DrawFn g_origSun = nullptr, g_origHemi = nullptr, g_origFog = nullptr;
volatile LONG g_linked = 0;
volatile LONG g_enabled = 1, g_fog = 1;
float g_scale = 1.0f;
volatile LONG g_mask = ~0x10;  // light groups that get Skyrim's values ("lightmask")

SRWLOCK g_lock = SRWLOCK_INIT;
bridge::LightCmd g_cmd = {};
bool g_cmdFresh = false;
bridge::LightCmd* g_view = nullptr;
uint64_t g_lastUpdates = 0;
ULONGLONG g_lastAdvance = 0;

// Per light object: its group and DDDA's own values (read before we write; DDDA
// rewrites them every frame), for the log.
struct Slot {
    uintptr_t obj;
    uint32_t group;
    float native[6];
    bool has;
};
Slot g_slots[3][8];
volatile LONG g_draws[3] = {};
const char* const kKindName[3] = {"sun", "hemi", "fog"};

bool Finite(const float* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

bool Snapshot(bridge::LightCmd* out) {
    AcquireSRWLockShared(&g_lock);
    bool ok = g_cmdFresh;
    if (ok) *out = g_cmd;
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

Slot* SlotFor(int kind, uintptr_t obj) {
    Slot* free = nullptr;
    for (Slot& s : g_slots[kind]) {
        if (s.obj == obj) return &s;
        if (!s.obj && !free) free = &s;
    }
    if (!free) free = &g_slots[kind][0];
    *free = Slot{obj, 0, {}, false};
    return free;
}

// Reads the group and `nf` float3 fields; false if the object cannot be read.
bool Native(Slot* s, const uintptr_t* offs, int nf) {
    float cur[6];
    uint32_t group = 0;
    __try {
        group = *reinterpret_cast<volatile uint32_t*>(s->obj + kGroup);
        for (int f = 0; f < nf; ++f)
            for (int i = 0; i < 3; ++i) cur[f * 3 + i] = reinterpret_cast<volatile float*>(s->obj + offs[f])[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!Finite(cur, nf * 3)) return false;
    s->group = group;
    memcpy(s->native, cur, nf * 3 * sizeof(float));
    s->has = true;
    return true;
}

void Write3(uintptr_t addr, const float* v) {
    __try {
        auto* p = reinterpret_cast<volatile float*>(addr);
        p[0] = v[0], p[1] = v[1], p[2] = v[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Scaled(const float* sky, float* out) {
    for (int i = 0; i < 3; ++i) out[i] = sky[i] * g_scale;
}

void ToDD(const float* s, float* d) { d[0] = s[0], d[1] = s[2], d[2] = -s[1]; }

bool Active(bridge::LightCmd* l) {
    return g_linked && g_enabled && Snapshot(l) && (l->flags & bridge::kLightValid);
}

bool Wanted(const Slot* s) { return (s->group & static_cast<uint32_t>(g_mask)) != 0; }

void __fastcall SunDraw(void* self, void*) {
    bridge::LightCmd l;
    if (Active(&l)) {
        auto obj = reinterpret_cast<uintptr_t>(self);
        Slot* s = SlotFor(0, obj);
        const uintptr_t offs[2] = {kColor, kSunDir};
        if (Native(s, offs, 2) && Wanted(s)) {
            InterlockedIncrement(&g_draws[0]);
            float col[3];
            // A vertical light (DDDA's fill from straight above) gets Skyrim's ambient
            // and keeps its direction; the others are the sun/moon.
            if (std::fabs(s->native[4]) > 0.99f) {
                Scaled(l.ambientUp, col);
            } else {
                float dir[3];
                Scaled(l.sunColor, col);
                ToDD(l.sunDir, dir);          // Skyrim: travel direction (downwards)
                for (float& v : dir) v = -v;  // DDDA: towards the light
                Write3(obj + kSunDir, dir);
            }
            Write3(obj + kColor, col);
        }
    }
    g_origSun(self);
}

void __fastcall HemiDraw(void* self, void*) {
    bridge::LightCmd l;
    if (Active(&l)) {
        auto obj = reinterpret_cast<uintptr_t>(self);
        Slot* s = SlotFor(1, obj);
        const uintptr_t offs[2] = {kColor, kHemiGround};
        if (Native(s, offs, 2) && Wanted(s)) {
            InterlockedIncrement(&g_draws[1]);
            float up[3], down[3];
            Scaled(l.ambientUp, up);
            Scaled(l.ambientDown, down);
            Write3(obj + kColor, up);
            Write3(obj + kHemiGround, down);
        }
    }
    g_origHemi(self);
}

void __fastcall FogDraw(void* self, void*) {
    bridge::LightCmd l;
    if (Active(&l) && g_fog) {
        auto obj = reinterpret_cast<uintptr_t>(self);
        Slot* s = SlotFor(2, obj);
        const uintptr_t offs[1] = {kFogColor};
        if (Native(s, offs, 1)) {  // not a uLight: no group to filter
            InterlockedIncrement(&g_draws[2]);
            float col[3];
            Scaled(l.fogColor, col);
            Write3(obj + kFogColor, col);
            float start = (std::max)(0.0f, l.fogNear * kSkyrimToDD);
            float end = (std::max)(start + 1000.0f, l.fogFar * kSkyrimToDD);
            __try {
                *reinterpret_cast<volatile float*>(obj + kFogStart) = start;
                *reinterpret_cast<volatile float*>(obj + kFogEnd) = end;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    g_origFog(self);
}

bool Patch(uintptr_t vtable, uintptr_t expected, void* hook, DrawFn* orig) {
    auto* slot = reinterpret_cast<uintptr_t*>(g_base + vtable + kDrawSlot * sizeof(uintptr_t));
    uintptr_t current = 0;
    __try {
        current = *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (current != g_base + expected) {
        g_log("relight: vtable %08X slot 9 holds %08X, expected %08X; not hooking",
              static_cast<unsigned>(g_base + vtable), static_cast<unsigned>(current),
              static_cast<unsigned>(g_base + expected));
        return false;
    }
    DWORD old;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return false;
    *orig = reinterpret_cast<DrawFn>(current);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(slot), static_cast<LONG>(reinterpret_cast<uintptr_t>(hook)));
    VirtualProtect(slot, sizeof(*slot), old, &old);
    return true;
}

}  // namespace

void Init(LogFn log, uintptr_t base) {
    g_log = log;
    g_base = base;
}

bool Install() {
    // All three validated first: a different game build is left untouched.
    uintptr_t v[3] = {kSunVtable, kHemiVtable, kFogVtable}, e[3] = {kSunDraw, kHemiDraw, kFogDraw};
    for (int i = 0; i < 3; ++i) {
        uintptr_t cur = 0;
        __try {
            cur = *reinterpret_cast<uintptr_t*>(g_base + v[i] + kDrawSlot * sizeof(uintptr_t));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (cur != g_base + e[i]) {
            g_log("relight: unexpected vtable contents (%08X); lighting sync disabled", static_cast<unsigned>(cur));
            return false;
        }
    }
    bool ok = Patch(kSunVtable, kSunDraw, reinterpret_cast<void*>(&SunDraw), &g_origSun) &&
              Patch(kHemiVtable, kHemiDraw, reinterpret_cast<void*>(&HemiDraw), &g_origHemi) &&
              Patch(kFogVtable, kFogDraw, reinterpret_cast<void*>(&FogDraw), &g_origFog);
    g_log("relight: draw hooks %s", ok ? "installed" : "FAILED");
    return ok;
}

void SetLinked(bool linked) { InterlockedExchange(&g_linked, linked ? 1 : 0); }

void SetOptions(bool enabled, bool fog, float scale, uint32_t mask) {
    if ((g_enabled != 0) != enabled || (g_fog != 0) != fog || g_scale != scale ||
        static_cast<uint32_t>(g_mask) != mask)
        g_log("relight: %s, fog %s, scale %.2f, groups %08X", enabled ? "ON" : "OFF (nolight)",
              fog ? "on" : "off (nofog)", scale, mask);
    InterlockedExchange(&g_enabled, enabled ? 1 : 0);
    InterlockedExchange(&g_fog, fog ? 1 : 0);
    InterlockedExchange(&g_mask, static_cast<LONG>(mask));
    g_scale = scale;
}

void Poll() {
    if (!g_view) {
        static ULONGLONG lastTry = 0;
        if (GetTickCount64() - lastTry < 1000) return;
        lastTry = GetTickCount64();
        HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, bridge::kLightMappingName);
        if (!h) return;
        g_view = static_cast<bridge::LightCmd*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(bridge::LightCmd)));
        CloseHandle(h);
        if (!g_view) return;
        g_log("relight: connected to Skyrim's lighting");
    }
    bridge::LightCmd c = {};
    bool ok = false;
    for (int tries = 0; tries < 4 && !ok; ++tries) {
        uint32_t s0 = g_view->seq;
        if (s0 & 1) continue;
        MemoryBarrier();
        memcpy(&c, const_cast<const bridge::LightCmd*>(g_view), sizeof(c));
        MemoryBarrier();
        ok = g_view->seq == s0;
    }
    ULONGLONG now = GetTickCount64();
    if (ok && c.magic == bridge::kLightMagic && c.version == bridge::kLightVersion) {
        if (c.updates != g_lastUpdates) {
            g_lastUpdates = c.updates;
            g_lastAdvance = now;
        }
    } else {
        ok = false;
    }
    bool fresh = ok && now - g_lastAdvance < bridge::kLightStaleMs;
    // A view can outlive its writer: after Skyrim reloaded, the bridge was left reading
    // an old section that never advanced (2026-10-02) while the plugin wrote a new one.
    // Stale for a while: drop the view and open the mapping again by name.
    static ULONGLONG staleSince = 0;
    if (fresh) {
        staleSince = 0;
    } else if (!staleSince) {
        staleSince = now;
    } else if (now - staleSince > 3000) {
        UnmapViewOfFile(g_view);
        g_view = nullptr;
        staleSince = 0;
        g_log("relight: lighting stale, reconnecting");
    }
    AcquireSRWLockExclusive(&g_lock);
    if (fresh) g_cmd = c;
    g_cmdFresh = fresh;
    ReleaseSRWLockExclusive(&g_lock);

    static ULONGLONG lastLog = 0;
    if (g_linked && now - lastLog >= 10000) {
        lastLog = now;
        g_log("relight: %s draws sun %ld hemi %ld fog %ld | Skyrim hour %.1f sun (%.2f %.2f %.2f) amb (%.2f %.2f %.2f) "
              "fog (%.2f %.2f %.2f)",
              fresh ? "fresh" : "STALE", g_draws[0], g_draws[1], g_draws[2], c.hour, c.sunColor[0], c.sunColor[1],
              c.sunColor[2], c.ambientUp[0], c.ambientUp[1], c.ambientUp[2], c.fogColor[0], c.fogColor[1],
              c.fogColor[2]);
        // Every DDDA light seen (its own values, before ours): which groups exist.
        for (int k = 0; k < 3; ++k)
            for (const Slot& s : g_slots[k])
                if (s.obj && s.has)
                    g_log("relight:   %s %08X group %08X%s DD (%.3f %.3f %.3f) (%.3f %.3f %.3f)", kKindName[k],
                          static_cast<unsigned>(s.obj), k == 2 ? 0u : s.group, k == 2 || Wanted(&s) ? "" : " (skipped)", s.native[0],
                          s.native[1], s.native[2], s.native[3], s.native[4], s.native[5]);
    }
}

}  // namespace relight
