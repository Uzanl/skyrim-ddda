# DDDA memory map

Steam build 2364871. `DDDA.exe` is 32-bit and loads at 0x400000 with no ASLR.

**Body height (2026-10-03, tools/recon/pawnheight.py):** characters keep the MT Framework
uCoord layout: position `+0x40`, rotation `+0x50`, scale `+0x60` (x, y, z). The y scale
(`+0x64`) is the editor's body height: Jack Shriker 0.74 (visibly the shortest), the Arisen
0.89, Diana 0.96, Mariana 1.05 (183 cm in DDDA's editor). The scale does not give where the
head is: posture bends the body (Mariana's head is lower than her scale says).

**Skeleton (2026-10-03, tools/recon/headjoint.py):** `[char+0x364]` is the joint array
(object vtable `0x143A8E8`). Joint k is at `+k*0x140`: world position (vec4, w = 1) at
`+0x40`, then the bind offset from its parent (hips: 0, 111, 0) and a quaternion. The joint
count is the low byte of `char+0x378` (also `+0x38C`): 69 Arisen, 68 Mariana and Diana, 65
Jack. The highest joint within 40 cm of the feet's axis is the head (Arisen 144.5 cm above
the feet, Mariana 165.6, Diana 160.8, Jack 120.5). The bridge sends it as
`Actor.headHeight` every frame for the labels.

## Characters (the Arisen and party pawns)

Only the player's own party uses these two classes. In Gran Soren, other
players' wandering pawns are not instances of them.

| Class | vtable | slot 8 (`+0x20`, per-frame `move()`, thiscall, no args) |
|---|---|---|
| Arisen (player) | `0x15E90D0` | `DDDA.exe+755F20` (a `jmp` to the base move `+761280`) |
| Party pawn | `0x15E8468` | `DDDA.exe+753650` |

Fields shared by both classes. The pawn object stride is 0x5930, so all fields are below that.

| Data | Location | Type |
|---|---|---|
| Position (X, Y up, Z) | `char+0x40` | float[3], units probably cm |
| Status object | `[char+0x4BC]` points to status+0xCF0 | |
| Current HP / max HP | `[char+0x4BC]+0x1D8` / `+0x1DC` | float |
| Save-data record | `[char+0x3DEC]`, name at `+0x70C` (UTF-8) | |

Telling main and hired pawns apart: the pawn records sit right after the
Arisen's record:
`pawnRecord = arisenRecord + 0x7F0 + 0x1660 * slot`. Slot 0 is the main pawn and
slots 1 and 2 are the hired pawns.

Object lifetime: the Arisen's object kept its address across an area load. The
pawn objects are reallocated on every load.

The `move()` functions run on several worker threads, in parallel.

## Camera

`[DDDA.exe+14D1578]+DF0` (and the equivalent `[+14D09E0]+EC4`, `[+14D08F4]+2B8`) is
the **camera** position, not the Arisen's. It orbits about 300 units around the
body when the camera is rotated. During area loads it reads `(0, 200, -700)`.

## Code addresses

- `DDDA.exe+376F50` (ApplyDamage): takes HP off a character. `eax` = its vital block,
  stack: damage (float, positive) and a second argument; `ret 8`. Does
  `[vital+8] -= damage`. Vital block: HP `+8`, max `+0xC`, owning character `+0x1B4`
  (the same HP as `[char+0x4BC]+0x1D8`, so vital = `[char+0x4BC]+0x1D0`; seen live on a pawn).
- `DDDA.exe+488DA0`: per-frame pawn routine; the character is in EAX (custom convention).
  It is called from the pawn `move()` (and ~50 other places). It writes the HP every
  frame (`[vital+8] += [esp+0x1C]`, then clamps to max x `[char+0x20CC]`), so a write
  watchpoint on a pawn's HP catches it (+488F95, +488FFD) thousands of times
  (2026-10-06); it is not the damage.

## sUnit and the focus pause

`[DDDA.exe+14D09E0]` is **sUnit** (vtable `0x15654E0`), the unit manager, not the player.
- `+0x620`: move-line count (37). Line `i` flags are at `+0x28 + i*0x18`; bit 1 is the per-line pause.
- `+0xD30` / `+0xD34`: 64-bit move mask. A unit's `move()` runs only if
  `(~D30 & unit+0x18) | (~D34 & unit+0x1C) == 0`
  (in `DDDA.exe+9B7350`, the per-line unit loop).
- **Focus pause:** on focus loss the game clears bit `0x02000000` of `+0xD34`.
  Every character has it in `+0x1C` (`0x16000000`). It is restored a couple
  of seconds after focus returns.
- Swallowing `WM_ACTIVATEAPP` / `WM_ACTIVATE` / `WM_KILLFOCUS` / `WM_NCACTIVATE` in
  the window procedure prevents the pause on every focus loss except the first
  one after launch, whose cause is still unknown. The bridge therefore also sets the bit back while
  unfocused.
- Ruled out as the pause source: DirectInput (the game already uses
  `DISCL_NONEXCLUSIVE|DISCL_BACKGROUND`, A interface), its `SetWindowsHookExA` hook
  (id 13 = `WH_KEYBOARD_LL`), and hidden windows (only `DIEmWin` and IME windows exist).
- `[[DDDA.exe+14D0B28]+0x68]==0 && byte[+0x6C]!=0` is another, separate pause
  (probably menus), checked in the main loop and in the sUnit update.
- Main frame function: `DDDA.exe+1DD60` (calls every manager's update).

## Gotchas
- DDDA pauses when its window loses focus (see above). The bridge handles it.
- Area coordinates are per area: leaving Gran Soren moved Y from about 700 to about 32000, and Z can jump by about 10000 at area boundaries.
- Several "registry" objects hold lists of character and status pointers, for example `[DDDA.exe+14D0380]+820`, `[DDDA.exe+14D0A44]`, `[DDDA.exe+122221C]` and `[[DDDA.exe+14D09E0]+8FC]`. They are allocator or engine lists whose order and contents change. Do not use them as party chains.
- Never write memory without sanity checks. A blind write sweep crashed the game.
- Hardware-breakpoint tools must suspend threads, clear the debug registers and drain queued debug events before detaching. Otherwise a pending single-step is re-raised in the game and crashes it.

## Tools
See [tools/recon/README.md](../tools/recon/README.md). It lists which scripts are
current and which are obsolete or wrong (several read the camera chain as the
position).

## Main camera object (validated 2026-10-01)

The game camera is a `uCameraBase` subclass, vtable `0x159AC90` (other camera
objects with vtables `0x159ABD0`/`0x159AE00` exist but hold idle defaults). It is
not reached through a static pointer yet; the bridge hooks its class instead.
MtDTI property names (from the registration function at `+D28850`) confirm `+40`
`mCameraPos` and `+60` `mTargetPos`.

| Offset | Field |
|---|---|
| `+30` | far clip |
| `+34` | near clip (32) |
| `+38` | aspect (1.778) |
| `+3C` | vertical FOV, degrees (55) |
| `+40` | mCameraPos (xyz) |
| `+50` | up vector (xyz) |
| `+60` | mTargetPos (xyz) |
| `+70` | followed character (the Arisen) |

Vtable slot 9 (`DDDA.exe+3D7180`) runs once per frame (about 60 Hz, measured with
`hwexec.py`) and computes the pose. Overwriting `+3C..+6C` right after it returns
makes the game render from that pose: the render viewport (an object with vtable
`0x1445D24` that holds the rotation rows, the position at `+40` and the frustum
half-sizes) is built from it later in the frame. When the override stops, the
game's own camera takes over again smoothly.

## Hiding models (validated 2026-10-01)

- `uModel` parts display mask: 16 dwords at `+0x110`, one bit per mesh part.
  Zero hides the model. The game does not rewrite it every frame (`mTransparency`
  at `+0x158` is rewritten every frame, so writing it does nothing).
- Models the Arisen carries point at it through `+0x30`: `uWeaponPl` (vtable
  `0x1602730`, move `0xC1B0D0`) and the lantern `uOmObj10410` (vtable `0x1603DD8`,
  move `0xC26E40`). Both move() slots run every frame. Hiding the lantern model
  leaves its light on.
- Class names: MT objects return their MtDTI from vtable slot 4
  (`mov eax, DTI; ret`); the name pointer is at `DTI+4` (`tools/recon/dtiname.py`,
  `findclass.py`, `ownedby.py`).

## Renderer (D3D9, traced 2026-10-01)

DDDA renders a light pre-pass frame of about 1000 to 2500 draws. In order:
small lookup and luminance targets; reflection and character cube or portrait
targets (64 to 256 px); the **shadow map** (1024x3072 R32F, `D3DFMT 114`, with
its own D24S8); the **G-buffer** (screen size A8R8G8B8, cleared to `00FFFFFF`,
holding normals/depth for every mesh); a second geometry pass into another screen
target; light accumulation (stencil-masked light volumes); half-resolution passes;
the **main color pass** (forward materials, sky, alpha); `StretchRect` copies and
post-processing (bloom, tone mapping at 960/480/240 px); then the **HUD** pass into
the final target, and a last full-screen draw to the back buffer.

- Main depth: a screen-size D24S8 surface (not readable as a texture).
- Mesh vertex declarations always contain a NORMAL (MT packs skinning data into
  other usages, so BLENDWEIGHT/BLENDINDICES are not a reliable "skinned" test).
  Full-screen quads and light volumes have no NORMAL.
- Per character, each mesh part has its own managed vertex buffer (pawn bodies
  are about 240 KB each). The HUD, effects and some small meshes share a 2 MB
  vertex buffer, and DDDA alternates two of them frame by frame.

## Character scenery adjust: cScrAdjust (found 2026-10-01)

Every character (uPlayer, uCmc) embeds a `cScrAdjust` (vtable `0x159704C`) at
**`+0x19C0`**. It keeps the character on DDDA's ground and out of walls ("Scr" =
scroll, MT's term for the stage geometry). MtDTI properties (offsets in cScrAdjust):

| Offset | Property | Default |
|---|---|---|
| `+0x1AC` | mIsSleep | 0 |
| `+0x1AD` | mIsGroundCheck | 1 |
| `+0x1AE` | mIsCellingAdjust | 0 |
| `+0x1AF` | mIsScrFollow | 0 |
| `+0x1B0` | mIsFallSpeedKeep | 1 |
| `+0x1B1` | mIsSlopeAdjustIgnore | 0 |
| `+0x190`/`+0x194` | mGroundCheckOffset / ...G | -20 / -20 |
| `+0xD0` | mCapsule | |
| `+0x120`/`+0x130`/`+0x140` | mOldPos / mRealPos / mResPos | |

**Experiment (pin experiment, bridge `ddda_experiment.txt` = `pin <dy>`):** pawns
pinned each frame after their move() in a formation around the Arisen. With the
adjust active, DDDA moved them vertically every frame (+70 cm/frame pushed out of
the ground, -45 cm/frame falling). Horizontally it accepted the pin. With
**`mIsSleep = 1`** on the three pawns, the drift became 0 in all axes, also when
pinned 1 m above the ground. So sleeping the scenery adjust frees a character
from DDDA's ground, walls and gravity, and its position is fully ours.

## Foot IK (found 2026-10-01)

Pawns freed from DDDA's ground (cScrAdjust asleep) knelt whenever they stood
above DDDA's ground (user screenshots/videos; confirmed by pinning them 60 cm
under and 80 cm over the ground, and by raising only the render matrix).

- char+0x2E74 -> `cPlIKCtrl` (vtable `0x15E64A4`): +0x04 owner, +0x0C..+0x18 four
  `uCnsIK` (vtable `0x1430928`), +0x2C four enable bytes, +0x7C mFBIKMasterEnable
  (rewritten every frame as `char[+0x3CF3] == 0` by code at `+764466`), +0x80
  uFullbodyIKHuman. Neither the FBIK flag nor the enable bytes caused the kneeling.
- `uCnsIK` properties: mCollisionEnable +0xC4A (already 0), mHeelOffset +0xC64,
  mHeelHeight +0xC68, mGroundDistance +0xC78 (200), mEffectorPos +0xCC0.
- **`uCnsIK` vtable slot 36 (`0xE59C70`) fits the foot onto DDDA's ground**:
  thiscall(effector*, hitCache*), `ret 8`, no return value. It moves effector.y to
  the ground hit and caches the hit (+0x60.. normal, +0x70.. position). Skipping
  it leaves the foot where the animation put it. The bridge skips it for the
  freed party's constraints.
- Rejected: raising only the model matrix (+0xA0/+0xE0 translation) reproduced the
  kneeling and sometimes made the bodies vanish.
- The debugger tools crashed DDDA once (hwexec on a function called many times per
  frame, `DDDA.exe.5532.dmp`). Prefer static analysis for hot functions.

## Open-world tiles and the floating origin (2026-10-02)

The open world (st100) is split into 100 m tiles `st100_{M}m{N}n`. Positions
(char+0x40 etc.) are tile-local: global = local + ((N-50)*10000, 0, (M-50)*10000).
Collision and waypoint formats are described in [terrain-proxy.md](terrain-proxy.md).

- `uStageSplitCtrl` (vtable `0x160CD58`, one live object, not reachable from a static
  pointer found so far; the `chain.py` hits were false positives) holds "Player Now":
  **N(x) at +0x40, M(z) at +0x44** (int32), mArea +0x48, mFNo +0x4C. Its vtable slot 8
  (`0xC5CA60`) is its per-frame move(), so it can be captured with a vtable hook like the
  characters.
- **Ordering (tools/recon/tilewatch.py):** when the Arisen crosses an edge, its local
  coordinate wraps first (e.g. z -7 -> 9993) and N/M update one sample later. For that
  short moment local + N/M is 100 m off. Track the tile from the coordinate jump itself
  (|delta local| > 5000 in one frame) and use N/M only to resync while they agree.
- The wrap happens exactly at the tile edge (no hysteresis): local stays in [0, 10000).

## Camera-occlusion fade (2026-10-02)

DDDA fades characters standing between the camera and the player. The pawn routine
(`+0x48C414`, inside move(), but the fade is also recomputed later in the frame) writes
`char+0x2514 = char+0x2510 x camera factor`; the character draw (`+0x76A070`) loads it
(`movss xmm0, [esi+0x2514]` at `+0x76A07B`) into uModel mTransparency (`+0x158`) and
sets flag `0x40000000` in `+0x108` when it is below 1. Writing +0x158 or resetting
+0x2514 after move() does not hold; the bridge patches the load at `+0x76A07B` to read a
constant 1.0 while linked (found with tools/recon/fadewatch.py and hwbp.py).

## Combat: damage and enemy classes (2026-10-06)

- The three call sites from ddda-dinput8's DamageLog (`0xAAAF78`, `0xBAA3E8`, `0xBB7245`,
  each `push ecx; movss [esp], xmm1; call 0x44B710`, target in `ebx`/`esi`) never fired
  in game 2026-10-06 (the Arisen's spells, a pawn's melee and arrows). `0x44B710` only adds
  damage to a global statistic, `[0x18FA4BC]+0xB88AC`.
