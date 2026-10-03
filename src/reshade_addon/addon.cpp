// DDDABridge ReShade add-on (x64, Skyrim SE): shows DDDA's frames inside Skyrim.
//
// Reads the frames the DDDA bridge publishes (frame_shared.h) and draws the newest
// one full screen over Skyrim's back buffer right before present, transparent
// wherever DDDA's G-buffer is empty, so the pawns stand in Skyrim's world.
//
// Only masked frames are drawn: DDDA renders the party alone only while Skyrim's
// camera drives it, which starts once a save has loaded. So nothing shows at the
// main menu or on loading screens, and the overlay disappears when frames stop
// arriving (kStaleMs).
//
// Depth test (step 2c): DDDA's G-buffer holds a 24-bit perspective depth (R high
// byte, G, B low; near 32 cm), so each pawn pixel's view distance is known. Skyrim's
// main depth buffer (the depth-stencil view with the most draws this frame) is
// copied at present, linearised with the camera's near/far, and a pawn pixel is
// dropped where Skyrim drew something closer.
//
// Reprojection (latency fix): a DD frame reaches Skyrim a few frames after the
// Skyrim camera it was rendered for, so with a moving camera the pawns lagged
// behind the world ("dragged"). Each frame carries that Skyrim camera pose
// (frame_shared.h, kSlotHasPose); at present every output pixel's view ray in the
// CURRENT Skyrim camera is intersected with the DD depth (fixed-point iteration) and
// projected into the frame's camera to fetch the pixel it shows now.
//
// Under the UI: Skyrim draws its finished 3D image onto the back buffer with one
// draw, then the interface (HUD, menus) with more draws onto the same target. The
// party is drawn right before the second back-buffer draw of a frame, so menus,
// compass and crosshair stay on top; frames without a second draw fall back to
// drawing at present (where it used to be, over the UI).
//
// D3D11 only; drawing uses the native D3D11 objects ReShade exposes, and restores
// the pipeline state it touches.
//
// Install: build/x64/DDDABridge.addon64 next to SkyrimSE.exe (ReShade 6.x with
// full add-on support). Log lines go to ReShade.log.
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <reshade.hpp>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../common/bridge_shared.h"
#include "../common/frame_shared.h"

extern "C" __declspec(dllexport) const char* NAME = "DDDABridge";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Shows Dragon's Dogma: Dark Arisen frames inside Skyrim (DD inside Skyrim bridge).";

using namespace reshade::api;

namespace {

constexpr ULONGLONG kStaleMs = 250;  // no new masked frame for this long: draw nothing

// DDDA's camera clip planes (uCameraBase +0x34 near, +0x30 far), centimetres.
constexpr float kDdNear = 32.0f;
constexpr float kDdFar = 1600000.0f;
constexpr float kDdToSkyrim = 70.0f / 100.0f;  // DD cm -> Skyrim units

// Skyrim 1.7.104 (same addresses as the SKSE plugin): PlayerCamera* -> camera root
// NiNode (+0x20) -> first child NiCamera (+0x118) -> frustum near/far (+0x160).
constexpr uintptr_t kPlayerCameraPtr = 0x031A5478;

const char kShader[] = R"(
Texture2D tex : register(t0);        // DDDA color, alpha = party mask
Texture2D gbuf : register(t1);       // DDDA G-buffer: packed 24-bit depth
Texture2D<float> skyDepth : register(t2);
SamplerState smp : register(s0);
cbuffer Params : register(b0) {
    float skyNear, skyFar, ddNear, ddFar;
    float ddToSky, useSkyDepth, ddWidth, ddHeight;
    float3 nPos; float warp;    // current Skyrim camera (Skyrim units)
    float3 nRight; float pad0;
    float3 nUp; float pad1;
    float3 nFwd; float pad2;
    float3 oPos; float pad3;    // the camera the DD frame was rendered for
    float3 oRight; float pad4;
    float3 oUp; float pad5;
    float3 oFwd; float pad6;
    float4 nFr;                 // NiFrustum left, right, top, bottom
    float4 oFr;
};
float Linear(float d, float n, float f) { return n * f / (f - d * (f - n)); }
// DD view depth at uv, Skyrim units along the frame camera's forward axis.
float FrameZ(float2 uv) {
    int2 c = clamp(int2(uv * float2(ddWidth, ddHeight)), int2(0, 0), int2(ddWidth, ddHeight) - 1);
    uint3 b = (uint3)round(gbuf.Load(int3(c, 0)).rgb * 255);
    float d = (b.r * 65536.0 + b.g * 256.0 + b.b) / 16777215.0;
    return min(Linear(d, ddNear, ddFar) * ddToSky, 200000.0);
}
float3 RayNow(float2 uv) {
    return nRight * lerp(nFr.x, nFr.y, uv.x) + nUp * lerp(nFr.z, nFr.w, uv.y) + nFwd;
}
float2 ProjectFrame(float3 p) {
    float3 v = p - oPos;
    float z = max(dot(v, oFwd), 1e-3);
    float x = dot(v, oRight) / z, y = dot(v, oUp) / z;
    return float2((x - oFr.x) / (oFr.y - oFr.x), (oFr.z - y) / (oFr.z - oFr.w));
}
void vs_main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
// The mask (alpha) is eroded by one texel: DDDA's anti-aliasing blends the pawns'
// outermost pixels with its empty background, which would show as a light halo.
float4 ps_main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 src = uv;
    float pawn = 0;
    bool consistent = true;
    if (warp > 0) {
        // Find the point this pixel sees now on the DD frame's depth surface. Near a
        // moving pawn's edge there are two answers (the background, or the pawn where
        // it was), and starting the search at the same uv picked the pawn: it showed
        // twice ("duplicating"). So the search starts from the background (far away,
        // rotation only), refines with the depth found, and the result must agree with
        // the depth it was computed from; otherwise this pixel shows background.
        float3 r = RayNow(uv);
        float rf = max(dot(r, oFwd), 1e-3);
        float base = dot(nPos - oPos, oFwd);
        float z = 200000.0;
        float3 p = nPos;
        [unroll] for (int i = 0; i < 4; ++i) {
            p = nPos + r * max((z - base) / rf, 0.0);
            src = ProjectFrame(p);
            z = FrameZ(src);
        }
        p = nPos + r * max((z - base) / rf, 0.0);
        float2 check = ProjectFrame(p);
        consistent = abs(FrameZ(check) - z) < max(30.0, 0.03 * z);
        src = check;
        pawn = dot(p - nPos, nFwd);
    }
    if (!consistent || any(src < 0) || any(src > 1)) return float4(0, 0, 0, 0);
    float4 c = tex.Sample(smp, src);
    float a = c.a;
    a = min(a, tex.Sample(smp, src, int2(1, 0)).a);
    a = min(a, tex.Sample(smp, src, int2(-1, 0)).a);
    a = min(a, tex.Sample(smp, src, int2(0, 1)).a);
    a = min(a, tex.Sample(smp, src, int2(0, -1)).a);
    if (a > 0 && useSkyDepth > 0) {
        if (warp <= 0) pawn = FrameZ(src);
        float sky = Linear(skyDepth.Load(int3(pos.xy, 0)), skyNear, skyFar);
        if (pawn > sky * 1.02 + 5) a = 0;  // something of Skyrim's is in front
    }
    return float4(c.rgb, a);
}

