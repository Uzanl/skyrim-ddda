// See isolate.h.
#include <windows.h>

#include <d3d9.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "isolate.h"

namespace isolate {
namespace {

constexpr uintptr_t kPartsMask = 0x110;  // uModel parts display mask (16 dwords)
constexpr int kMaskWords = 16;
constexpr ULONGLONG kFreshMs = 500;       // party objects seen this recently are alive
// Each cycle withholds about 9 frames (Skyrim shows the last one frozen: a visible
// hitch on the pawns, reported 2026-10-02 at the 8 s period), so cycles are kept rare
// once the party is known.
constexpr ULONGLONG kRelearnMs = 30000;    // periodic cycle
constexpr ULONGLONG kOnDemandGapMs = 1000;  // minimum gap between cycles while the party is still being learned
constexpr ULONGLONG kOnDemandWindowMs = 10000;  // on-demand cycles only this long after linking
constexpr ULONGLONG kOnDemandGapLateMs = 5000;  // after that, unknown meshes (mostly new scenery) wait this long
constexpr ULONGLONG kFirstLearnMs = 1500;  // after linking: the camera just jumped, DDDA is streaming
constexpr ULONGLONG kRetryMs = 500;        // after a cycle discarded because the scene changed
constexpr int kSettleFrames = 3;          // frames for a mask change to reach the renderer
constexpr int kRecordFrames = 2;          // two frames, so per-frame ring buffers cancel out

LogFn g_log = nullptr;
volatile LONG g_linked = 0;
volatile LONG g_learnPaused = 0;
bool g_wasLinked = false;  // render thread

// Party objects, written by game threads.
struct PartyObj {
    uintptr_t obj;
    bool pawn;
    ULONGLONG lastSeen;
};
constexpr int kMaxParty = 32;
SRWLOCK g_partyLock = SRWLOCK_INIT;
PartyObj g_party[kMaxParty];

void Note(uintptr_t obj, bool pawn) {
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_partyLock);
    PartyObj* slot = nullptr;
    for (PartyObj& p : g_party) {
        if (p.obj == obj) {
            slot = &p;
            break;
        }
        if (!slot && (!p.obj || now - p.lastSeen > 5000)) slot = &p;
    }
    if (slot) {
        slot->obj = obj;
        slot->pawn = pawn;
        slot->lastSeen = now;
    }
    ReleaseSRWLockExclusive(&g_partyLock);
}

// Render thread state.
// Vertex declaration kinds. Meshes always carry a NORMAL; DDDA's HUD quads use
// exactly POSITION float3 @0, "NORMAL" ubyte4n @12 (vertex color), "TANGENT"
// float2 @16 (UV). Everything else non-indexed is a full-screen or utility pass.
enum class DeclKind : uint8_t { Plain, Mesh, Hud };
std::unordered_map<IDirect3DVertexDeclaration9*, DeclKind> g_declKind;
std::unordered_set<IDirect3DVertexBuffer9*> g_frameVbs;  // mesh VBs drawn this frame
// Party buffers drawn per frame over the last ~2 s: a pawn's new mesh (another LOD,
// new equipment) replaces one it had, so that count drops; scenery coming into view
// does not change it.
constexpr int kPartyHistory = 120;
int g_partyDrawn[kPartyHistory] = {};
int g_partyDrawnIdx = 0;
int g_cyclesPeriodic = 0, g_cyclesOnDemand = 0;
ULONGLONG g_cycleLogDue = 0;
std::unordered_set<IDirect3DVertexBuffer9*> g_partyVbs;
std::unordered_set<IDirect3DVertexBuffer9*> g_otherVbs;  // proven not the party's (drawn while hidden)
std::unordered_set<IDirect3DVertexBuffer9*> g_beforeVbs, g_hiddenVbs, g_afterVbs;
IDirect3DSurface9* g_gbuffer = nullptr;
bool g_filtering = false;
int g_filteredFrames = 0;  // Presents since filtering turned on
// DDDA presents one frame behind the draws being recorded, so the first frames
// after filtering turns on still show the whole scene; they are not published.
constexpr int kFilterWarmupFrames = 2;

enum class Learn { Idle, RecordBefore, HideSettle, RecordHidden, ShowSettle, RecordAfter };
Learn g_state = Learn::Idle;
int g_counter = 0;
ULONGLONG g_nextAllowed = 0;  // no cycle starts before this
ULONGLONG g_periodicDue = 0;  // a cycle starts at this time even without unknown meshes
ULONGLONG g_linkTick = 0;     // when the current link started
ULONGLONG g_lastCycleEnd = 0;

struct SavedMask {
    uintptr_t obj;
    uint32_t vtable;
    uint32_t mask[kMaskWords];
};
SavedMask g_saved[kMaxParty];
int g_savedCount = 0;

DeclKind KindOf(IDirect3DVertexDeclaration9* decl) {
    if (!decl) return DeclKind::Plain;
    auto it = g_declKind.find(decl);
    if (it != g_declKind.end()) return it->second;
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH];
    UINT n = MAXD3DDECLLENGTH;
    DeclKind kind = DeclKind::Plain;
    if (SUCCEEDED(decl->GetDeclaration(el, &n))) {
        UINT count = 0;
        bool normal = false;
        for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i, ++count)
            if (el[i].Usage == D3DDECLUSAGE_NORMAL) normal = true;
        auto is = [&](UINT i, BYTE usage, BYTE type, WORD offset) {
            return el[i].Stream == 0 && el[i].Usage == usage && el[i].UsageIndex == 0 && el[i].Type == type &&
                   el[i].Offset == offset;
        };
        if (count == 3 && is(0, D3DDECLUSAGE_POSITION, D3DDECLTYPE_FLOAT3, 0) &&
            is(1, D3DDECLUSAGE_NORMAL, D3DDECLTYPE_UBYTE4N, 12) && is(2, D3DDECLUSAGE_TANGENT, D3DDECLTYPE_FLOAT2, 16))
            kind = DeclKind::Hud;
        else if (normal)
            kind = DeclKind::Mesh;
    }
    g_declKind[decl] = kind;
    return kind;
}

