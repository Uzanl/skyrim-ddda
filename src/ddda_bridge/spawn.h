// Spawn test (combat stand-ins, docs/ddda-memory.md "Enemy placement"): DDDA's own
// "create an enemy from its placement" (+0x3613D0) is watched; each real call's arguments
// and a copy of its placement record are kept. With a line "spawn N" in ddda_experiment.txt
// (bridge session only), every change of N calls it again once, from the Arisen's move(),
// with a copy of the last record moved 4 m in front of the Arisen. In a session, enemy
// archives are kept loaded (one extra reference), so a spawn after the link has them.
#pragma once

#include <windows.h>

#include <cstdint>

namespace spawn {

using LogFn = void (*)(const char* fmt, ...);
void Install(LogFn log, uintptr_t base);

// Bridge thread: ddda_experiment.txt's "spawn N" (-1 = no such line), the session flag,
// and the tile origin (global = local + ((n - 50), 0, (m - 50)) * 10000).
void SetRequest(int n, bool session, bool originValid, int tileN, int tileM);
// The bridge's live tile origin (read on game threads every frame).
void SetTileOrigin(const volatile LONG* n, const volatile LONG* m, const volatile LONG* valid);
// ddda_experiment.txt's "spawnkind ID" (archive ID, hex; 0 = off): spawns are then made
// from nothing (kind table row, archive requested by ID, new record and layout).
void SetKind(int archiveId);
// ddda_experiment.txt's "standins" (bridge session only): one DDDA enemy at each hostile
// Skyrim NPC that DDDAGhosts publishes (bridge::Npcs), kept at the NPC's spot.
void SetStandins(bool on);
// Arisen's move() hook, after the original: runs a pending spawn.
void ArisenTick(uintptr_t arisen);
// Bridge thread: logs captures and spawn results.
void Poll();

}  // namespace spawn
