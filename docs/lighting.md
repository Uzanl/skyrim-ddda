# Lighting: Skyrim's light on the pawns

DDDA draws the pawns with its own sun, ambient light, fog and post-processing; the
frame is then composited into Skyrim. The goal is that Skyrim's lighting applies.

## Version 2 (2026-10-02 ~21:30): in game, the user thinks it works

v1 (below) had no visible effect: the pawns stayed black at Skyrim night, with or
without it. A live census (`tools/recon/findclass.py "Light|Fog"`) showed why: the world
is lit by the weather-driven subclasses **uSkyInfiniteLight** (vtable `0x160DA70`),
**uSkyHemiSphereLight** (`0x160DB40`) and **uSkyColorFog** (`0x160D3E0`), which share the
base draw functions (slot 9 = `0xE15C70`, `0xE15550`, `0xE47F80`). The base-class objects
v1 hooked are in other light groups (uLight mGroup +0x34: 0x2000, 0x80000) and do not
light the world. sWeatherManager rewrites the uSky values every frame (a poke does not
stick), so they are still written in draw.

Census at DDDA night (group: role, DDDA's own colour):
- uSkyInfiniteLight 0x10 (sky dome, ~0.08, skipped), 0x10C7 (sun; 0 at night, direction
  below the horizon), 0x10C5 (vertical fill, the ambient colour, dir (0,1,0)).
- uSkyHemiSphereLight 0x10 (sky dome, 0.1-0.15, skipped), 0x10C7 (ambient, ~0.005-0.07).
- uSkyColorFog: one object (not a uLight: no group, never filtered).

Changes: colours = Skyrim value x `lightscale` (absolute; v1's "brightest DDDA value
seen" reference broke when DDDA started at night); DDDA's direction points TOWARDS the
light (moon y > 0, sun below the horizon y < 0); the vertical fill gets Skyrim's ambient
and keeps its direction; `lightmask HEX` (default `FFFFFFEF`, all groups but 0x10); the
log lists every DDDA light with its group and own values every 10 s.

Bug found in the same test: after Skyrim reloaded a save, the bridge kept reading an old
section of the light mapping that never advanced (two sections with the same name, seen
in DDDA's memory), so the lighting went STALE and stopped. The bridge now unmaps and
reopens the mapping after 3 s stale ("relight: lighting stale, reconnecting").

## Version 1 (2026-10-02): superseded, see above

Skyrim's sun, ambient and fog are sent to DDDA, and DDDA's own lights are set to them
right before they are drawn. DDDA's renderer then lights the pawns with Skyrim's values
(its shading, normals and materials stay intact).

**Skyrim side** (`src/skse_plugin/plugin.cpp`, `LightThread`, every 100 ms), mapping
`Local\DDDA_SkyrimBridge_light_v1` (`bridge::LightCmd`, 112 bytes, seqlock):
- `RE::Sky` singleton: pointer at RVA `0x31E0D80` (Sky::GetSingleton, ID 13878 ->
  `0x1C7D00`, `mov rax, [rip+...]`). Layout from CommonLibSSE-po3 `RE/S/Sky.h`:
  `skyColor[17]` +0xA8 (TESWeather::ColorTypes: 1 fog near, 4 sunlight), `fogNear`
  +0x194, `fogFar` +0x198, `currentGameHour` +0x1B0, `directionalAmbientColors[3][2]`
  +0x200 (+X -X +Y -Y +Z -Z).
- Sun direction: Sky +0x80 `Sun*` -> +0x38 `NiDirectionalLight*` -> `worldDir` +0x140.
  The sign is normalised so the light travels downwards (the sun by day, the moon at
  night: the dominant light is above the horizon).
- Interiors (player parent cell flag bit 0): the cell's INTERIOR_DATA (XCLL, cell
  +0x60): ambient, directional and fog colours (bytes), fog near/far. Direction: fixed,
  nearly vertical.
- The plugin log prints the values every 10 s ("light: hour ...").

**DDDA side** (`src/ddda_bridge/relight.cpp`), static RE with `tools/recon/sdti.py` and
`sprops.py` (addresses with base 0x400000):

| Class | vtable | Draw (slot 9) | Fields written |
|---|---|---|---|
| uInfiniteLight (sun) | `0x142EC98` | `0xE15C70` (copies colour into a render block) | mColor +0x50 (rgb), mDir +0x230 |
| uHemiSphereLight (ambient) | `0x142EED8` | `0xE15550` (uLight's) | mColor +0x50 (sky), mRevColor +0x230 (ground) |
| uColorFog (distance fog) | `0x1430598` | `0xE47F80` (copies +0x1A0, +0x188, +0x18C...) | mColor +0x1A0, mStart +0x188, mEnd +0x18C |

Other fields found: uLight mGroup +0x34, mBalance +0x38, mMode +0x44; uHemiSphereLight
mDir +0x240; uColorFog mDistanceType +0x180, mFogColorType +0x184, mExponentDensity
+0x190, mDensity +0x194, mRangeBase +0x1B0, mDiffuseBlend +0x1B1, curves +0x1C0..+0x280.
Slot 8 is move, slot 9 draw (the hemisphere light has no move of its own). Draw runs
after all of the frame's update code, so DDDA's weather (sWeatherManager) cannot undo
the values.

**Scaling:** DDDA's and Skyrim's light ranges differ. Each DDDA light remembers the
brightest value it had this session ("DDDA daylight"); Skyrim's colour is applied
relative to a clear-day Skyrim reference (sun 0.75, ambient 0.35, fog 0.5 luminance).
So Skyrim at clear noon gives DDDA's daylight brightness, Skyrim at night is as much
darker as Skyrim says. Values DDDA rewrites are detected (current != what we wrote) and
become the new native value. Direction sign: DDDA's own vector pointing down means it
also stores the travel direction.

**Switches** (`ddda_experiment.txt`, live): `nolight` (off), `nofog` (keep DDDA's fog),
`lightscale X` (multiply the transferred light, for calibration). Only while linked.

**Logs:** DDDA `ddda_bridge.log` every 10 s: "relight: fresh draws sun N hemi N fog N |
DD native ... | refs ... | Skyrim hour ...". "draws" counting up proves the hooks run;
the native values tell DDDA's ranges for calibration.

**Test without Skyrim:** `py tools/light_driver.py cycle` publishes presets (day,
sunset, night, fog, interior; 10 s each) on the same mapping; run it with DDDA +
`tools/terrain/terrain_sim.py` (the relighting only runs while linked). Never with
Skyrim running.

**Known risks / first test checklist:**
- If DDDA's session starts at night, its "daylight" reference is the night value and the
  pawns stay dark: check the refs in the log; `lightscale` compensates; a real fix is
  to sync DDDA's clock (not found yet: sWeatherManager `0x15655D0` name, DTI
  `0x198C02C`, vtable `0x1566C00`; cZC*Param classes hold per-time light sets).
- Several uInfiniteLight objects may exist (sun plus others); all are overridden while
  linked. If something unrelated changes (e.g. lantern-like lights), restrict by object.
- Fog distances in DDDA units are assumed to be cm; if the pawns turn fog-coloured,
  `nofog`.

## Later

- **Post-processing match:** DDDA's tone mapping/bloom differ from Skyrim's; a colour
  correction in the ReShade add-on (exposure/tint) may be needed after calibration.
- **Skyrim's shadows on the pawns** (house or tree shadow): Skyrim's directional shadow
  map in the add-on, applied to the pawn pixels with their depth.
- **The pawns' shadows on Skyrim's ground:** DDDA's shadow map (1024x3072 R32F, only the
  party casts since the scenery is skipped) exported with its matrices and projected
  onto Skyrim's depth in the add-on.
- **Point lights** (torches, interiors): Skyrim's nearby lights as DDDA uPointLight.
