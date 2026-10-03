// Live Skyrim collision export (docs/terrain-proxy.md, "Live Havok collision").
//
// Every exterior cell that is attached (Skyrim keeps Havok physics only for the
// loaded grid around the player) is harvested once per session: the triangles of
// every static body's shape inside the cell's square (terrain, houses, decks,
// stairs, fences, rocks, trees) are written to Data\SKSE\Plugins\DDDA_havok\
// as one file per cell, in Skyrim units and axes (z up). tools/terrain/stream.py
// builds DDDA's collision tiles and waypoint graphs from these files where they
// exist, and from Skyrim.esm (terrain + object raster) elsewhere.
//
// Shape walking is ported from SkyCraft (MIT, github.com/chasmlol/SkyCraft,
// skse/src/Collision.cpp): Havok layouts are partly reverse-engineered there, so
// every read runs under SEH.
#pragma once

namespace havok_export {

// Main thread, a few times a second while in the world.
void Update();
// A new game was loaded: cells are harvested again (another save may differ).
void Reset();

}  // namespace havok_export
