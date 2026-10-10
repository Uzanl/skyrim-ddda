#pragma once
// Damage log (recon for combat, docs/ddda-memory.md "Combat"): logs who takes how much
// damage in DDDA. Log-only: the game's behaviour is unchanged, so it runs in plain DDDA
// too (fighting needs the party awake, and "hold" sleeps it in an unlinked session).
#include <cstdint>

namespace damagelog {

using LogFn = void (*)(const char* fmt, ...);

// Patches the three call sites after checking their bytes; logs and skips any that differ.
void Install(LogFn log, uintptr_t base);
// move() hooks: the party's objects (role 0 Arisen, 1 main pawn, 2-3 hired), so shooter
// candidates are recognised even for party members not yet seen in a hit.
void NoteParty(int role, uint32_t obj);
// Bridge thread: writes the hits recorded since the last call to the log.
void Poll();

}  // namespace damagelog
