// DDDAGhosts: SKSE plugin (CommonLibSSE) for the DD-inside-Skyrim bridge.
// See docs/ghosts.md.
//
// Ground: for each party member (pawn position from the DDDA bridge, mapped into
// Skyrim with the link anchor that DDDABridge.dll publishes) it casts a ray for
// Skyrim's walkable surface and publishes that height in DDDA units (Ground,
// bridge_shared.h). The DDDA bridge puts the pawns at that height.
//
// Ghosts (milestone G1, currently off): one visible actor per pawn, a clone of the
// player's base with AI off, teleported every frame.
//
// Live collision export (havok_export.h): Skyrim's collision of every loaded exterior cell
// goes to files that tools/terrain/stream.py turns into DDDA ground.
//
// Ghosts are never meant to reach a save: they carry kGhostName, are deleted before
// saving and when a game loads, and are removed whenever the link is down.
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

#include "bridge_shared.h"
#include "havok_export.h"

namespace {

constexpr const char* kGhostName = "DDDA Ghost";
// Spawning is off while the DDDA side is reworked (docs/ghosts.md, "Neutralising
// DDDA's world"). Visible clones also ended up in autosaves. Cleanup of leftover
// ghosts on load stays on.
constexpr bool kSpawnGhosts = false;
constexpr std::chrono::milliseconds kUpdatePeriod{16};

void SetupLog() {
    auto dir = SKSE::log::log_directory();
    if (!dir) return;
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "DDDAGhosts.log").string(), true);
    auto log = std::make_shared<spdlog::logger>("ghosts", std::move(sink));
    log->set_level(spdlog::level::info);
    log->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%H:%M:%S.%e] %v");
}

// --- Shared memory from the bridge ------------------------------------------------

template <class T>
const T* OpenView(const wchar_t* name) {
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!h) return nullptr;
    auto* v = static_cast<const T*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(T)));
    CloseHandle(h);  // the view keeps the mapping alive
    return v;
}

// Copies a seqlocked struct (both bridge layouts start with magic, version, seq).
template <class T>
bool Snapshot(const T* src, T* out) {
    for (int tries = 0; tries < 100; ++tries) {
        uint32_t a = src->seq;
        if (a & 1) continue;
        MemoryBarrier();
        memcpy(out, const_cast<const T*>(src), sizeof(T));
        MemoryBarrier();
        if (src->seq == a) return true;
    }
    return false;
}

const bridge::State* g_state = nullptr;
const bridge::Anchor* g_anchor = nullptr;
const bridge::Shift* g_shift = nullptr;
float g_shiftX = 0.0f, g_shiftZ = 0.0f;  // DDDA's treadmill shift, read with the link

bool ReadLink(bridge::State* st, bridge::Anchor* an) {
    if (!g_state) g_state = OpenView<bridge::State>(bridge::kMappingName);
    if (!g_anchor) g_anchor = OpenView<bridge::Anchor>(bridge::kAnchorMappingName);
    if (!g_state || !g_anchor || g_state->magic != bridge::kMagic || g_anchor->magic != bridge::kAnchorMagic)
        return false;
    if (!Snapshot(g_state, st) || !Snapshot(g_anchor, an) || !an->valid) return false;
    if (!g_shift) g_shift = OpenView<bridge::Shift>(bridge::kShiftMappingName);
    bridge::Shift sh;
    if (g_shift && g_shift->magic == bridge::kShiftMagic && Snapshot(g_shift, &sh)) {
        g_shiftX = sh.dx;
        g_shiftZ = sh.dz;
    }
    return true;
}

// DDDA position -> Skyrim position (x, y only; z comes from Skyrim's ground).
RE::NiPoint3 ToSkyrim(const float* dd, const bridge::Anchor& an) {
    float dx = dd[0] - (an.dd[0] + g_shiftX), dz = dd[2] - (an.dd[2] + g_shiftZ);
    return {an.sky[0] + dx * bridge::kDDToSkyrim, an.sky[1] - dz * bridge::kDDToSkyrim, an.sky[2]};
}

// --- Ghosts (main thread only) ------------------------------------------------------

std::array<RE::ActorHandle, 3> g_ghosts;  // per pawn role (main, hired 1, hired 2)

