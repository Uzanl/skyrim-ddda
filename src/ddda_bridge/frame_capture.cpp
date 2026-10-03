// See frame_capture.h. Hooks are vtable patches: IDirect3D9::CreateDevice to find
// the device, then the device's Present to capture and Reset to release our
// D3DPOOL_DEFAULT surface (Reset fails otherwise). The d3d9 runtime keeps device
// vtables in heap memory and may switch the device to a different one after
// creation, so Maintain() re-applies the device hooks when that happens.
//
// DDDA uses plain D3D9 (no D3D9Ex), so frames cannot be shared on the GPU: each
// frame is scaled into a render target, read back to system memory and copied
// into the mapping. Capture only runs while a reader keeps `readerTick` fresh.
#include <windows.h>

#include <d3d9.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "../common/frame_shared.h"
#include "frame_capture.h"
#include "frame_trace.h"
#include "isolate.h"

namespace capture {
namespace {

constexpr UINT kCaptureWidth = frame::kMaxWidth;  // wider frames are scaled down (aspect kept)

// vtable indices
constexpr int kD3D9CreateDevice = 16;
constexpr int kDeviceReset = 16;
constexpr int kDevicePresent = 17;

using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

LogFn g_log = nullptr;
Create9Fn g_origCreate9 = nullptr;
CreateDeviceFn g_origCreateDevice = nullptr;
ResetFn g_origReset = nullptr;
PresentFn g_origPresent = nullptr;
IDirect3DDevice9* volatile g_device = nullptr;
void** g_hookedVtable = nullptr;
volatile LONG g_rehooks = 0;

frame::Header* g_hdr = nullptr;
uint8_t* g_view = nullptr;
IDirect3DSurface9* g_rt = nullptr;   // scaled copy of the back buffer (GPU)
IDirect3DSurface9* g_sys = nullptr;  // read-back target (system memory)
IDirect3DSurface9* g_rtMask = nullptr;   // scaled copy of the G-buffer
IDirect3DSurface9* g_sysMask = nullptr;
D3DFORMAT g_maskFmt = D3DFMT_UNKNOWN;
UINT g_w = 0, g_h = 0;
D3DFORMAT g_fmt = D3DFMT_UNKNOWN;
bool g_capturing = false;
bool g_warned = false;

void PatchEntry(void** vt, int index, void* hook, void** orig) {
    DWORD old;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
    *orig = vt[index];
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&vt[index]), reinterpret_cast<LONG>(hook));
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
}

bool OpenMapping() {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, frame::kMappingBytes,
                                  frame::kMappingName);
    if (!h) {
        g_log("frame CreateFileMapping failed: %lu", GetLastError());
        return false;
    }
    g_view = static_cast<uint8_t*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, frame::kMappingBytes));
    if (!g_view) {
        g_log("frame MapViewOfFile failed: %lu", GetLastError());
        CloseHandle(h);
        return false;
    }
    g_hdr = reinterpret_cast<frame::Header*>(g_view);
    g_hdr->magic = frame::kMagic;
    g_hdr->version = frame::kVersion;
    g_hdr->writerPid = GetCurrentProcessId();
    g_hdr->format = frame::kFormatBGRA8;
    return true;
}

void ReleaseSurfaces() {
    for (IDirect3DSurface9** s : {&g_rt, &g_sys, &g_rtMask, &g_sysMask}) {
        if (*s) (*s)->Release();
        *s = nullptr;
    }
    g_w = g_h = 0;
    g_maskFmt = D3DFMT_UNKNOWN;
}

// Scales `src` into (rt, sys) and copies it into the mapping at `dst`. Creates the
// surfaces on first use (format of `src`).
// Ring of the poses the camera hook applied, newest at g_poseCount - 1.
constexpr uint32_t kPoseRing = 8;
SRWLOCK g_poseLock = SRWLOCK_INIT;
float g_poses[kPoseRing][16];
bool g_posesValid[kPoseRing];
uint32_t g_poseCount = 0;
volatile LONG g_poseDelay = 1;  // see PollExperiment in dllmain.cpp
float g_latched[16];  // newest pose at the previous Present (kPoseLatch)
bool g_latchedValid = false;
uint32_t g_countAtPresent = 0;
uint32_t g_updatesHist[4];  // camera updates between Presents: 0, 1, 2, 3+
ULONGLONG g_lastHistLog = 0;