- **Damage log, verified in game 2026-10-06** (plain DDDA, the user fought wolves and
  bandits with a mage, pawns melee and bow): `src/ddda_bridge/damage_log.cpp` hooks
  ApplyDamage's entry and logged 68 hits, game unaffected. Every hit, magic or physical,
  party or enemy, came through it:
  - victims: `uEm0200` (800 HP, the wolves), `uHumanEnemy` (bandits, 1000 or 1300 HP),
    `uCmc` (the party's own characters), and breakable objects (`uOmObj7515`/`7520`, 100 HP);
  - the vital block works for enemies too: `[vital+8]`/`+0xC` gave live HP falling to 0,
    and `[vital+0x1B4]+0x40` their position;
  - all hits were called from **one hit-handling function**, at `+36E27D` (all victims) and
    `+36E31B` (a second vital: `uEm0200` takes each hit twice, on two different vital blocks);
  - in that function `ebp` is the hit record: damage at `[ebp+0x7C]`. `edi+0xCCC` is
    tested against `uEnemy`'s DTI (`0x19A1130`).
- **The attacker is `[rec+0x50]`** (the same at `+0x54`), verified in game 2026-10-10 (50
  hits against bandits, plain DDDA): the Arisen's melee gave `uPlayer`, a bandit's melee on a
  pawn gave `uHumanEnemy`, bandits hitting barrels (`uOmObj7515`) gave `uHumanEnemy`. Ranged
  hits give the **shell** instead (`uShlArrow`, `uShlHoming`, `uShlBase`), from either side.
  Hits of 0.0 damage came with arg2 = 1 (probably blocked or guarded; not checked).
  The shooter inside the shell: the log copies 0x800 bytes of it at the hit. Seen in game
  2026-10-10: no direct pointer to a character in 40 shells (pawn and enemy arrows, homing
  spells). `uShl*` registers no properties of its own (only uModel's). A second run with
  pointers into known characters and one level further (46 shells) found only `shl+0x36C`
  -> `cObjCollision::NodeHitInfo` (what the shell touched, not its shooter: once a pawn,
  on an arrow that hurt the Arisen). So the shooter is not a plain pointer (maybe a handle
  or ID); parked, a fallback is enough for combat (the nearest party member shooting).
  Once, a homing spell's hit record had `rec+0x284` = uPlayer, and once a `uShlBase` that
  killed a wolf had `rec+0x98` = a pawn (`uCmc`). Third run (2026-10-10, 15 min of play,
  121 hits, 82 shells), with the party's objects from the move() hooks as known characters:
  still no fixed offset. Candidates fall at a different offset nearly every hit (Arisen at
  `+0x334`, `+0x330`, `+0x284`, `+0x6F4`; one shell pointed to the Arisen and a bandit), and the
  32 homing spells pointed to no party member. `rec+0x284` held a pawn twice and a `uEm9000`
  once. **Conclusion: the shooter is not a plain pointer in the shell; parked.** Melee: 25 of
  25 hits gave the attacker at `rec+0x50`. Other attacker classes seen: `uEm9000`,
  `uOmObj1515` (an object or trap), `uShlSpeed`, `uShlCheckConst`, `uShlLightningThunder`.
- Enemy and targeting classes (from `tools/recon/sdti.py`, DTI / vtable):
  `uEnemy` (019A1130 / 015DF2A8), `uHumanEnemy` (019A3DB4 / 015EF670),
  `cCharParamEnemy`, `sAISensorTarget` (0198AC58 / 01559DF8, the AI's target sensor),
  `cAISensorTarget`, `sLockOnManager::cLockOnTarget`, `cTargetEnemy`,
  `cLayoutSetEnemy` and `cSetInfoEnemy` (enemy placement in a layout: a lead for spawning),
  `cLinkUnitEnemy`.

## AI target list: sAISensorTarget (2026-10-10)

`[0x18D9274]` (vtable `0x1559DF8`, constructor `+0x15120`). Layout from the code:
- `+0x20` pending array (count `+0x24`, items `+0x30`): moved into the live list by the
  update (`+0x15220`, vtable slot 6), which sets bit `0x10` in each entry's `+4`.
- `+0x34` live array (count `+0x38`, items `+0x44`), sorted by group; `+0x48`: 13 dwords,
  first index of each group (-1 = empty).
- Entry: `+0x04` flags (bit 0 skipped, bit 1 active), `+0x0C` mask, `+0x44` group (0..12).
- Register: `+0x153F0` (eax = manager, entry pushed), called e.g. from `+0x1B2490`, which
  then sets bit `0x4`. Query: `+0x15460(query*)` walks the groups in a query mask and
  filters by the entry's `+0x0C`; an AI caller at `+0x457A12`.
- **Live, plain DDDA near wolves** (`tools/recon/sensortargets.py`, read-only): 272 entries.
  | Group | Entries | Class | Owner (`entry+0x58`) |
  |---|---|---|---|
  | 1 | 2 | cAISensorTargetUnit | the Arisen (`uPlayer`, twice) |
  | 2 | 3 | cAISensorTargetUnit | the pawns (`uCmc`) |
  | 3 | 10 | cAISensorTargetUnit | enemies (`uEm0200` wolves) |
  | 5 | 54 | cAISensorTargetUnit | breakable objects (`uOmObj7515/7520/11000/4510/8000`) |
  | 9 / 10 / 11 | 105 / 60 / 26 | ...StageAction / ...GeneralPoint / ...Npc | stage points, NPCs |
- Unit entry: `+0x04` flags (`0x16` active; `0x14` = bit 1 off, wolves at one spot, likely
  dead; party went `0x16` -> `0x1E` between two reads: bit 3 maybe alert or combat), `+0x08`
  13, `+0x0C` mask 7, **`+0x20` the owner's position, global** (wolf local 633/7559 ->
  10635/127568), `+0x30` a unit direction, `+0x40` 100 (radius?), `+0x44` group, `+0x4C` 1,
  `+0x50` 1000 (range?), `+0x58` owner, `+0x5C` 3.

**Hijacked enemy (built 2026-10-10, not tested):** `src/ddda_bridge/hijack.cpp`, line `hijack`
in `ddda_experiment.txt`, bridge session only. First run (2026-10-10, linked to Skyrim near
Riverwood): no enemy was loaded at all, neither at the save spot nor after the link (the
target list held only the party). So it now takes the nearest active group-3 entry's owner
at any distance, and the spot follows the Arisen (re-placed after 3 m, height set again
after 20 m), so an enemy taken at the save spot follows the party through the link.
Second run (2026-10-10): at the save spot it took a wolf 10 m away that left the active
enemies 26 ms later (never pinned; the spot of a wolf seen inactive earlier, likely a
corpse); at the link every enemy of the save area unloaded. **DDDA keeps enemies only in
the party's area, and the area mapped to Riverwood has none: stand-ins must be spawned.**
It hooks its class's move() (slot 8, vtable and code range checked), and
after each move() writes its x/z to a spot 6 m in front of the Arisen (camera -> Arisen
direction; height the Arisen's + 50 cm when placed), syncing its scenery adjust's position
copies if one is found in the object by vtable (`0x159704C`). Its AI keeps running. Log
lines `hijack:`; hits on it show in the damage log.

## Enemy placement: sSetManager (static, 2026-10-10)

Nothing public spawns enemies in DDDA (searched 2026-10-10; a Bitterblack Isle randomizer,
Nexus dragonsdogma/mods/670, edits enemy placement data inside the stage archives).
- `sSetManager` `[0x18FA504]` (vtable `0x15623D4`, constructor around `+0x9FD4F`) owns lot
  managers `cLotMgr<cLayoutSetEnemy>` (vt `0x15624B8`), `<cLayoutSetNpc>` (`0x15624D4`),
  `<cLayoutSetOm>` (`0x15624F0`) and `<cLayoutSetDynamic>` (`0x156250C`, constructor `+0xAA0B0`).
- `cLayoutSetEnemy` (vt `0x1593E30`) with `cLayoutSetEnemy::cEmArcLoad` (vt `0x1593E78`):
  an enemy's archive (model, motion) is loaded before it is placed.
  `cLayoutSetDynamic` (vt `0x1593A3C`, `cLotData` `0x1593A58`, code around `+0x35FEC9`):
  maybe the runtime path (ambushes, reinforcements); not read yet.
- `cSetInfoEnemy` (vt `0x1597258`) and per-type `cSetInfoEnemyNNNN`: placement records.
- `uEnemy`'s constructor: `+0x6A7B60`, new object in `edi`; calls the base constructor
  `+0x44A100`, then writes uEnemy's vtable (`0x15DF2A8`). 30+ enemy constructors call it.
  **Spawn log** (`src/ddda_bridge/spawn_log.cpp`, log-only, also in plain DDDA): hooks its
  entry and logs each enemy (`spawn:` with class and position 0.5 s later) and the return
  addresses found on the stack (`callers:`). **Verified in game 2026-10-10** (plain DDDA,
  the user fought): 113 enemies (69 `uEm0200` wolves, 26 `uEm0400`, a few others); 18 with a
  call chain (the other 95 stack copies failed: the 1 KB read probably crossed the end of
  the stack; read in smaller pieces next time).
- **How an enemy is created (from those chains, read statically):**
  - `+0x3613D0` (in cLayoutSetEnemy's code; esi = the layout object, `[ebp+8]` the placement
    data, `[ebp+0x10]+8` the placement record, checked to be a `cSetInfoEnemy`; `ret 0xC`)
    calls at `+0x361448`
    **`+0x33E00` = create an enemy unit**: `eax` = move line (15), `ecx` = the enemy class's
    DTI (`[layout+4]`), `edi` = where to store the new unit, stack: `[0x18FA4B0]`,
    `0x80000000`, `0x16000000` (the move mask every character has at `+0x1C`), 1, 0;
    `ret 0x14`. It checks the DTI is a uEnemy (`[0x19A1134]`), moves some enemy kinds to
    another line (a table of 37 ids, `DTI+0x1C`), calls the generic unit creation
    `+0x21FE0` and returns the new unit in `[edi]`.
  - Then the caller hands the unit to the placement record (its vfunc `+0x20`, likely
    position and setup) and copies layout fields into it (`+0x20EC`, `+0x20F8`, ...).
  - Two paths reach `+0x3613D0`: area loading through `+0x361060` (from the lot manager,
    `+0xAA2D1`/`+0xAA321`), and a "dynamic" one through `+0x35F700` and `+0x360450`
    (cLayoutSetDynamic's slot 6) called from a unit's move() (`+0x9BDABC`): enemies that
    appear during play.
  - **Setting the new enemy up** (static, 2026-10-10): the placement record's vfunc `+0x20`
    (slot 8) takes the unit (thiscall, `ret 4`), in three levels:
    - `cSetInfoEnemy` (vt `0x1597258`) slot 8 `+0x3A0000`: calls the level below, checks the
      unit is a uEnemy, calls the unit's vfunc `+0x144` with the record's position, copies
      about 20 record fields (`+0xE0..+0x107`) into the unit (`+0x2B90`, `+0x5C14..+0x5E74`,
      ...) and the position again as a home point (`+0xE50`);
    - `+0x3A4B80`: more record fields (`+0x74..+0x90`) into `+0x209E..+0x20C4`;
    - **`cSetInfoCoord`** (vt `0x15970B4`) slot 8 `+0x39F230`: `mOrder` `+0x60` -> unit `+0x38`,
      **`mPosition` `+0x30` -> unit `+0x40`**, `mAngle` `+0x40` -> rotation (`+0x9EAE30`),
      `mScale` `+0x50` -> unit `+0x60`, then the unit's vfuncs `+0x50` and `+0x54`.
      (Properties from `tools/recon/sprops.py 015970B4`: also mSetID, mName, mDrawDistance
      `+0x64`, mIsOnSplitAreaIgnore `+0x68`.)
  - After that, `+0x361970(eax = layout data, unit)` does more setup (not read).
  - Live (2026-10-10, bridge session near wolves, `tools/recon/findclass.py`): 76
    `cLayoutSetEnemy` objects (stride 0xC0; `+4` an `rLayout`, `+0x74` a `cGroupParam`), 61
    `cSetInfoEnemy0200` records (wolves; `+0x0C` the name "em0200", **`+0x30` the position in
    global coordinates**, `+0x44` an angle, `+0x50` scale 1, several fields pointing inside
    the record itself, e.g. `+0xE0` = record + 0xA0).
  - **Spawn test: the creation works** (2026-10-10 20:31, bridge session without Skyrim,
    "hold" on, save near wolves): 10 real wolves were created at load and captured; `spawn 1`
    logged `created; unit uEm0200` at local (1502, 33619, 8781), 4 m from the Arisen
    (1825, 33569, 8546): the global record position became the right local one by itself.
    It never moved and did not appear in the AI target list. Within about 30 s **every wolf,
    the 10 real ones and ours, was destroyed** (vtable back to `MtObject`, position unchanged:
    they did not fall; no hits logged). The user: in the bridge session no enemy is ever
    seen. In plain DDDA the same wolves stay. A second spawn with "hold" off (20:35) was
    destroyed within 11 s too, so "hold" is not the cause (the user then fell into the void:
    without "hold" the party stood over the overlay's ground). The overlay only replaces each
    tile's `h` collision (`arc.rebuild`, other entries kept).
  - **Destroy log** (verified in game 2026-10-10): the spawn log also hooks uEnemy's
    destructor `+0x6A8730` (thiscall; 10 bytes `53 56 8B F1 8B 8E F4 5F 00 00` replayed) and
    logs `destroy:` with class, position and callers (the stack is copied in 64-byte pieces).
    Every destruction came from sUnit's per-line loop (`+0x9B74EA`, from the main frame
    `+0x1DFD9`): a unit's state is the low 3 bits of `+4` (1 new, 2 active, 3 kill requested,
    4 dying: vfunc `+0x44` then delete). The kill request is written inline in ~80 places
    (`and eax, 0xFFFFFFFB; or eax, 3; mov [reg+4], eax`).
  - Timeline in a bridge session: tiles read from the overlay, wolves created ~5 s later,
    **all destroyed ~2 s after that**, as soon as the world ran.
  - **Cause: the overlay's generated collision** (2026-10-10, overlay files moved aside and
    the save reloaded in game, then put back): without the overlay's 22 waypoint graphs
    around the wolves they still died; without its 25 collision archives they lived (the
    original ground there). Likely DDDA removes an enemy whose placement has no matching
    ground. Ideas: generated tiles could keep DDDA's original ground under enemy
    placements, or stand-ins get a placement on the generated ground.
  - **Spawn on the original ground (20:45): the wolf lived, moved 13 m with its own AI and
    was killed by arrows from the party** (727 and 395 damage, `uShlArrow` attacker), its
    corpse removed 4 s later, the normal end of a dead enemy. So DDDA's AI treats a spawned
    enemy as a real one.
  - **Spawn while linked to Skyrim (20:54):** the record was captured at the save spot (the
    wolves there died as usual), then the link moved the party to the Riverwood mapping and
    `spawn 2` created a wolf 4 m from the Arisen, on the generated ground. It was **not
    destroyed** (40 s), state 2 (active, bit 0x400), masks `+0x18` 0x80000000 / `+0x1C`
    0x16000000 with sUnit's D30/D34 all ones, but it never moved and was not in the AI target
    list. Then DDDA showed **"Fatal error. Failed open file.
    `nativePC\sound\se\em\e02\e0200\e0200.bmse` 3"**: the wolf's resources had been released when the party left the
    wolves' area, and the new wolf asked for them. **An enemy's archive (model, sounds,
    parameters) must be loaded before it is created** (`cLayoutSetEnemy::cEmArcLoad`, vt
    `0x1593E78`).
- **Enemy archives and keeping them loaded (2026-10-10):**
  - `cEmArcLoad` (created at `+0x360EF0`, 0x18 bytes, kept in a list at `+0x80` of the
    layout owner) makes a loader at `+0x10` (0x60 bytes, vt `0x155A110`, init `+0x18DF0`):
    up to 4 entries of 0x14 at `+8` (resource, id, handle -1, ...), count `+0x54`, then
    `+0x58` = 2, `+0x5C` = 1 to start. `+0x18F20(eax = loader, id)` adds an entry; `+0x19050`
    releases them all (`+0x9E0B90` per resource = sResource vfunc `+0x38`). cEmArcLoad
    objects are temporary: none were alive in game once the wolves were loaded.
  - `cResource` properties: mPath (`+0x08`), mRefCount `+0x48`, mAttr `+0x4C`, mSize `+0x54`,
    mID `+0x58`. sResource (`[0x18D0AA0]`, vt `0x1560480`) release `+0x9BA940`: lock at `+4`,
    `--mRefCount`, unload at 0.
  - **sResource's table of loaded resources: `+0x40D8`, 16384 pointers** (read live: 6464
    entries in plain DDDA near wolves). The wolf's archive is an `rArchive` (vt `0x142E2CC`)
    with the path **`rom\enemy\em0200`** (6.1 MB, 9 references with 10 wolves); also loaded:
    `rom\enemy\em0100` (29 MB) and the pawns' chat `rom\pwnmsg\em\emNNNN_*`. The wolves do
    not point to their archive directly.
  - **Pinning (built 2026-10-10, not tested):** in a session, `spawn.cpp` scans that table
    every 2 s and gives each `rom\enemy\em*` archive one extra reference, under sResource's
    lock (up to 12; logged `spawn: pinned enemy archive`).
  - `src/ddda_bridge/spawn.cpp`: it hooks
    `+0x3613D0`'s entry (9 bytes `55 8B EC 83 E4 F0 83 EC 34`), keeps the last real call (eax
    object, layout, holder) with a copy of its record, and on each change of `spawn N` in
    `ddda_experiment.txt` (bridge session only) calls it once from the Arisen's move() with a
    copy whose `+0x30` is 4 m in front of the Arisen (local + tile origin). Log `spawn:`.
  - **Plan for a spawn test:** remember the arguments of a real call to `+0x3613D0` (layout
    object, data, record holder) for a wolf, then call it again with a copy of the record
    whose `mPosition` is next to the Arisen. The game's own code then creates and sets up
    the wolf. Not known: whether the wolf's archive must be loaded (`cEmArcLoad`; a wolf
    area has it loaded), and whether a second enemy from the same placement confuses the
    lot manager.