void DeleteRef(RE::TESObjectREFR* ref) {
    ref->Disable();
    ref->SetDelete(true);
}

// Removes every ghost, including ones left over from an earlier session.
void DeleteAllGhosts(const char* why) {
    int n = 0;
    for (auto& h : g_ghosts) {
        if (auto a = h.get()) {
            DeleteRef(a.get());
            ++n;
        }
        h.reset();
    }
    if (auto* lists = RE::ProcessLists::GetSingleton()) {
        for (auto& h : lists->highActorHandles) {
            auto a = h.get();
            if (a && a->GetDisplayFullName() && std::strcmp(a->GetDisplayFullName(), kGhostName) == 0 &&
                !a->IsMarkedForDeletion()) {
                DeleteRef(a.get());
                ++n;
            }
        }
    }
    if (n) spdlog::info("deleted {} ghost(s): {}", n, why);
}

RE::Actor* SpawnGhost(int role) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* base = player ? player->GetActorBase() : nullptr;
    if (!base) return nullptr;
    auto ref = player->PlaceObjectAtMe(base, false);
    auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
    if (!actor) return nullptr;
    actor->SetDisplayName(kGhostName, true);
    actor->EnableAI(false);
    spdlog::info("spawned ghost {:08X} for pawn role {}", actor->GetFormID(), role);
    return actor;
}

// Height of the first walkable surface at (x, y): terrain, floors, planks, rocks.
// The path-pick ray ignores actors, so it never hits the ghost itself. It starts
// kStepHeight above the terrain (or above `fallback` where there is none, e.g.
// interiors), never above the ghost: starting above the ghost let it climb roofs
// frame by frame wherever the pawn's spot is inside a Skyrim building.
constexpr float kStepHeight = 120.0f;

float GroundZ(float x, float y, float fallback) {
    const float s = RE::bhkWorld::GetWorldScale();
    auto* tes = RE::TES::GetSingleton();
    float land;
    bool hasLand = tes && tes->GetLandHeight(RE::NiPoint3{x, y, fallback + 10000.0f}, land);
    const float fromZ = (hasLand ? land : fallback) + kStepHeight;
    const float toZ = fromZ - 4000.0f;
    RE::bhkPickData pick{};
    pick.rayInput.from = RE::hkVector4(x * s, y * s, fromZ * s, 0.0f);
    pick.rayInput.to = RE::hkVector4(x * s, y * s, toZ * s, 0.0f);
    pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kPathPick);
    if (tes && tes->Pick(pick) && pick.rayOutput.HasHit())
        return fromZ + (toZ - fromZ) * pick.rayOutput.hitFraction;
    return hasLand ? land : fallback;
}

// --- Ground publishing -------------------------------------------------------------

bridge::Ground* g_ground = nullptr;

bridge::Ground* GroundOut() {
    if (g_ground) return g_ground;
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(bridge::Ground),
                                  bridge::kGroundMappingName);
    if (!h) return nullptr;
    g_ground = static_cast<bridge::Ground*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bridge::Ground)));
    if (g_ground) {
        g_ground->magic = bridge::kGroundMagic;
        g_ground->version = bridge::kGroundVersion;
        spdlog::info("ground mapping ready");
    }
    return g_ground;
}

void PublishGround(const bridge::State& st, const bridge::Anchor& an) {
    bridge::Ground* g = GroundOut();
    if (!g) return;
    float y[bridge::kRoleCount] = {};
    uint32_t valid = 0;
    float fallback = RE::PlayerCharacter::GetSingleton()->GetPositionZ();
    for (uint32_t r = 0; r < bridge::kRoleCount; ++r) {
        const bridge::Actor& a = st.actors[r];
        if (!(a.flags & bridge::kActorPresent)) continue;
        RE::NiPoint3 p = ToSkyrim(a.pos, an);
        float z = GroundZ(p.x, p.y, fallback);
        y[r] = an.dd[1] + (z - an.sky[2]) / bridge::kDDToSkyrim;
        valid |= 1u << r;
    }
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g->seq));  // odd: writing
    g->validMask = valid;
    memcpy(g->groundY, y, sizeof(y));
    g->updates++;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g->seq));  // even: done
}

bool InWorld() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    return player && player->GetParentCell() && !RE::UI::GetSingleton()->GameIsPaused();
}

