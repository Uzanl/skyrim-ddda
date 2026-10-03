// See frame_trace.h. Hooks only record pointers until a trace is armed; while a
// frame is traced every draw is written with the state it uses. The same hooks
// feed isolate.h, which may skip draws (traced as "...-SKIP").
#include <windows.h>

#include <d3d9.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <unordered_map>

#include "frame_trace.h"
#include "isolate.h"

namespace trace {
namespace {

// IDirect3DDevice9 vtable indices
constexpr int kSetRenderTarget = 37;
constexpr int kSetDepthStencilSurface = 39;
constexpr int kClear = 43;
constexpr int kSetViewport = 47;
constexpr int kSetTexture = 65;
constexpr int kDrawPrimitive = 81;
constexpr int kDrawIndexedPrimitive = 82;
constexpr int kDrawPrimitiveUP = 83;
constexpr int kDrawIndexedPrimitiveUP = 84;
constexpr int kSetVertexDeclaration = 87;
constexpr int kSetFVF = 89;
constexpr int kSetVertexShader = 92;
constexpr int kSetPixelShader = 107;
constexpr int kStretchRect = 34;
constexpr int kSetStreamSource = 100;

using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetDepthStencilFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*);
using ClearFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetViewportFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using DrawPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT,
                                                           UINT);
using DrawPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
                                                             const void*, D3DFORMAT, const void*, UINT);
using SetVertexDeclarationFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
using SetFVFFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD);
using SetVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetPixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetStreamSourceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
using StretchRectFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
                                                  IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);

SetRenderTargetFn o_SetRenderTarget;
SetDepthStencilFn o_SetDepthStencil;
ClearFn o_Clear;
SetViewportFn o_SetViewport;
SetTextureFn o_SetTexture;
DrawPrimitiveFn o_DrawPrimitive;
DrawIndexedPrimitiveFn o_DrawIndexedPrimitive;
DrawPrimitiveUPFn o_DrawPrimitiveUP;
DrawIndexedPrimitiveUPFn o_DrawIndexedPrimitiveUP;
SetVertexDeclarationFn o_SetVertexDeclaration;
SetFVFFn o_SetFVF;
SetVertexShaderFn o_SetVertexShader;
SetPixelShaderFn o_SetPixelShader;
StretchRectFn o_StretchRect;
SetStreamSourceFn o_SetStreamSource;

LogFn g_log = nullptr;
std::wstring g_folder;
volatile LONG g_armed = 0;
bool g_tracing = false;

// Current state (render thread only).
IDirect3DSurface9* g_rt[4];
IDirect3DSurface9* g_ds;
IDirect3DVertexDeclaration9* g_decl;
DWORD g_fvf;
IDirect3DVertexShader9* g_vs;
IDirect3DPixelShader9* g_ps;
IDirect3DBaseTexture9* g_tex[8];
IDirect3DVertexBuffer9* g_vb0;
UINT g_vb0Offset, g_vb0Stride;

// Trace output.
std::string g_out;
std::unordered_map<const void*, std::string> g_names;  // pointer -> short id
std::string g_resources;                                // descriptions, one per id
int g_nextId[4];
int g_draws;

void Out(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_out += buf;
}

std::string FormatName(D3DFORMAT f) {
    char buf[32];
    if (f > 0xFF) {
        char c[5] = {static_cast<char>(f & 0xFF), static_cast<char>((f >> 8) & 0xFF),
                     static_cast<char>((f >> 16) & 0xFF), static_cast<char>((f >> 24) & 0xFF), 0};
        snprintf(buf, sizeof(buf), "'%s'", c);
    } else {
        snprintf(buf, sizeof(buf), "%d", f);
    }
    return buf;
}

std::string Name(const void* p, char kind);

std::string DescribeSurface(IDirect3DSurface9* s) {
    D3DSURFACE_DESC d;
    if (FAILED(s->GetDesc(&d))) return "?";
    char buf[160];
    snprintf(buf, sizeof(buf), "surface %ux%u fmt %s usage %lX pool %d ms %d", d.Width, d.Height,
             FormatName(d.Format).c_str(), d.Usage, d.Pool, d.MultiSampleType);
    std::string r = buf;
    IDirect3DTexture9* tex = nullptr;
    if (SUCCEEDED(s->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex))) && tex) {
        r += " of " + Name(tex, 'T');
        tex->Release();
    }
    return r;
}

