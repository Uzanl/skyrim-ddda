# Ghosts: DDDA's pawns as Skyrim actors

Status (2026-10-01): G0 verified; G1 verified (3 visible clones stood exactly where the pawns are, user screenshot), raycast ground pending test.

## Why

The pawns live in DDDA: they walk on DDDA's terrain and only DDDA's units can see
them. A **ghost** is an invisible Skyrim actor per pawn that stands where the
pawn is (mapped into Skyrim's world). It gives:

1. **Ground**: the ghost stands on Skyrim's ground, so its height tells how much
   the pawn must be raised or lowered when drawn.
2. **Being targetable**: Skyrim's enemies see, attack and hit the ghosts; hits are
   forwarded to the pawn's HP in DDDA.

(The other direction, pawns attacking Skyrim enemies, needs DDDA-side stand-ins;
see the end of this file.)

## Pieces

- `src/skse_ghosts/` — **DDDAGhosts.dll**, a second SKSE plugin built with
  CommonLibSSE (powerof3 fork, supports 1.7.104; `external/commonlibsse-po3`,
  built with `build_skse.bat`, CMake + vcpkg from Visual Studio). It is kept apart
  from the hand-written `DDDABridge.dll` (camera link), which keeps working as is.
- Shared memory (planned):
  - `anchor`: written by DDDABridge: the Skyrim and DDDA anchor points of the
    current link, so DDDAGhosts can map pawn positions into Skyrim
    (Skyrim = skyAnchor + (dx, −dz, dy) · 70/100 for a DDDA offset (dx, dy, dz)).
  - `ghosts`: written by DDDAGhosts, per pawn: the ghost's ground height (and later
    hits taken), read by the DDDA bridge.

## Milestones

| | Goal | Check |
|---|---|---|
| G0 | CommonLibSSE plugin loads on 1.7.104 | log shows runtime and player position |
| G1 | 3 visible ghosts follow the pawns (mapped positions, Skyrim ground height) | user sees stand-in NPCs where the pawns are |
| G2 | Pawns drawn at the ghosts' ground height (DDDA side: raise/lower each pawn) | feet on Skyrim's ground |
| G3 | Ghosts invisible, in the player's team; enemies attack them; damage → pawn HP | a bandit fights the pawns, their HP drops in DDDA |
| G4 | DDDA stand-ins for Skyrim enemies: pawns attack them; damage → Skyrim actor | pawns kill a Skyrim bandit |

## Findings

- G0: `DDDAGhosts loading on runtime 1.7.104.0`, player and cell read correctly.
- G1: ghosts = `PlaceObjectAtMe(player base)` clones, `SetDisplayName("DDDA Ghost")`,
  `EnableAI(false)`, `SetPosition(p, true)` each frame (an SKSE task queued from a
  16 ms thread). The logged target and actual positions matched, and the ghosts had 3D.
- The ghosts occlude the pawns through the depth test (they stand in the same
  place); invisible ghosts (G3) will stop that.
- `TES::GetLandHeight` is terrain only (the ghosts stood at -24 under Riverwood's
  plank walkway). Now a `TES::Pick` ray on layer `kPathPick` (ignores actors) is cast
  down from 150 units above the ghost, with the land height as fallback.
- Links: DDDABridge.dll publishes `Local\DDDA_SkyrimBridge_anchor_v1` (bridge::Anchor).

## Water test (2026-10-03): built, not tested in game

Question: do ghosts make Skyrim's river react to the pawns (wading ripples, splashes,
water sounds), and does that still work when they are invisible?

- Switch: `DDDAGhosts_test.txt` next to `DDDAGhosts.dll` (`Data\SKSE\Plugins`), first line
  `visible` or `invisible`; no file or anything else = off. Re-read every second, so it can
  be changed while Skyrim runs.
