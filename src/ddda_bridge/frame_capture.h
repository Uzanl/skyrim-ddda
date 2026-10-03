// D3D9 frame capture for the DDDA bridge: copies each presented frame (scaled
// down) into the shared mapping of frame_shared.h while a reader asks for it.
#pragma once

namespace capture {

using LogFn = void (*)(const char* fmt, ...);

// Hook for DDDA.exe's Direct3DCreate9 import. Call SetOriginal with the real
// function before the game runs.
void* Direct3DCreate9Hook();
void SetOriginal(LogFn log, void* direct3DCreate9);

// Call periodically (bridge thread): the d3d9 runtime can switch the device to
// another vtable after creation, which drops the Present/Reset hooks.
void Maintain();

// The Skyrim camera pose (bridge_shared.h SkyPose, 16 floats) the camera hook applied
// for the frame being built; stored with that frame for the add-on's reprojection.
// nullptr: this frame was not rendered from a Skyrim pose.
void SetRenderPose(const float* pose);

// Which recent pose a captured frame gets: 0 = the one set last before its Present,
// N = the one set N camera updates earlier (if DDDA renders a frame with an older
// camera than the latest update). Tunable live (ddda_experiment.txt "posedelay N").
// kPoseLatch ("posedelay latch"): the pose that was the newest at the PREVIOUS
// Present, which follows DDDA when it runs 0 or 2 camera updates between frames.
constexpr int kPoseLatch = -1;
void SetPoseDelay(int delay);

}  // namespace capture