// --- Reprojection mesh (the default when the frame has a pose) ---------------------
// Every 2x2 block of the DD frame is a grid cell; each grid vertex is lifted to 3D with
// the DD depth in the frame's camera and projected with Skyrim's current camera, so
// rotation, translation and orbiting all reproject with the right parallax. Our own
// depth buffer keeps the nearest surface where the warped grid folds over itself.
// Triangles stretched across a depth edge (pawn to background) are discarded per pixel
// where the frame's depth at the sampled texel disagrees with the interpolated one.
static const uint2 kCorner[6] = {uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(0, 1), uint2(1, 0), uint2(1, 1)};
void vs_mesh(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uvOld : TEXCOORD0,
             out float zOld : TEXCOORD1, out float zNew : TEXCOORD2) {
    uint cellsX = (uint(ddWidth) - 1) / 2;
    uint cell = id / 6, k = id % 6;
    int2 texel = int2((cell % cellsX + kCorner[k].x) * 2, (cell / cellsX + kCorner[k].y) * 2);
    texel = min(texel, int2(ddWidth, ddHeight) - 1);
    uvOld = (texel + 0.5) / float2(ddWidth, ddHeight);
    zOld = FrameZ(uvOld);
    float3 rOld = oRight * lerp(oFr.x, oFr.y, uvOld.x) + oUp * lerp(oFr.z, oFr.w, uvOld.y) + oFwd;
    float3 v = oPos + rOld * zOld - nPos;
    zNew = dot(v, nFwd);
    if (zNew < 1.0) {  // behind the current camera
        pos = float4(0, 0, -1, 1);
        return;
    }
    float x = dot(v, nRight) / zNew, y = dot(v, nUp) / zNew;
    float2 uvNew = float2((x - nFr.x) / (nFr.y - nFr.x), (nFr.z - y) / (nFr.z - nFr.w));
    pos = float4(uvNew.x * 2 - 1, 1 - uvNew.y * 2, saturate(zNew / 200000.0), 1);
}
float4 ps_mesh(float4 pos : SV_Position, float2 uvOld : TEXCOORD0, float zOld : TEXCOORD1,
               float zNew : TEXCOORD2) : SV_Target {
    if (abs(FrameZ(uvOld) - zOld) > max(30.0, 0.05 * zOld)) discard;  // stretched across an edge
    float4 c = tex.Sample(smp, uvOld);
    float a = c.a;
    a = min(a, tex.Sample(smp, uvOld, int2(1, 0)).a);
    a = min(a, tex.Sample(smp, uvOld, int2(-1, 0)).a);
    a = min(a, tex.Sample(smp, uvOld, int2(0, 1)).a);
    a = min(a, tex.Sample(smp, uvOld, int2(0, -1)).a);
    if (a <= 0) discard;
    if (useSkyDepth > 0) {
        float sky = Linear(skyDepth.Load(int3(pos.xy, 0)), skyNear, skyFar);
        if (zNew > sky * 1.02 + 5) discard;  // something of Skyrim's is in front
    }
    return float4(c.rgb, a);
}
)";

// Pawn labels (see "Pawn labels" below): one quad per call, placed in pixels.
const char kLabelShader[] = R"(
Texture2D label : register(t0);
Texture2D<float> skyDepth : register(t2);
SamplerState smp : register(s0);
cbuffer Label : register(b0) {
    float4 rect;     // left, top, right, bottom in clip space
    float4 uvRect;
    float4 color;    // rgb, alpha (fade)
    float4 mode;     // x: 0 solid, 1 text, 2 dot; y: occlusion test; z: head depth (Skyrim units)
    float4 headPix;  // xy: the head's pixel; z, w: Skyrim near, far
};
float Linear(float d, float n, float f) { return n * f / (f - d * (f - n)); }
void vs_label(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    float2 t = float2(id & 1, id >> 1);
    pos = float4(lerp(rect.x, rect.z, t.x), lerp(rect.y, rect.w, t.y), 0, 1);
    uv = lerp(uvRect.xy, uvRect.zw, t);
}
float4 ps_label(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    if (mode.y > 0) {  // a wall of Skyrim's between the camera and the head hides the label
        float sky = Linear(skyDepth.Load(int3(headPix.xy, 0)), headPix.z, headPix.w);
        if (sky < mode.z * 0.97 - 10) discard;
    }
    if (mode.x == 1) {
        float4 t = label.Sample(smp, uv);
        return float4(t.rgb, t.a * color.a);
    }
    if (mode.x == 2) {  // the party colour dot: a bright core and a soft glow
        float r = length(uv * 2 - 1);
        float core = saturate((0.55 - r) * 8), glow = saturate(1 - r) * 0.55;
        float3 c = lerp(color.rgb, 1.0, core * 0.35);
        return float4(c, saturate(core + glow) * color.a);
    }
    return color;
}
)";

void Log(reshade::log::level level, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    reshade::log::message(level, buf);
}

template <class T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// --- Frame source --------------------------------------------------------------

const frame::Header* g_hdr = nullptr;
const uint8_t* g_view = nullptr;
ULONGLONG g_lastOpenTry = 0;
uint64_t g_lastFrame = 0;
std::vector<uint8_t> g_pixels;  // BGRA, alpha from the mask
std::vector<uint8_t> g_gbuf;    // DDDA G-buffer plane (packed depth)
uint32_t g_frameW = 0, g_frameH = 0;
ULONGLONG g_lastFrameTick = 0;  // when the last masked frame arrived
float g_framePose[16];          // the Skyrim camera the shown frame was rendered for
bool g_frameHasPose = false;

bool OpenMapping() {
    if (g_hdr) return true;
    ULONGLONG now = GetTickCount64();
    if (now - g_lastOpenTry < 1000) return false;
    g_lastOpenTry = now;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, frame::kMappingName);
    if (!mapping) return false;
    auto* view = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
    CloseHandle(mapping);  // the view keeps the mapping alive
    auto* hdr = reinterpret_cast<const frame::Header*>(view);
    if (!view || hdr->magic != frame::kMagic || hdr->version != frame::kVersion ||
        hdr->format != frame::kFormatBGRA8) {
        Log(reshade::log::level::warning, "DDDABridge: frame mapping present but header is wrong");
        if (view) UnmapViewOfFile(view);
        return false;
    }
    g_view = view;
    g_hdr = hdr;
    Log(reshade::log::level::info, "DDDABridge: connected to DDDA frames (writer pid %u)", hdr->writerPid);
    return true;
}