bool CopyPlane(IDirect3DDevice9* dev, IDirect3DSurface9* src, IDirect3DSurface9** rt, IDirect3DSurface9** sys,
               D3DFORMAT* fmt, uint8_t* dst) {
    D3DSURFACE_DESC d;
    if (FAILED(src->GetDesc(&d)) || (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8)) return false;
    if (!*rt || d.Format != *fmt) {
        if (*rt) (*rt)->Release();
        if (*sys) (*sys)->Release();
        *rt = *sys = nullptr;
        if (FAILED(dev->CreateRenderTarget(g_w, g_h, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, rt, nullptr)) ||
            FAILED(dev->CreateOffscreenPlainSurface(g_w, g_h, d.Format, D3DPOOL_SYSTEMMEM, sys, nullptr))) {
            if (*rt) (*rt)->Release();
            *rt = nullptr;
            return false;
        }
        *fmt = d.Format;
    }
    // Point filtering: a scaled mask must never blend a pawn edge with the white background.
    if (FAILED(dev->StretchRect(src, nullptr, *rt, nullptr, D3DTEXF_POINT)) ||
        FAILED(dev->GetRenderTargetData(*rt, *sys)))
        return false;
    D3DLOCKED_RECT lr;
    if (FAILED((*sys)->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
    const uint8_t* p = static_cast<const uint8_t*>(lr.pBits);
    for (UINT y = 0; y < g_h; ++y) memcpy(dst + y * g_w * 4, p + y * lr.Pitch, g_w * 4);
    (*sys)->UnlockRect();
    return true;
}

bool ReaderWants() {
    return g_hdr && GetTickCount() - g_hdr->readerTick < frame::kReaderTimeoutMs;
}

void Capture(IDirect3DDevice9* dev) {
    if (!isolate::PublishThisFrame()) return;  // the pawns are hidden for learning
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    if (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8) {
        if (!g_warned) g_log("frame capture: unsupported back buffer format %d", d.Format);
        g_warned = true;
        bb->Release();
        return;
    }
    UINT w = d.Width < kCaptureWidth ? d.Width : kCaptureWidth;
    UINT h = static_cast<UINT>(static_cast<uint64_t>(d.Height) * w / d.Width);
    if (w == 0 || h == 0 || h > frame::kMaxHeight) {
        bb->Release();
        return;
    }
    if (!g_rt || w != g_w || h != g_h || d.Format != g_fmt) {
        ReleaseSurfaces();  // also drops the mask surfaces, sized the same way
        HRESULT a = dev->CreateRenderTarget(w, h, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_rt, nullptr);
        HRESULT b = dev->CreateOffscreenPlainSurface(w, h, d.Format, D3DPOOL_SYSTEMMEM, &g_sys, nullptr);
        if (FAILED(a) || FAILED(b)) {
            g_log("frame capture: surface creation failed (%08lX, %08lX)", a, b);
            ReleaseSurfaces();
            bb->Release();
            return;
        }
        g_w = w;
        g_h = h;
        g_fmt = d.Format;
        g_log("frame capture: back buffer %ux%u fmt %d, capturing %ux%u", d.Width, d.Height, d.Format, w, h);
    }
    HRESULT hr = dev->StretchRect(bb, nullptr, g_rt, nullptr, D3DTEXF_LINEAR);
    bb->Release();
    if (FAILED(hr) || FAILED(dev->GetRenderTargetData(g_rt, g_sys))) return;
    D3DLOCKED_RECT lr;
    if (FAILED(g_sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;

    uint32_t i = (g_hdr->latest + 1) % frame::kSlots;
    frame::Slot& s = g_hdr->slots[i];
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&s.seq));  // odd: writing
    s.width = w;
    s.height = h;
    s.pitch = w * 4;
    uint8_t* dst = g_view + frame::SlotOffset(i);
    const uint8_t* src = static_cast<const uint8_t*>(lr.pBits);
    for (UINT y = 0; y < h; ++y) memcpy(dst + y * s.pitch, src + y * lr.Pitch, s.pitch);
    g_sys->UnlockRect();
    IDirect3DSurface9* gbuf = isolate::GBuffer();
    s.flags = gbuf && CopyPlane(dev, gbuf, &g_rtMask, &g_sysMask, &g_maskFmt, g_view + frame::MaskOffset(i))
                  ? frame::kSlotHasMask
                  : 0;
    AcquireSRWLockExclusive(&g_poseLock);
    LONG delay = g_poseDelay;
    if (delay == kPoseLatch) {
        if (g_latchedValid) {
            memcpy(s.skyPose, g_latched, sizeof(s.skyPose));
            s.flags |= frame::kSlotHasPose;
        }
    } else if (g_poseCount > static_cast<uint32_t>(delay)) {
        uint32_t k = (g_poseCount - 1 - static_cast<uint32_t>(delay)) % kPoseRing;
        if (g_posesValid[k]) {
            memcpy(s.skyPose, g_poses[k], sizeof(s.skyPose));
            s.flags |= frame::kSlotHasPose;
        }
    }
    // Latch the newest pose for the next frame, and count updates per frame.
    uint32_t newest = (g_poseCount + kPoseRing - 1) % kPoseRing;
    g_latchedValid = g_poseCount > 0 && g_posesValid[newest];
    if (g_latchedValid) memcpy(g_latched, g_poses[newest], sizeof(g_latched));
    uint32_t n = g_poseCount - g_countAtPresent;
    g_countAtPresent = g_poseCount;
    ++g_updatesHist[n < 3 ? n : 3];
    ReleaseSRWLockExclusive(&g_poseLock);
    ULONGLONG now = GetTickCount64();
    if (now - g_lastHistLog >= 10000) {
        g_lastHistLog = now;
        g_log("camera updates per frame (10 s): 0:%u 1:%u 2:%u 3+:%u; pose mode %ld", g_updatesHist[0],
              g_updatesHist[1], g_updatesHist[2], g_updatesHist[3], delay);
        memset(g_updatesHist, 0, sizeof(g_updatesHist));
    }
    s.frameId = ++g_hdr->frames;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    s.qpcTime = static_cast<uint64_t>(q.QuadPart);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&s.seq));  // even: done
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_hdr->latest), static_cast<LONG>(i));
}

