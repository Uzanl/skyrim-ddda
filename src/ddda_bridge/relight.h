// Skyrim's lighting on the pawns (docs/lighting.md): while linked, DDDA's sun
// (uSkyInfiniteLight), ambient (uSkyHemiSphereLight) and distance fog (uSkyColorFog) get Skyrim's
// values (bridge_shared.h LightCmd, sent by the SKSE plugin) right before each of them is
// drawn (vtable slot 9, after all of the frame's update code, so DDDA's weather cannot
// overwrite them).
//
// Colours are Skyrim's times "lightscale" (v1 scaled them relative to DDDA's brightest
// value seen, which failed when DDDA started at night). Only lights whose group matches
// "lightmask" are touched (default: all but 0x10, the sky dome). Directions: Skyrim's
// sun direction mapped to DDDA's axes, pointing towards the light (DDDA's convention).
#pragma once

#include <cstdint>

namespace relight {

using LogFn = void (*)(const char* fmt, ...);
void Init(LogFn log, uintptr_t base);
bool Install();          // patches the three vtable slots (validated first)

// Bridge thread.
void SetLinked(bool linked);
void Poll();             // reads the plugin's LightCmd
// ddda_experiment.txt: nolight, nofog, lightscale X, lightmask HEX
void SetOptions(bool enabled, bool fog, float scale, uint32_t mask);

}  // namespace relight