// Copies the newest complete masked frame into g_pixels. Returns false if there is
// no new one or the copy was overtaken by the writer.
bool ReadFrame() {
    uint32_t i = g_hdr->latest % frame::kSlots;
    const frame::Slot& s = g_hdr->slots[i];
    uint32_t seq = s.seq;
    if (seq & 1) return false;
    MemoryBarrier();
    uint64_t id = s.frameId;
    if (id == g_lastFrame) return false;
    uint32_t w = s.width, h = s.height, pitch = s.pitch;
    if (!(s.flags & frame::kSlotHasMask)) {  // DDDA is not rendering for Skyrim (yet)
        g_lastFrame = id;
        return false;
    }
    if (w == 0 || h == 0 || w > frame::kMaxWidth || h > frame::kMaxHeight || pitch < w * 4) return false;
    g_pixels.resize(static_cast<size_t>(w) * h * 4);
    g_gbuf.resize(static_cast<size_t>(w) * h * 4);
    const uint8_t* color = g_view + frame::SlotOffset(i);
    const uint8_t* mask = g_view + frame::MaskOffset(i);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* c = color + static_cast<size_t>(y) * pitch;
        const uint8_t* m = mask + static_cast<size_t>(y) * pitch;
        uint8_t* out = g_pixels.data() + static_cast<size_t>(y) * w * 4;
        memcpy(out, c, static_cast<size_t>(w) * 4);
        memcpy(g_gbuf.data() + static_cast<size_t>(y) * w * 4, m, static_cast<size_t>(w) * 4);
        for (uint32_t x = 0; x < w; ++x, m += 4)
            out[x * 4 + 3] = m[0] != 0xFF || m[1] != 0xFF || m[2] != 0xFF ? 0xFF : 0x00;
    }
    float pose[16];
    bool hasPose = (s.flags & frame::kSlotHasPose) != 0;
    if (hasPose) memcpy(pose, s.skyPose, sizeof(pose));
    MemoryBarrier();
    if (s.seq != seq) return false;  // overtaken while copying
    g_frameHasPose = hasPose;
    if (hasPose) memcpy(g_framePose, pose, sizeof(pose));
    g_lastFrame = id;
    g_frameW = w;
    g_frameH = h;
    g_lastFrameTick = GetTickCount64();
    return true;
}

// --- D3D11 drawing ---------------------------------------------------------------

ID3D11Device* g_dev = nullptr;
ID3D11Texture2D* g_tex = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
ID3D11Texture2D* g_gbufTex = nullptr;
ID3D11ShaderResourceView* g_gbufSrv = nullptr;
uint32_t g_texW = 0, g_texH = 0;
ID3D11Buffer* g_params = nullptr;
// Copy of Skyrim's depth buffer, taken at present.
ID3D11Texture2D* g_depthCopy = nullptr;
ID3D11ShaderResourceView* g_depthSrv = nullptr;
D3D11_TEXTURE2D_DESC g_depthDesc = {};
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11VertexShader* g_vsMesh = nullptr;  // reprojection mesh
ID3D11PixelShader* g_psMesh = nullptr;
ID3D11DepthStencilState* g_depthLess = nullptr;
ID3D11Texture2D* g_meshDepth = nullptr;  // our own depth buffer, back-buffer sized
ID3D11DepthStencilView* g_meshDsv = nullptr;
UINT g_meshDepthW = 0, g_meshDepthH = 0;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11DepthStencilState* g_noDepth = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
std::unordered_map<ID3D11Resource*, ID3D11RenderTargetView*> g_rtvs;  // per back buffer
bool g_initFailed = false;
uint64_t g_shown = 0;
bool g_warpLogged = false;

// Diagnostic switches, read once a second from DDDABridge_debug.txt next to
// SkyrimSE.exe (any of the words, one per line): "nowarp" disables reprojection,
// "nodepth" disables the depth test against Skyrim's scene, "oldwarp" uses the old
// per-pixel reprojection instead of the mesh.
bool g_debugNoWarp = false, g_debugNoDepth = false, g_debugOldWarp = false;

void PollDebugSwitches() {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = n && n < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"DDDABridge_debug.txt");
    bool noWarp = false, noDepth = false, oldWarp = false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") == 0 && f) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "nowarp", 6) == 0) noWarp = true;
            if (strncmp(line, "nodepth", 7) == 0) noDepth = true;
            if (strncmp(line, "oldwarp", 7) == 0) oldWarp = true;
        }
        fclose(f);
    }
    if (noWarp != g_debugNoWarp || noDepth != g_debugNoDepth || oldWarp != g_debugOldWarp)
        Log(reshade::log::level::info, "DDDABridge: debug switches: reprojection %s, depth test %s",
            noWarp ? "OFF" : oldWarp ? "on (old per-pixel search)" : "on (mesh)", noDepth ? "OFF" : "on");
    g_debugNoWarp = noWarp;
    g_debugNoDepth = noDepth;
    g_debugOldWarp = oldWarp;
}

void ReleaseDepthCopy() {
    SafeRelease(g_depthSrv);
    SafeRelease(g_depthCopy);
    g_depthDesc = {};
}

void ReleaseLabels();

void ReleaseAll() {
    ReleaseLabels();
    SafeRelease(g_srv);
    SafeRelease(g_tex);
    SafeRelease(g_gbufSrv);
    SafeRelease(g_gbufTex);
    SafeRelease(g_params);
    ReleaseDepthCopy();
    SafeRelease(g_vs);
    SafeRelease(g_ps);
    SafeRelease(g_vsMesh);
    SafeRelease(g_psMesh);
    SafeRelease(g_depthLess);
    SafeRelease(g_meshDsv);
    SafeRelease(g_meshDepth);
    g_meshDepthW = g_meshDepthH = 0;
    SafeRelease(g_sampler);
    SafeRelease(g_blend);
    SafeRelease(g_noDepth);
    SafeRelease(g_raster);
    for (auto& kv : g_rtvs) kv.second->Release();
    g_rtvs.clear();
    g_texW = g_texH = 0;
    g_dev = nullptr;
}

bool Compile(const char* entry, const char* target, ID3DBlob** out, const char* src = kShader,
             size_t len = sizeof(kShader) - 1) {
    using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                          LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    static D3DCompileFn compile = [] {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        return m ? reinterpret_cast<D3DCompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
    }();
    if (!compile) return false;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(src, len, "ddda_bridge", nullptr, nullptr, entry, target,
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
    if (errors) {
        Log(reshade::log::level::error, "DDDABridge: shader %s: %s", entry,
            static_cast<const char*>(errors->GetBufferPointer()));
        errors->Release();
    }
    return SUCCEEDED(hr);
}

bool InitD3D(ID3D11Device* dev) {
    if (g_dev == dev) return true;
    if (g_initFailed) return false;
    ReleaseAll();
    ID3DBlob *vs = nullptr, *ps = nullptr, *vsm = nullptr, *psm = nullptr;
    bool ok = Compile("vs_main", "vs_5_0", &vs) && Compile("ps_main", "ps_5_0", &ps) &&
              Compile("vs_mesh", "vs_5_0", &vsm) && Compile("ps_mesh", "ps_5_0", &psm) &&
              SUCCEEDED(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
              SUCCEEDED(dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps)) &&
              SUCCEEDED(dev->CreateVertexShader(vsm->GetBufferPointer(), vsm->GetBufferSize(), nullptr, &g_vsMesh)) &&
              SUCCEEDED(dev->CreatePixelShader(psm->GetBufferPointer(), psm->GetBufferSize(), nullptr, &g_psMesh));
    SafeRelease(vs);
    SafeRelease(ps);
    SafeRelease(vsm);
    SafeRelease(psm);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC dd = {};
    D3D11_DEPTH_STENCIL_DESC dl = {};
    dl.DepthEnable = TRUE;
    dl.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dl.DepthFunc = D3D11_COMPARISON_LESS;
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = 192;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&cb, nullptr, &g_params)) &&
         SUCCEEDED(dev->CreateSamplerState(&sd, &g_sampler)) && SUCCEEDED(dev->CreateBlendState(&bd, &g_blend)) &&
         SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_noDepth)) &&
         SUCCEEDED(dev->CreateDepthStencilState(&dl, &g_depthLess)) &&
         SUCCEEDED(dev->CreateRasterizerState(&rd, &g_raster));
    if (!ok) {
        Log(reshade::log::level::error, "DDDABridge: D3D11 setup failed; drawing disabled");
        ReleaseAll();
        g_initFailed = true;
        return false;
    }
    g_dev = dev;
    return true;
}

