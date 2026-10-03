// DDDABridge: SKSE64 plugin (x64) that reads the DDDA bridge (bridge_shared.h)
// and reports the DD party in Skyrim's console, and sends Skyrim's camera to
// DDDA (CameraCmd) so DDDA renders from the same viewpoint.
//
// Built without SKSE/CommonLib headers: the few SKSE ABI structs it needs are
// declared below (layouts from SKSE 2.3.1's PluginAPI.h / gamethreads.h), and
// the console addresses are SKSE 2.3.1's for Skyrim 1.7.104 only. The plugin
// refuses to load on any other runtime.
#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

#include "../common/bridge_shared.h"

#if !defined(_M_X64)
#error "Skyrim SE/AE is 64-bit; build this plugin for x64."
#endif

// --- SKSE ABI (from SKSE 2.3.1 PluginAPI.h) --------------------------------
namespace skse {

using PluginHandle = uint32_t;

constexpr uint32_t kRuntime_1_7_104 = 0x01070680;  // MAKE_EXE_VERSION(1, 7, 104)
constexpr uint32_t kInterface_Task = 4;
constexpr uint32_t kInterface_Messaging = 5;

struct Interface {
    uint32_t skseVersion;
    uint32_t runtimeVersion;
    uint32_t editorVersion;
    uint32_t isEditor;
    void* (*QueryInterface)(uint32_t id);
    PluginHandle (*GetPluginHandle)();
    uint32_t (*GetReleaseIndex)();
};

class TaskDelegate {
public:
    virtual void Run() = 0;
    virtual void Dispose() = 0;
};

struct TaskInterface {
    uint32_t interfaceVersion;
    void (*AddTask)(TaskDelegate* task);
    void (*AddUITask)(void* task);
};

struct MessagingInterface {
    struct Message {
        const char* sender;
        uint32_t type;
        uint32_t dataLen;
        void* data;
    };
    using EventCallback = void (*)(Message* msg);
    enum : uint32_t {
        kPostLoad,
        kPostPostLoad,
        kPreLoadGame,
        kPostLoadGame,
        kSaveGame,
        kDeleteGame,
        kInputLoaded,
        kNewGame,
        kDataLoaded,
    };
    uint32_t interfaceVersion;
    bool (*RegisterListener)(PluginHandle listener, const char* sender, EventCallback handler);
};

struct PluginVersionData {
    uint32_t dataVersion;
    uint32_t pluginVersion;
    char name[256];
    char author[256];
    char supportEmail[252];
    uint32_t versionIndependenceEx;
    uint32_t versionIndependence;
    uint32_t compatibleVersions[16];
    uint32_t seVersionRequired;
};

}  // namespace skse

extern "C" __declspec(dllexport) skse::PluginVersionData SKSEPlugin_Version = {
    1,  // kVersion
    1,
    "DDDABridge",
    "",
    "",
    0,
    0,  // hardcoded addresses: exactly one runtime
    {skse::kRuntime_1_7_104, 0},
    0,
};