- Ghosts are placed every frame at the pawns' feet with the add-on's label mapping (the
  camera command's DD camera and the Skyrim camera it was made from, terrain mode only),
  not the old anchor mapping. Height: the pawn's own feet (on the river bed under water).
- Invisible = `Actor::SetAlpha(0)` (not the invisibility effect: enemies would ignore it).
- `DDDAGhosts.log` prints each ghost every 2 s with `in water` (`TESObjectREFR::IsInWater`).
- First try (21:00): the log followed the pawns (`in water true` in the river), but the
  bodies stayed where they spawned, at the door, and the water did nothing. With AI off,
  `SetPosition` moved only the reference's data.
- Second try (21:06): AI on, `Update3DPosition(true)` and the pawn's velocity on the
  character controller. Skyrim crashed about 20 s in, right after the ghosts were deleted
  when the terrain link dropped. No crash logger is installed, so the cause is unknown.
- Third try (21:2x): AI off, `SetPosition(p, true)` plus `Update3DPosition(true)`. The
  ghosts followed the pawns (the user saw them), but the water did nothing. Ghosts are
  off again (`DDDAGhosts_test.txt` = off).
- **Direct ripples (built, not tested):** no actor at all. Every update, for each pawn,
  the cell's water height at its feet (`TESObjectCELL::GetWaterHeight`, as SkyCraft reads
  it); with the feet 0-160 units under the surface, `TESWaterSystem::AddRipple(surface
  point, scale)` (AE ID 32217, exposed by CommonLibSSE): every 0.15 s at scale 1 while
  walking, every 1 s at 0.5 standing. The log says `ripple:` at the first one.
- **Ripples work** (the user's video, 2026-10-03 21:27: "funciona"), but looked odd: long
  straight parallel streaks behind the walking pawns instead of rings (the water
  simulation piles up strong, frequent ripples). Now scale 0.3 every 0.3 s walking (half
  the scale, every 1.2 s standing), tunable live with a line `ripple SCALE SECONDS` in
  `DDDAGhosts_test.txt`. Not seen yet.
- **Ripple spy (built, not run):** to copy how Skyrim ripples around the wading
  Dragonborn, DDDAGhosts finds every `call rel32` to AddRipple in SkyrimSE.exe at load and
  routes it through `LoggedAddRipple<k>` (SKSE trampoline), which passes the call on.
  With a line `spy` in `DDDAGhosts_test.txt` the log lists the call sites at load, every
  call within 300 units of the player (position, scale, time since the last one; first
  40) and a per-site summary every 2 s. If the player's wading makes no AddRipple calls,
  it goes through another path (TESWaterSystem keeps `wadingWaterData` and
  `actorsInWater` per actor).
- Also in the video: pawns standing in the river are drawn whole over the water (no
  submerged part): Skyrim's water is not in the depth the add-on tests against.

## Skeleton in Skyrim (idea, 2026-10-03)

The user's idea: give each pawn its skeleton "with physics" in Skyrim. The DDDA bridge
already reads every joint's world position each frame (`[char+0x364]`, 65-69 joints,
docs/ddda-memory.md "Skeleton"). Kinematic Havok capsules in Skyrim's world, one per
bone pair, placed from those joints, would give Skyrim a body that matches the pawn's
pose: weapons, arrows and spells hit where the pawn really is, and loose objects get
pushed. A better hit target for combat than a standing actor; it does not by itself make
an actor Skyrim's AI can target (the ghost does that). Not started. Copying DDDA's
animation onto a Skyrim skeleton was ruled out: the skeletons differ, and an invisible
body gains nothing from it.

## Neutralising DDDA's world (2026-10-01)

The user asked for the robust route first: falls into DDDA's void, DDDA walls and
cliffs, and pawns wandering after DDDA monsters all come from DDDA's world.

- Found `cScrAdjust` at char+0x19C0. With `mIsSleep` (+0x1AC) = 1, DDDA stops all
  ground, wall and gravity correction (pin experiment: drift 0, also 1 m in the
  air; the user saw the pawns floating and standing idle, no fall animation).
