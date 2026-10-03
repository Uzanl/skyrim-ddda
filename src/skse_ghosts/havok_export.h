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
// Interiors: the player's interior cell is harvested whole into DDDA_havok\interior\
// {cell form id}.bin, and DDDA_havok\current.txt says "interior {id}" or "exterior";
// the streamer builds the interior's ground in an "arena" of DDDA's map from it.
//
// Shape walking is ported from SkyCraft (MIT, github.com/chasmlol/SkyCraft,
// skse/src/Collision.cpp): Havok layouts are partly reverse-engineered there, so
// every read runs under SEH.
#pragma once

namespace havok_export {

// Main thread, a few times a second while in the world.
void Update();
// Plugin load and every game load: cells are harvested again (another save may differ)
// and the "player in an interior" report is cleared until the next update.
void Reset();

}  // namespace havok_export
