// Hijacked enemy (combat test, docs/ddda-memory.md "AI target list"): with a line "hijack"
// in ddda_experiment.txt, in a bridge session only, the nearest live enemy of DDDA's AI
// target list (sAISensorTarget group 3) is held every frame at a spot 6 m in front of the
// Arisen (x/z written after its own move(); its AI, animation and gravity keep running).
// Question: do the pawns attack it, and do their hits land (damage_log.cpp)?
#pragma once

#include <cstdint>

namespace hijack {

using LogFn = void (*)(const char* fmt, ...);
void Init(LogFn log, uintptr_t base);

// Bridge thread, every tick. arisen = the Arisen's tile-local position (null if unknown).
void SetEnabled(bool on);
void Poll(const float* arisen);

}  // namespace hijack