namespace {

// Skyrim 1.7.104 RVAs, from SKSE 2.3.1 GameAPI.{h,cpp}.
constexpr uintptr_t kConsoleManagerPtr = 0x031DFB70;  // ConsoleManager* g_console
constexpr uintptr_t kConsoleVPrint = 0x0090F1E0;      // ConsoleManager::VPrint(fmt, va_list)
// From Address Library 1.7.104 (format 5, see tools/recon/skyrim/addrlib.py).
constexpr uintptr_t kPlayerCameraPtr = 0x031A5478;     // ID 400802, PlayerCamera*
constexpr uintptr_t kPlayerCharacterPtr = 0x03230778;  // ID 403521, PlayerCharacter*

// Object layouts, verified live on 1.7.104 (tools/recon/skyrim).
constexpr uintptr_t kCameraRoot = 0x20;      // PlayerCamera -> NiNode*
constexpr uintptr_t kWorldRotate = 0x7C;     // NiAVObject world NiMatrix3, row-major
constexpr uintptr_t kWorldTranslate = 0xA0;  // NiAVObject world NiPoint3
constexpr uintptr_t kChildren = 0x118;       // NiNode children data (NiAVObject**)
constexpr uintptr_t kFrustum = 0x150;        // NiCamera left, right, top, bottom
constexpr uintptr_t kRefPosition = 0x54;     // TESObjectREFR position
constexpr uintptr_t kRefParentCell = 0x60;   // TESObjectREFR TESObjectCELL*; null at the main menu

// Skyrim: Z up, 70 units per metre. DD: Y up, centimetres.
constexpr float kSkyrimToDD = 100.0f / 70.0f;
constexpr DWORD kCameraPeriodMs = 2;

constexpr DWORD kPollMs = 250;
constexpr ULONGLONG kReportMs = 3000;
constexpr ULONGLONG kStaleMs = 2000;  // no bridge publishes for this long = DDDA gone

uintptr_t g_base = 0;
skse::PluginHandle g_handle = 0;
skse::TaskInterface* g_task = nullptr;
std::atomic<bool> g_inGame{false};
std::atomic<bool> g_anchorReset{true};

// Latest Arisen position from the bridge, for anchoring.
SRWLOCK g_ddLock = SRWLOCK_INIT;
bool g_arisenPresent = false;
float g_arisenPos[3];
FILE* g_log = nullptr;
CRITICAL_SECTION g_logLock;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

void OpenLog() {
    InitializeCriticalSection(&g_logLock);
    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return;
    std::wstring dir = std::wstring(docs) + L"\\My Games\\Skyrim Special Edition\\SKSE";
    CoTaskMemFree(docs);
    CreateDirectoryW(dir.c_str(), nullptr);
    g_log = _wfsopen((dir + L"\\DDDABridge.log").c_str(), L"w", _SH_DENYWR);
}

// Must run on the game's main thread (via the SKSE task queue).
void ConsolePrint(const char* fmt, ...) {
    auto* mgr = *reinterpret_cast<void**>(g_base + kConsoleManagerPtr);
    if (!mgr) return;
    using VPrintFn = void (*)(void*, const char*, va_list);
    va_list ap;
    va_start(ap, fmt);
    reinterpret_cast<VPrintFn>(g_base + kConsoleVPrint)(mgr, fmt, ap);
    va_end(ap);
}

class PrintTask : public skse::TaskDelegate {
public:
    explicit PrintTask(std::string text) : text_(std::move(text)) {}
    void Run() override { ConsolePrint("%s", text_.c_str()); }
    void Dispose() override { delete this; }

private:
    std::string text_;
};

void QueueConsole(const std::string& text) {
    if (g_task && g_inGame) g_task->AddTask(new PrintTask(text));
}

// --- Bridge reader ----------------------------------------------------------

struct Reader {
    HANDLE mapping = nullptr;
    const bridge::State* view = nullptr;
    uint64_t lastUpdates = 0;
    ULONGLONG lastAdvance = 0;

    void Close() {
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
        view = nullptr;
        mapping = nullptr;
    }

    bool Open() {
        mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, bridge::kMappingName);
        if (!mapping) return false;
        view = static_cast<const bridge::State*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(bridge::State)));
        if (!view || view->magic != bridge::kMagic || view->version != bridge::kVersion) {
            Log("bridge mapping found but header is wrong (magic %08X version %u)", view ? view->magic : 0,
                view ? view->version : 0);
            Close();
            return false;
        }
        lastUpdates = 0;
        lastAdvance = GetTickCount64();
        return true;
    }

    bool Snapshot(bridge::State* out) const {
        for (int tries = 0; tries < 1000; ++tries) {
            uint32_t a = view->seq;
            if (a & 1) continue;
            MemoryBarrier();
            *out = *const_cast<const bridge::State*>(view);
            MemoryBarrier();
            if (view->seq == a) return true;
        }
        return false;
    }
};

