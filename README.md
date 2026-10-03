# Skyrim DDDA

Runs the real *Dragon's Dogma: Dark Arisen* "inside" *Skyrim SE/AE*. DDDA keeps
running as its own process and computes the Arisen, the pawns and combat.
Skyrim shows the result. The two processes talk through shared memory
(SkyCraft-style bridge).

## Status (2026-10-01)

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
| 3. Ground: pawns' feet on Skyrim's terrain | not started (pawns float or sink where the two terrains differ) |
| Combat vs Skyrim NPCs, pawn spells/effects, Rift | not started |

## Where we stopped (2026-10-02, night)

What changed this evening, and its state:

- **Lighting v2: works** (the user: "acho que foi"). v1 had hooked lights that do not light
  the world. The world lights are the weather-driven `uSky*` classes. The fixes were an
  absolute scale, the "towards the light" direction, and reconnecting to a stale light
  mapping after Skyrim reloads. [docs/lighting.md](docs/lighting.md). Shadows are not
  started (see the "Later" section of that doc).
- **Pawn hitches: fixed, not measured.** They came from the isolate learning cycles, which
  scenery set off about every 5 s. Cycles now run only when a pawn loses a mesh, plus every
  30 s. There is a live `nolearn` switch. [docs/bridge.md](docs/bridge.md), isolate.
- **Live Havok collision: works** (the user: "a colisão funcionou"). DDDAGhosts exports
  Skyrim's real static physics per loaded cell to `Data\SKSE\Plugins\DDDA_havok\`. The
  streamer builds DDDA's collision from those triangles, with invisible ramps at
  15-70 cm lips (DDDA steps up less than Skyrim). Navigation uses the floor reachable on
  foot (bridges, docks, stairs) and a 2 m graph whose nodes snap to the middle of narrow
  passages. The dense graph is **not tested**: the pawns climbed the stairs by "forcing"
  a straight line before it. Also check whether 2500-node graphs slow DDDA down.
  [docs/terrain-proxy.md](docs/terrain-proxy.md), "Live Havok collision".
- **Interiors: pause only.** Entering a house sent the party to ungenerated DDDA ground
  (flicker, knock-downs, a fall into the void). The Skyrim plugin now pauses the link
  inside interiors; the party waits outside and rejoins on the way out. Verified by the
  user: they stay outside.
- **SkyCraft** (Minecraft in Skyrim, MIT, same Skyrim build) was read and its lessons are
  written up: [docs/skycraft-notes.md](docs/skycraft-notes.md) (Havok export, hit
  pipeline IDs, damage refunding, avoid nodes).

Next, in order:
1. Test the dense graph on the stairs and the narrow bridge, and watch DDDA's performance.
2. **Pawns inside interiors**: export the interior cell's Havok, move the mapping to a
   DDDA "arena" tile (like the edge leaps), and build the ground from the interior's
   triangles only.
3. Combat, following SkyCraft's plan: Skyrim "ghost" actors at the pawns with damage
   refunded and mirrored to their HP, then DDDA stand-ins for Skyrim enemies with damage
   going through Skyrim's hit pipeline. Pawn arrows and spells must also be drawn
   (isolate keeps only party meshes).
4. Shadows, post-processing match, point lights ([docs/lighting.md](docs/lighting.md)).

**To play:**
1. Open DDDA, then Skyrim. The DDDA bridge starts the streamer by itself (hidden,
   `ddda_streamer.txt` next to DDDA.exe: line 1 the folder, line 2 the command). It is
   killed when DDDA closes, refuses to run twice, and logs to `tools/terrain/stream.log`.
   It regenerates tiles when new Skyrim cells are exported. Delete `ddda_streamer.txt`
   to start it by hand instead (`cd tools/terrain; py -u stream.py run 63 52`).
2. If the graph layout changes (NODES or NODE_STEP), delete
   `tools/terrain/stream_state.json` and regenerate **with DDDA closed**.
3. A tile DDDA already loaded is read again only after a save reload.

## Where we stopped (2026-10-02)