std::string DescribeTexture(IDirect3DBaseTexture9* t) {
    char buf[160];
    if (t->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC d;
        if (FAILED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d))) return "texture ?";
        snprintf(buf, sizeof(buf), "texture %ux%u fmt %s usage %lX levels %lu", d.Width, d.Height,
                 FormatName(d.Format).c_str(), d.Usage, t->GetLevelCount());
        return buf;
    }
    snprintf(buf, sizeof(buf), "texture type %d", t->GetType());
    return buf;
}

std::string DescribeDecl(IDirect3DVertexDeclaration9* decl) {
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH];
    UINT n = MAXD3DDECLLENGTH;
    if (FAILED(decl->GetDeclaration(el, &n))) return "decl ?";
    static const char* kUsage[] = {"POS", "BW", "BI", "NRM", "PSIZE", "TEX", "TAN", "BIN", "TF", "POST", "COL", "FOG", "DEP", "SMP"};
    std::string r = "decl";
    for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i) {
        char buf[48];
        snprintf(buf, sizeof(buf), " %s%u:t%u@s%u+%u", el[i].Usage < 14 ? kUsage[el[i].Usage] : "?", el[i].UsageIndex,
                 el[i].Type, el[i].Stream, el[i].Offset);
        r += buf;
    }
    return r;
}