std::string Describe(const bridge::State& s) {
    static const char* kNames[] = {"Arisen", "Main pawn", "Hired 1", "Hired 2"};
    std::string out = "[DD]";
    for (uint32_t r = 0; r < bridge::kRoleCount; ++r) {
        const bridge::Actor& a = s.actors[r];
        char buf[160];
        if (!(a.flags & bridge::kActorPresent)) {
            snprintf(buf, sizeof(buf), " %s: --", kNames[r]);
        } else if (a.flags & bridge::kActorHpValid) {
            snprintf(buf, sizeof(buf), " %s: %.0f/%.0f HP @(%.0f, %.0f, %.0f)", kNames[r], a.hp, a.hpMax, a.pos[0],
                     a.pos[1], a.pos[2]);
        } else {
            snprintf(buf, sizeof(buf), " %s: @(%.0f, %.0f, %.0f)", kNames[r], a.pos[0], a.pos[1], a.pos[2]);
        }
        out += buf;
        if (r + 1 < bridge::kRoleCount) out += " |";
    }
    return out;
}

DWORD WINAPI BridgeThread(LPVOID) {
    Reader reader;
    bool connected = false;
    ULONGLONG lastReport = 0;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (!reader.view && reader.Open()) {
            connected = true;
            Log("connected to DDDA bridge (writer pid %u)", reader.view->writerPid);
            QueueConsole("[DD] Connected to Dragon's Dogma.");
        }
        if (reader.view) {
            bridge::State s;
            if (reader.Snapshot(&s)) {
                if (s.updates != reader.lastUpdates) {
                    reader.lastUpdates = s.updates;
                    reader.lastAdvance = now;
                }
                if (now - reader.lastAdvance > kStaleMs) {
                    // DDDA exited (or hung); drop the mapping so a restart is picked up.
                    Log("bridge stopped updating; disconnecting");
                    QueueConsole("[DD] Lost Dragon's Dogma.");
                    reader.Close();
                    connected = false;
                    AcquireSRWLockExclusive(&g_ddLock);
                    g_arisenPresent = false;
                    ReleaseSRWLockExclusive(&g_ddLock);
                    g_anchorReset = true;
                } else {
                    const bridge::Actor& a = s.actors[bridge::kArisen];
                    AcquireSRWLockExclusive(&g_ddLock);
                    g_arisenPresent = (a.flags & bridge::kActorPresent) != 0;
                    memcpy(g_arisenPos, a.pos, sizeof(g_arisenPos));
                    ReleaseSRWLockExclusive(&g_ddLock);
                }
                if (reader.view && now - lastReport >= kReportMs) {
                    lastReport = now;
                    std::string line = Describe(s);
                    Log("%s", line.c_str());
                    QueueConsole(line);
                }
            }
        }
        (void)connected;
        Sleep(kPollMs);
    }
}

// --- Camera sender -----------------------------------------------------------

struct SkyCamera {
    float pos[3];
    float rot[9];  // row-major; column 1 = forward, column 2 = up
    float frustum[4];
    float player[3];
};

