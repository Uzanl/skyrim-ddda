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
  spells). `uShl*` registers no properties of its own (only uModel's). Now the `shooter:`
  line also lists pointers into a known character (`&Class+off`) and one pointer further
  (`->`); built, not tested. Once, a homing spell's hit record had `rec+0x284` = uPlayer.
- Enemy and targeting classes (from `tools/recon/sdti.py`, DTI / vtable):
  `uEnemy` (019A1130 / 015DF2A8), `uHumanEnemy` (019A3DB4 / 015EF670),
  `cCharParamEnemy`, `sAISensorTarget` (0198AC58 / 01559DF8, the AI's target sensor),
  `cAISensorTarget`, `sLockOnManager::cLockOnTarget`, `cTargetEnemy`,
  `cLayoutSetEnemy` and `cSetInfoEnemy` (enemy placement in a layout: a lead for spawning),
  `cLinkUnitEnemy`.