// Short stable ids: S (surface), T (texture), V (decl), X (vertex shader), P (pixel shader).
std::string Name(const void* p, char kind) {
    if (!p) return "-";
    auto it = g_names.find(p);
    if (it != g_names.end()) return it->second;
    static int counters[128];
    char id[16];
    snprintf(id, sizeof(id), "%c%d", kind, ++counters[static_cast<unsigned char>(kind)]);
    g_names[p] = id;
    std::string desc;
    switch (kind) {
    case 'S': desc = DescribeSurface(const_cast<IDirect3DSurface9*>(static_cast<const IDirect3DSurface9*>(p))); break;
    case 'T':
        desc = DescribeTexture(const_cast<IDirect3DBaseTexture9*>(static_cast<const IDirect3DBaseTexture9*>(p)));
        break;
    case 'V':
        desc = DescribeDecl(const_cast<IDirect3DVertexDeclaration9*>(static_cast<const IDirect3DVertexDeclaration9*>(p)));
        break;
    case 'B': {
        D3DVERTEXBUFFER_DESC d;
        auto* vb = const_cast<IDirect3DVertexBuffer9*>(static_cast<const IDirect3DVertexBuffer9*>(p));
        char b[96];
        if (SUCCEEDED(vb->GetDesc(&d))) {
            snprintf(b, sizeof(b), "vertex buffer %u bytes usage %lX pool %d", d.Size, d.Usage, d.Pool);
            desc = b;
        }
        break;
    }
    default: break;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%s = %p ", id, p);
    g_resources += buf + desc + "\n";
    return id;
}

void DrawLine(const char* what, UINT prims, UINT verts) {
    ++g_draws;
    std::string tex;
    for (int i = 0; i < 8; ++i)
        if (g_tex[i]) tex += " t" + std::to_string(i) + "=" + Name(g_tex[i], 'T');
    Out("  draw %d %s prims=%u verts=%u rt=%s,%s ds=%s %s fvf=%lX vs=%s ps=%s vb=%s+%u/%u%s\n", g_draws, what, prims,
        verts, Name(g_rt[0], 'S').c_str(), Name(g_rt[1], 'S').c_str(), Name(g_ds, 'S').c_str(),
        g_decl ? Name(g_decl, 'V').c_str() : "-", g_fvf, Name(g_vs, 'X').c_str(), Name(g_ps, 'P').c_str(),
        Name(g_vb0, 'B').c_str(), g_vb0Offset, g_vb0Stride, tex.c_str());
}

HRESULT STDMETHODCALLTYPE H_SetRenderTarget(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
    if (i < 4) g_rt[i] = s;
    if (g_tracing) Out("SetRenderTarget %lu = %s\n", i, Name(s, 'S').c_str());
    return o_SetRenderTarget(d, i, s);
}
HRESULT STDMETHODCALLTYPE H_SetDepthStencil(IDirect3DDevice9* d, IDirect3DSurface9* s) {
    g_ds = s;
    if (g_tracing) Out("SetDepthStencil %s\n", Name(s, 'S').c_str());
    return o_SetDepthStencil(d, s);
}
HRESULT STDMETHODCALLTYPE H_Clear(IDirect3DDevice9* d, DWORD n, const D3DRECT* r, DWORD f, D3DCOLOR c, float z,
                                  DWORD st) {
    if (g_tracing)
        Out("Clear flags=%lX color=%08lX z=%g stencil=%lu rt=%s ds=%s\n", f, c, z, st, Name(g_rt[0], 'S').c_str(),
            Name(g_ds, 'S').c_str());
    isolate::OnClear(d, f, c);
    return o_Clear(d, n, r, f, c, z, st);
}
HRESULT STDMETHODCALLTYPE H_SetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* v) {
    if (g_tracing && v) Out("SetViewport %lu,%lu %lux%lu z %g..%g\n", v->X, v->Y, v->Width, v->Height, v->MinZ, v->MaxZ);
    return o_SetViewport(d, v);
}
HRESULT STDMETHODCALLTYPE H_SetTexture(IDirect3DDevice9* d, DWORD s, IDirect3DBaseTexture9* t) {
    if (s < 8) g_tex[s] = t;
    return o_SetTexture(d, s, t);
}
HRESULT STDMETHODCALLTYPE H_DrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT sv, UINT pc) {
    bool skip = isolate::SkipDraw(g_decl, g_vb0, false);
    if (g_tracing) DrawLine(skip ? "DP-SKIP" : "DP", pc, 0);
    return skip ? D3D_OK : o_DrawPrimitive(d, t, sv, pc);
}
HRESULT STDMETHODCALLTYPE H_DrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv,
                                                 UINT si, UINT pc) {
    bool skip = isolate::SkipDraw(g_decl, g_vb0, true);
    if (g_tracing) DrawLine(skip ? "DIP-SKIP" : "DIP", pc, nv);
    return skip ? D3D_OK : o_DrawIndexedPrimitive(d, t, bv, mi, nv, si, pc);
}
HRESULT STDMETHODCALLTYPE H_DrawPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT pc, const void* v, UINT s) {
    bool skip = isolate::SkipDraw(g_decl, nullptr, false);
    if (g_tracing) DrawLine(skip ? "DPUP-SKIP" : "DPUP", pc, 0);
    return skip ? D3D_OK : o_DrawPrimitiveUP(d, t, pc, v, s);
}
HRESULT STDMETHODCALLTYPE H_DrawIndexedPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc,
                                                   const void* i, D3DFORMAT f, const void* v, UINT s) {
    bool skip = isolate::SkipDraw(g_decl, nullptr, true);
    if (g_tracing) DrawLine(skip ? "DIPUP-SKIP" : "DIPUP", pc, nv);
    return skip ? D3D_OK : o_DrawIndexedPrimitiveUP(d, t, mi, nv, pc, i, f, v, s);
}
HRESULT STDMETHODCALLTYPE H_SetVertexDeclaration(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* v) {
    g_decl = v;
    return o_SetVertexDeclaration(d, v);
}
HRESULT STDMETHODCALLTYPE H_SetFVF(IDirect3DDevice9* d, DWORD f) {
    g_fvf = f;
    g_decl = nullptr;
    return o_SetFVF(d, f);
}
HRESULT STDMETHODCALLTYPE H_SetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* s) {
    g_vs = s;
    return o_SetVertexShader(d, s);
}
HRESULT STDMETHODCALLTYPE H_SetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* s) {
    g_ps = s;
    return o_SetPixelShader(d, s);
}
HRESULT STDMETHODCALLTYPE H_StretchRect(IDirect3DDevice9* d, IDirect3DSurface9* s, const RECT* sr, IDirect3DSurface9* t,
                                        const RECT* tr, D3DTEXTUREFILTERTYPE f) {
    if (g_tracing) Out("StretchRect %s -> %s\n", Name(s, 'S').c_str(), Name(t, 'S').c_str());
    return o_StretchRect(d, s, sr, t, tr, f);
}