bool ReadPtr(uintptr_t addr, uintptr_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uintptr_t*>(addr);
        return *out >= 0x10000;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadFloats(uintptr_t addr, float* out, int n) {
    __try {
        for (int i = 0; i < n; ++i) out[i] = reinterpret_cast<const volatile float*>(addr)[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadSkyCamera(SkyCamera* c) {
    uintptr_t cam, root, children, nicam, player;
    uintptr_t cell;
    // No parent cell: not in the world (main menu after "quit to main menu"; SKSE
    // sends no message for that), so the link to DDDA is released.
    if (!ReadPtr(g_base + kPlayerCameraPtr, &cam) || !ReadPtr(cam + kCameraRoot, &root) ||
        !ReadPtr(root + kChildren, &children) || !ReadPtr(children, &nicam) ||
        !ReadPtr(g_base + kPlayerCharacterPtr, &player) || !ReadPtr(player + kRefParentCell, &cell))
        return false;
    if (!ReadFloats(root + kWorldTranslate, c->pos, 3) || !ReadFloats(root + kWorldRotate, c->rot, 9) ||
        !ReadFloats(nicam + kFrustum, c->frustum, 4) || !ReadFloats(player + kRefPosition, c->player, 3))
        return false;
    for (float v : c->pos)
        if (!std::isfinite(v)) return false;
    return c->frustum[2] > 0.01f && c->frustum[2] < 10.0f;
}

// Skyrim (x, y, z), Z up -> DD (x, y, z), Y up. Keeps handedness.
void ToDDDir(const float* s, float* d) {
    d[0] = s[0];
    d[1] = s[2];
    d[2] = -s[1];
}

// Reads DDDA's treadmill shift (bridge_shared.h, Shift); {0, 0} until DDDA has one.
void ReadShift(float* dx, float* dz) {
    static const bridge::Shift* view = nullptr;
    *dx = *dz = 0.0f;
    if (!view) {
        HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, bridge::kShiftMappingName);
        if (!h) return;
        view = static_cast<const bridge::Shift*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(bridge::Shift)));
        CloseHandle(h);
        if (!view) return;
    }
    if (view->magic != bridge::kShiftMagic || view->version != bridge::kShiftVersion) return;
    for (int tries = 0; tries < 100; ++tries) {
        uint32_t a = view->seq;
        if (a & 1) continue;
        MemoryBarrier();
        float x = view->dx, z = view->dz;
        MemoryBarrier();
        if (view->seq == a) {
            *dx = x;
            *dz = z;
            return;
        }
    }
}

void WriteCmd(bridge::CameraCmd* c, uint32_t flags, const float* pos, const float* target, const float* up,
              float fovY, const float* body, float shiftX = 0.0f, float shiftZ = 0.0f,
              const SkyCamera* sky = nullptr) {
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&c->seq));  // odd: writing
    c->flags = flags;
    c->fovY = fovY;
    memcpy(c->pos, pos, sizeof(c->pos));
    memcpy(c->target, target, sizeof(c->target));
    memcpy(c->up, up, sizeof(c->up));
    memcpy(c->body, body, sizeof(c->body));
    c->shiftX = shiftX;
    c->shiftZ = shiftZ;
    // The Skyrim camera this command comes from (bridge_shared.h SkyPose layout).
    if (sky) {
        memcpy(c->skyPose, sky->pos, 3 * sizeof(float));
        memcpy(c->skyPose + 3, sky->rot, 9 * sizeof(float));
        memcpy(c->skyPose + 12, sky->frustum, 4 * sizeof(float));
    } else {
        memset(c->skyPose, 0, sizeof(c->skyPose));
    }
    c->updates++;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&c->seq));  // even: done
}

// Publishes the link anchor for DDDAGhosts (bridge_shared.h, Anchor).
bridge::Anchor* OpenAnchorMapping() {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(bridge::Anchor),
                                  bridge::kAnchorMappingName);
    auto* a = h ? static_cast<bridge::Anchor*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bridge::Anchor)))
                : nullptr;
    if (!a) {
        Log("anchor mapping failed: %lu", GetLastError());
        return nullptr;
    }
    a->magic = bridge::kAnchorMagic;
    a->version = bridge::kAnchorVersion;
    return a;
}

void PublishAnchor(bridge::Anchor* a, bool valid, const float* sky, const float* dd) {
    if (!a) return;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&a->seq));  // odd: writing
    a->valid = valid ? 1 : 0;
    memcpy(a->sky, sky, sizeof(a->sky));
    memcpy(a->dd, dd, sizeof(a->dd));
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&a->seq));  // even: done
}

// Terrain mode (docs/terrain-proxy.md). When Data\SKSE\Plugins\DDDABridge_terrain.ini
// exists (written by tools/terrain/skyterrain.py), the anchor is fixed: Skyrim point
// `sky` is DDDA GLOBAL open-world point `dd`, the same transform used to generate
// DDDA's collision from Skyrim's terrain. Commands then carry kGlobalCoords and DDDA
// converts them to its tile-local space. Format: two lines "sky x y z" and "dd x y z".
struct TerrainLink {
    bool on = false;
    float sky[3] = {};
    float dd[3] = {};
};

