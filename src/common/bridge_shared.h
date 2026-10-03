// Shared-memory layout between the DDDA side (32-bit dinput8.dll proxy) and
// readers (64-bit SKSE plugin, test tools). Fixed-width fields only, no
// pointers, so the layout is identical in both bitnesses.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bridge {

inline constexpr wchar_t kMappingName[] = L"Local\\DDDA_SkyrimBridge_v2";
inline constexpr uint32_t kMagic = 0x52424444;  // "DDBR"
inline constexpr uint32_t kVersion = 2;

enum StateFlags : uint32_t {
    kCamValid = 1u << 0,
    kHooksActive = 1u << 1,  // move() hooks installed; actors are live
};

enum ActorFlags : uint32_t {
    kActorPresent = 1u << 0,  // move() ran for this actor within kPresentMs
    kActorHpValid = 1u << 1,
};

enum Role : uint32_t {
    kArisen = 0,
    kMainPawn = 1,
    kHiredPawn1 = 2,
    kHiredPawn2 = 3,
    kRoleCount = 4,
};

inline constexpr uint32_t kPresentMs = 1000;

struct Actor {
    uint32_t flags;
    uint32_t msSinceSeen;  // since this actor's move() last ran
    float pos[3];          // body position, DD world units (Y up, ~cm)
    float hp;
    float hpMax;
    uint32_t reserved;
};

// Seqlock: the writer makes `seq` odd while writing and even when done.
// Readers retry if `seq` is odd or changed during their copy.
struct State {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t seq;
    uint32_t flags;
    uint64_t updates;  // increments every publish
    uint64_t qpcTime;  // QueryPerformanceCounter at publish
    uint32_t writerPid;
    uint32_t reserved;
    float cam[3];      // camera position, same space as actor positions
    uint32_t tile;     // open-world tile of the local coordinates: M << 16 | N; 0 = unknown.
                       // global = local + ((N - 50) * 10000, 0, (M - 50) * 10000)
    Actor actors[kRoleCount];  // indexed by Role
};

static_assert(sizeof(Actor) == 32);
static_assert(offsetof(State, updates) == 16);
static_assert(offsetof(State, cam) == 40);
static_assert(offsetof(State, actors) == 56);
static_assert(sizeof(State) == 56 + 4 * 32);

// --- Camera command (the other direction: Skyrim/tools -> DDDA) -------------
// While `flags & kCamOverride` is set and `updates` keeps changing, DDDA renders
// from this camera instead of its own. With `kMoveArisen` it also places the
// Arisen at `body` (x and z; DD keeps it on its own ground) every frame.
// Both sides create-or-open the mapping, so
// start order does not matter; the writer fills `magic`/`version`.
inline constexpr wchar_t kCamMappingName[] = L"Local\\DDDA_SkyrimBridge_cam_v3";
inline constexpr uint32_t kCamMagic = 0x43424444;  // "DDBC"
inline constexpr uint32_t kCamVersion = 3;
inline constexpr uint32_t kCamStaleMs = 500;  // DDDA drops an override that stops updating

enum CameraFlags : uint32_t {
    kCamOverride = 1u << 0,
    kMoveArisen = 1u << 1,
    // Terrain mode (docs/terrain-proxy.md): pos/target/body are GLOBAL open-world
    // coordinates; DDDA converts them to its tile-local space. The party then walks on
    // generated collision that matches Skyrim's terrain, with DDDA's own physics and
    // AI: no treadmill, nobody freed from DDDA's ground, only the Arisen's x/z written.
    kGlobalCoords = 1u << 2,
};

struct CameraCmd {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t seq;  // seqlock, as in State
    uint32_t flags;
    uint64_t updates;  // writer increments every write
    uint32_t writerPid;
    float fovY;       // vertical FOV in degrees; 0 keeps DDDA's own
    float pos[3];     // DD world units, same space as Actor::pos
    float target[3];  // point the camera looks at
    float up[3];
    float shiftX;   // the Shift (below) the writer used for this command
    float body[3];  // where the Arisen should stand (the Dragonborn's feet in DD space)
    float shiftZ;
    // v3: the Skyrim camera this command was made from (SkyPose layout, 16 floats).
    // DDDA stores it with every frame it renders from this command, so the ReShade
    // add-on can reproject the frame to Skyrim's camera at present (latency fix).
    float skyPose[16];
};

// --- Treadmill shift (DDDA bridge -> Skyrim plugins) ------------------------------
// While linked, the party is kept near where the link started in DDDA, inside the
// area DDDA's pawns can navigate: when the Arisen gets too far, the whole party is
// moved back and this shift grows by that move. Every DDDA <-> Skyrim conversion
// adds it to the anchor's DDDA point (x, z). Skyrim writers echo the shift they
// used (CameraCmd shiftX/Z), so DDDA can correct commands computed with an old one.
inline constexpr wchar_t kShiftMappingName[] = L"Local\\DDDA_SkyrimBridge_shift_v1";
inline constexpr uint32_t kShiftMagic = 0x53424444;  // "DDBS"
inline constexpr uint32_t kShiftVersion = 1;

