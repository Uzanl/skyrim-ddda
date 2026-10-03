// See havok_export.h.
#include "havok_export.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/spdlog.h>

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <thread>
#include <vector>

namespace havok_export {
namespace {

using Clock = std::chrono::steady_clock;

constexpr float kCellSize = 4096.0f;            // Skyrim units per exterior cell
constexpr auto kStableFor = std::chrono::seconds(3);  // attached this long before harvesting
// An interior is loaded whole before the player sees it: harvested almost at once (3 s
// was most of the wait before the party could join, 2026-10-03).
constexpr auto kInteriorStableFor = std::chrono::milliseconds(500);
constexpr int kMaxKeys = 16384;
constexpr std::size_t kMaxTris = 600000;
constexpr const char* kDir = "Data\\SKSE\\Plugins\\DDDA_havok";

// File layout (little endian), read by tools/terrain/havok.py.
struct FileHeader {
    char magic[4];         // "DDHK"
    uint32_t version;      // 1
    uint32_t worldspace;   // form id
    int32_t cellX, cellY;  // exterior cell coordinates
    uint32_t count;        // triangles
    uint32_t bodies;       // bodies that touched the cell
    uint32_t reserved;
};
static_assert(sizeof(FileHeader) == 32);
enum TriFlags : uint32_t {
    kTriStairHelper = 1u << 0,  // invisible ramp over stairs (COL_LAYER kStairHelper)
    // bits 8..15: the body's collision layer
};
struct FileTri {
    float v[9];  // three vertices, Skyrim units, z up
    uint32_t flags;
};
static_assert(sizeof(FileTri) == 40);

struct Tri {
    float v[9];
    uint32_t flags;
};

struct Job {
    std::vector<Tri> tris;
    uint32_t flags = 0;  // for the body being walked
    float lo[3], hi[3];  // region, Skyrim units
    int faults = 0;
};

bool Finite(const float* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > 1.0e7f) return false;
    return true;
}

// Havok transform: rotation columns at [0..2], [4..6], [8..10]; translation at [12..14].
void XfPoint(const float* xf, const float* p, float* out) {
    for (int i = 0; i < 3; ++i) out[i] = xf[i] * p[0] + xf[4 + i] * p[1] + xf[8 + i] * p[2] + xf[12 + i];
}

void XfDir(const float* xf, const float* d, float* out) {
    for (int i = 0; i < 3; ++i) out[i] = xf[i] * d[0] + xf[4 + i] * d[1] + xf[8 + i] * d[2];
}

void XfCompose(const float* parent, const float* child, float* out) {
    for (int c = 0; c < 3; ++c) {
        XfDir(parent, child + c * 4, out + c * 4);
        out[c * 4 + 3] = 0.0f;
    }
    XfPoint(parent, child + 12, out + 12);
    out[15] = 1.0f;
}

bool XfLooksValid(const float* xf) {
    if (!Finite(xf, 16)) return false;
    for (int c = 0; c < 3; ++c) {
        const float* col = xf + c * 4;
        if (std::fabs(col[0] * col[0] + col[1] * col[1] + col[2] * col[2] - 1.0f) > 0.05f) return false;
    }
    return true;
}

const float* Vec(const void* base, std::size_t off) {
    return reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(base) + off);
}

template <class T>
T Field(const void* base, std::size_t off) {
    T v;
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

void AabbOf(const RE::hkAabb& box, float k, float* lo, float* hi) {
    alignas(16) float mn[4], mx[4];
    _mm_store_ps(mn, box.min.quad);
    _mm_store_ps(mx, box.max.quad);
    for (int i = 0; i < 3; ++i) lo[i] = mn[i] * k, hi[i] = mx[i] * k;
}

bool Overlaps(const float* alo, const float* ahi, const float* blo, const float* bhi) {
    return alo[0] <= bhi[0] && ahi[0] >= blo[0] && alo[1] <= bhi[1] && ahi[1] >= blo[1] && alo[2] <= bhi[2] &&
           ahi[2] >= blo[2];
}

bool Included(RE::COL_LAYER layer) {
    switch (layer) {
    case RE::COL_LAYER::kStatic:
    case RE::COL_LAYER::kAnimStatic:
    case RE::COL_LAYER::kTransparent:
    case RE::COL_LAYER::kTrees:
    case RE::COL_LAYER::kTerrain:
    case RE::COL_LAYER::kGround:
    case RE::COL_LAYER::kStairHelper:
        return true;
    default:  // clutter (kProps, kClutter...), actors, water, triggers, invisible walls
        return false;
    }
}

void PushTri(Job& job, const float* a, const float* b, const float* c) {
    Tri t{{a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]}, job.flags};
    if (Finite(t.v, 9)) job.tris.push_back(t);
}