TerrainLink LoadTerrainLink() {
    TerrainLink t;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = n && n < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (!slash) return t;
    *slash = 0;
    std::wstring file = std::wstring(path) + L"\\Data\\SKSE\\Plugins\\DDDABridge_terrain.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"r") != 0 || !f) return t;
    char line[256];
    bool sky = false, dd = false;
    while (fgets(line, sizeof(line), f)) {
        float a, b, c;
        if (sscanf_s(line, "sky %f %f %f", &a, &b, &c) == 3) {
            t.sky[0] = a, t.sky[1] = b, t.sky[2] = c;
            sky = true;
        } else if (sscanf_s(line, "dd %f %f %f", &a, &b, &c) == 3) {
            t.dd[0] = a, t.dd[1] = b, t.dd[2] = c;
            dd = true;
        }
    }
    fclose(f);
    t.on = sky && dd;
    return t;
}

// Maps Skyrim's camera into DD's world with a fixed anchor: the Skyrim player's
// feet at anchoring time correspond to the Arisen's feet (or, in terrain mode, the
// fixed anchor from DDDABridge_terrain.ini).
// --- Lighting sender (docs/lighting.md) ----------------------------------------
// RE::Sky (CommonLibSSE-po3 layout): the singleton pointer is read by Sky::GetSingleton
// (ID 13878 -> RVA 0x1C7D00: mov rax, [rip+...] = 0x31E0D80).
constexpr uintptr_t kSkyPtr = 0x031E0D80;
constexpr uintptr_t kSkySun = 0x80;            // Sun*
constexpr uintptr_t kSunLight = 0x38;          // NiPointer<NiDirectionalLight>
constexpr uintptr_t kDirLightWorldDir = 0x140;  // NiPoint3
constexpr uintptr_t kSkyColors = 0xA8;         // NiColor skyColor[17] (TESWeather::ColorTypes)
constexpr int kColorFogNear = 1, kColorSunlight = 4;
constexpr uintptr_t kSkyFogNear = 0x194, kSkyFogFar = 0x198, kSkyHour = 0x1B0;
constexpr uintptr_t kSkyDirAmbient = 0x200;    // NiColor [3][2]: +X, -X, +Y, -Y, +Z, -Z
constexpr uintptr_t kCellFlags = 0x40;         // u16, bit 0 = interior
constexpr uintptr_t kCellData = 0x60;          // INTERIOR_DATA* for interiors
constexpr DWORD kLightPeriodMs = 100;

