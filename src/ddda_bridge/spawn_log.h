#pragma once
// Spawn log (recon for combat stand-ins, docs/ddda-memory.md "Enemy placement"): logs every
// enemy DDDA constructs (uEnemy's base constructor) with the code addresses found on the
// stack, to find the function that creates an enemy. Log-only, so it also runs in plain DDDA.
#include <cstdint>

namespace spawnlog {

using LogFn = void (*)(const char* fmt, ...);

// Patches the constructor's entry after checking its bytes; logs and skips if they differ.
void Install(LogFn log, uintptr_t base);
// Bridge thread: writes the enemies constructed since the last call to the log.
void Poll();

}  // namespace spawnlog
