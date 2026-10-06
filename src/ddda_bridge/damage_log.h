#pragma once
// Damage log (recon for combat, docs/ddda-memory.md "Combat"): logs who takes how much
// damage in DDDA. Log-only: the game's behaviour is unchanged, so it runs in plain DDDA
// too (fighting needs the party awake, and "hold" sleeps it in an unlinked session).
#include <cstdint>

namespace damagelog {

using LogFn = void (*)(const char* fmt, ...);

// Patches the three call sites after checking their bytes; logs and skips any that differ.
void Install(LogFn log, uintptr_t base);
// Bridge thread: writes the hits recorded since the last call to the log.
void Poll();

}  // namespace damagelog