bool UploadFrame(ID3D11DeviceContext* ctx) {
    if (g_frameW != g_texW || g_frameH != g_texH) {
        SafeRelease(g_srv);
        SafeRelease(g_tex);
        SafeRelease(g_gbufSrv);
        SafeRelease(g_gbufTex);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = g_frameW;
        td.Height = g_frameH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // DDDA's byte order
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_tex)) ||
            FAILED(g_dev->CreateShaderResourceView(g_tex, nullptr, &g_srv)) ||
            FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_gbufTex)) ||
            FAILED(g_dev->CreateShaderResourceView(g_gbufTex, nullptr, &g_gbufSrv))) {
            SafeRelease(g_srv);
            SafeRelease(g_tex);
            SafeRelease(g_gbufTex);
            g_texW = g_texH = 0;
            return false;
        }
        g_texW = g_frameW;
        g_texH = g_frameH;
        Log(reshade::log::level::info, "DDDABridge: DD frame texture %ux%u", g_texW, g_texH);
    }
    for (auto [tex, src] : {std::pair{g_tex, &g_pixels}, std::pair{g_gbufTex, &g_gbuf}}) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        for (uint32_t y = 0; y < g_texH; ++y)
            memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                   src->data() + static_cast<size_t>(y) * g_texW * 4, static_cast<size_t>(g_texW) * 4);
        ctx->Unmap(tex, 0);
    }
    return true;
}

// --- Skyrim's depth buffer --------------------------------------------------------

// Draws per depth-stencil view this frame; the scene's main depth buffer has the most.
uint64_t g_curDsv = 0;
std::unordered_map<uint64_t, uint32_t> g_dsvDraws;
bool g_depthWarned = false;

// Back-buffer draws this frame (see "Under the UI" above).
std::vector<uint64_t> g_backBuffers;  // the swap chain's back buffer resources
uint64_t g_curBackBuffer = 0;         // bound render target 0 if it is a back buffer
uint32_t g_bbDraws = 0;
bool g_drawnThisFrame = false;
uint64_t g_framesUnderUi = 0, g_framesAtPresent = 0;
ULONGLONG g_lastUiReport = 0;
uint32_t g_bbSizes[12];  // vertex/index counts of this frame's first back-buffer draws (for the log)

bool CompositeParty(command_list* cmd, resource backBuffer);

void OnBindTargets(command_list* cmd, uint32_t count, const resource_view* rtvs, resource_view dsv) {
    g_curDsv = dsv.handle;
    g_curBackBuffer = 0;
    if (count && rtvs[0].handle && !g_backBuffers.empty()) {
        uint64_t res = cmd->get_device()->get_resource_from_view(rtvs[0]).handle;
        for (uint64_t bb : g_backBuffers)
            if (bb == res) g_curBackBuffer = res;
    }
}

void CountDraw(command_list* cmd, uint32_t vertices) {
    if (g_curDsv) ++g_dsvDraws[g_curDsv];
    if (!g_curBackBuffer) return;
    if (g_bbDraws < 12) g_bbSizes[g_bbDraws] = vertices;
    if (++g_bbDraws == 2 && !g_drawnThisFrame) {  // the interface starts now
        g_drawnThisFrame = true;
        if (CompositeParty(cmd, resource{g_curBackBuffer})) ++g_framesUnderUi;
    }
}

bool OnDraw(command_list* cmd, uint32_t vertices, uint32_t, uint32_t, uint32_t) {
    CountDraw(cmd, vertices);
    return false;
}

bool OnDrawIndexed(command_list* cmd, uint32_t indices, uint32_t, uint32_t, int32_t, uint32_t) {
    CountDraw(cmd, indices);
    return false;
}

// A view counted this frame may be destroyed before present; never touch it then.
void OnDestroyView(device*, resource_view view) {
    g_dsvDraws.erase(view.handle);
    if (g_curDsv == view.handle) g_curDsv = 0;
}

DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT DepthTypeless(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
    default: return f;
    }
}

// The depth texture behind a depth-stencil view, if it is single-sampled, screen
// sized and of a depth format we can read. Caller releases.
ID3D11Texture2D* SceneDepthTexture(uint64_t view, uint32_t bw, uint32_t bh, D3D11_TEXTURE2D_DESC* d) {
    ID3D11Resource* res = nullptr;
    reinterpret_cast<ID3D11DepthStencilView*>(view)->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
    SafeRelease(res);
    if (!tex) return nullptr;
    tex->GetDesc(d);
    if (d->Width == bw && d->Height == bh && d->SampleDesc.Count == 1 && d->ArraySize == 1 &&
        DepthSrvFormat(d->Format) != DXGI_FORMAT_UNKNOWN)
        return tex;
    tex->Release();
    return nullptr;
}

// Copies the busiest screen-size depth buffer of this frame into g_depthCopy.
// Smaller ones (shadow maps, menu renders) can be busier in some frames.
bool CaptureSkyrimDepth(ID3D11DeviceContext* ctx, uint32_t bw, uint32_t bh) {
    ID3D11Texture2D* tex = nullptr;
    D3D11_TEXTURE2D_DESC d = {};
    uint32_t bestDraws = 0;
    for (auto& kv : g_dsvDraws) {
        if (kv.second <= bestDraws) continue;
        D3D11_TEXTURE2D_DESC cd;
        if (ID3D11Texture2D* t = SceneDepthTexture(kv.first, bw, bh, &cd)) {
            SafeRelease(tex);
            tex = t;
            d = cd;
            bestDraws = kv.second;
        }
    }
    g_dsvDraws.clear();
    if (!tex) {
        if (!g_depthWarned) Log(reshade::log::level::warning, "DDDABridge: no screen-size Skyrim depth buffer this frame");
        g_depthWarned = true;
        return false;
    }
    bool ok = true;
    if (d.Width != g_depthDesc.Width || d.Height != g_depthDesc.Height || d.Format != g_depthDesc.Format) {
        ReleaseDepthCopy();
        D3D11_TEXTURE2D_DESC cd = d;
        cd.Format = DepthTypeless(d.Format);
        cd.MipLevels = 1;
        cd.Usage = D3D11_USAGE_DEFAULT;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        cd.CPUAccessFlags = 0;
        cd.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DepthSrvFormat(d.Format);
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ok = SUCCEEDED(g_dev->CreateTexture2D(&cd, nullptr, &g_depthCopy)) &&
             SUCCEEDED(g_dev->CreateShaderResourceView(g_depthCopy, &sd, &g_depthSrv));
        if (ok) {
            g_depthDesc = d;
            Log(reshade::log::level::info, "DDDABridge: Skyrim depth buffer %ux%u format %u (%u draws)", d.Width,
                d.Height, d.Format, bestDraws);
        } else {
            ReleaseDepthCopy();
        }
    }
    if (ok) ctx->CopyResource(g_depthCopy, tex);
    tex->Release();
    return ok;
}