bool ReadBytes(uintptr_t addr, uint8_t* out, int n) {
    __try {
        for (int i = 0; i < n; ++i) out[i] = reinterpret_cast<const volatile uint8_t*>(addr)[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool FiniteN(const float* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

bool ReadSkyLight(bridge::LightCmd* l) {
    uintptr_t sky, player, cell;
    if (!ReadPtr(g_base + kSkyPtr, &sky)) return false;
    float colors[17 * 3], amb[18], fog[2], hour;
    if (!ReadFloats(sky + kSkyColors, colors, 17 * 3) || !ReadFloats(sky + kSkyDirAmbient, amb, 18) ||
        !ReadFloats(sky + kSkyFogNear, fog, 2) || !ReadFloats(sky + kSkyHour, &hour, 1))
        return false;
    float dir[3] = {0.3f, 0.2f, -0.93f};
    uintptr_t sun, light;
    if (ReadPtr(sky + kSkySun, &sun) && ReadPtr(sun + kSunLight, &light)) {
        float d[3];
        if (ReadFloats(light + kDirLightWorldDir, d, 3) && FiniteN(d, 3)) {
            float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 1e-3f) {
                // The light that matters is above the horizon (the sun by day, the moon at
                // night): make it travel downwards whatever the stored sign.
                float s = (d[2] > 0.0f ? -1.0f : 1.0f) / len;
                for (int i = 0; i < 3; ++i) dir[i] = d[i] * s;
            }
        }
    }
    l->flags = bridge::kLightValid;
    l->hour = hour;
    memcpy(l->sunDir, dir, sizeof(dir));
    memcpy(l->sunColor, colors + 3 * kColorSunlight, 12);
    memcpy(l->ambientUp, amb + 12, 12);    // +Z
    memcpy(l->ambientDown, amb + 15, 12);  // -Z
    memcpy(l->fogColor, colors + 3 * kColorFogNear, 12);
    l->fogNear = fog[0];
    l->fogFar = fog[1];
    // Interiors: the cell's own lighting (XCLL) instead of the weather.
    uint8_t flags[2];
    uintptr_t data;
    if (ReadPtr(g_base + kPlayerCharacterPtr, &player) && ReadPtr(player + kRefParentCell, &cell) &&
        ReadBytes(cell + kCellFlags, flags, 2) && (flags[0] & 1) && ReadPtr(cell + kCellData, &data)) {
        uint8_t c[12];
        float f[2];
        if (ReadBytes(data, c, 12) && ReadFloats(data + 0x0C, f, 2)) {
            l->flags |= bridge::kLightInterior;
            for (int i = 0; i < 3; ++i) {
                l->ambientUp[i] = l->ambientDown[i] = c[i] / 255.0f;
                l->sunColor[i] = c[4 + i] / 255.0f;
                l->fogColor[i] = c[8 + i] / 255.0f;
            }
            l->sunDir[0] = 0.2f, l->sunDir[1] = 0.1f, l->sunDir[2] = -0.97f;
            l->fogNear = f[0];
            l->fogFar = f[1];
        }
    }
    return FiniteN(l->sunColor, 3) && FiniteN(l->ambientUp, 6) && FiniteN(l->fogColor, 3) &&
           std::isfinite(l->fogNear) && std::isfinite(l->fogFar);
}

DWORD WINAPI LightThread(LPVOID) {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(bridge::LightCmd),
                                  bridge::kLightMappingName);
    auto* out = h ? static_cast<bridge::LightCmd*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bridge::LightCmd)))
                  : nullptr;
    if (!out) {
        Log("light mapping failed: %lu", GetLastError());
        return 1;
    }
    out->magic = bridge::kLightMagic;
    out->version = bridge::kLightVersion;
    ULONGLONG lastLog = 0;
    for (;;) {
        bridge::LightCmd l = {};
        bool ok = g_inGame && ReadSkyLight(&l);
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&out->seq));  // odd: writing
        MemoryBarrier();
        if (ok) {
            out->flags = l.flags;
            out->hour = l.hour;
            memcpy(out->sunDir, l.sunDir, sizeof(l.sunDir));
            memcpy(out->sunColor, l.sunColor, sizeof(l.sunColor));
            memcpy(out->ambientUp, l.ambientUp, sizeof(l.ambientUp));
            memcpy(out->ambientDown, l.ambientDown, sizeof(l.ambientDown));
            memcpy(out->fogColor, l.fogColor, sizeof(l.fogColor));
            out->fogNear = l.fogNear;
            out->fogFar = l.fogFar;
            out->updates = out->updates + 1;
        } else {
            out->flags = 0;
        }
        MemoryBarrier();
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&out->seq));  // even: done
        ULONGLONG now = GetTickCount64();
        if (ok && now - lastLog >= 10000) {
            lastLog = now;
            Log("light: hour %.2f%s sun dir (%.2f %.2f %.2f) colour (%.2f %.2f %.2f) ambient up (%.2f %.2f %.2f) "
                "down (%.2f %.2f %.2f) fog (%.2f %.2f %.2f) %.0f..%.0f",
                l.hour, (l.flags & bridge::kLightInterior) ? " interior" : "", l.sunDir[0], l.sunDir[1], l.sunDir[2],
                l.sunColor[0], l.sunColor[1], l.sunColor[2], l.ambientUp[0], l.ambientUp[1], l.ambientUp[2],
                l.ambientDown[0], l.ambientDown[1], l.ambientDown[2], l.fogColor[0], l.fogColor[1], l.fogColor[2],
                l.fogNear, l.fogFar);
        }
        Sleep(kLightPeriodMs);
    }
}

// The player is in an interior cell (its own coordinates, unrelated to the world map).
bool PlayerInInterior() {
    uintptr_t player, cell;
    uint8_t flags[2];
    return ReadPtr(g_base + kPlayerCharacterPtr, &player) && ReadPtr(player + kRefParentCell, &cell) && cell &&
           ReadBytes(cell + kCellFlags, flags, 2) && (flags[0] & 1);
}