HRESULT STDMETHODCALLTYPE H_SetStreamSource(IDirect3DDevice9* d, UINT n, IDirect3DVertexBuffer9* vb, UINT off,
                                            UINT stride) {
    if (n == 0) {
        g_vb0 = vb;
        g_vb0Offset = off;
        g_vb0Stride = stride;
    }
    return o_SetStreamSource(d, n, vb, off, stride);
}

void Hook(void** vt, int index, void* hook, void** orig) {
    if (vt[index] == hook) return;
    DWORD old;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
    *orig = vt[index];
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&vt[index]), reinterpret_cast<LONG>(hook));
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
}

void Finish() {
    std::wstring path = g_folder + L"\\ddda_frame_trace.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"w");
    if (f) {
        fprintf(f, "# DDDA one-frame D3D9 trace: %d draws\n# resources\n%s\n# calls\n%s", g_draws,
                g_resources.c_str(), g_out.c_str());
        fclose(f);
    }
    g_log("frame trace written: %d draws, %zu bytes", g_draws, g_out.size());
    g_out.clear();
    g_out.shrink_to_fit();
}

}  // namespace

void Init(LogFn log, const wchar_t* folder) {
    g_log = log;
    g_folder = folder;
}

void HookDevice(void** vt) {
#define HOOK(idx, fn, orig) Hook(vt, idx, reinterpret_cast<void*>(&fn), reinterpret_cast<void**>(&orig))
    HOOK(kSetRenderTarget, H_SetRenderTarget, o_SetRenderTarget);
    HOOK(kSetDepthStencilSurface, H_SetDepthStencil, o_SetDepthStencil);
    HOOK(kClear, H_Clear, o_Clear);
    HOOK(kSetViewport, H_SetViewport, o_SetViewport);
    HOOK(kSetTexture, H_SetTexture, o_SetTexture);
    HOOK(kDrawPrimitive, H_DrawPrimitive, o_DrawPrimitive);
    HOOK(kDrawIndexedPrimitive, H_DrawIndexedPrimitive, o_DrawIndexedPrimitive);
    HOOK(kDrawPrimitiveUP, H_DrawPrimitiveUP, o_DrawPrimitiveUP);
    HOOK(kDrawIndexedPrimitiveUP, H_DrawIndexedPrimitiveUP, o_DrawIndexedPrimitiveUP);
    HOOK(kSetVertexDeclaration, H_SetVertexDeclaration, o_SetVertexDeclaration);
    HOOK(kSetFVF, H_SetFVF, o_SetFVF);
    HOOK(kSetVertexShader, H_SetVertexShader, o_SetVertexShader);
    HOOK(kSetPixelShader, H_SetPixelShader, o_SetPixelShader);
    HOOK(kStretchRect, H_StretchRect, o_StretchRect);
    HOOK(kSetStreamSource, H_SetStreamSource, o_SetStreamSource);
#undef HOOK
}

void OnPresent(IDirect3DDevice9*) {
    if (g_tracing) {
        g_tracing = false;
        Finish();
    }
    if (InterlockedExchange(&g_armed, 0)) {
        g_tracing = true;
        g_draws = 0;
        g_out = "Present (frame start)\n";
    }
}

void Poll() {
    std::wstring req = g_folder + L"\\ddda_trace_request";
    if (GetFileAttributesW(req.c_str()) != INVALID_FILE_ATTRIBUTES && DeleteFileW(req.c_str())) {
        InterlockedExchange(&g_armed, 1);
        g_log("frame trace requested");
    }
}

}  // namespace trace