bool ReadPtr(uintptr_t addr, uintptr_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uintptr_t*>(addr);
        return *out >= 0x10000;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadNearFar(uintptr_t nicam, float* nearFar) {
    __try {
        nearFar[0] = *reinterpret_cast<const volatile float*>(nicam + 0x160);
        nearFar[1] = *reinterpret_cast<const volatile float*>(nicam + 0x164);
        return nearFar[0] > 0.0f && nearFar[1] > nearFar[0];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Skyrim camera near/far planes; falls back to the values seen in game.
void SkyrimNearFar(float* nearFar) {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), cam, root, children, nicam;
    if (ReadPtr(base + kPlayerCameraPtr, &cam) && ReadPtr(cam + 0x20, &root) && ReadPtr(root + 0x118, &children) &&
        ReadPtr(children, &nicam) && ReadNearFar(nicam, nearFar))
        return;
    nearFar[0] = 15.0f;
    nearFar[1] = 353840.0f;
}

bool ReadFloats(uintptr_t addr, float* out, int n) {
    __try {
        for (int i = 0; i < n; ++i) out[i] = reinterpret_cast<const volatile float*>(addr)[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Current Skyrim camera in the SkyPose layout (same reads as the SKSE plugin):
// camera root NiNode world translate (+0xA0) and rotate (+0x7C), NiCamera frustum (+0x150).
bool SkyrimPose(float* pose) {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), cam, root, children, nicam;
    if (!ReadPtr(base + kPlayerCameraPtr, &cam) || !ReadPtr(cam + 0x20, &root) || !ReadPtr(root + 0x118, &children) ||
        !ReadPtr(children, &nicam) || !ReadFloats(root + 0xA0, pose, 3) || !ReadFloats(root + 0x7C, pose + 3, 9) ||
        !ReadFloats(nicam + 0x150, pose + 12, 4))
        return false;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(pose[i])) return false;
    return pose[14] > 0.01f && pose[14] < 10.0f;
}

// Constant-buffer camera block: pos, right, up, forward as float4 rows.
void PutCamera(float* cb, const float* pose) {
    const float* r = pose + 3;
    const float rows[4][3] = {{pose[0], pose[1], pose[2]}, {r[0], r[3], r[6]}, {r[2], r[5], r[8]}, {r[1], r[4], r[7]}};
    for (int k = 0; k < 4; ++k) {
        cb[k * 4 + 0] = rows[k][0];
        cb[k * 4 + 1] = rows[k][1];
        cb[k * 4 + 2] = rows[k][2];
        cb[k * 4 + 3] = 0.0f;
    }
}

ID3D11RenderTargetView* BackBufferRtv(ID3D11Resource* bb) {
    auto it = g_rtvs.find(bb);
    if (it != g_rtvs.end()) return it->second;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(g_dev->CreateRenderTargetView(bb, nullptr, &rtv))) return nullptr;
    g_rtvs[bb] = rtv;
    return rtv;
}

bool EnsureMeshDepth(UINT w, UINT h) {
    if (g_meshDsv && g_meshDepthW == w && g_meshDepthH == h) return true;
    SafeRelease(g_meshDsv);
    SafeRelease(g_meshDepth);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_meshDepth)) ||
        FAILED(g_dev->CreateDepthStencilView(g_meshDepth, nullptr, &g_meshDsv))) {
        SafeRelease(g_meshDepth);
        return false;
    }
    g_meshDepthW = w;
    g_meshDepthH = h;
    return true;
}

// mesh: draw the reprojection mesh (needs the warp parameters) instead of the
// full-screen pass.
void Draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, bool mesh) {
    // Save what we change.
    ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
    ID3D11BlendState* oldBlend = nullptr;
    FLOAT oldFactor[4];
    UINT oldMask;
    ctx->OMGetBlendState(&oldBlend, oldFactor, &oldMask);
    ID3D11DepthStencilState* oldDs = nullptr;
    UINT oldRef;
    ctx->OMGetDepthStencilState(&oldDs, &oldRef);
    ID3D11RasterizerState* oldRs = nullptr;
    ctx->RSGetState(&oldRs);
    UINT oldVpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT oldVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ctx->RSGetViewports(&oldVpCount, oldVp);
    D3D11_PRIMITIVE_TOPOLOGY oldTopo;
    ctx->IAGetPrimitiveTopology(&oldTopo);
    ID3D11InputLayout* oldLayout = nullptr;
    ctx->IAGetInputLayout(&oldLayout);
    ID3D11VertexShader* oldVs = nullptr;
    ctx->VSGetShader(&oldVs, nullptr, nullptr);
    ID3D11PixelShader* oldPs = nullptr;
    ctx->PSGetShader(&oldPs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSrv[3] = {};
    ctx->PSGetShaderResources(0, 3, oldSrv);
    ID3D11Buffer* oldCb = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &oldCb);
    ID3D11SamplerState* oldSampler = nullptr;
    ctx->PSGetSamplers(0, 1, &oldSampler);

    mesh = mesh && EnsureMeshDepth(static_cast<UINT>(vp.Width), static_cast<UINT>(vp.Height));
    if (mesh) {
        ctx->ClearDepthStencilView(g_meshDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        ctx->OMSetRenderTargets(1, &rtv, g_meshDsv);
        ctx->OMSetDepthStencilState(g_depthLess, 0);
    } else {
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->OMSetDepthStencilState(g_noDepth, 0);
    }
    ctx->OMSetBlendState(g_blend, nullptr, 0xFFFFFFFF);
    ctx->RSSetState(g_raster);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(mesh ? g_vsMesh : g_vs, nullptr, 0);
    ctx->PSSetShader(mesh ? g_psMesh : g_ps, nullptr, 0);
    ID3D11ShaderResourceView* srvs[3] = {g_srv, g_gbufSrv, g_depthSrv};
    ctx->PSSetShaderResources(0, 3, srvs);
    ctx->PSSetConstantBuffers(0, 1, &g_params);
    ctx->PSSetSamplers(0, 1, &g_sampler);
    ID3D11ShaderResourceView* oldVsSrv[2] = {};
    ID3D11Buffer* oldVsCb = nullptr;
    if (mesh) {  // the mesh's vertex shader reads the frame and its depth
        ctx->VSGetShaderResources(0, 2, oldVsSrv);
        ctx->VSGetConstantBuffers(0, 1, &oldVsCb);
        ctx->VSSetShaderResources(0, 2, srvs);
        ctx->VSSetConstantBuffers(0, 1, &g_params);
        UINT cellsX = (g_texW - 1) / 2, cellsY = (g_texH - 1) / 2;
        ctx->Draw(cellsX * cellsY * 6, 0);
        ctx->VSSetShaderResources(0, 2, oldVsSrv);
        ctx->VSSetConstantBuffers(0, 1, &oldVsCb);
        for (auto*& v : oldVsSrv) SafeRelease(v);
        SafeRelease(oldVsCb);
    } else {
        ctx->Draw(3, 0);
    }

    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
    ctx->OMSetBlendState(oldBlend, oldFactor, oldMask);
    ctx->OMSetDepthStencilState(oldDs, oldRef);
    ctx->RSSetState(oldRs);
    ctx->RSSetViewports(oldVpCount, oldVp);
    ctx->IASetPrimitiveTopology(oldTopo);
    ctx->IASetInputLayout(oldLayout);
    ctx->VSSetShader(oldVs, nullptr, 0);
    ctx->PSSetShader(oldPs, nullptr, 0);
    ctx->PSSetShaderResources(0, 3, oldSrv);
    ctx->PSSetConstantBuffers(0, 1, &oldCb);
    ctx->PSSetSamplers(0, 1, &oldSampler);
    for (auto*& r : oldRtv) SafeRelease(r);
    SafeRelease(oldDsv);
    SafeRelease(oldBlend);
    SafeRelease(oldDs);
    SafeRelease(oldRs);
    SafeRelease(oldLayout);
    SafeRelease(oldVs);
    SafeRelease(oldPs);
    for (auto*& v : oldSrv) SafeRelease(v);
    SafeRelease(oldCb);
    SafeRelease(oldSampler);
}

// --- Pawn labels -------------------------------------------------------------------
// DDDA shows each pawn's name above its head, with a thin health bar under it (green:
// current health, grey: the rest up to its current maximum) and a dot in the pawn's
// party colour (main pawn red, first hired yellow, second hired blue). The DDDA HUD is
// filtered out of the frames (isolate), so the labels are drawn here, at Skyrim's
// resolution, from the bridge State (positions, health) and Names (DDDA bridge).
// Screen sizes follow DDDA's at 1080p and scale with the back buffer's height.
constexpr float kLabelHeadDd = 215.0f;      // DD cm above the feet
constexpr float kLabelFadeStart = 1750.0f;  // Skyrim units (25 m): labels fade out...
constexpr float kLabelFadeEnd = 2450.0f;    // ...until 35 m
constexpr float kLabelFontPx = 30.0f;       // at 1080 lines
constexpr float kLabelBarW = 230.0f, kLabelBarH = 6.0f, kLabelDot = 16.0f;
const float kLabelDotColor[bridge::kRoleCount][3] = {
    {1, 1, 1}, {0.86f, 0.16f, 0.16f}, {0.92f, 0.78f, 0.15f}, {0.22f, 0.32f, 0.95f}};

ID3D11VertexShader* g_vsLabel = nullptr;
ID3D11PixelShader* g_psLabel = nullptr;
ID3D11Buffer* g_labelCb = nullptr;

struct NameTex {
    std::string name;
    int px = 0;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0, pad = 0;
};
NameTex g_nameTex[bridge::kRoleCount];

void ReleaseLabels() {
    SafeRelease(g_vsLabel);
    SafeRelease(g_psLabel);
    SafeRelease(g_labelCb);
    for (NameTex& t : g_nameTex) {
        SafeRelease(t.srv);
        SafeRelease(t.tex);
        t = NameTex{};
    }
}

bool InitLabels() {
    if (g_vsLabel) return true;
    ID3DBlob *vs = nullptr, *ps = nullptr;
    bool ok = Compile("vs_label", "vs_5_0", &vs, kLabelShader, sizeof(kLabelShader) - 1) &&
              Compile("ps_label", "ps_5_0", &ps, kLabelShader, sizeof(kLabelShader) - 1) &&
              SUCCEEDED(g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vsLabel)) &&
              SUCCEEDED(g_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_psLabel));
    SafeRelease(vs);
    SafeRelease(ps);
    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = 80;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(g_dev->CreateBuffer(&cb, nullptr, &g_labelCb));
    if (!ok) {
        Log(reshade::log::level::error, "DDDABridge: label setup failed; no pawn labels");
        ReleaseLabels();
    }
    return ok;
}