// Logs why updates are skipped, at most every 2 s per reason.
void Skipped(const char* why) {
    static const char* last = nullptr;
    static auto lastTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (why != last || now - lastTime > std::chrono::seconds(2)) {
        spdlog::info("update skipped: {}", why);
        last = why;
        lastTime = now;
    }
}

void Update() {
    bridge::State st;
    bridge::Anchor an;
    if (!InWorld()) {
        Skipped("not in the world or paused");
        return;
    }
    static auto lastExport = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - lastExport > std::chrono::milliseconds(250)) {
        lastExport = std::chrono::steady_clock::now();
        havok_export::Update();
    }
    if (!ReadLink(&st, &an)) {
        Skipped("no link to DDDA");
        if (g_ghosts[0] || g_ghosts[1] || g_ghosts[2]) DeleteAllGhosts("link down");
        return;
    }
    PublishGround(st, an);
    if (!kSpawnGhosts) return;
    static auto lastReport = std::chrono::steady_clock::now();
    bool report = std::chrono::steady_clock::now() - lastReport > std::chrono::seconds(2);
    if (report) lastReport = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
        const bridge::Actor& pawn = st.actors[bridge::kMainPawn + i];
        auto ghost = g_ghosts[i].get();
        if (!(pawn.flags & bridge::kActorPresent)) {
            if (ghost) {
                DeleteRef(ghost.get());
                g_ghosts[i].reset();
            }
            continue;
        }
        if (!ghost) {
            RE::Actor* a = SpawnGhost(i);
            if (!a) continue;
            g_ghosts[i] = a->GetHandle();
            ghost = g_ghosts[i].get();
            if (!ghost) continue;
        }
        RE::NiPoint3 p = ToSkyrim(pawn.pos, an);
        p.z = GroundZ(p.x, p.y, RE::PlayerCharacter::GetSingleton()->GetPositionZ());
        ghost->SetPosition(p, true);
        if (report) {
            auto now = ghost->GetPosition();
            spdlog::info("ghost {} {:08X}: target ({:.0f}, {:.0f}, {:.0f}) actual ({:.0f}, {:.0f}, {:.0f}) 3D {} disabled {}",
                         i, ghost->GetFormID(), p.x, p.y, p.z, now.x, now.y, now.z, ghost->Is3DLoaded(),
                         ghost->IsDisabled());
        }
    }
}

std::atomic<bool> g_updateQueued{false};
std::atomic<bool> g_gameReady{false};

void UpdateLoop() {
    for (;;) {
        std::this_thread::sleep_for(kUpdatePeriod);
        if (!g_gameReady || g_updateQueued.exchange(true)) continue;
        SKSE::GetTaskInterface()->AddTask([] {
            g_updateQueued = false;  // first: a failing update must not stop the loop
            try {
                Update();
            } catch (const std::exception& e) {
                static int n = 0;
                if (++n <= 20) spdlog::error("update failed: {}", e.what());
            }
        });
    }
}

void OnMessage(SKSE::MessagingInterface::Message* msg) {
    switch (msg->type) {
    case SKSE::MessagingInterface::kPreLoadGame:
        g_gameReady = false;
        break;
    case SKSE::MessagingInterface::kPostLoadGame:
    case SKSE::MessagingInterface::kNewGame:
        DeleteAllGhosts("game loaded");
        havok_export::Reset();
        g_gameReady = true;
        break;
    case SKSE::MessagingInterface::kSaveGame:
        DeleteAllGhosts("saving");  // respawned by the next update
        break;
    default:
        break;
    }
}

}  // namespace

SKSE_PLUGIN_VERSION = []() {
    SKSE::PluginVersionData v{};
    v.PluginVersion({0, 1, 0, 0});
    v.PluginName("DDDAGhosts");
    v.AuthorName("Skyrim DDDA");
    v.UsesAddressLibrary();
    v.UsesUpdatedStructs();
    return v;
}();

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SetupLog();
    spdlog::info("DDDAGhosts loading on runtime {}", REX::FModule::GetExecutingModule().GetFileVersion().string());
    if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
        spdlog::error("messaging listener failed");
        return false;
    }
    std::thread(UpdateLoop).detach();
    return true;
}