The pawns follow the player through Skyrim on DDDA's own physics and AI: DDDA's
open-world tiles are regenerated from Skyrim's terrain around the party (terrain mode +
streaming, [docs/terrain-proxy.md](docs/terrain-proxy.md)), and the image is reprojected
to Skyrim's camera ([docs/bridge.md](docs/bridge.md)). While linked the Arisen is
kinematic (physics off, NOT immortal): see
[docs/party-in-terrain-mode.md](docs/party-in-terrain-mode.md) for what was changed, how
to revert it for a playable Arisen, and DDDA's fall-damage rules. Leaps at the edges of
DDDA's map work (verified in the 2-game test: 3 leaps, pawns kept following, no damage).
Static objects (houses, fences, rocks, tree trunks) are generated into DDDA's collision
from the real Skyrim models: built and installed, NOT tested in game yet (the first,
box-only version was tested: fences stopped the pawns, houses' boxes were too big).
Open items are listed in [docs/terrain-proxy.md](docs/terrain-proxy.md), "Static
objects": in-game test, low decks/steps as floors, walkways, a finer waypoint grid.
To play: start the streamer first (`cd tools/terrain; py -u stream.py run 63 52`); a
background run here stops after 2 h. Then combat stand-ins.
Lighting v1 (Skyrim's sun, ambient and fog applied to DDDA's lights while linked) is
built and installed, NOT tested: [docs/lighting.md](docs/lighting.md) (switches
`nolight`, `nofog`, `lightscale X`; test without Skyrim: `tools/light_driver.py`).
Previous binaries in backups/pre_light.

## Where we stopped (2026-10-01, evening)

The pawns render inside Skyrim at full resolution. Open items, in order:

1. ~~2c depth~~ done (G-buffer = packed perspective depth; tested against Skyrim's depth).
2. **Performance**: not measured at full resolution yet (60 fps at 960x540).
   Each frame copies 2 x 8 MB (color + mask) through the CPU. Options: 1280x720,
   a 1-byte mask, or reading back only the rows/rect that contain pawns.
3. **Ground offset**: pawns stand on DD's ground; in Skyrim they float or sink.
4. **Effects**: pawn spells and particles are skipped (they come from the shared
   per-frame vertex buffer, which the filter drops), and so is the HUD.
5. The ~20 s DDDA freeze while Skyrim loads or closes is still unexplained.

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

**Play:** open DDDA and load a save, then open Skyrim through `skse64_loader.exe` and
load a save. The party links after a few seconds. Skyrim's collision is exported to
`Data\SKSE\Plugins\DDDA_havok\` as you walk around.

**Undo:** the streamer rewrites DDDA's tile archives (`nativePC\rom\stage\stage100`)
and keeps every original in `backups/ddda_arc/` first. To get plain DDDA back:
1. Delete `ddda_streamer.txt`.
2. Run `cd tools/terrain; py stream.py restore`.
3. Remove `dinput8.dll`.

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
  dllmain.cpp                  party capture, camera override, Arisen hiding/following, focus
  frame_capture.cpp            D3D9 Present hook, frame read-back into shared memory
  frame_trace.cpp              D3D9 state hooks + one-frame call trace (ddda_trace_request)
  isolate.cpp                  party-only rendering (learns the party's vertex buffers)
  relight.cpp                  Skyrim's sun, ambient and fog on DDDA's uSky* lights
src/skse_plugin/plugin.cpp   Skyrim SKSE plugin (x64): reads the party, sends Skyrim's camera and
                             lighting, pauses the link in interiors
src/skse_ghosts/             DDDAGhosts (CommonLibSSE, build_skse.bat): ground raycasts and
  havok_export.cpp             live export of Skyrim's static collision per cell
tools/terrain/               DDDA tile formats (sbc, way, arc) and the streamer (stream.py,
                             havok.py = live collision, obstacles.py = .esm fallback)
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
folder. The DLL itself never touches game files or saves, but the terrain streamer
does: it rewrites tile archives, with backups. See "Undo" above.

Only Steam build 2364871 of DDDA is supported. On any other build, the DLL
detects unexpected code and installs no game hooks; it only forwards input.

## Game settings

DDDA must run in **windowed** mode (`FullScreen=OFF` in
`%LOCALAPPDATA%\CAPCOM\DRAGONS DOGMA DARK ARISEN\config.ini`). Do not minimize
it: the game checks `IsIconic`.