void EmitBox(Job& job, const float* c, const float (*axis)[3], const float* half) {
    float corner[8][3];
    for (int i = 0; i < 8; ++i) {
        const float sx = (i & 1) ? 1.0f : -1.0f, sy = (i & 2) ? 1.0f : -1.0f, sz = (i & 4) ? 1.0f : -1.0f;
        for (int k = 0; k < 3; ++k)
            corner[i][k] = c[k] + axis[0][k] * half[0] * sx + axis[1][k] * half[1] * sy + axis[2][k] * half[2] * sz;
    }
    static constexpr int kQuads[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    for (const auto& q : kQuads) {
        PushTri(job, corner[q[0]], corner[q[1]], corner[q[2]]);
        PushTri(job, corner[q[0]], corner[q[2]], corner[q[3]]);
    }
}

void EmitAabb(Job& job, const RE::hkpShape* shape, const float* xf, float k) {
    RE::hkAabb box;
    shape->GetAabbImpl(*reinterpret_cast<const RE::hkTransform*>(xf), 0.0f, box);
    float lo[3], hi[3];
    AabbOf(box, k, lo, hi);
    if (!Finite(lo, 3) || !Finite(hi, 3) || hi[0] - lo[0] > 4500 || hi[1] - lo[1] > 4500 || hi[2] - lo[2] > 4500)
        return;
    const float c[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    const float half[3] = {(hi[0] - lo[0]) / 2, (hi[1] - lo[1]) / 2, (hi[2] - lo[2]) / 2};
    static constexpr float kAxes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    EmitBox(job, c, kAxes, half);
}

// Convex hull from its planes (n.p + d <= 0 inside): clip a big square on each plane by all
// the other planes.
void EmitConvex(Job& job, const std::vector<std::array<float, 4>>& planes, const float* lo, const float* hi) {
    const float ex = hi[0] - lo[0], ey = hi[1] - lo[1], ez = hi[2] - lo[2];
    const float diag = std::sqrt(ex * ex + ey * ey + ez * ez) + 1.0f;
    const float mid[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto cross = [](const float* a, const float* b, float* o) {
        o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
    };
    for (std::size_t i = 0; i < planes.size(); ++i) {
        const float n[3] = {planes[i][0], planes[i][1], planes[i][2]};
        const float nl = std::sqrt(dot(n, n));
        if (nl < 1e-6f) continue;
        const float dist = (dot(n, mid) + planes[i][3]) / (nl * nl);
        const float o[3] = {mid[0] - n[0] * dist, mid[1] - n[1] * dist, mid[2] - n[2] * dist};
        const bool upish = std::fabs(n[2]) >= 0.9f * nl;
        const float ref[3] = {upish ? 1.0f : 0.0f, 0.0f, upish ? 0.0f : 1.0f};
        float t1[3], t2[3];
        cross(ref, n, t1);
        float l1 = std::sqrt(dot(t1, t1));
        if (l1 < 1e-6f) continue;
        for (float& v : t1) v /= l1;
        cross(n, t1, t2);
        float l2 = std::sqrt(dot(t2, t2));
        for (float& v : t2) v /= l2;
        std::vector<std::array<float, 3>> poly;
        const float s1[4] = {-1, 1, 1, -1}, s2[4] = {-1, -1, 1, 1};
        for (int q = 0; q < 4; ++q)
            poly.push_back({o[0] + (t1[0] * s1[q] + t2[0] * s2[q]) * diag, o[1] + (t1[1] * s1[q] + t2[1] * s2[q]) * diag,
                            o[2] + (t1[2] * s1[q] + t2[2] * s2[q]) * diag});
        for (std::size_t j = 0; j < planes.size() && poly.size() >= 3; ++j) {
            if (j == i) continue;
            const auto& cp = planes[j];
            std::vector<std::array<float, 3>> out;
            for (std::size_t v = 0; v < poly.size(); ++v) {
                const auto& A = poly[v];
                const auto& B = poly[(v + 1) % poly.size()];
                const float da = cp[0] * A[0] + cp[1] * A[1] + cp[2] * A[2] + cp[3];
                const float db = cp[0] * B[0] + cp[1] * B[1] + cp[2] * B[2] + cp[3];
                if (da <= 0.0f) out.push_back(A);
                if ((da <= 0.0f) != (db <= 0.0f)) {
                    const float t = da / (da - db);
                    out.push_back({A[0] + (B[0] - A[0]) * t, A[1] + (B[1] - A[1]) * t, A[2] + (B[2] - A[2]) * t});
                }
            }
            poly.swap(out);
        }
        for (std::size_t v = 1; v + 1 < poly.size(); ++v) PushTri(job, poly[0].data(), poly[v].data(), poly[v + 1].data());
    }
}

void Collect(const RE::hkpShape* shape, const float* xf, Job& job, int depth);

// Havok shape layouts are partly reverse-engineered; never let a bad read take down the game.
bool GuardedCollect(const RE::hkpShape* shape, const float* xf, Job* job) {
    __try {
        Collect(shape, xf, *job, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GuardedAabb(const RE::hkpShape* shape, const float* xf, RE::hkAabb* out) {
    __try {
        shape->GetAabbImpl(*reinterpret_cast<const RE::hkTransform*>(xf), 0.0f, *out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::set<int> g_loggedTypes;

void Collect(const RE::hkpShape* shape, const float* xf, Job& job, int depth) {
    if (!shape || depth > 8 || job.tris.size() > kMaxTris) return;
    const float k = RE::bhkWorld::GetWorldScaleInverse();  // Havok -> Skyrim units
    using T = RE::hkpShapeType;
    const auto type = shape->type;
    switch (type) {
    case T::kMOPP:
    case T::kBVTree: {
        auto* bv = static_cast<const RE::hkpBvTreeShape*>(shape);
        // Region box -> shape-local (inverse transform of the 8 corners).
        float llo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, lhi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (int c = 0; c < 8; ++c) {
            const float p[3] = {((c & 1) ? job.hi[0] : job.lo[0]) / k, ((c & 2) ? job.hi[1] : job.lo[1]) / k,
                                ((c & 4) ? job.hi[2] : job.lo[2]) / k};
            const float d[3] = {p[0] - xf[12], p[1] - xf[13], p[2] - xf[14]};
            for (int i = 0; i < 3; ++i) {
                const float v = xf[i * 4] * d[0] + xf[i * 4 + 1] * d[1] + xf[i * 4 + 2] * d[2];  // R^T d
                llo[i] = (std::min)(llo[i], v);
                lhi[i] = (std::max)(lhi[i], v);
            }
        }
        RE::hkAabb local;
        local.min = RE::hkVector4(llo[0], llo[1], llo[2], 0.0f);
        local.max = RE::hkVector4(lhi[0], lhi[1], lhi[2], 0.0f);
        static std::vector<RE::hkpShapeKey> keys(kMaxKeys);
        const auto found = (std::min)(bv->QueryAabbImpl(local, keys.data(), kMaxKeys), static_cast<uint32_t>(kMaxKeys));
        const auto* container = bv->GetContainer();
        if (!container) return;
        for (uint32_t i = 0; i < found; ++i) {
            RE::hkpShapeBuffer buffer;
            Collect(container->GetChildShape(keys[i], buffer), xf, job, depth + 1);
        }
        return;
    }
    case T::kList:
    case T::kCollection:
    case T::kCompressedMesh:
    case T::kExtendedMesh:
    case T::kTriangleCollection:
    case T::kConvexList: {
        const auto* container = shape->GetContainer();
        if (!container) {
            EmitAabb(job, shape, xf, k);
            return;
        }
        int guard = 0;
        for (auto key = container->GetFirstKey(); key != RE::HK_INVALID_SHAPE_KEY && guard < 200000;
             key = container->GetNextKey(key), ++guard) {
            RE::hkpShapeBuffer buffer;
            const auto* child = container->GetChildShape(key, buffer);
            if (!child) continue;
            RE::hkAabb box;
            child->GetAabbImpl(*reinterpret_cast<const RE::hkTransform*>(xf), 0.0f, box);
            float lo[3], hi[3];
            AabbOf(box, k, lo, hi);
            if (Overlaps(lo, hi, job.lo, job.hi)) Collect(child, xf, job, depth + 1);
        }
        return;
    }
    case T::kTriangle: {
        float w[3][3];
        for (int v = 0; v < 3; ++v) {
            float h[3];
            XfPoint(xf, Vec(shape, 0x30 + v * 0x10), h);
            for (int i = 0; i < 3; ++i) w[v][i] = h[i] * k;
        }
        PushTri(job, w[0], w[1], w[2]);
        return;
    }
    case T::kBox: {
        const float* half = Vec(shape, 0x30);
        const float radius = Field<float>(shape, 0x20);
        const float zero[3] = {0, 0, 0};
        float c[3], axis[3][3], h[3];
        XfPoint(xf, zero, c);
        for (int i = 0; i < 3; ++i) {
            c[i] *= k;
            for (int j = 0; j < 3; ++j) axis[i][j] = xf[i * 4 + j];
            h[i] = (half[i] + radius) * k;
        }
        if (Finite(c, 3) && Finite(h, 3)) EmitBox(job, c, axis, h);
        return;
    }
    case T::kCapsule:
    case T::kSphere: {
        const float radius = Field<float>(shape, 0x20) * k;
        const float zero[3] = {0, 0, 0};
        float a[3], b[3];
        XfPoint(xf, type == T::kCapsule ? Vec(shape, 0x30) : zero, a);
        XfPoint(xf, type == T::kCapsule ? Vec(shape, 0x40) : zero, b);
        for (int i = 0; i < 3; ++i) a[i] *= k, b[i] *= k;
        if (!Finite(a, 3) || !Finite(b, 3) || !std::isfinite(radius) || radius > 5000.0f) return;
        float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float len = std::sqrt(ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]);
        float axis[3][3]{};
        if (len > 1e-3f)
            for (int i = 0; i < 3; ++i) axis[2][i] = ab[i] / len;
        else
            axis[2][2] = 1.0f;
        const float ref[3] = {std::fabs(axis[2][2]) < 0.9f ? 0.0f : 1.0f, 0.0f, std::fabs(axis[2][2]) < 0.9f ? 1.0f : 0.0f};
        axis[0][0] = ref[1] * axis[2][2] - ref[2] * axis[2][1];
        axis[0][1] = ref[2] * axis[2][0] - ref[0] * axis[2][2];
        axis[0][2] = ref[0] * axis[2][1] - ref[1] * axis[2][0];
        const float l0 = std::sqrt(axis[0][0] * axis[0][0] + axis[0][1] * axis[0][1] + axis[0][2] * axis[0][2]);
        for (float& v : axis[0]) v /= l0;
        axis[1][0] = axis[2][1] * axis[0][2] - axis[2][2] * axis[0][1];
        axis[1][1] = axis[2][2] * axis[0][0] - axis[2][0] * axis[0][2];
        axis[1][2] = axis[2][0] * axis[0][1] - axis[2][1] * axis[0][0];
        const float c[3] = {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2};
        const float half[3] = {radius, radius, len / 2 + radius};
        EmitBox(job, c, axis, half);
        return;
    }
    case T::kConvexVertices: {
        const auto& planes = *reinterpret_cast<const RE::hkArray<RE::hkVector4>*>(reinterpret_cast<const uint8_t*>(shape) + 0x78);
        const float radius = Field<float>(shape, 0x20);
        if (planes.size() <= 0 || planes.size() > 512) {
            EmitAabb(job, shape, xf, k);
            return;
        }
        RE::hkAabb box;
        shape->GetAabbImpl(*reinterpret_cast<const RE::hkTransform*>(xf), 0.0f, box);
        float lo[3], hi[3];
        AabbOf(box, k, lo, hi);
        std::vector<std::array<float, 4>> world;
        for (int32_t i = 0; i < planes.size(); ++i) {
            alignas(16) float p[4];
            _mm_store_ps(p, planes.data()[i].quad);
            float nw[3];
            XfDir(xf, p, nw);
            const float dw = p[3] - (nw[0] * xf[12] + nw[1] * xf[13] + nw[2] * xf[14]) - radius;
            world.push_back({nw[0], nw[1], nw[2], dw * k});
        }
        if (Finite(lo, 3) && Finite(hi, 3)) EmitConvex(job, world, lo, hi);
        return;
    }
    case T::kConvexTransform:
    case T::kConvexTranslate: {
        const auto* child = Field<const RE::hkpShape*>(shape, 0x30);
        alignas(16) float local[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (type == T::kConvexTransform)
            std::memcpy(local, Vec(shape, 0x40), sizeof(local));
        else
            std::memcpy(local + 12, Vec(shape, 0x40), sizeof(float) * 3);
        if (!child || !XfLooksValid(local)) {
            EmitAabb(job, shape, xf, k);
            return;
        }
        alignas(16) float composed[16];
        XfCompose(xf, local, composed);
        Collect(child, composed, job, depth + 1);
        return;
    }
    case T::kTransform: {
        const auto* child = Field<const RE::hkpShape*>(shape, 0x28);
        alignas(16) float local[16];
        std::memcpy(local, Vec(shape, 0x50), sizeof(local));
        if (!child || !XfLooksValid(local)) {
            EmitAabb(job, shape, xf, k);
            return;
        }
        alignas(16) float composed[16];
        XfCompose(xf, local, composed);
        Collect(child, composed, job, depth + 1);
        return;
    }
    default:
        if (g_loggedTypes.insert(static_cast<int>(type)).second)
            spdlog::info("havok export: shape type {} handled as its box (convex {})", static_cast<int>(type),
                         shape->IsConvex());
        if (shape->IsConvex()) EmitAabb(job, shape, xf, k);
        return;
    }
}

// --- harvesting -----------------------------------------------------------------------------

struct CellKey {
    uint32_t world;
    int x, y;
    auto operator<=>(const CellKey&) const = default;
};

std::map<CellKey, Clock::time_point> g_firstSeen;  // attached cells not harvested yet
std::set<CellKey> g_done;                          // harvested this session

// Exterior cells: DDDA_havok\{world}_{x}_{y}.bin. Interior cells: DDDA_havok\interior\{cell}.bin,
// with the cell's form id as `worldspace` and x = y = 0.
std::string FileName(CellKey key, bool interior) {
    char name[160];
    if (interior)
        std::snprintf(name, sizeof(name), "%s\\interior\\%08X.bin", kDir, key.world);
    else
        std::snprintf(name, sizeof(name), "%s\\%08X_%d_%d.bin", kDir, key.world, key.x, key.y);
    return name;
}

void WriteFile(CellKey key, bool interior, std::vector<Tri> tris, uint32_t bodies) {
    std::thread([key, interior, tris = std::move(tris), bodies] {
        const std::string name = FileName(key, interior);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(name).parent_path(), ec);
        std::string tmp = name + ".tmp";
        FILE* f = nullptr;
        if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f) {
            spdlog::error("havok export: cannot write {}", tmp);
            return;
        }
        FileHeader h{{'D', 'D', 'H', 'K'}, 1, key.world, key.x, key.y, static_cast<uint32_t>(tris.size()), bodies, 0};
        fwrite(&h, sizeof(h), 1, f);
        static_assert(sizeof(Tri) == sizeof(FileTri));
        if (!tris.empty()) fwrite(tris.data(), sizeof(Tri), tris.size(), f);
        fclose(f);
        MoveFileExA(tmp.c_str(), name.c_str(), MOVEFILE_REPLACE_EXISTING);
    }).detach();
}

// An exterior cell: the bodies inside its square. An interior cell: its whole physics world.
void Harvest(RE::bhkWorld* bhk, CellKey key, bool interior) {
    auto* world = bhk->GetWorld1();
    if (!world) return;
    const auto t0 = Clock::now();
    const float k = RE::bhkWorld::GetWorldScaleInverse();
    Job job;
    job.lo[0] = key.x * kCellSize, job.hi[0] = (key.x + 1) * kCellSize;
    job.lo[1] = key.y * kCellSize, job.hi[1] = (key.y + 1) * kCellSize;
    job.lo[2] = -200000.0f, job.hi[2] = 200000.0f;
    if (interior)
        for (int i = 0; i < 2; ++i) job.lo[i] = -1.0e6f, job.hi[i] = 1.0e6f;
    uint32_t bodies = 0;
    std::map<int, int> layers;
    {
        RE::BSReadLockGuard lock(bhk->worldLock);
        auto walk = [&](RE::hkpSimulationIsland* island) {
            if (!island) return;
            auto& entities = island->entities;
            for (int32_t i = 0; i < entities.size(); ++i) {
                auto* entity = entities.data()[i];
                if (!entity) continue;
                const auto& col = entity->collidable;
                const auto layer = col.GetCollisionLayer();
                if (!Included(layer)) continue;
                const auto* shape = col.shape;
                const auto* xf = static_cast<const float*>(col.motion);
                if (!shape || !xf || !Finite(xf, 16)) continue;
                RE::hkAabb box;
                if (!GuardedAabb(shape, xf, &box)) continue;
                float lo[3], hi[3];
                AabbOf(box, k, lo, hi);
                if (!Overlaps(lo, hi, job.lo, job.hi)) continue;
                ++bodies;
                ++layers[static_cast<int>(layer)];
                job.flags = (static_cast<uint32_t>(layer) << 8) | (layer == RE::COL_LAYER::kStairHelper ? kTriStairHelper : 0);
                if (!GuardedCollect(shape, xf, &job)) ++job.faults;
            }
        };
        // Fixed bodies only: terrain and static objects. Doors and other moving things are in the
        // simulation islands and would be harvested in whatever state they are in right now.
        walk(world->fixedIsland);
    }
    const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::string layerText;
    for (auto [l, n] : layers) layerText += " " + std::to_string(l) + ":" + std::to_string(n);
    spdlog::info("havok export: {} {:08X} ({}, {}): {} bodies, {} triangles, {} faults, {:.1f} ms | layers{}",
                 interior ? "interior" : "cell", key.world, key.x, key.y, bodies, job.tris.size(), job.faults, ms,
                 layerText);
    WriteFile(key, interior, std::move(job.tris), bodies);
}

// DDDA_havok\current.txt: "interior {cell form id}" or "exterior", rewritten when that changes,
// so the streamer knows when to build an interior's ground (docs/terrain-proxy.md, "Interiors").
uint32_t g_reported = 0xFFFFFFFF;  // interior cell reported last (0: exterior)

void ReportCell(uint32_t interiorCell) {
    if (interiorCell == g_reported) return;
    std::error_code ec;
    std::filesystem::create_directories(kDir, ec);
    const std::string name = std::string(kDir) + "\\current.txt", tmp = name + ".tmp";
    FILE* f = nullptr;
    if (fopen_s(&f, tmp.c_str(), "w") != 0 || !f) return;
    if (interiorCell)
        std::fprintf(f, "interior %08X\n", interiorCell);
    else
        std::fprintf(f, "exterior\n");
    fclose(f);
    if (!MoveFileExA(tmp.c_str(), name.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
    g_reported = interiorCell;
    spdlog::info("havok export: player in {} {:08X}", interiorCell ? "interior" : "exterior", interiorCell);
}

}  // namespace

void Reset() {
    g_firstSeen.clear();
    g_done.clear();
    // Until the next update says where the player is, nobody is in an interior (a report left by
    // a game that closed inside one would stop the streamer's world streaming).
    std::error_code ec;
    std::filesystem::remove(std::string(kDir) + "\\current.txt", ec);
    g_reported = 0xFFFFFFFF;
}

void Update() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* tes = RE::TES::GetSingleton();
    if (!player || !tes) return;
    auto* playerCell = player->GetParentCell();
    if (!playerCell) return;
    const auto now = Clock::now();
    if (playerCell->IsInteriorCell()) {
        // An interior is loaded whole: harvest it once it has been attached for a while.
        const uint32_t id = playerCell->GetFormID();
        ReportCell(id);
        const CellKey key{id, 0, 0};
        auto* bhk = playerCell->GetbhkWorld();
        if (!bhk || !playerCell->IsAttached() || g_done.count(key)) return;
        std::erase_if(g_firstSeen, [&](const auto& e) { return e.first != key; });
        auto [it, fresh] = g_firstSeen.try_emplace(key, now);
        if (fresh || now - it->second < kInteriorStableFor) return;
        Harvest(bhk, key, true);
        g_done.insert(key);
        g_firstSeen.erase(it);
        return;
    }
    ReportCell(0);
    if (!tes->gridCells) return;
    auto* worldspace = player->GetWorldspace();
    auto* bhk = playerCell->GetbhkWorld();
    if (!worldspace || !bhk) return;
    const uint32_t n = tes->gridCells->length;
    std::set<CellKey> attached;
    for (uint32_t gx = 0; gx < n; ++gx) {
        for (uint32_t gy = 0; gy < n; ++gy) {
            auto* cell = tes->gridCells->GetCell(gx, gy);
            if (!cell || !cell->IsAttached() || cell->IsInteriorCell()) continue;
            auto* coords = cell->GetCoordinates();
            if (!coords) continue;
            attached.insert({worldspace->GetFormID(), coords->cellX, coords->cellY});
        }
    }
    // Forget cells that detached before they were harvested (their timer restarts).
    std::erase_if(g_firstSeen, [&](const auto& e) { return !attached.count(e.first); });
    // At most one cell per call (a town cell takes a few milliseconds).
    for (const CellKey& key : attached) {
        if (g_done.count(key)) continue;
        auto [it, fresh] = g_firstSeen.try_emplace(key, now);
        if (fresh || now - it->second < kStableFor) continue;
        Harvest(bhk, key, false);
        g_done.insert(key);
        g_firstSeen.erase(it);
        break;
    }
}

}  // namespace havok_export