void SafeCapture(IDirect3DDevice9* dev) {
    __try {
        Capture(dev);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!g_warned) g_log("frame capture: exception %08lX", GetExceptionCode());
        g_warned = true;
    }
}

HRESULT STDMETHODCALLTYPE PresentHook(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd,
                                      const RGNDATA* dirty) {
    trace::OnPresent(dev);
    isolate::OnPresent();
    // Skyrim only draws party-only frames, so nothing is read back before that starts.
    bool want = ReaderWants() && isolate::Filtering();
    if (want != g_capturing) {
        g_capturing = want;
        g_log("frame capture %s (%llu frames so far)", want ? "ON" : "OFF", g_hdr ? g_hdr->frames : 0ull);
        if (!want) ReleaseSurfaces();
    }
    if (want) SafeCapture(dev);
    return g_origPresent(dev, src, dst, wnd, dirty);
}

HRESULT STDMETHODCALLTYPE ResetHook(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    ReleaseSurfaces();
    return g_origReset(dev, pp);
}

HRESULT STDMETHODCALLTYPE CreateDeviceHook(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                           D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    HRESULT hr = g_origCreateDevice(d3d, adapter, type, wnd, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        bool mapped = g_hdr || OpenMapping();
        g_device = *out;
        Maintain();
        g_log("d3d9 device %p created (vtable %p), frame mapping %s", *out, *reinterpret_cast<void***>(*out),
              mapped ? "ok" : "FAILED");
    }
    return hr;
}

IDirect3D9* WINAPI Direct3DCreate9Impl(UINT sdk) {
    IDirect3D9* d3d = g_origCreate9(sdk);
    static volatile LONG done = 0;
    if (d3d && InterlockedCompareExchange(&done, 1, 0) == 0) {
        void** vt = *reinterpret_cast<void***>(d3d);
        PatchEntry(vt, kD3D9CreateDevice, reinterpret_cast<void*>(&CreateDeviceHook),
                   reinterpret_cast<void**>(&g_origCreateDevice));
        g_log("Direct3DCreate9(%u) hooked", sdk);
    }
    return d3d;
}

}  // namespace

void* Direct3DCreate9Hook() {
    return reinterpret_cast<void*>(&Direct3DCreate9Impl);
}

void Maintain() {
    IDirect3DDevice9* dev = g_device;
    if (!dev) return;
    void** vt;
    __try {
        vt = *reinterpret_cast<void***>(dev);
        if (vt == g_hookedVtable && vt[kDevicePresent] == reinterpret_cast<void*>(&PresentHook)) return;
        // A slot that already holds our hook keeps the original we saved earlier.
        if (vt[kDevicePresent] != reinterpret_cast<void*>(&PresentHook))
            PatchEntry(vt, kDevicePresent, reinterpret_cast<void*>(&PresentHook),
                       reinterpret_cast<void**>(&g_origPresent));
        if (vt[kDeviceReset] != reinterpret_cast<void*>(&ResetHook))
            PatchEntry(vt, kDeviceReset, reinterpret_cast<void*>(&ResetHook), reinterpret_cast<void**>(&g_origReset));
        trace::HookDevice(vt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    g_hookedVtable = vt;
    g_log("d3d9 device %p: Present/Reset hooked on vtable %p (hook #%ld)", dev, vt, InterlockedIncrement(&g_rehooks));
}

void SetOriginal(LogFn log, void* direct3DCreate9) {
    g_log = log;
    g_origCreate9 = reinterpret_cast<Create9Fn>(direct3DCreate9);
}

void SetRenderPose(const float* pose) {
    AcquireSRWLockExclusive(&g_poseLock);
    uint32_t k = g_poseCount % kPoseRing;
    g_posesValid[k] = pose != nullptr;
    if (pose) memcpy(g_poses[k], pose, sizeof(g_poses[k]));
    ++g_poseCount;
    ReleaseSRWLockExclusive(&g_poseLock);
}

void SetPoseDelay(int delay) {
    InterlockedExchange(&g_poseDelay, delay == kPoseLatch ? kPoseLatch
                                      : delay < 0                         ? 0
                                      : delay >= static_cast<int>(kPoseRing) ? kPoseRing - 1
                                                                          : delay);
}

}  // namespace capture
