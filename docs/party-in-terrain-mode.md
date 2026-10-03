# The party in terrain mode: what DDDA does to them, and why the bridge does what it does

Findings of the 2026-10-02 session (terrain streaming tests with tools/terrain/terrain_sim.py,
logs in DDDA's `ddda_bridge.log`). Code: `src/ddda_bridge/dllmain.cpp`, section
"Terrain mode".

## The Arisen is kinematic while linked (NOT immortal)

**What was changed:** only the Arisen's *physics*. While the terrain link is active
(`kGlobalCoords` commands from Skyrim), `TerrainFollowUnsafe`:

1. puts its scenery adjust to sleep (`SleepAdjust(self, true)`: `cScrAdjust.mIsSleep`,
   char+0x19C0+0x1AC = 1; no gravity, no ground snap, no wall push);
2. writes its position every frame at Skyrim's feet + 2 cm (`kArisenLift`), x, y and z;
3. copies that position into the adjust's own copies and zeroes its velocity
   (`SyncAdjust`, see below).

**What was NOT changed:** its HP, damage, status or any combat logic. It cannot take
**fall** damage any more (it never falls), but anything else that hurts it in DDDA still
does. Nothing was patched in the damage code.

**Why:** with its own physics, the Arisen was pushed forward down steep Skyrim slopes
(~45 degrees, where the Dragonborn simply walks) and fell from step to step: it lost
601 HP and died twice during a 1 km simulated walk. DDDA's ground now matches Skyrim's,
so a kinematic Arisen stands on it anyway.

**To make the Arisen a real, playable character in Skyrim later**, undo exactly this:
- in `TerrainFollowUnsafe`, do not call `SleepAdjust(self, true)` and do not write
  `p[1]` (only x/z, as before 2026-10-02), or drive it from DDDA's own input instead of
  Skyrim's player;
- keep `SyncAdjust` for any *teleport* of it (see the fall-damage rule below);
- the previous (non-kinematic) code is not kept (the project is not under version
  control). It wrote only x/z each frame and let DDDA's physics keep the height; a gap
  over 3 m (`kTerrainLag`) also set the height to Skyrim's feet, and over 20 m
  (`kTerrainLeap`) or 20 m off in height it ran a protection like the link's.

Known limit of the kinematic Arisen: on a Skyrim roof or rock (not part of DDDA's
generated ground yet) it floats above DDDA's ground there; the pawns may then fail to
reach it (DDDA used to "glue" pawns onto an unreachable leader).

## DDDA's fall damage: how it really works

- The scenery adjust (`cScrAdjust`, char+0x19C0) keeps the previous frame's position in
  **mOldPos +0x120, mRealPos +0x130, mResPos +0x140** (vec4) and a **velocity at +0x150**
  (y = -49.5 while falling). When a character is moved far DOWN by writing only its
  position, the next physics step reads that as a fall at that speed: pawns put 34 m
  lower were knocked down; the Arisen lost HP over two downward moves.
- Moving a character UP is harmless (upward "velocity"): the link lifts the party 506 m
  without damage.
- `SyncAdjust(obj)` (copy the new position into the three fields, zero the velocity)
  makes a teleport invisible to the fall damage. Use it for every teleport.
- DDDA's own warp (pawns far from the Arisen are brought to it) is damage-free; the
  bridge leaves pawns to it instead of moving them down itself.

## Knocked-down pawns go back to the Rift

A pawn at 0 HP is knocked down; if nobody helps it up within a few seconds it returns
to the Rift and its object is destroyed (it "stops updating" in the bridge). That was
the "pawns vanished" symptom; reloading the save brings them back. The bridge's
**party watch** logs `KNOCKED DOWN` / `STOPPED UPDATING` with positions the moment it
happens.

## Other DDDA behaviours found on the way

- **Camera-occlusion fade:** characters between the camera and the player fade out
  (char+0x2514); the draw loads it at +0x76A07B. Patched to a constant 1.0 while linked
  (docs/ddda-memory.md).
- **Focus pause:** the first focus loss can still pause the world for a moment (the
  bridge restores the move mask); minimising DDDA is not safe. During a pause the Arisen
  stopped and the target ran ahead, which triggered the old leap logic.
- **Tile origin:** the local coordinate wraps one frame before uStageSplitCtrl's N/M; the
  bridge follows the wrap and only resyncs from N/M after 2 quiet seconds
  (`kResyncQuietMs`): a resync in the middle of two leaps once set a wrong tile.
- **Special terrain surfaces:** some Gransys tiles (river/waterfall: 59m52n, 60m52n)
  have terrain attribute 0x0600 and triangle flags (0, 1). Copied onto generated ground,
  the party stood on flat plateaus above the slope. sbcgen now always writes plain
  ground (attribute 0x01, flags (514, 2)).
- **Tile archives are read on load:** an archive replaced on disk while DDDA runs is
  used the next time that tile loads (the basis of streaming); loaded tiles keep their
  old data until they unload.
- **Hold:** with streaming on, a save made at the old height would drop the party into
  the void at load (they fell 1.6 km once). `hold` in ddda_experiment.txt keeps the
  party's adjust asleep until Skyrim links; the link then lifts them to the ground.
- **Link hiccups:** a stall of the camera sender under 2 s (`kRelinkMs`) is the same link;
  re-running the link move on a stall once moved the Arisen 37 m down.

## Test results that matter

| Run | Result |
|---|---|
| leap_test (771 m) | DDDA read a tile installed while running; pawns warped to the Arisen in < 0.5 s |
| streaming 1-6 | streamer kept the ground ahead over 4+ tiles; failures fixed one by one (plateaus, pawn knock-downs, Arisen falls) |
| 6 (SyncAdjust) | full 1 km loop, pawns unharmed; Arisen died from slope falls -> made kinematic |
| real 2-game leaps | 3 edge leaps (61m52n->66m46n, 62m47n->54m51n, 52m53n->65m45n): protect "leap" each time, pawns within 1 m after, HP unchanged, the user saw them keep following |
| 7 (kinematic Arisen) | **clean**: full 1 km loop, no protects after the link, nobody knocked down, Arisen HP unchanged and 0-0.34 m above the ground; farthest pawn median 17 m, p90 50 m, max 122 m |