DWORD WINAPI CameraThread(LPVOID) {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(bridge::CameraCmd),
                                  bridge::kCamMappingName);
    auto* cmd = h ? static_cast<bridge::CameraCmd*>(
                        MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bridge::CameraCmd)))
                  : nullptr;
    if (!cmd) {
        Log("camera mapping failed: %lu", GetLastError());
        return 1;
    }
    cmd->magic = bridge::kCamMagic;
    cmd->version = bridge::kCamVersion;
    cmd->writerPid = GetCurrentProcessId();
    bridge::Anchor* anchorOut = OpenAnchorMapping();
    bool anchorPublished = false;

    bool anchored = false, sending = false;
    float skyAnchor[3] = {}, ddAnchor[3] = {};
    float zero[3] = {}, yUp[3] = {0.0f, 1.0f, 0.0f};
    TerrainLink terrain = LoadTerrainLink();
    ULONGLONG lastIniCheck = GetTickCount64();
    if (terrain.on)
        Log("terrain mode: Skyrim (%.0f, %.0f, %.0f) = DDDA global (%.0f, %.0f, %.0f)", terrain.sky[0], terrain.sky[1],
            terrain.sky[2], terrain.dd[0], terrain.dd[1], terrain.dd[2]);
    for (;;) {
        // Terrain streaming changes the mapping when the party leaps to another part of
        // DDDA's map (tools/terrain/stream.py rewrites the ini): follow it live.
        if (terrain.on && GetTickCount64() - lastIniCheck >= 250) {
            lastIniCheck = GetTickCount64();
            TerrainLink now = LoadTerrainLink();
            if (now.on && (memcmp(now.sky, terrain.sky, sizeof(now.sky)) || memcmp(now.dd, terrain.dd, sizeof(now.dd)))) {
                terrain = now;
                memcpy(skyAnchor, terrain.sky, sizeof(skyAnchor));
                memcpy(ddAnchor, terrain.dd, sizeof(ddAnchor));
                Log("terrain mapping changed: Skyrim (%.0f, %.0f, %.0f) = DDDA global (%.0f, %.0f, %.0f)", terrain.sky[0],
                    terrain.sky[1], terrain.sky[2], terrain.dd[0], terrain.dd[1], terrain.dd[2]);
            }
        }
        if (terrain.on) {
            g_anchorReset = false;
            if (!anchored) {
                memcpy(skyAnchor, terrain.sky, sizeof(skyAnchor));
                memcpy(ddAnchor, terrain.dd, sizeof(ddAnchor));
                anchored = true;
                QueueConsole("[DD] Terrain link: the party walks on Skyrim's ground.");
            }
        }
        if (g_anchorReset.exchange(false) && anchored) {
            anchored = false;
            Log("camera anchor cleared");
        }
        SkyCamera sc = {};
        bool ok = g_inGame && ReadSkyCamera(&sc);
        // Interiors have their own coordinates: mapped onto DDDA's world they sent the party
        // to ungenerated ground (leaps, flicker, a fall into the void; 2026-10-02). Until
        // interiors get their own ground, the link pauses: the party waits outside (the
        // bridge's "hold" keeps it in place) and joins the player again on the way out.
        static bool wasInterior = false;
        bool interior = ok && terrain.on && PlayerInInterior();
        if (interior != wasInterior) {
            wasInterior = interior;
            Log("player %s an interior: link %s", interior ? "entered" : "left", interior ? "paused" : "resumed");
            QueueConsole(interior ? "[DD] Interior: the party waits outside." : "[DD] Outside again: the party rejoins you.");
        }
        if (interior) ok = false;
        bool arisen;
        float ddPos[3];
        AcquireSRWLockShared(&g_ddLock);
        arisen = g_arisenPresent;
        memcpy(ddPos, g_arisenPos, sizeof(ddPos));
        ReleaseSRWLockShared(&g_ddLock);

        if (ok && arisen && !anchored) {
            memcpy(skyAnchor, sc.player, sizeof(skyAnchor));
            memcpy(ddAnchor, ddPos, sizeof(ddAnchor));
            anchored = true;
            Log("camera anchored: Skyrim (%.0f, %.0f, %.0f) = DD (%.0f, %.0f, %.0f)", skyAnchor[0], skyAnchor[1],
                skyAnchor[2], ddAnchor[0], ddAnchor[1], ddAnchor[2]);
            QueueConsole("[DD] Camera linked: you are standing where the Arisen stands.");
        }
        bool send = ok && anchored;
        // DDDAGhosts' ground service works with the dynamic anchor only.
        bool publish = send && !terrain.on;
        if (publish != anchorPublished) {
            PublishAnchor(anchorOut, publish, skyAnchor, ddAnchor);
            anchorPublished = publish;
        }
        if (send) {
            float rel[3] = {(sc.pos[0] - skyAnchor[0]) * kSkyrimToDD, (sc.pos[1] - skyAnchor[1]) * kSkyrimToDD,
                            (sc.pos[2] - skyAnchor[2]) * kSkyrimToDD};
            float fwdS[3] = {sc.rot[1], sc.rot[4], sc.rot[7]};
            float upS[3] = {sc.rot[2], sc.rot[5], sc.rot[8]};
            float relBody[3] = {(sc.player[0] - skyAnchor[0]) * kSkyrimToDD,
                                (sc.player[1] - skyAnchor[1]) * kSkyrimToDD,
                                (sc.player[2] - skyAnchor[2]) * kSkyrimToDD};
            float d[3], db[3], fwd[3], up[3], pos[3], target[3], body[3];
            ToDDDir(rel, d);
            ToDDDir(relBody, db);
            ToDDDir(fwdS, fwd);
            ToDDDir(upS, up);
            float shiftX = 0.0f, shiftZ = 0.0f;
            if (!terrain.on) ReadShift(&shiftX, &shiftZ);
            const float shift[3] = {shiftX, 0.0f, shiftZ};
            for (int i = 0; i < 3; ++i) {
                pos[i] = ddAnchor[i] + shift[i] + d[i];
                target[i] = pos[i] + fwd[i] * 100.0f;
                body[i] = ddAnchor[i] + shift[i] + db[i];
            }
            float fovY = 2.0f * std::atan(sc.frustum[2]) * 57.2957795f;
            uint32_t flags = bridge::kCamOverride | bridge::kMoveArisen | (terrain.on ? bridge::kGlobalCoords : 0u);
            WriteCmd(cmd, flags, pos, target, up, fovY, body, shiftX, shiftZ, &sc);
        } else if (sending) {
            WriteCmd(cmd, 0, zero, zero, yUp, 0.0f, zero);
        }
        if (send != sending) {
            sending = send;
            Log("camera sending %s", sending ? "ON" : "OFF");
        }
        Sleep(kCameraPeriodMs);
    }
}

