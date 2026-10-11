# Skyrim DDDA

Runs the real *Dragon's Dogma: Dark Arisen* "inside" *Skyrim SE/AE*. DDDA keeps
running as its own process and computes the Arisen, the pawns and combat.
Skyrim shows the result. The two processes talk through shared memory
(SkyCraft-style bridge).

## Status (2026-10-10)

| Step | State |
|---|---|
| DDDA-side DLL (dinput8 proxy) loads safely | done, verified in game |
| Read the party (Arisen + main pawn + 2 hired): role, position, HP | done, verified in game and across an area load |
| DDDA keeps running while unfocused, and ignores keyboard/mouse while unfocused | done, verified in game |
| 1. Skyrim reads the party (SKSE plugin, console report) | done, verified in game |
| Skyrim drives DDDA's camera (first and third person) | done, verified in game |
| Arisen hidden while Skyrim drives (body, weapon, lantern) | done, verified in game |
| The Arisen follows the Dragonborn invisibly, so the pawns follow the player | done, verified in game |
| 2a. DDDA frames shown inside Skyrim (ReShade add-on) | done, verified in game (60 fps at 960x540) |
| 2b. DDDA renders only the party; pawns drawn full screen over Skyrim | done, verified in game (screenshot); full-res capture plus halo fix verified ("resolution improved") |
| 2c. Depth test against Skyrim (walls in front of pawns) | done, verified in game |
| Reprojection to Skyrim's current camera (no drag when the camera moves, third person) | done, verified in game |
| 3. Ground: DDDA's tiles regenerated from Skyrim's terrain around the party (terrain mode + streaming), with leaps at DDDA's map edges | done, verified in game (the party follows through Riverwood and beyond) |
| Houses, fences, rocks, decks, stairs, bridges: Skyrim's live Havok collision in DDDA's tiles, continuous invisible ramps over steps and stairs | done, verified in game ("a colisão funcionou"; stairs: pawns climbed a Riverwood stair 2026-10-03, "funcionou") |
| Pathfinding on that ground: floors reachable on foot, 2 m waypoint graph centred in narrow passages | done, verified in game 2026-10-03 (narrow bridge and a stair in Riverwood; 57-60 fps) |
| Lighting: Skyrim's sun, ambient and fog on DDDA's lights | done, verified in game |
| Streamer starts and stops with DDDA (no terminal) | done, verified |
| Bridge session only (`play_bridge.bat`): DDDA from Steam stays the plain game; generated tiles in an overlay, the game's files never written | overlay verified in game 2026-10-03 (log: session started, 59 tiles read from the overlay around a link); plain start from Steam not checked in game yet |
| Interiors: the party follows inside (the interior's collision in an "arena" of DDDA's map) | done, verified in game 2026-10-03 (three Riverwood houses, "funcionou ok"); entering takes 4-6 s, leaving is immediate |
| Pawn labels in Skyrim (name, health bar, party colour, like DDDA's) | done, verified in game 2026-10-03 ("funcionou"); bar lowered, narrower, 7 px thick, 24 cm over each pawn's head joint read from the skeleton every frame (posture counts): verified in game 2026-10-03 ("está ok agora") |
| Water reacting to the pawns in Skyrim's rivers | ghosts did not make the water react (2026-10-03); direct ripples (`AddRipple` at wading pawns, Skyrim's own scale found with a spy): verified in game 2026-10-03 ("funcionou bem") (docs/ghosts.md, "Water test") |
| Pawns fall into the void when the save is away from the link spot | open bug, reported 2026-10-03; analysis in docs/party-in-terrain-mode.md |
| Shadows (Skyrim's on the pawns, the pawns' on Skyrim's ground) | not started |
| Combat vs Skyrim NPCs, pawn spells/effects, Rift | a DDDA enemy spawned next to the Arisen fights the pawns on Skyrim's ground (verified in game 2026-10-10, `spawn N` test); recon: damage log on DDDA's ApplyDamage verified in game 2026-10-06 (every hit, magic or physical, party or enemy, with victim class and HP); attacker field `[rec+0x50]` verified in game 2026-10-10 (a character for melee, a shell for arrows and spells); shell's shooter not found (parked); plan in docs/skycraft-notes.md, findings in docs/ddda-memory.md, "Combat" |

## Where we stopped (2026-10-10)

- **Combat recon (2026-10-06):** started on combat ahead of the void bug. Crash
  Logger SSE 1.25.0 is installed for Skyrim. The DDDA DLL logs every hit (`damage:`
  lines in `ddda_bridge.log`, `src/ddda_bridge/damage_log.cpp`). It only logs, so it
  also runs in DDDA from Steam. **Verified in game** (the user fought wolves and
  bandits): 68 hits, magic and physical, with the victim's class (`uEm0200` wolves,
  `uHumanEnemy` bandits, `uCmc` the party), damage, live HP and position; all through
  DDDA's ApplyDamage (`+376F50`), called from one hit function (`+36E27D`/`+36E31B`).
  - **Attacker found (verified in game 2026-10-10, 50 hits vs bandits):** the hit
    record's `+0x50` (the hit function's `ebp`) holds who hit: `uPlayer` or `uHumanEnemy`
    for melee, the projectile (`uShlArrow`, `uShlHoming`) for arrows and spells.
  - **Shooter of a projectile: not found, parked** (2026-10-10, three in-game runs, 168
    shells): no plain pointer to a character in the shell. Not needed to go on: combat can
    credit the nearest party member that is shooting.
  - Then: find the enemy list the pawns' AI reads (`sAISensorTarget`), and try to
    "hijack" a live enemy (AI frozen, moved every frame to a Skyrim NPC's mapped spot)
    as the stand-in for that NPC: do the pawns attack it?
  - Findings: [docs/ddda-memory.md](docs/ddda-memory.md), "Code addresses" and "Combat";
    plan: [docs/skycraft-notes.md](docs/skycraft-notes.md), "Combat".

- **Water: works** ("funcionou bem"). Ghosts (Skyrim actors at the pawns) followed
  the pawns but the river did not react, and with AI on they crashed Skyrim; they are off.
  Ripples are made directly where a pawn wades (`TESWaterSystem::AddRipple`) with the
  scale Skyrim uses for the player (0.01 standing, found by hooking Skyrim's own calls).
  Pawns in the river are still drawn whole over the water (no submerged part). [docs/ghosts.md](docs/ghosts.md), "Water test".
- **DDDA from Steam ignores Skyrim** (built, installed). Started without
  `play_bridge.bat`, DDDA still took the terrain link and moved the party over its own
  ground, and the pawns fell into the void. Always start it with `play_bridge.bat`.
- **Pawn labels at each pawn's head** (verified in game, "está ok agora"): the bar sits
  24 cm over the highest head joint, read from DDDA's skeleton every frame, so height,
  posture and crouching count. The skeleton is `[char+0x364]`
  ([docs/ddda-memory.md](docs/ddda-memory.md), "Skeleton").
- **Open bug (the user, parked):** if the Arisen is not where the save was made, pawns
  may fall into the void and die. Likely cause and fixes in
  [docs/party-in-terrain-mode.md](docs/party-in-terrain-mode.md), "Open bug".
- **The project no longer touches the plain game** (overlay verified in game by the log). The user
  found pawns that no longer followed in plain DDDA: the streamer had rewritten 204
  tiles in the game folder (Skyrim's collision and graphs, an invisible "map on top")
  and left "hold" on (the party's physics asleep while unlinked). All 376 archives were
  restored (byte-identical to the backups). Now generated tiles go into
  `tools/terrain/overlay/`, the DLL opens them instead of the game's only in a bridge
  session (`play_bridge.bat`), and the streamer and "hold" work only in a session.
  To check in game: `ddda_bridge.log` must show `overlay: CreateFile? opens archives`
  and `overlay: nativePC\...` lines for generated tiles; DDDA from Steam must log
  `no bridge session`. [docs/terrain-proxy.md](docs/terrain-proxy.md), "Overlay".
- **Interiors: the party follows inside** (the user: "funcionou ok", three Riverwood
  houses). DDDAGhosts exports the interior cell's collision; the streamer builds it into
  an "arena" far away in DDDA's map and writes `DDDABridge_interior.ini`; the plugin
  switches to that mapping and the party leaps in. Leaving is immediate (the world's
  ground is still there). [docs/terrain-proxy.md](docs/terrain-proxy.md), "Interiors".
  - Entering took 4-6 s: the export (0.5 s after the cell attaches), then the arena
    (3-6 s while the main loop generated a world tile in the same Python process). The
    arena now has its own worker process (offline: 1.8 s under load); not measured in
    game yet. Re-entering the same interior reuses its arena (0.1 s in a simulation).
  - Fixed on the way: pawns were dragged "like a magnet" after arriving (the protection
    held them at the Arisen's current spot each frame; now at the arrival spot), and were
    invisible 1.7 s (the isolate forgot the party's buffers when the link paused; now it
    keeps them 2 minutes).
  - A bug put one arena on Riverwood's own tiles (a streamer restarted while the player
    was inside took the old arena as the party's tile). Arenas now keep away from every
    tile the player has been near.
- **Stairs and the dense graph: work** (earlier today). Ramps are continuous cones from
  flat built floors; they cost 9-42k triangles per Riverwood tile. A large city and steep
  stone stairs are not tested.
- **Pawn labels: work** (the user: "funcionou"). The add-on draws DDDA's name, health
  bar and party-colour dot above each pawn; names come from DDDA's save records
  ([docs/bridge.md](docs/bridge.md), "Pawn labels").
- Decided 2026-10-03: navigation stays DDDA's own. Letting Skyrim's navmesh lead the
  pawns was considered and dropped.

Next, in order:
0. Combat recon: the target list is mapped (docs/ddda-memory.md, "AI target list": group 3 = enemies, 1 Arisen, 2 pawns; each entry keeps its owner's global position); the hijacked-enemy test showed that the area mapped to Riverwood has no enemies and DDDA unloads the save area's enemies at the link (2026-10-10), so stand-ins must be spawned: the spawn log (verified in game, 113 enemies) led to DDDA's enemy creation, `+0x33E00` (docs/ddda-memory.md, "Enemy placement"); the setup is read too (placement record -> position `+0x40`); spawn test (2026-10-10): DDDA's own creation made a wolf 4 m from the Arisen, but in a bridge session every enemy (the real ones too) is destroyed within ~30 s; the cause is the overlay's generated collision (without it the wolves live); on the original ground a spawned wolf lived, moved and was killed by the party's arrows (docs/ddda-memory.md, "Enemy placement"); linked to Skyrim, a spawned wolf on the generated ground was not destroyed but froze, and DDDA stopped with "Failed open file ... e0200.bmse": its archive had been unloaded with the save area; **a spawned wolf fought the party on Skyrim's ground (verified in game 2026-10-10: the pawns shot it, it bit the main pawn, it died)**, with its archive pinned; one earlier spawn was destroyed 0.5 s after creation (cause open). Next, any enemy anywhere: archives can be requested by ID and the 112 enemy kinds (12 human) are in a static table (docs/ddda-memory.md, "Loading any enemy"); the record and layout object are still copied from a real creation (first bullet above).
0b. The void bug above, parked by the user (fix 1 first: protection until the target's tiles came from the overlay).
1. Interiors, polish: measure the entry with the arena worker, larger interiors (dungeons are not tested; the graph inside a small house splits into
   islands around furniture), interior cells connected by load doors.
2. Combat, following SkyCraft's plan: Skyrim "ghost" actors at the pawns with damage
   refunded and mirrored to their HP, then DDDA stand-ins for Skyrim enemies with damage
   going through Skyrim's hit pipeline. Pawn arrows and spells must also be drawn
   (isolate keeps only party meshes). Idea from the user: Skyrim collision capsules on
   each pawn's DDDA skeleton (bones read live), so Skyrim's hits and arrows land on the
   real body; see docs/ghosts.md, "Skeleton in Skyrim". Ghosts with AI on crashed
   Skyrim: Crash Logger SSE 1.25.0 is installed now (2026-10-06); its logs go to
   `Documents\My Games\Skyrim Special Edition\SKSE\crash-*.log`.
3. Shadows, post-processing match, point lights ([docs/lighting.md](docs/lighting.md)).
   Labels could shrink with distance (they overlap when pawns are far and close together).
4. Performance at full resolution is not measured (each frame copies colour and mask, 2 x
   8 MB, through the CPU). Options: a 1-byte mask, or reading back only the pawns' rect.

**To play:**
1. Start DDDA with `play_bridge.bat` (a bridge session), then Skyrim. DDDA started from
   Steam is the plain game: no streamer, no generated tiles, no "hold". In a session the
   bridge starts the streamer by itself (hidden, `ddda_streamer.txt` next to DDDA.exe:
   line 1 the folder, line 2 the command). It is killed when DDDA closes, refuses to run
   twice, and logs to `tools/terrain/stream.log`. It regenerates tiles when new Skyrim
   cells are exported. Delete `ddda_streamer.txt` to start it by hand instead
   (`cd tools/terrain; py -u stream.py run 63 52`; DDDA still needs `play_bridge.bat`
   to read the tiles).
2. If the graph layout changes (NODES or NODE_STEP), run `py stream.py clear`
   (generated tiles and `stream_state.json`) and regenerate **with DDDA closed**.
3. A tile DDDA already loaded is read again only after a save reload.

## Setup from a fresh clone

Nothing from either game is in this repository (no archives, meshes, extracted data or
binaries). Everything is built and extracted on your own machine from your own copies.

**Needs**
- Dragon's Dogma: Dark Arisen (Steam, build 2364871) and Skyrim Special Edition **1.7.104**
  (AE runtime), with SKSE 2.3.1 and Address Library for SKSE Plugins (AE).
- Visual Studio 2022 or newer with **Desktop development with C++** (ships CMake, Ninja
  and vcpkg, used by `build_skse.bat`).
- ReShade 6.8.0 **with full add-on support**, installed for `SkyrimSE.exe` (DirectX 11).
- Python 3 (developed on 3.14) with `pip install numpy scipy lz4`.

**Steps**
1. `git clone --recursive <repo>`. This brings `external/commonlibsse-po3` (CommonLibSSE,
   powerof3 fork) and `external/reshade` (v6.8.0) as submodules. If you forgot
   `--recursive`, run `git submodule update --init`.
2. Game paths are hardcoded for `E:\SteamLibrary\steamapps\common\...`. If your games are
   elsewhere, change `DATA` in `tools/terrain/skyland.py` / `skyobjects.py`, `ROM`,
   `SKYRIM_PLUGINS` and `EXPERIMENT` in `tools/terrain/stream.py`, and `DIR` in
   `tools/terrain/havok.py`.
3. Build: run `build.bat` (the DDDA DLL, the Skyrim plugin, the ReShade add-on, test
   tools) and `build_skse.bat` (DDDAGhosts; the first run builds vcpkg dependencies, several
   minutes).
4. Install, with the games closed:
   - `build\x86\dinput8.dll` goes next to `DDDA.exe`.
   - `build\x64\DDDABridge.dll` and `build\skse\DDDAGhosts.dll` go into
     `Skyrim Special Edition\Data\SKSE\Plugins\`.
   - `build\x64\DDDABridge.addon64` goes next to `SkyrimSE.exe`.
5. Extract the Skyrim data the streamer needs (from your Skyrim.esm, Update.esm and BSAs,
   into `tools/terrain/out/`):
   ```
   cd tools/terrain
   py skyland.py       # terrain heights   -> out/Tamriel_heights.npz
   py skyobjects.py    # placed objects    -> out/Tamriel_objects.npz
   py meshcache.py     # model shapes      -> out/Tamriel_meshpts.npz (about 1 min)
   ```
6. Streamer mapping (Riverwood):
   `py stream.py config 22528 -43008 20000 130000`. It writes
   `tools/terrain/stream_config.json`; leaps at DDDA's map edges change it later.
7. Make DDDA start the streamer by itself: create `ddda_streamer.txt` next to `DDDA.exe`
   with two lines, the folder and the command, for example:
   ```
   C:\path\to\repo\tools\terrain
   "C:\path\to\pythonw.exe" -u stream.py run 63 52
   ```
8. Game settings: DDDA windowed (see "Game settings" below), Skyrim borderless windowed.

**Play:** start DDDA with `play_bridge.bat` and load a save, then open Skyrim through `skse64_loader.exe` and
load a save. The party links after a few seconds. Skyrim's collision is exported to
`Data\SKSE\Plugins\DDDA_havok\` as you walk around.

**Plain DDDA:** start it from Steam. Nothing in DDDA's folder is rewritten: generated
tiles live in `tools/terrain/overlay/` and are read only in a bridge session. To remove
the project, delete `dinput8.dll`, `ddda_streamer.txt` and `ddda_experiment.txt`.

## How to run

1. Start DDDA (Steam), load into the world.
2. Start Skyrim through `skse64_loader.exe` (borderless windowed), load a save.
   The console prints `[DD] Camera linked`. A few seconds later the pawns appear.
3. Close Skyrim to give DDDA its camera back; the Arisen reappears.

## Layout

```
src/common/bridge_shared.h   party state + camera command layouts (fixed-width; same for x86 and x64)
src/common/frame_shared.h    frame transport layout (color + mask planes)
src/ddda_bridge/             the DDDA-side DLL, built as dinput8.dll (x86):
  dllmain.cpp                  party capture, camera override, Arisen hiding/following, focus,
                               bridge session (ddda_session.txt from play_bridge.bat)
  file_overlay.cpp             in a session, DDDA's file opens get tools/terrain/overlay's copy
  frame_capture.cpp            D3D9 Present hook, frame read-back into shared memory
  frame_trace.cpp              D3D9 state hooks + one-frame call trace (ddda_trace_request)
  isolate.cpp                  party-only rendering (learns the party's vertex buffers)
  relight.cpp                  Skyrim's sun, ambient and fog on DDDA's uSky* lights
  damage_log.cpp               combat recon: logs each hit's target, damage and HP (log-only)
  hijack.cpp                   combat test: holds DDDA's nearest enemy in front of the Arisen
  spawn_log.cpp                combat recon: logs each enemy DDDA constructs and its callers (log-only)
  spawn.cpp                    combat test: re-runs DDDA's enemy creation with a record moved next to the Arisen
src/skse_plugin/plugin.cpp   Skyrim SKSE plugin (x64): reads the party, sends Skyrim's camera and
                             lighting, switches to an interior's arena mapping
src/skse_ghosts/             DDDAGhosts (CommonLibSSE, build_skse.bat): ground raycasts and
  havok_export.cpp             live export of Skyrim's static collision per cell
play_bridge.bat              starts DDDA in a bridge session
tools/terrain/               DDDA tile formats (sbc, way, arc) and the streamer (stream.py,
                             overlay.py = where generated tiles go, never the game folder,
                             havok.py = live collision, obstacles.py = .esm fallback,
                             trail.py = party trails over the floor and graphs)
src/reshade_addon/addon.cpp  Skyrim ReShade add-on (x64): draws DDDA's frames over Skyrim
external/reshade/include     ReShade 6.8.0 add-on headers (git sparse checkout)
tools/bridge_reader/         x64 console reader for the bridge (test client)
tools/cam_driver/            x64 test writer: orbits DDDA's camera around the Arisen
tools/dll_smoketest/         x86 programs that load the DLL outside the game
tools/recon/                 Python reverse-engineering scripts (see its README)
docs/bridge.md               what the DLL does and the bridge protocol
docs/ddda-memory.md          DDDA memory map and engine findings
docs/terrain-proxy.md        DDDA's ground from Skyrim (tiles, streaming, live Havok collision)
docs/lighting.md             lighting sync; docs/skycraft-notes.md: lessons from SkyCraft
build.bat                    builds everything into build/
```

## Build

Needs Visual Studio 2022 or later with the **Desktop development with C++**
workload. Run `build.bat`. The outputs are:

- `build/x86/dinput8.dll`: the bridge
- `build/x86/load_test.exe`, `build/x86/dinput_test.exe`: smoke tests (run them with DDDA closed)
- `build/x64/bridge_reader.exe [seconds]`: prints the live bridge state
- `build/x64/cam_driver.exe [seconds] [radius] [fovY]`: camera override test
- `build/x64/DDDABridge.dll`: SKSE plugin (Skyrim 1.7.104 + SKSE 2.3.1 only)
- `build/x64/DDDABridge.addon64`: ReShade add-on

## Install (Skyrim side)

- `build/x64/DDDABridge.dll` into `Skyrim Special Edition\Data\SKSE\Plugins\`
  (log: `Documents\My Games\Skyrim Special Edition\SKSE\DDDABridge.log`).
- ReShade 6.8.0 **with full add-on support** installed for `SkyrimSE.exe`
  (DirectX 10/11/12), and `build/x64/DDDABridge.addon64` next to `SkyrimSE.exe`
  (log lines in `ReShade.log`).
- Skyrim in borderless windowed mode (launcher Options: Windowed + Borderless).
  In exclusive fullscreen, alt-tab made ReShade shut down.

## Install / uninstall (DDDA side)

With DDDA closed, copy `build/x86/dinput8.dll` into the game folder
(`...\steamapps\common\DDDA\`, next to `DDDA.exe`). The DLL writes
`ddda_bridge.log` into the same folder.

To uninstall, delete `dinput8.dll` (and `ddda_bridge.log`) from the game
folder. Neither the DLL nor the streamer writes game files or saves: generated tiles
stay in `tools/terrain/overlay/`.

Only Steam build 2364871 of DDDA is supported. On any other build, the DLL
detects unexpected code and installs no game hooks; it only forwards input.

## Game settings

DDDA must run in **windowed** mode (`FullScreen=OFF` in
`%LOCALAPPDATA%\CAPCOM\DRAGONS DOGMA DARK ARISEN\config.ini`). Do not minimize
it: the game checks `IsIconic`.
