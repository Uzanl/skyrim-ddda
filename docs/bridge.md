# The DDDA bridge DLL

`src/ddda_bridge/dllmain.cpp` builds as an x86 `dinput8.dll`. DDDA imports
`DINPUT8.dll!DirectInput8Create` (its only import from it), so Windows loads this
proxy from the game folder at startup. All offsets it uses are explained in
[ddda-memory.md](ddda-memory.md).

## What it does

| Feature | How | Game memory written |
|---|---|---|
| Forward input | `DirectInput8Create` goes to `%SystemRoot%\SysWOW64\dinput8.dll` | none |
| Find the party every frame | Replaces slot 8 (`move()`) of the player vtable `0x15E90D0` and the pawn vtable `0x15E8468`. The hook records `this`, then calls the original | the 2 vtable slots |
| Classify roles | Arisen = player class. Pawn slot = (`[pawn+0x3DEC]` − `[arisen+0x3DEC]` − 0x7F0) / 0x1660: 0 = main, 1–2 = hired | none |
| Publish | A bridge thread copies the captured state to shared memory every 8 ms | none |
| Keep running unfocused | (1) subclasses the game window and sends `WM_ACTIVATEAPP`/`WM_ACTIVATE`/`WM_KILLFOCUS`/`WM_NCACTIVATE` deactivations to `DefWindowProc`. (2) While unfocused, sets bit `0x02000000` of sUnit+0xD34 back on if the game cleared it | the move-mask bit, only while unfocused |
| No cross-talk with other windows | While DDDA is not the foreground window: (1) its `WH_KEYBOARD_LL` hook (installed through our `SetWindowsHookExA` IAT hook) stops receiving keys, which still pass down the hook chain; (2) `GetAsyncKeyState`/`GetKeyState` (IAT hooks) return 0; (3) DirectInput devices are forced to background/non-exclusive mode, and keyboard/mouse reads return "nothing pressed". The game creates only one DirectInput device; its keyboard comes from (1) and (2) | none (IAT entries and dinput8's COM vtables) |

### Safety rules
- Every game-memory read goes through SEH-guarded helpers. `move()` hooks run
  the capture inside `__try`, so a bad read can't crash the game.
- Before patching, it checks that both vtable slots hold the expected function
  addresses. If they don't (another game build), it hooks nothing.
- The move-mask write happens only if `[DDDA.exe+14D09E0]` has the sUnit vtable.
- `move()` runs on several worker threads, so captures go through an SRW lock.

## Shared memory protocol (v2)

Mapping `Local\DDDA_SkyrimBridge_v2`, layout `bridge::State` in
`src/common/bridge_shared.h` (184 bytes, fixed-width fields only).

- **Seqlock:** the writer makes `seq` odd, writes, then makes it even. Readers
  copy the struct and retry if `seq` was odd or changed (see
  `tools/bridge_reader/main.cpp`).
- `flags`: `kCamValid`, `kHooksActive`.
- `cam[3]`: camera position (same space as actors).
- `actors[4]`, indexed by role (`kArisen`, `kMainPawn`, `kHiredPawn1`, `kHiredPawn2`):
  - `flags`: `kActorPresent` (its `move()` ran within the last 1000 ms), `kActorHpValid`.
  - `msSinceSeen`: `0xFFFFFFFF` = never seen this session.
  - `pos[3]`: body position, DD world units (Y up, about cm). **Coordinates are per area**, and jump at area loads.
  - `hp`, `hpMax`: current and max HP (max includes augments; for example, Mariana's base is 2131 and her max is 2401).

## Log (`ddda_bridge.log` in the game folder)

Lines to look for:
- `move() hooks installed`: party capture is active. If you see `unexpected vtable contents` instead, the game build differs.
- `background mode: subclassed window ...`: focus messages are being filtered.
- `world was paused while unfocused; move mask restored`: the first-focus-loss fix triggered.
- Every 10 s, or when presence changes: swallowed-message counters, then `present=<bitmask by role> rejects=N` with each actor's position and HP.

## Verified in game (2026-10-01)
- All 4 roles classified correctly, with HP matching the in-game menu (the user confirmed values).
- The HP and pawn capture survived an area load (Gran Soren ↔ outside).
- With focus messages filtered, the game kept running while alt-tabbed on every
  focus change after the first (the user saw the pawns walking).
- The game also kept running on the first focus loss after launch. In that session
  the log showed `unpauses=0`, so message filtering alone was enough that time; the
  forced move-mask fallback is in place but has not been seen firing.
- Keyboard and mouse isolation: while alt-tabbed, typing elsewhere did not reach DDDA
  (`keysBlocked` counted 300+ keys), and controls worked normally once focused again
  (the user confirmed).

## Camera command (Skyrim/tools → DDDA)

Mapping `Local\DDDA_SkyrimBridge_cam_v2`, layout `bridge::CameraCmd`. Both sides
create-or-open it. While `flags & kCamOverride` is set and `updates` keeps
changing (stale after 500 ms), the bridge writes `pos`, `target`, `up` and
`fovY` (0 = keep DD's) into the main camera after its per-frame update (vtable
`0x159AC90` slot 9 hook). Values are sanity-checked first. The log shows
`camera hook installed` and `camera override ON/OFF (applied N frames)`.

`tools/cam_driver` (`cam_driver [seconds] [radius] [fovY]`) orbits the camera
around the Arisen. Verified 2026-10-01: the game rendered the orbit smoothly
(1188 frames in 20 s) and returned to its own camera afterwards.

## Skyrim camera sender (SKSE plugin)

`src/skse_plugin/plugin.cpp` runs a camera thread (every 2 ms) that reads Skyrim's
camera and writes `CameraCmd`:

- `PlayerCamera*` at RVA `0x31A5478` (Address Library ID 400802), `PlayerCharacter*`
  at `0x3230778` (ID 403521). The 1.7.104 Address Library is "format 5", a flat
  `uint32` table indexed by ID (`tools/recon/skyrim/addrlib.py`).
- Camera root `NiNode` at `PlayerCamera+0x20`: world rotation `+0x7C` (row-major;
  column 1 = forward, column 2 = up), world position `+0xA0`. The `NiCamera` is its
  first child (`[[root+0x118]]`), frustum `+0x150` (left, right, top, bottom;
  vertical FOV = 2·atan(top)). Player position at `+0x54`.
- Mapping: DD = DD anchor + (x, z, −y) · 100/70 relative to the Skyrim anchor. The
  anchor is taken once per load/connection: the Skyrim player's feet at that time
  equal the Arisen's feet.

- In world: the player's parent cell (`+0x60`) is non-null. It is null at the main
  menu after "quit to main menu", which SKSE reports with no message. Without a
  cell the plugin stops sending, so DDDA unlinks (the Arisen reappears and capture
  stops). Checked live: the cell was null at the menu and a `TESObjectCELL` in game.
  (`Main`+0x10..0x17 flags were identical in both states, so they are not usable.)

Verified 2026-10-01: DDDA's view followed Skyrim's camera (the user confirmed in game). The log once showed a 10 ms OFF/ON flicker of the override on the DDDA side.

## Hiding the Arisen

While a camera override is active, the bridge zeroes the parts masks of the Arisen,
its weapon and its lantern from their own move() hooks, and restores the saved
masks 500 ms after the override ends. A saved mask is only written back if the
mask is still zero, and entries are not dropped when the world pauses. Verified
2026-10-01: the Arisen disappeared when Skyrim linked and came back with weapon and
lantern after Skyrim closed; the log showed one clean ON/OFF with no flicker.

## The Arisen follows the Dragonborn (CameraCmd v2)

With `kMoveArisen`, the bridge writes `body` x/z into the Arisen's position (`+0x40`)
after every move(); DD snaps the height to its own ground. The camera height is then
shifted by (Arisen's real feet Y − `body` Y), so it keeps Skyrim's eye height above
the Arisen's real feet and does not sink into DD's ground. Verified 2026-10-01: the
pawns followed the player around Skyrim, and the camera stayed above ground.

**Fall guard (2026-10-01):** after a long walk in Skyrim, the mapped position left
DD's terrain. The Arisen and the pawns fell for minutes (y reached −950000) and
DDDA crashed; the save was not affected. `FollowTick` now tracks the last grounded
position. A drop of more than 25 cm per frame counts as falling (walking downhill
stays well below that). After 5 m of falling, the Arisen is put back on the last
grounded position and following pauses for 2 s. Logged as `Arisen was falling`.
At the edge of DD's world the party now stays behind instead of falling.
Second version (2026-10-01, after the user fell into DD's void again): the first
guard treated any frame dropping less than 25 cm as grounded. The start of a fall
is that slow, so the saved position was in mid-air, and the Arisen was put back
there and fell again 5 times a second. Now a position is grounded only after 30
frames in a row with less than 4 cm of vertical change. After a rescue the Arisen
waits at the edge until the target comes 1.5 m closer than it was (`Arisen
follows again`). Following starts only once a grounded position is known.

## Frames: DDDA → Skyrim (steps 2a/2b)

### Capture (`src/ddda_bridge/frame_capture.cpp`)
- DDDA imports `d3d9.dll!Direct3DCreate9` (plain D3D9, no D3D9Ex, so no shared
  GPU surfaces). The import is hooked, then `IDirect3D9::CreateDevice` (vtable
  slot 16), then the device's `Reset` (16) and `Present` (17).
- **The d3d9 runtime keeps device vtables in heap memory and switches the device
  to another vtable after creation**, which silently drops vtable hooks.
  `capture::Maintain()` (bridge thread, every 8 ms) re-applies them when the
  device's vtable pointer changes.
- In `Present`, while a reader keeps `readerTick` fresh (< 1 s): the back buffer
  is copied (`StretchRect` → render target → `GetRenderTargetData` → system
  memory) into the mapping. With party-only rendering on, DDDA's G-buffer is
  copied the same way (point filtered) as the mask plane.
- Capture is at full back-buffer resolution (up to 1920x1080). Measured: 60 fps
  at 960x540; full resolution not measured yet.
- `Reset` releases our `D3DPOOL_DEFAULT` surfaces first (otherwise Reset fails).

### Transport (`src/common/frame_shared.h`)
Mapping `Local\DDDA_SkyrimBridge_frame_v2`: a 4 KB header, then 3 slots with a
color plane and a mask plane each (BGRA8, up to 1920x1080). The writer fills slot
`(latest + 1) % 3` under that slot's seqlock and then publishes `latest`. Slot
flag `kSlotHasMask` means the mask plane is valid. The reader writes
`readerTick = GetTickCount()` every frame it wants frames; the writer stops
capturing after 1 s without one (Skyrim stops presenting when unfocused).

### Party-only rendering (`src/ddda_bridge/isolate.cpp`)
While linked (camera override on), every **indexed** mesh draw (vertex declaration
has a NORMAL) whose stream-0 vertex buffer is not one of the party's is skipped.
This removes terrain, buildings, NPCs and monsters. **Non-indexed** draws are
full-screen and utility quads (shadow-map clears, the sun light, fog, post-
processing) and are never skipped, except the HUD's. The HUD is recognised by its
vertex declaration: exactly POSITION float3 @0, NORMAL ubyte4n @12 (a color),
TANGENT float2 @16 (UVs).
- The first rule skipped every draw with a NORMAL, including those quads (DDDA's
  quads also carry a NORMAL). The shadow map was then never cleared, so the
  trees' shadows from before filtering stayed frozen on the pawns as leaf-like
  dapples (the user spotted it in Mariana's hair). The sun-light quad and the
  post-processing were also dropped. Fixed 2026-10-01; pending test.
- **Short unlinks keep the learned buffers** (`kForgetMs`, 2 min): the link pauses while an
  interior's arena is built, and relearning from scratch left the pawns invisible 1.7 s
  after it resumed (2026-10-03).
- **Learning the party's vertex buffers:** every 4 s the bridge records the mesh
  vertex buffers of 2 visible frames, hides the pawns and their carried models
  (parts masks), records 2 hidden frames, shows them again and records 2 more
  visible frames. It uses two frames each, because DDDA alternates two per-frame
  shared vertex buffers. A buffer drawn before and after but not while hidden is
  added; one drawn while hidden is removed; one not drawn at all keeps its status.
  Hidden frames are not published. Logged as `isolate: party vertex buffers N (+a -r ...)`.
  - The first version replaced the set each cycle. Two symptoms followed: scenery
    flashed in at world load (the camera moved between the hidden and visible
    frames), and pawns came back without clothes after being off screen (their
    buffers were not drawn during the cycle, so they were dropped). Both were
    reported in game on 2026-10-01. The accumulate plus before/after rule fixed the
    clothes. Scenery still flashed for about 5 s at world load: the first cycle ran
    0.2 s after linking, while DDDA was still streaming the place the camera had
    jumped to (before 124, hidden 55, after 160 buffers, so 52 false positives).
  - So the first cycle now waits 1.5 s after linking. A cycle is also discarded,
    and retried after 0.5 s, when its before and after sets differ by more than
    4 + 10% of the buffers (logged `learning cycle discarded`). The user confirmed this
    "improved a lot"; one flash remained. It was the first published frame:
    DDDA presents a frame that was drawn before filtering turned on.
  - So frames are published only after 2 filtered Presents (`kFilterWarmupFrames`).
    Confirmed fixed, together with the main-menu unlink.
  - Pawns still started without clothes at world load: the first cycle saw only a
    few of their meshes (another level of detail, or partly off screen), and the
    rest waited for the next periodic cycle.
  - So learning is now also on demand. Buffers drawn while hidden are remembered
    as "not the party's". A mesh buffer in neither set starts a cycle at once, at
    most one cycle per second (each cycle withholds about 9 frames). The periodic
    4 s cycle remains. Pending test.
  - 2026-10-02: the user saw the pawns hitch at regular intervals. A/B with the live
    `nolearn` switch (ddda_experiment.txt) proved it: about 10 on-demand cycles a
    minute, set off by scenery coming into view, each freezing the pawns for ~9
    frames. DDDA's vertex declarations have no BLENDINDICES/BLENDWEIGHT, so
    "skinned" could not tell characters from scenery. Now, after the first 10 s of a
    link, an unknown mesh starts a cycle only while fewer party buffers are drawn
    than in the last ~2 s (a pawn lost a mesh: another LOD, new equipment). The
    periodic cycle is every 30 s. The log line "learning cycles in the last 60 s"
    counts them. Installed 22:14; the user has not reported hitches since, but this
    has not been measured.
- **Mask:** the G-buffer is detected as the render target cleared to `00FFFFFF`
  at screen size. With only the party drawn, a pixel is a pawn when its G-buffer
  RGB is not exactly white.
- Pawn spells and particles use the shared per-frame vertex buffer, so they are
  skipped too (known gap).

### Skyrim add-on (`src/reshade_addon/addon.cpp`)
ReShade 6.8.0 add-on, `present` event, native D3D11 (from `device::get_native()`).
It uploads the newest frame (alpha from the mask) to a dynamic texture and draws
a full-screen triangle with alpha blending. Shaders are compiled at load time with
`d3dcompiler_47.dll`, and all touched pipeline state is saved and restored. The
pixel shader erodes the alpha by one texel, because DDDA's anti-aliasing blends
the pawns' outer pixels with its white background (this was the white halo).
Only masked (party-only) frames are drawn, and nothing is drawn once no new one has
arrived for 250 ms. So the overlay starts only after a save has loaded and the
camera is linked, and never shows at the main menu, on loading screens or as a
frozen frame. DDDA likewise reads frames back only while party-only rendering is on.

### Pawn labels (2026-10-03, verified in game)
DDDA's floating name, health bar and party-colour dot above each pawn are filtered out
with the rest of its HUD, so the add-on draws them itself at Skyrim's resolution
(`DrawLabels`, after the party). Data: the bridge State (positions, HP) and the
`Names` mapping (`Local\DDDA_SkyrimBridge_names_v1`, written by the DDDA bridge from
each pawn's save-data record, `[char+0x3DEC]+0x70C`, so hired pawns update by
themselves). A pawn's DD position reaches Skyrim through the last camera command: its
DD camera position is the Skyrim camera it was made from (`skyPose`). Look copied from
a DDDA screenshot: Palatino Linotype rendered with GDI into a texture (warm white,
soft shadow), a thin yellow-green bar with grey for the missing health, a dot in the
party slot's colour (main pawn red, first hired yellow, second blue). Fixed screen size
(scaled from 1080p), the bar's bottom 190 DD cm above the feet (215 until the user asked
for it nearer the head; bar 150 x 7 px at 1080p, was 230 x 6, then 150 x 9: "too thick"); fades out from 25 to 35 m and is
hidden when Skyrim's depth at the head is nearer than the head (a wall in between).

### Depth test (step 2c)
- **DDDA's G-buffer RGB is a 24-bit perspective depth** (R high byte, G, B low;
  `00FFFFFF` = far). Measured 2026-10-01 with `tools/recon/depthfit.py`: 307
  samples fit d = 0.991 - 29.8/z, which matches the camera's near plane of 32 cm
  (uCameraBase +0x34) with a very far plane (+0x30, 1.6e6). So
  z = n*f / (f - d*(f - n)), in DD centimetres; x0.7 gives Skyrim units.
- The add-on counts draws per depth-stencil view (ReShade `bind_render_targets_and_depth_stencil`,
  `draw`, `draw_indexed`). At present it copies the screen-size view with the most draws
  (Skyrim's scene depth), reads Skyrim's near/far from the NiCamera (+0x160/+0x164),
  and drops pawn pixels where Skyrim's linear depth is closer (2% + 5 units bias).
  Destroyed views are removed from the counts. **Verified in game 2026-10-01**: the
  Dragonborn and walls hide the pawns. Skyrim's depth is D24S8 (format 44), standard
  (not reversed). In some frames a 512x512 buffer had the most draws, so selection
  now only considers screen-size buffers.

### One-frame trace (`src/ddda_bridge/frame_trace.cpp`)
Create `ddda_trace_request` in the DDDA folder; the next frame's D3D9 calls
(render targets, clears, declarations, shaders, textures, vertex buffers, every
draw, `-SKIP` for filtered draws) go to `ddda_frame_trace.txt`.
`tools/recon/tracepasses.py`, `drawclass.py` and `partyvbs.py` analyse traces.

## Display latency and reprojection (2026-10-02)

A DD frame reaches Skyrim a few frames after the Skyrim camera it was rendered for,
so a moving camera made the pawns "drag", and moving characters/camera exposed DDDA's
own effects. What works now, in order of the fixes:

1. **Pose with every frame.** CameraCmd v3 carries the Skyrim camera pose (`skyPose`:
   position, 3x3 rotation, NiFrustum); the bridge re-reads the command inside the camera
   hook (no 8 ms thread delay) and the frame capture stores the pose in the slot
   (frame mapping v3, `kSlotHasPose`).
2. **Pose pairing.** DDDA renders each frame with the camera of the previous update
   (exactly one camera update per frame, measured): `posedelay 1` is the default
   (`ddda_experiment.txt` can override it: `posedelay N` or `posedelay latch`).
3. **DDDA's camera-occlusion fade** (characters between the camera and the player fade
   out) is disabled while linked by patching the character draw's fade load
   (`+0x76A07B`) to a constant 1.0 (docs/ddda-memory.md has the details).
4. **Reprojection mesh** in the ReShade add-on: each 2x2 texel cell of the DD frame is
   lifted to 3D with the DD depth in the frame's camera and projected with the current
   Skyrim camera (its own depth buffer, edge triangles discarded by a depth mismatch).
   A per-pixel inverse search was tried first: fine for rotation, but it broke up with
   camera translation (third person, walking).
5. **Under the UI.** The party is composited before the second back-buffer draw of a
   frame (Skyrim's scene copy is the first), so menus and HUD stay on top.

Live switches for diagnosis: `DDDABridge_debug.txt` next to SkyrimSE.exe (`nowarp`,
`nodepth`, `oldwarp`).