struct Shift {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t seq;  // seqlock, as in State
    uint32_t generation;    // increments on every change
    float dx;
    float dz;
};

static_assert(sizeof(Shift) == 24);

// --- Link anchor (Skyrim side, DDDABridge -> DDDAGhosts) -------------------------
// The anchor of the current camera link: Skyrim position `sky` (the player's feet
// when linking) corresponds to DDDA position `dd` (the Arisen's feet). A DDDA
// offset (dx, dy, dz) maps to Skyrim (dx, -dz, dy) * kDDToSkyrim from `sky`.
inline constexpr wchar_t kAnchorMappingName[] = L"Local\\DDDA_SkyrimBridge_anchor_v1";
inline constexpr uint32_t kAnchorMagic = 0x41424444;  // "DDBA"
inline constexpr uint32_t kAnchorVersion = 1;
inline constexpr float kDDToSkyrim = 70.0f / 100.0f;

struct Anchor {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t seq;  // seqlock, as in State
    uint32_t valid;         // 1 while linked
    float sky[3];
    float dd[3];
};

static_assert(sizeof(Anchor) == 40);

// --- Skyrim ground under the party (DDDAGhosts -> DDDA bridge) -------------------
// For each role, the height (DDDA Y, cm) of Skyrim's walkable surface under that
// actor's mapped position. Bit r of `validMask` means groundY[r] is valid.
inline constexpr wchar_t kGroundMappingName[] = L"Local\\DDDA_SkyrimBridge_ground_v1";
inline constexpr uint32_t kGroundMagic = 0x47424444;  // "DDBG"
inline constexpr uint32_t kGroundVersion = 1;
inline constexpr uint32_t kGroundStaleMs = 500;

struct Ground {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t seq;  // seqlock, as in State
    uint32_t validMask;
    uint64_t updates;  // writer increments every write
    float groundY[kRoleCount];
};

static_assert(sizeof(Ground) == 40);

// --- Skyrim lighting (SKSE plugin -> DDDA bridge, docs/lighting.md) -------------
// Skyrim's current sun, ambient and fog, in Skyrim's space (Z up) and colour range
// (weather colours, 0..1). The bridge relights DDDA's sun (uInfiniteLight), ambient
// (uHemiSphereLight) and fog (uColorFog) with them while linked.
inline constexpr wchar_t kLightMappingName[] = L"Local\\DDDA_SkyrimBridge_light_v1";
inline constexpr uint32_t kLightMagic = 0x4C424444;  // "DDBL"
inline constexpr uint32_t kLightVersion = 1;
inline constexpr uint32_t kLightStaleMs = 2000;

enum LightFlags : uint32_t {
    kLightValid = 1u << 0,
    kLightInterior = 1u << 1,  // values come from the interior cell (no sun)
};

struct LightCmd {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t seq;  // seqlock, as in State
    uint32_t flags;
    uint64_t updates;       // writer increments every write
    float hour;             // Skyrim game hour 0..24
    float sunDir[3];        // direction the sun/moon light travels (unit, pointing down), Skyrim space
    float sunColor[3];      // sunlight (exterior) or directional (interior) colour
    float ambientUp[3];     // directional ambient from above (+Z)
    float ambientDown[3];   // directional ambient from below (-Z)
    float fogColor[3];      // near fog colour
    float fogNear;          // Skyrim units
    float fogFar;
    float reserved[4];
};

static_assert(sizeof(LightCmd) == 112);
static_assert(offsetof(LightCmd, updates) == 16);
static_assert(offsetof(LightCmd, hour) == 24);
static_assert(offsetof(LightCmd, fogNear) == 88);

// --- Party names (DDDA bridge -> Skyrim ReShade add-on) --------------------------
// The pawns' names as DDDA shows them above their heads, so the add-on draws the same
// labels in Skyrim (name, health bar, party colour dot). UTF-8, NUL-terminated; empty
// = unknown (the add-on then shows no name).
inline constexpr wchar_t kNamesMappingName[] = L"Local\\DDDA_SkyrimBridge_names_v1";
inline constexpr uint32_t kNamesMagic = 0x4E424444;  // "DDBN"
inline constexpr uint32_t kNamesVersion = 1;
inline constexpr uint32_t kNameBytes = 64;

struct Names {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t seq;  // seqlock, as in State
    uint32_t reserved;
    char name[kRoleCount][kNameBytes];
};

static_assert(sizeof(Names) == 16 + 4 * 64);

static_assert(offsetof(CameraCmd, updates) == 16);
static_assert(offsetof(CameraCmd, pos) == 32);
static_assert(offsetof(CameraCmd, body) == 72);
static_assert(offsetof(CameraCmd, skyPose) == 88);
static_assert(sizeof(CameraCmd) == 152);

// Skyrim camera pose as 16 floats: position (3), world rotation (9, row-major;
// column 0 = right, 1 = forward, 2 = up), NiFrustum left, right, top, bottom (4,
// tangents at unit distance). All zero = no pose.
inline constexpr int kSkyPoseFloats = 16;

}  // namespace bridge