// The name rendered by GDI (a serif like DDDA's, grey-scale antialiasing) into a texture:
// warm white text over a soft dark shadow.
bool MakeNameTex(NameTex& t, const std::string& name, int px) {
    SafeRelease(t.srv);
    SafeRelease(t.tex);
    t.name = name;
    t.px = px;
    wchar_t wide[128] = {};
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wide, 127);
    const int len = static_cast<int>(wcslen(wide));
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Palatino Linotype");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SIZE size = {};
    GetTextExtentPoint32W(dc, wide, len, &size);
    t.pad = px / 6 + 2;
    t.w = size.cx + 2 * t.pad;
    t.h = size.cy + 2 * t.pad;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = t.w;
    bi.bmiHeader.biHeight = -t.h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = bmp && bits && size.cx > 0;
    if (ok) {
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        memset(bits, 0, static_cast<size_t>(t.w) * t.h * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, t.pad, t.pad, wide, len);
        GdiFlush();
        const auto* src = static_cast<const uint8_t*>(bits);
        std::vector<float> cov(static_cast<size_t>(t.w) * t.h);
        for (size_t i = 0; i < cov.size(); ++i) cov[i] = src[i * 4 + 1] / 255.0f;
        // Shadow: the coverage spread by ~px/12 and moved down-right a little.
        const int r = (std::max)(1, px / 12), off = (std::max)(1, px / 20);
        std::vector<uint8_t> rgba(cov.size() * 4);
        for (int y = 0; y < t.h; ++y) {
            for (int x = 0; x < t.w; ++x) {
                float sh = 0;
                for (int dy = -r; dy <= r; ++dy) {
                    for (int dx = -r; dx <= r; ++dx) {
                        const int sx = x - off + dx, sy = y - off + dy;
                        if (sx < 0 || sy < 0 || sx >= t.w || sy >= t.h) continue;
                        const float fall = 1.0f - 0.5f * static_cast<float>(dx * dx + dy * dy) / (r * r + 1);
                        sh = (std::max)(sh, cov[static_cast<size_t>(sy) * t.w + sx] * fall);
                    }
                }
                const float c = cov[static_cast<size_t>(y) * t.w + x];
                const float a = (std::max)(c, sh * 0.75f);
                const float k = a > 0 ? c / a : 0;  // text over its shadow
                uint8_t* o = &rgba[(static_cast<size_t>(y) * t.w + x) * 4];
                o[0] = static_cast<uint8_t>(225 * k);  // B, G, R: warm white
                o[1] = static_cast<uint8_t>(238 * k);
                o[2] = static_cast<uint8_t>(245 * k);
                o[3] = static_cast<uint8_t>(255 * a);
            }
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = t.w;
        td.Height = t.h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {rgba.data(), static_cast<UINT>(t.w * 4), 0};
        ok = SUCCEEDED(g_dev->CreateTexture2D(&td, &init, &t.tex)) &&
             SUCCEEDED(g_dev->CreateShaderResourceView(t.tex, nullptr, &t.srv));
        SelectObject(dc, oldBmp);
    }
    SelectObject(dc, oldFont);
    if (bmp) DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(dc);
    return ok;
}

template <class T>
bool ReadSeqlocked(const T* view, T* out) {
    for (int i = 0; i < 4; ++i) {
        const uint32_t s0 = view->seq;
        if (s0 & 1) continue;
        memcpy(out, const_cast<const T*>(view), sizeof(T));
        if (view->seq == s0) return true;
    }
    return false;
}

template <class T>
const T* OpenView(const wchar_t* name, const T*& view, ULONGLONG& lastTry) {
    if (view) return view;
    const ULONGLONG now = GetTickCount64();
    if (now - lastTry < 1000) return nullptr;
    lastTry = now;
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!h) return nullptr;
    view = static_cast<const T*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(T)));
    CloseHandle(h);
    return view;
}

struct LabelCb {
    float rect[4], uv[4], color[4], mode[4], head[4];
};