bool CopyMask(uintptr_t from, uint32_t* to) {
    __try {
        for (int i = 0; i < kMaskWords; ++i) to[i] = reinterpret_cast<const volatile uint32_t*>(from)[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteMask(uintptr_t to, const uint32_t* from) {
    __try {
        for (int i = 0; i < kMaskWords; ++i) reinterpret_cast<volatile uint32_t*>(to)[i] = from[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadVtable(uintptr_t obj, uint32_t* vt) {
    __try {
        *vt = *reinterpret_cast<const volatile uint32_t*>(obj);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void HideParty() {
    static const uint32_t kZero[kMaskWords] = {};
    ULONGLONG now = GetTickCount64();
    PartyObj objs[kMaxParty];
    AcquireSRWLockShared(&g_partyLock);
    memcpy(objs, g_party, sizeof(objs));
    ReleaseSRWLockShared(&g_partyLock);
    g_savedCount = 0;
    for (const PartyObj& p : objs) {
        if (!p.obj || now - p.lastSeen > kFreshMs) continue;
        SavedMask& s = g_saved[g_savedCount];
        if (!ReadVtable(p.obj, &s.vtable) || !CopyMask(p.obj + kPartsMask, s.mask)) continue;
        if (memcmp(s.mask, kZero, sizeof(kZero)) == 0) continue;  // already hidden by someone else
        s.obj = p.obj;
        WriteMask(p.obj + kPartsMask, kZero);
        ++g_savedCount;
    }
}

void ShowParty() {
    static const uint32_t kZero[kMaskWords] = {};
    for (int i = 0; i < g_savedCount; ++i) {
        SavedMask& s = g_saved[i];
        uint32_t vt, cur[kMaskWords];
        // Only undo our own change: same object, still all zero.
        if (ReadVtable(s.obj, &vt) && vt == s.vtable && CopyMask(s.obj + kPartsMask, cur) &&
            memcmp(cur, kZero, sizeof(cur)) == 0)
            WriteMask(s.obj + kPartsMask, s.mask);
    }
    g_savedCount = 0;
}

}  // namespace

void Init(LogFn log) {
    g_log = log;
}

void SetLinked(bool linked) {
    InterlockedExchange(&g_linked, linked ? 1 : 0);
}

void NotePawn(uintptr_t pawn) {
    Note(pawn, true);
}

bool IsPawn(uintptr_t obj) {
    if (!obj) return false;
    bool found = false;
    AcquireSRWLockShared(&g_partyLock);
    for (const PartyObj& p : g_party)
        if (p.obj == obj && p.pawn) found = true;
    ReleaseSRWLockShared(&g_partyLock);
    return found;
}

void NoteCarried(uintptr_t model) {
    Note(model, false);
}

// Indexed mesh draws are skipped unless they use a party vertex buffer. Non-indexed
// draws are full-screen and utility quads (shadow-map clears, the sun light,
// post-processing) and always run, except the HUD's. Skipping the shadow-map clear
// once left the trees' pre-filter shadows frozen on the pawns.
bool SkipDraw(IDirect3DVertexDeclaration9* decl, IDirect3DVertexBuffer9* vb, bool indexed) {
    DeclKind kind = KindOf(decl);
    if (!indexed) return g_filtering && kind == DeclKind::Hud;
    if (kind == DeclKind::Plain) return false;  // light volumes, FVF draws
    if (vb) g_frameVbs.insert(vb);
    return g_filtering && !(vb && g_partyVbs.count(vb));
}

void OnClear(IDirect3DDevice9* dev, DWORD flags, D3DCOLOR color) {
    if (!(flags & D3DCLEAR_TARGET) || (color & 0x00FFFFFF) != 0x00FFFFFF) return;
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
    D3DSURFACE_DESC d;
    if (SUCCEEDED(rt->GetDesc(&d)) && d.Width >= 640 && d.Height >= 360 && rt != g_gbuffer) {
        g_gbuffer = rt;
        g_log("isolate: G-buffer %p (%ux%u fmt %d)", rt, d.Width, d.Height, d.Format);
    }
    rt->Release();  // owned by the game; we only keep the pointer for StretchRect
}

// Learning cycle: record the mesh vertex buffers of 2 visible frames, hide the
// party, record 2 hidden frames, show it again, record 2 more visible frames.
// - drawn before and after but not while hidden: the party's (added);
// - drawn while hidden: not the party's (removed);
// - not drawn at all (the pawns were off screen): keeps its previous status.
// Requiring "before and after" rejects scenery that came into view because the
// camera moved during the cycle; a cycle whose before and after sets differ a lot
// (the scene itself changed) is discarded.
void OnPresent() {
    bool linked = g_linked != 0;
    ULONGLONG now = GetTickCount64();
    if (linked && !g_wasLinked) {
        g_nextAllowed = g_periodicDue = now + kFirstLearnMs;
        g_linkTick = now;
    }
    g_wasLinked = linked;
    switch (g_state) {
    case Learn::Idle: {
        // A mesh never classified (new equipment, another level of detail, scenery
        // coming into view) triggers a cycle early, so pawns don't wait seconds for
        // parts of their outfit.
        bool unknown = false;
        int partyDrawn = 0;
        for (IDirect3DVertexBuffer9* vb : g_frameVbs) {
            if (g_partyVbs.count(vb))
                ++partyDrawn;
            else if (!g_otherVbs.count(vb))
                unknown = true;
        }
        int recentMax = 0;
        for (int c : g_partyDrawn) recentMax = (std::max)(recentMax, c);
        g_partyDrawn[g_partyDrawnIdx] = partyDrawn;
        g_partyDrawnIdx = (g_partyDrawnIdx + 1) % kPartyHistory;
        // Right after linking, unknown meshes are often the pawns' own (another LOD,
        // parts that were off screen): learn them quickly. Later they are mostly
        // scenery coming into view (a cycle every ~5 s, each a visible hitch: user
        // report 2026-10-02), so they count only while fewer party buffers are drawn
        // than recently (a pawn lost a mesh: it was replaced by an unknown one).
        bool early = now - g_linkTick < kOnDemandWindowMs;
        bool onDemand = unknown && (early || (now >= g_lastCycleEnd + kOnDemandGapLateMs && partyDrawn < recentMax));
        bool paused = g_learnPaused && !early && !g_partyVbs.empty();
        if (linked && !paused && now >= g_nextAllowed && (onDemand || now >= g_periodicDue)) {
            ++(onDemand ? g_cyclesOnDemand : g_cyclesPeriodic);
            g_beforeVbs = g_frameVbs;  // the frame that just finished
            g_state = Learn::RecordBefore;
            g_counter = kRecordFrames - 1;
        }
        break;
    }
    case Learn::RecordBefore:
        g_beforeVbs.insert(g_frameVbs.begin(), g_frameVbs.end());
        if (--g_counter == 0) {
            HideParty();
            g_state = Learn::HideSettle;
            g_counter = kSettleFrames;
        }
        break;
    case Learn::HideSettle:
        if (--g_counter == 0) {
            g_state = Learn::RecordHidden;
            g_hiddenVbs.clear();
            g_counter = kRecordFrames + 1;  // the next Present closes the first hidden frame
        }
        break;
    case Learn::RecordHidden:
        if (g_counter-- <= kRecordFrames) g_hiddenVbs.insert(g_frameVbs.begin(), g_frameVbs.end());
        if (g_counter == 0) {
            ShowParty();
            g_state = Learn::ShowSettle;
            g_counter = kSettleFrames;
        }
        break;
    case Learn::ShowSettle:
        if (--g_counter == 0) {
            g_state = Learn::RecordAfter;
            g_afterVbs.clear();
            g_counter = kRecordFrames + 1;
        }
        break;
    case Learn::RecordAfter:
        if (g_counter-- <= kRecordFrames) g_afterVbs.insert(g_frameVbs.begin(), g_frameVbs.end());
        if (g_counter == 0) {
            size_t changed = 0;
            for (IDirect3DVertexBuffer9* vb : g_beforeVbs) changed += !g_afterVbs.count(vb);
            for (IDirect3DVertexBuffer9* vb : g_afterVbs) changed += !g_beforeVbs.count(vb);
            g_state = Learn::Idle;
            if (changed > 4 + g_beforeVbs.size() / 10) {
                g_log("isolate: learning cycle discarded, scene changed (%zu buffers differ)", changed);
                g_nextAllowed = g_periodicDue = now + kRetryMs;
                break;
            }
            size_t added = 0, removed = 0;
            for (IDirect3DVertexBuffer9* vb : g_beforeVbs)
                if (g_afterVbs.count(vb) && !g_hiddenVbs.count(vb) && g_partyVbs.insert(vb).second) {
                    g_otherVbs.erase(vb);
                    ++added;
                }
            for (IDirect3DVertexBuffer9* vb : g_hiddenVbs) {
                removed += g_partyVbs.erase(vb);
                g_otherVbs.insert(vb);
            }
            if (added || removed)
                g_log("isolate: party vertex buffers %zu (+%zu -%zu; before %zu, hidden %zu, after %zu)",
                      g_partyVbs.size(), added, removed, g_beforeVbs.size(), g_hiddenVbs.size(), g_afterVbs.size());
            g_nextAllowed = now + kOnDemandGapMs;
            g_periodicDue = now + kRelearnMs;
            g_lastCycleEnd = now;
        }
        break;
    }
    if (!linked && g_state == Learn::Idle) {
        g_partyVbs.clear();
        g_otherVbs.clear();
    }
    bool filtering = linked && !g_partyVbs.empty();
    if (filtering != g_filtering) {
        g_filtering = filtering;
        g_filteredFrames = 0;
        g_log("isolate: party-only rendering %s", filtering ? "ON" : "OFF");
    } else if (filtering && g_filteredFrames < kFilterWarmupFrames) {
        ++g_filteredFrames;
    }
    g_frameVbs.clear();
    if (linked && now >= g_cycleLogDue) {
        if (g_cycleLogDue)
            g_log("isolate: learning cycles in the last 60 s: %d on demand, %d periodic (each hides the pawns ~9 frames)",
                  g_cyclesOnDemand, g_cyclesPeriodic);
        g_cyclesOnDemand = g_cyclesPeriodic = 0;
        g_cycleLogDue = now + 60000;
    }
}

void SetLearnPaused(bool paused) {
    if ((g_learnPaused != 0) != paused) g_log("isolate: learning %s", paused ? "PAUSED (nolearn)" : "on");
    InterlockedExchange(&g_learnPaused, paused ? 1 : 0);
}

bool Filtering() {
    return g_filtering && g_filteredFrames >= kFilterWarmupFrames;
}

bool PublishThisFrame() {
    return g_state == Learn::Idle || g_state == Learn::RecordBefore || g_state == Learn::RecordAfter;
}

IDirect3DSurface9* GBuffer() {
    return Filtering() ? g_gbuffer : nullptr;
}

}  // namespace isolate