- Implemented (pending test): while linked, the bridge sleeps the adjust of the
  Arisen and the pawns from their own move() hooks.
  - The Arisen takes the full mapped Skyrim player position, height included.
  - Each pawn keeps DDDA's horizontal movement (AI, animations) and takes the
    height of Skyrim's ground under it. `DDDAGhosts.dll` raycasts at each mapped
    party position and publishes `Local\DDDA_SkyrimBridge_ground_v1`
    (bridge::Ground, DDDA Y per role), with no Skyrim actors spawned.
  - On unlink, everyone is put back at the Arisen's link-start position
    (formation offsets) before the adjust wakes, so nobody falls off DDDA's map.
- First test: following worked until the player went far. Then the Arisen's
  DDDA position left DDDA's navigable area, the pawns could not path to it, and
  DDDA warped them onto the Arisen every frame (all four at identical coordinates).
- **Treadmill** (pending test): the party stays within 20 m of the link-start
  position in DDDA. When the Arisen gets further, the DDDA bridge moves the whole
  party back and grows a shift (`Local\\DDDA_SkyrimBridge_shift_v1`,
  bridge::Shift). Both Skyrim plugins add the shift to the anchor's DDDA point.
  Camera commands echo the shift they used (CameraCmd shiftX/Z, formerly reserved
  fields), and DDDA corrects stale ones (`AlignToShift`), so the image does not
  jump. Logged as `treadmill: party moved back`.
- Treadmill test: the recentring worked (4 moves), but the pawns still got
  glued. The Arisen had taken Skyrim's height and gone down a hill 1.7 m *under*
  DDDA's ground; the pawns path on DDDA's navmesh and could not reach it.
- So now the Arisen's adjust stays awake: it stands on DDDA's surface (x/z ours,
  treadmill, fall guard). Only the pawns are freed. The DDDA camera uses Skyrim's
  height directly while freed, since pawns and camera must share Skyrim's height
  scale and DDDA's scenery is not drawn. Pending test.
- That version **killed the Arisen** (HP 0, 2026-10-01 22:00; nothing was saved).
  A treadmill move put it on a rock 22 m up. It fell, and the fall guard put it
  back on a ledge 50 times; the fall damage killed it. DDDA also warped the
  pawns ~230 m away.
- Current version (pending test): the Arisen's adjust sleeps too. It stays at the
  link-start height (no falls possible) within a 10 m treadmill radius of the
  link-start spot. The pawns are unchanged (DDDA AI x/z, Skyrim ground y).
  Remaining risk: if DDDA's terrain around the link-start spot is not flat, the
  pawns may fail to path to the Arisen and get glued again. The next fallback is
  Skyrim-side pathing (ghosts).
- Lesson: never let our writes move a DDDA character with physics on onto
  unknown terrain, and never let a guard repeat a fall.
- **SAFE MODE (2026-10-01, current):** `kMoveParty = false` in the bridge. The
  Arisen is not moved for Skyrim and no physics is touched. Camera, frames, depth
  and hiding still work; the pawns stay around the link-start spot. Why:
  - DDDA warps the pawns onto the Arisen whenever the Arisen jumps (the
    treadmill's recentring looks like a teleport), so the treadmill caused the
    gluing.
  - The Arisen died a second time: a "safe return" target had no ground, and the
    party fell about 100 m.
  - Lesson: never wake physics or place a character at a position not verified
    as grounded in this session.
- Kneeling: the slot-36 ground-fit hook never fired; the fit runs in
  uCnsIK::uCnsJoint slot 24. Setting uCnsIK.mGroundDistance (+0xC78) to 0 (the game
  kept it) is applied to freed pawns. It is dormant in safe mode and unconfirmed.
- Next design to try: drive the pawns through DDDA's own commands (pawn orders
  "go"/"come", or the AI's move target) instead of moving the Arisen, or find and
  disable the "warp pawns to the leader" rule.
- **Gluing experiment (2026-10-01, 22:5x):** the Arisen was teleported 12 m (1 m
  above ground) while a write watchpoint sat on a pawn's position. The pawns did
  not glue: they ran to him and arrived at different spots, and only the normal
  per-frame movement code wrote their positions (`+44D047`, `+44D09D`, `+39E95F`).
  So the gluing is DDDA's fallback when the **leader is unreachable** (off the
  navmesh: buried, on a rock top, or floating over uneven ground), not a reaction
  to the leader jumping.