void OnSkseMessage(skse::MessagingInterface::Message* msg) {
    if (!msg) return;
    switch (msg->type) {
    case skse::MessagingInterface::kPostLoadGame:
    case skse::MessagingInterface::kNewGame:
        g_inGame = true;
        Log("game loaded; console reports enabled");
        break;
    case skse::MessagingInterface::kPreLoadGame:
        g_inGame = false;
        g_anchorReset = true;
        break;
    default:
        break;
    }
}

}  // namespace

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const skse::Interface* skse) {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    OpenLog();
    Log("DDDABridge loading: runtime %08X, skse %08X", skse->runtimeVersion, skse->skseVersion);
    if (skse->isEditor || skse->runtimeVersion != skse::kRuntime_1_7_104) {
        Log("unsupported runtime; plugin disabled");
        return false;
    }
    g_handle = skse->GetPluginHandle();
    g_task = static_cast<skse::TaskInterface*>(skse->QueryInterface(skse::kInterface_Task));
    auto* messaging = static_cast<skse::MessagingInterface*>(skse->QueryInterface(skse::kInterface_Messaging));
    if (!g_task || !messaging || !messaging->RegisterListener(g_handle, "SKSE", OnSkseMessage)) {
        Log("missing SKSE interfaces; plugin disabled");
        return false;
    }
    HANDLE t = CreateThread(nullptr, 0, BridgeThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    t = CreateThread(nullptr, 0, CameraThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    t = CreateThread(nullptr, 0, LightThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    Log("DDDABridge loaded");
    return true;
}