// occ: occlusion test on/off, head depth (Skyrim units), head pixel x, y, Skyrim near, far.
void LabelQuad(ID3D11DeviceContext* ctx, const D3D11_VIEWPORT& vp, float x0, float y0, float x1, float y1,
               const float* color, float alpha, float mode, ID3D11ShaderResourceView* srv, const float* occ) {
    LabelCb c = {{x0 / vp.Width * 2 - 1, 1 - y0 / vp.Height * 2, x1 / vp.Width * 2 - 1, 1 - y1 / vp.Height * 2},
                 {0, 0, 1, 1},
                 {color[0], color[1], color[2], alpha},
                 {mode, occ[0], occ[1], 0},
                 {occ[2], occ[3], occ[4], occ[5]}};
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_labelCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, &c, sizeof(c));
    ctx->Unmap(g_labelCb, 0);
    ID3D11ShaderResourceView* srvs[3] = {srv, nullptr, g_depthSrv};
    ctx->PSSetShaderResources(0, 3, srvs);
    ctx->Draw(4, 0);
}

const bridge::State* g_stateView = nullptr;
const bridge::CameraCmd* g_camView = nullptr;
const bridge::Names* g_namesView = nullptr;
ULONGLONG g_stateTry = 0, g_camTry = 0, g_namesTry = 0;
bool g_labelsLogged = false;

// Draws the labels onto rtv. `now` is Skyrim's current camera (SkyPose layout). The
// pawns' DD positions reach Skyrim through the last camera command: its DD camera
// position is the Skyrim camera it was made from (skyPose), and DD (x, y, z) is Skyrim
// (x, -z, y) times kDdToSkyrim.
void DrawLabels(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, const float* now,
                bool depth, const float* nearFar) {
    bridge::State st;
    bridge::CameraCmd cmd;
    if (!OpenView(bridge::kMappingName, g_stateView, g_stateTry) ||
        !OpenView(bridge::kCamMappingName, g_camView, g_camTry) || !ReadSeqlocked(g_stateView, &st) ||
        !ReadSeqlocked(g_camView, &cmd) || st.magic != bridge::kMagic || !st.tile ||
        !(cmd.flags & bridge::kCamOverride) || !(cmd.flags & bridge::kGlobalCoords) || !InitLabels())
        return;
    bridge::Names names = {};
    if (OpenView(bridge::kNamesMappingName, g_namesView, g_namesTry)) ReadSeqlocked(g_namesView, &names);
    const float scale = vp.Height / 1080.0f;
    const int px = static_cast<int>(std::lround(kLabelFontPx * scale));
    const float tileX = (static_cast<float>(st.tile & 0xFFFF) - 50.0f) * 10000.0f;
    const float tileZ = (static_cast<float>(st.tile >> 16) - 50.0f) * 10000.0f;
    const float* r = now + 3;
    const float right[3] = {r[0], r[3], r[6]}, fwd[3] = {r[1], r[4], r[7]}, up[3] = {r[2], r[5], r[8]};

    // Save what we change (as Draw does).
    ID3D11RenderTargetView* oldRtv = nullptr;
    ID3D11DepthStencilView* oldDsv = nullptr;
    ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    ID3D11BlendState* oldBlend = nullptr;
    FLOAT oldFactor[4];
    UINT oldMask;
    ctx->OMGetBlendState(&oldBlend, oldFactor, &oldMask);
    ID3D11DepthStencilState* oldDs = nullptr;
    UINT oldRef;
    ctx->OMGetDepthStencilState(&oldDs, &oldRef);
    ID3D11RasterizerState* oldRs = nullptr;
    ctx->RSGetState(&oldRs);
    UINT oldVpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT oldVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ctx->RSGetViewports(&oldVpCount, oldVp);
    D3D11_PRIMITIVE_TOPOLOGY oldTopo;
    ctx->IAGetPrimitiveTopology(&oldTopo);
    ID3D11InputLayout* oldLayout = nullptr;
    ctx->IAGetInputLayout(&oldLayout);
    ID3D11VertexShader* oldVs = nullptr;
    ctx->VSGetShader(&oldVs, nullptr, nullptr);
    ID3D11PixelShader* oldPs = nullptr;
    ctx->PSGetShader(&oldPs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSrv[3] = {};
    ctx->PSGetShaderResources(0, 3, oldSrv);
    ID3D11Buffer *oldCb = nullptr, *oldVsCb = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &oldCb);
    ctx->VSGetConstantBuffers(0, 1, &oldVsCb);
    ID3D11SamplerState* oldSampler = nullptr;
    ctx->PSGetSamplers(0, 1, &oldSampler);

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetDepthStencilState(g_noDepth, 0);
    ctx->OMSetBlendState(g_blend, nullptr, 0xFFFFFFFF);
    ctx->RSSetState(g_raster);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(g_vsLabel, nullptr, 0);
    ctx->PSSetShader(g_psLabel, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_labelCb);
    ctx->PSSetConstantBuffers(0, 1, &g_labelCb);
    ctx->PSSetSamplers(0, 1, &g_sampler);

    int drawn = 0;
    for (int role = bridge::kMainPawn; role < static_cast<int>(bridge::kRoleCount); ++role) {
        const bridge::Actor& a = st.actors[role];
        if (!(a.flags & bridge::kActorPresent)) continue;
        const float d[3] = {a.pos[0] + tileX - cmd.pos[0], a.pos[1] + kLabelHeadDd - cmd.pos[1],
                            a.pos[2] + tileZ - cmd.pos[2]};
        const float p[3] = {cmd.skyPose[0] + d[0] * kDdToSkyrim, cmd.skyPose[1] - d[2] * kDdToSkyrim,
                            cmd.skyPose[2] + d[1] * kDdToSkyrim};
        const float v[3] = {p[0] - now[0], p[1] - now[1], p[2] - now[2]};
        const float z = v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2];
        if (z < 30.0f || z > kLabelFadeEnd) continue;
        const float x = (v[0] * right[0] + v[1] * right[1] + v[2] * right[2]) / z;
        const float y = (v[0] * up[0] + v[1] * up[1] + v[2] * up[2]) / z;
        const float sx = (x - now[12]) / (now[13] - now[12]) * vp.Width;
        const float sy = (now[14] - y) / (now[14] - now[15]) * vp.Height;
        if (sx < -vp.Width * 0.2f || sx > vp.Width * 1.2f || sy < -vp.Height * 0.2f || sy > vp.Height * 1.2f) continue;
        const float alpha =
            z <= kLabelFadeStart ? 1.0f : 1.0f - (z - kLabelFadeStart) / (kLabelFadeEnd - kLabelFadeStart);
        // Hidden behind Skyrim's walls: Skyrim's depth at the point above the head (the
        // pawn itself is DDDA's, not in that buffer).
        const float occ[6] = {depth ? 1.0f : 0.0f, z, (std::max)(0.0f, (std::min)(vp.Width - 1, sx)),
                              (std::max)(0.0f, (std::min)(vp.Height - 1, sy)), nearFar[0], nearFar[1]};

        const float barW = kLabelBarW * scale, barH = (std::max)(2.0f, kLabelBarH * scale);
        const float dot = kLabelDot * scale, gap = 6 * scale;
        const float left = sx - (dot + gap + barW) / 2;
        const float barX = left + dot + gap, barY = sy - barH;
        // Bar: green for current health, grey for the rest up to the maximum.
        const float hp = (a.flags & bridge::kActorHpValid) && a.hpMax > 0
                             ? (std::max)(0.0f, (std::min)(1.0f, a.hp / a.hpMax))
                             : 1.0f;
        const float green[3] = {0.66f, 0.84f, 0.18f}, grey[3] = {0.55f, 0.55f, 0.55f}, shade[3] = {0, 0, 0};
        LabelQuad(ctx, vp, barX - 1, barY - 1, barX + barW + 1, barY + barH + 1, shade, alpha * 0.45f, 0, nullptr, occ);
        LabelQuad(ctx, vp, barX + barW * hp, barY, barX + barW, barY + barH, grey, alpha * 0.85f, 0, nullptr, occ);
        if (hp > 0) LabelQuad(ctx, vp, barX, barY, barX + barW * hp, barY + barH, green, alpha, 0, nullptr, occ);
        const float cy = barY + barH / 2;
        LabelQuad(ctx, vp, left, cy - dot / 2, left + dot, cy + dot / 2, kLabelDotColor[role], alpha, 2, nullptr, occ);
        // Name, left-aligned with the bar, just above it.
        names.name[role][bridge::kNameBytes - 1] = 0;
        const std::string name = names.magic == bridge::kNamesMagic ? names.name[role] : "";
        NameTex& t = g_nameTex[role];
        if (!name.empty() && (t.name != name || t.px != px || !t.srv)) MakeNameTex(t, name, px);
        if (!name.empty() && t.srv) {
            const float nx = barX - t.pad, ny = barY - t.h + t.pad * 0.6f;
            const float white[3] = {1, 1, 1};
            LabelQuad(ctx, vp, nx, ny, nx + t.w, ny + t.h, white, alpha, 1, t.srv, occ);
        }
        ++drawn;
    }
    if (drawn && !g_labelsLogged) {
        g_labelsLogged = true;
        Log(reshade::log::level::info, "DDDABridge: pawn labels shown (%d)", drawn);
    }

    ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);
    ctx->OMSetBlendState(oldBlend, oldFactor, oldMask);
    ctx->OMSetDepthStencilState(oldDs, oldRef);
    ctx->RSSetState(oldRs);
    ctx->RSSetViewports(oldVpCount, oldVp);
    ctx->IASetPrimitiveTopology(oldTopo);
    ctx->IASetInputLayout(oldLayout);
    ctx->VSSetShader(oldVs, nullptr, 0);
    ctx->PSSetShader(oldPs, nullptr, 0);
    ctx->PSSetShaderResources(0, 3, oldSrv);
    ctx->PSSetConstantBuffers(0, 1, &oldCb);
    ctx->VSSetConstantBuffers(0, 1, &oldVsCb);
    ctx->PSSetSamplers(0, 1, &oldSampler);
    SafeRelease(oldRtv);
    SafeRelease(oldDsv);
    SafeRelease(oldBlend);
    SafeRelease(oldDs);
    SafeRelease(oldRs);
    SafeRelease(oldLayout);
    SafeRelease(oldVs);
    SafeRelease(oldPs);
    for (auto*& v : oldSrv) SafeRelease(v);
    SafeRelease(oldCb);
    SafeRelease(oldVsCb);
    SafeRelease(oldSampler);
}

