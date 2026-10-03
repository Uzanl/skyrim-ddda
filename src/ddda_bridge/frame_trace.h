// One-frame D3D9 call trace for reverse engineering DDDA's renderer: render
// targets, clears, vertex declarations, shaders, textures and every draw call of
// a single frame, written to ddda_frame_trace.txt next to the bridge DLL.
// Requested by creating `ddda_trace_request` in the same folder.
#pragma once

#include <d3d9.h>

namespace trace {

using LogFn = void (*)(const char* fmt, ...);

void Init(LogFn log, const wchar_t* folder);
// Patches the device vtable slots the trace needs (idempotent).
void HookDevice(void** vtable);
// Called at the start of every Present.
void OnPresent(IDirect3DDevice9* dev);
// Called by the bridge thread; arms a trace if the request file exists.
void Poll();

}  // namespace trace