- **Rule for the robust design:** the Arisen must always stand on DDDA's
  navigable ground with physics on. Treadmill moves must only target a ground
  point verified this session (where the link started, after steady frames),
  moving the whole party together. Pawns keep physics on (no kneeling); their
  Skyrim-vs-DDDA height difference is handled on the Skyrim side (ghosts).
- Investigated but not needed yet: `cCmcGoto` (one live instance; fields 200 and
  50 at +0x44/+0xB8), `cAIActionInterfaceNpcSetGotoTarget(Ex)` (22/7 instances),
  `cAILevelAbstractFSM::cSetTargetPos`. Characters have no virtual pad
  (`mControlPad` belongs to uFreeCamera).
- **Robust follow (built 2026-10-01 ~23:00, pending test), `kMoveParty = true`:**
  - The Arisen keeps DDDA physics; only its x/z follow the player.
  - Home = the first grounded position (30 steady frames) after linking. Beyond
    10 m from home, the party is moved back to home: the Arisen lands 50 cm above
    home's ground, and the pawns shift by the same x/z delta and land at the same
    height. The Shift mapping keeps Skyrim's view continuous.
  - Fall guard: 3 m limit; a second rescue within 10 s goes to home; the party
    waits at the edge.
  - Nobody's scenery adjust or IK is touched any more; there is no teleport on
    unlink.
  - The camera height is relative to the Arisen's feet again.
- **Robust follow test (23:08 video):** the pawns followed through Riverwood as a
  group, with no gluing and no falls (12 treadmill moves). They sank where
  Skyrim's road rose, because they stood on DDDA's flat ground around home.
- **Next build (pending test):** the Arisen stays grounded. Pawns are freed again
  while linked (adjust asleep plus foot-IK `mGroundDistance` = 0, restored on
  unlink after placing them around the Arisen) and take Skyrim's ground height
  (raycast). The camera uses Skyrim's height while the party moves.
- **23:21 video:** with pawns at Skyrim height they followed, but often far
  ahead or behind ("confused"). The 10 m treadmill moved the party every 2-4 s
  while running (14 moves). Positions are shifted, but the pawn AI keeps its
  destination and path in absolute DDDA coordinates, so after each move the
  pawns run toward a stale point. The radius is now 30 m, so moves are rarer
  (pending test). A real fix would also shift the AI's targets on each move.
- **23:29, 30 m radius: home-fall loop.** The fall guard put the Arisen back at
  home (1523, 31095, 2168) about 4 times a second, and each time it fell about
  7 m (to y 30420). Home's support was probably a structure whose collision was
  not loaded after the Arisen left. The pawns were left about 80 m away ("far
  away" in Skyrim), and the camera jumps made the party-learning cycles fail
  (flicker). HP was intact. **Back to SAFE MODE** (`kMoveParty = false`).
- **Decision:** stop iterating live in the user's game. Build a player-simulator
  harness (a tool writes CameraCmd with scripted player motion: circles, long
  runs, hills) so DDDA-side following can be tested and logged alone. Only bring
  the user in once following is stable there. Also verify home's ground before
  using it, and debounce rescues (never repeat a rescue to the same spot).
- Still open: pawns are not blocked by Skyrim's walls (needs ghosts with
  collision), and DDDA monsters near the party's real DDDA position.

## Open questions

- Ghost base form: no ESP yet, so either a runtime copy of a vanilla NPC or a
  vanilla NPC with factions changed at runtime. It must not be invisible to AI
  (invisibility makes enemies ignore it); it uses alpha 0 instead.
- Moving ghosts: teleport per frame (simple; ignores Skyrim collision) or drive
  them with AI/velocity (keeps them out of walls, but they lag the pawn).
- Raising/lowering a pawn in DDDA: write its position (DDDA physics may fight it)
  or offset only its render transform.