// Draws the party onto `backBuffer` through the immediate context of `cmd`.
bool CompositeParty(command_list* cmd, resource backBuffer) {
    device* dev = cmd->get_device();
    if (dev->get_api() != device_api::d3d11 || !OpenMapping()) return false;
    const_cast<frame::Header*>(g_hdr)->readerTick = GetTickCount();  // ask DDDA to keep capturing
    auto* d3d = reinterpret_cast<ID3D11Device*>(dev->get_native());
    auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(cmd->get_native());
    if (!InitD3D(d3d)) return false;
    if (ReadFrame() && !UploadFrame(ctx)) return false;
    if (!g_srv || GetTickCount64() - g_lastFrameTick > kStaleMs) return false;

    resource bb = backBuffer;
    ID3D11RenderTargetView* rtv = BackBufferRtv(reinterpret_cast<ID3D11Resource*>(bb.handle));
    if (!rtv) return false;
    resource_desc bd = dev->get_resource_desc(bb);
    float bw = static_cast<float>(bd.texture.width), bh = static_cast<float>(bd.texture.height);
    bool depth = CaptureSkyrimDepth(ctx, bd.texture.width, bd.texture.height);
    float nearFar[2];
    SkyrimNearFar(nearFar);
    PollDebugSwitches();
    float params[48] = {nearFar[0], nearFar[1], kDdNear, kDdFar, kDdToSkyrim, depth && !g_debugNoDepth ? 1.0f : 0.0f,
                        static_cast<float>(g_texW), static_cast<float>(g_texH)};
    float now[16];
    bool warp = g_frameHasPose && !g_debugNoWarp && SkyrimPose(now);
    if (warp) {
        PutCamera(params + 8, now);
        PutCamera(params + 24, g_framePose);
        memcpy(params + 40, now + 12, 4 * sizeof(float));
        memcpy(params + 44, g_framePose + 12, 4 * sizeof(float));
        params[11] = 1.0f;  // warp
    }
    if (warp != g_warpLogged) {
        g_warpLogged = warp;
        Log(reshade::log::level::info, "DDDABridge: reprojection %s", warp ? "ON" : "OFF (no camera pose)");
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, params, sizeof(params));
    ctx->Unmap(g_params, 0);
    D3D11_VIEWPORT vp = {0, 0, bw, bh, 0, 1};
    Draw(ctx, rtv, vp, warp && !g_debugOldWarp);
    if (warp) DrawLabels(ctx, rtv, vp, now, depth && !g_debugNoDepth, nearFar);
    if (++g_shown == 1) Log(reshade::log::level::info, "DDDABridge: first DD frame shown in Skyrim");
    return true;
}

void OnPresent(command_queue* queue, swapchain* swap, const rect*, const rect*, uint32_t, const rect*) {
    struct EndFrame {
        ~EndFrame() {  // per-frame state, whatever path returns
            g_dsvDraws.clear();
            g_bbDraws = 0;
            g_drawnThisFrame = false;
            g_curBackBuffer = 0;
        }
    } endFrame;
    g_backBuffers.clear();
    for (uint32_t i = 0; i < swap->get_back_buffer_count(); ++i) g_backBuffers.push_back(swap->get_back_buffer(i).handle);
    if (!g_drawnThisFrame && CompositeParty(queue->get_immediate_command_list(), swap->get_current_back_buffer()))
        ++g_framesAtPresent;
    ULONGLONG now = GetTickCount64();
    if (now - g_lastUiReport >= 10000 && (g_framesUnderUi || g_framesAtPresent)) {
        g_lastUiReport = now;
        char seq[160] = {};
        for (uint32_t i = 0; i < g_bbDraws && i < 12; ++i)
            snprintf(seq + strlen(seq), sizeof(seq) - strlen(seq), " %u", g_bbSizes[i]);
        Log(reshade::log::level::info,
            "DDDABridge: party drawn under the UI in %llu frames, at present in %llu; this frame %u back-buffer "
            "draws, first sizes:%s",
            g_framesUnderUi, g_framesAtPresent, g_bbDraws, seq);
    }
}

void OnDestroySwapchain(swapchain*, bool) {
    for (auto& kv : g_rtvs) kv.second->Release();
    g_rtvs.clear();
}

void OnDestroyDevice(device*) {
    ReleaseAll();
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module)) return FALSE;
        reshade::register_event<reshade::addon_event::present>(&OnPresent);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindTargets);
        reshade::register_event<reshade::addon_event::draw>(&OnDraw);
        reshade::register_event<reshade::addon_event::draw_indexed>(&OnDrawIndexed);
        reshade::register_event<reshade::addon_event::destroy_resource_view>(&OnDestroyView);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(&OnDestroySwapchain);
        reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}
