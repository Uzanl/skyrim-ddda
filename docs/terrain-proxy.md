# Terrain proxy: let DDDA see Skyrim's ground natively

Goal: one engine moves the pawns (DDDA's own AI, physics and pathfinding), but
the world DDDA queries is a local copy of Skyrim's terrain around the party.
This replaces "sleep cScrAdjust and pin Y from Skyrim raycasts", which fights
DDDA every frame (spinning, kneeling, sinking, leader-unreachable warps).

Step 1 (static RE of `DDDA.exe`, 2026-10-02) is done. Tools: `tools/recon/sdis.py`
(disassembly and xrefs from disk), `sdti.py` (class name to DTI to vtable) and
`sprops.py` (MtDTI property offsets). The game does not need to run.

## Stage collision: sCollision

`sCollision` singleton: `[0x18D0DD0]` (vtable `0x142BEE4`, DTI `0x18D0E34`,
constructor `0xDF02B0`). Every scenery query goes through it:

| Caller | Call | What |
|---|---|---|
| Foot IK `uCnsIK` slot 36 (`0xE59C70`) | `0xDEA870` (thiscall on sCollision, `ret 0x10`) | Line cast (start/end segment, hit cache). 218 callers game-wide. Wraps core `0x115F650` |
| cScrAdjust update (`0x79E200`, skipped when `mIsSleep`) | `0x10AF350`, `0x10B4560`, `0x10B4710`, `0x10B4960` | Adjust-position variants (ground, walls, slopes). Result flags go to `cScrAdjust+0x1A8` (bit 8 means ceiling) |
| cScrAdjust with `mIsScrFollow` | `0x111B2B0` | Ride moving scenery |
| cScrAdjust with ceiling flag | `0x10FEA80` | Ceiling adjust |

Query kinds (vtables): ScrColInfoBasic `0x14471BC`, Find `0x14471D0`,
CastConvex `0x14471E4`, AdjustPosition `0x14471F8`, GetAreaPoly `0x1447248`,
Original `0x144720C`, PreTraverse `0x1447220`, Find4 `0x1447234` (constructors in
`0x11C2xxx`-`0x11C5xxx`).

### Two kinds of collision geometry inside sCollision

1. **Triangle meshes (SBC)**: `scr\stNNN\collision\stNNNh_NN` / `..e_NN` files
   (rCollision `0x143F780`), BVH broadphase (`cBVHCollision`, `cDynamicBVHCollision`,
   `cSbcArrayBP::cDBVTMaster`). Registered through `sCollision::SbcObject` at
   `sCollision+0x29C0` and reserve queues (`cSbcRegistReserveInfo` `0x1444B0C`,
   built by `0x1170B50`/`0x1170B80`, used by `0xFB4E60`/`0xFB4EC0`). There is also an
   online BVH builder (`cBVHCollision::cWorkBuildOnlineFast`) and `cDynamicSbc`.
   Movable owner unit: `uScrollCollisionSbc` (vtable `0x14498E8`): `SbcHandle` +0x38,
   `Pos` +0x50, `Qt` +0x60, `WMat` +0x70, `Sbc Active` (accessor property).
2. **Height fields**: `sCollision+0x3DE8` is a `cSbcHeightField` (vtable `0x1439B3C`):
   - `+0x08` count, `+0x14` array of `cHeightField*` (accessors `0xFB3940` = count,
     `0xFB3930(i)` = item).
   - `cHeightField` (vtable `0x1439B28`, 0x1C bytes, allocated by `0xFDACB0`): `+0x04`
     active byte, `+0x18` resource; `0x1004AB0` returns its `rCollisionHeightField`.
   - Resource `rCollisionHeightField` (vtable `0x1439A3C`, DTI `0x18D2AE0`, extension
     `sbch`, magic string `SBCH`). Loader is in `0xFD8xxx`-`0xFDAxxx`.
   - The core scenery query `0x1165560` loops over every **active** height field
     (callback `0x11645D0`), so a height field is seen by ground checks, walls, foot IK
     and line casts alike.

**Why this matters:** an `rCollisionHeightField` is exactly a "terrain proxy". If
we add one (or overwrite an existing one's samples) built from Skyrim heights, all
of DDDA's movement code sees Skyrim's ground natively, with no per-frame fighting.

## Navigation

- Services: `cAISvNavPathFinding` (vtable `0x143F408`, navmesh `rNavigationMesh`
  `0x143F5D8`), `cAISvWayPathFinding` (`0x143F450`, waypoint graph `rAIWayPoint`),
  `cAISvDynamicPathFinding` (`0x1447EB8`). Common interface: vtable slots 8 to 13.
- Route planner `cAIRouteInfo` (vtable `0x15790B4`), function `0x5AC6F0`: looks up the
  path service from the registry `[0x18D2470]` by DTI plus name (`"NavigationMesh"`
  `0x156D104`, `"FieldWayPoint"` `0x156D0F4`, lookup `0xF79B50`), then calls service
  slot 8 (`+0x20`) = **nearest node** for the start (`+0x0C`) and the goal (`+0x10`).
  If either is null, routing fails. This is the "leader unreachable, warp pawns to
  him" case seen in the 12 m teleport test.

## Runtime findings (read-only, 2026-10-02, open world st100, Arisen at 1523 31095 2168)

Tools: `hfdump.py` (height-field list), `sbcscan.py` (census of collision/nav resources).

- The live singleton is a subclass of sCollision (vtable `0x155B058`, likely `sCollisionExt`).
- **The height-field list is empty (count 0).** Gransys uses only SBC triangle meshes.
  A height-field proxy would have to be created, on a code path DDDA never uses
  (it is inherited from MT Framework).
- **The open world is tiled.** Files are `st100\collision\m60\marge\st100{h|e}_{col}m{row}n_mrg00`;
  a 4x4 block of tiles (cols 62-65, rows 52-55) was loaded around the Arisen.
  `h` = character/scenery hit mesh, `e` = second mesh (likely effects and shots;
  the cSplitSbc handles are `mSbcHandleScr` and `mSbcHandleEff`).
- **Tiles are 100 m (10000 units) and coordinates are tile-local.** Every tile's bbox is
  about x -1000..10600, z -600..11400, so DDDA uses a floating origin per tile (this
  explains the coordinate jumps of about 10000 at "area" boundaries). DDDA already
  has its own treadmill.
- rCollision layout: the path starts at `+0x08`, size at `+0x54`, and the SBC header
  is embedded at `+0x60` (`"SBC\xFF"`, then `+0x70` counts such as `00080026 00000DE2 00000DF0 000007A8`;
  bbox min at `+0x90`, max at `+0xA0`, buffers at `+0xC0..+0xCC`).
- **No rNavigationMesh is loaded in the open world.** Routing uses the per-tile waypoint
  graph `rAIWayPoint` (`st100\etc\st100h_{col}m{row}n_mrg00`, one per tile).
  Pawn following is therefore a waypoint-graph search; a proxy has to provide (or
  bypass) a waypoint graph too.

## SBC format (decoded 2026-10-02, tools/terrain/sbc.py)

Tile archives: `nativePC\rom\stage\stage100\split\m{C/10*10}\n{R/10*10}\st100_{C}m{R}n.arc`
(MT ARC v7, zlib; `tools/recon/arc.py` lists, extracts and rebuilds them byte-exactly).
Waypoint graphs: `...\split_way\...\st100_{C}m{R}n_way.arc`.

The full layout is in the `sbc.py` docstring. In short: header (counts, bbox, total BVH
node bytes) -> groups (0x50: bbox, id, primitive/triangle/vertex ranges) -> one
4-wide BVH ("BVHC") per group plus a top-level BVH over the groups -> triangles (32 B:
normal, 3 u16 group-relative vertex indices, flags) -> vertices (f32x4) -> surface
attributes (32 B, opaque) -> primitives (10 B: triA, triB or FFFF, 2 u16, attribute
index). BVH leaves point to primitives; a primitive is one triangle or a quad of two.
Unused BVH child slots repeat a real box.

Verified: byte-exact round trip on 18 tiles (h and e), and 36570 leaf->primitive->triangle->vertex
box checks with 0 errors. `sbcgen.py` builds a valid tile from a height grid
(opaque fields copied from a real tile's terrain group).

The player position is tile-local: the dump position (1523, 31095, 2168) matches the
terrain of tile **64m54n** exactly.

**In-game test (installed 2026-10-02):** `tools/terrain/tile_test.py` replaced tile
64m54n's `h` collision with a generated grid (2 m cells) sampled from its own terrain
and lowered by 1 m. The original is in `backups/ddda_arc/`; `py tile_test.py restore 64 54`
puts it back. **VERIFIED:** the user saw the party walking 1 m below the visible ground,
as predicted. DDDA accepts generated SBC tiles.

## Waypoint graph format (decoded 2026-10-02, tools/terrain/way.py)

From the loader `DDDA.exe+0xDFD5C0` (rAIWayPoint vtable `0x14481C4` slot 10). Packed
records: header counts, then nodes (id, pos, attrs, links {target, attrs, cost = metres,
size, height}, optional geometry, "exp" cross-tile links), then a depth-4 x/z quadtree
(85 cells, nodes in the 8x8 leaf level, Morton order with x in the low bit).

- **Coordinates are global:** for tile `st100_{m}m{n}n`, global = tile-local +
  ((n-50)*10000, 0, (m-50)*10000), so `m` is z and `n` is x. Fitted against the collision
  tiles: median height error 9 cm over 1737 nodes.
- **Exp = links into neighbouring tiles' graphs:** `dir << 16 | node`, with
  dir = (dm+1)*3 + (dn+1).
- Verified: byte-exact round trip; quadtree reproduced for 54/54 files.
  `way.from_heights()` generates a graph (8-neighbour grid on a height map, slope filter).

## Synthetic test (2026-10-02): SUCCESS

`tools/terrain/synth_test.py` replaced 9 collision tiles and 8 waypoint graphs around the
party with a plane 50 cm under the Arisen plus two hills (4 m and 2.5 m), all invisible.
Safety measures: exp links in the neighbouring original graphs that pointed into the
replaced graphs were stripped, and the generated graphs were padded to the original node
counts with isolated nodes at y=0. Result (`synth_watch.py` log):
- the whole party stood exactly on the generated ground (0 cm error);
- the pawns followed normally and climbed the 4 m hill (max +4.03 m / +4.37 m), crossing
  tiles;
- the only problem: the Arisen fell into the void when leaving the generated area at its
  west edge (the original terrain there does not match). The game recovered by itself.
  **The whole play area must be covered.**
- All 19 archives were restored afterwards.

## Riverwood and terrain mode (2026-10-02): WORKING

- `tools/terrain/skyland.py`: Tamriel heights from Skyrim.esm + Update.esm (LAND/VHGT).
- `tools/terrain/skyterrain.py`: Riverwood centre (Skyrim 22528, -43008) = DD tiles
  m61..64 x n50..53 centre (global 20000, 130000), scale 100/70, DD z = -Skyrim y; the
  riverbed is floored 1 m above DDDA's sea; 16 collision tiles + 22 graphs; it also
  writes `DDDABridge_terrain.ini` for the SKSE plugin.
- Bridge terrain mode (`kGlobalCoords`): the global <-> local tile origin comes from
  coordinate wraps plus uStageSplitCtrl N/M; only the Arisen's x/z is written; DDDA's
  physics and AI do the rest. `tools/terrain/terrain_sim.py` plays Skyrim's side.
- Results: the simulator passed (86 s); the real two-game test passed (the party
  followed the player through Riverwood). Not covered yet: houses, fences and rocks
  (only terrain).

## Streaming (2026-10-02): WORKING, with leaps at the map edges

`tools/terrain/stream.py` keeps every tile within 3 tiles of the party generated from
Skyrim's terrain (collision + fixed-layout waypoint graph, ~0.2 s per tile), reading the
party's tile from the bridge. Mapping: Riverwood anchor (horizontal) and Skyrim's lowest
terrain = DDDA sea + 1 m (vertical), so no generated ground is under DDDA's water; the
plugin's ini is written by the streamer. Only possible because DDDA reads tile archives
when they load (an archive replaced while the game runs is used next time).

**Leaps (VERIFIED 2026-10-02, real 2-game test, 3 leaps, nobody hurt):** when the party's
tile is within `LEAP_EDGE` = 2 tiles of a tile without collision (DDDA's map edge or a
hole), the streamer remaps the player's current Skyrim spot to the middle of the
deepest-inside tile at least 6 tiles away (`leap_target`), generates the 5x5 tiles
around it first (~7 s), then writes `stream_config.json` and the plugin ini. The mapping
gets a `base` (DD y of Skyrim height 0) chosen so the new ground is 3 m above the Arisen
(`LEAP_UP`; capped at `BASE_MAX`), so every leap goes UP. The plugin re-reads the ini
every 250 ms ("terrain mapping changed" in its log); the bridge sees the target jump
> 30 m (`kLeapDistance`) and runs the same protection as the link ("...: leap"), taking
the pawns up. For 20 s after a leap the streamer fills around the destination even if
the bridge still reports the old tile. Simulator: `terrain_sim.py 3 log stream east`
(the route is in Skyrim coordinates and follows mapping changes, like the plugin).
Static objects: see the next section.

## Static objects (2026-10-02): houses, fences, rocks, trunks. Built, test pending

Skyrim's placed objects become solid ground cells in the generated tiles, so DDDA's own
collision stops the pawns and its route planner goes around.

Pipeline (tools/terrain):
1. `skyobjects.py`: every REFR of Tamriel (Skyrim.esm + Update.esm, deleted ones
   skipped, Update overrides by form id) with its base's OBND, EditorID and MODL path
   -> `out/Tamriel_objects.npz` (252k objects, 3 s).
2. `bsa.py` (BSA v105 reader, LZ4 frames; `pip install lz4`) + `nif.py` (SSE NIF: header,
   NiNode tree, BSTriShape vertex data) + `meshcache.py`: surface points of the 874
   models used by obstacles -> `out/Tamriel_meshpts.npz` (8.2M points, 1 min).
   `py meshcache.py check`: mesh bounds match OBND for 284/301 models (the rest are
   OBNDs that are off in the game data, e.g. water pieces, rock piles).
3. `obstacles.py` (`pip install scipy`): which objects (STAT/MSTT, not walkable or
   decorative by EditorID, 86 cm+ tall, up to 26 m across; trees as 1 m trunk discs),
   placed with Skyrim's clockwise rotations (checked on wall chains: piece ends meet), mapped
   to DDDA, and sliced 30 cm to 3 m above the generated ground into a 25 cm raster; each
   object's outline is filled. Solid cells -> merged rectangles -> boxes in the tile's
   `h` SBC (same group, flags and attribute as the ground: DDDA tells walls by the
   normal; Gransys `h` files have walls with the same flags). Waypoint nodes within 60 cm
   of a solid cell are cut (no links, y 0 like the padding nodes) and links passing
   within 25 cm of one are dropped.
4. `stream.py`: `OBJECTS = True`; `make_height` attaches `h.obs`. A Riverwood tile:
   ~950 boxes, ~15k triangles (u16 limit 65535), 205/625 nodes cut, ~0.3 s.

Tools: `objects_test.py M N [png]` (dry run of one tile: SBC validate, graph round
trip, picture), `objects_view.py M N` (40 m close-up around the party: solid cells,
installed graphs, Arisen and pawns; north up).

History: the first version used OBND boxes. In game (16:16) the pawns stopped at a
woven fence (FenceWoven02: excluded at first for being thin and 1.5 m tall, then added),
but could not reach the player: Farmhouse04's OBND (24 x 20 m, an L-shaped house with a
porch) covered the garden the player stood in. Hence the real model shapes.

**Still open (next session):**
- Test the mesh version in game (Riverwood garden with the woven fence, houses). Not
  tested yet: reload the DDDA save so tiles are read again.
- Low decks and steps: every solid cell is at least 1 m tall (`WALL_MIN`), so a 40 cm
  porch deck in front of a door becomes a 1 m wall the pawns walk around instead of
  stepping on. Idea: cells whose object top is under ~60 cm above the ground become
  walkable floor at that height (a box with a flat top), not a wall.
- Walkways and bridges (EditorID "Walkway", "Bridge") are excluded on purpose: the pawns
  walk on the terrain under them. Making them floors needs the walkable surface as
  ground, not as walls.
- Narrow gaps (garden gates, between houses) can vanish from the 4 m waypoint grid: try
  a 2 m grid (50 x 50 nodes per graph; node limits of the WAY format not checked).
- Roofs and rocks the player climbs: the kinematic Arisen floats above the solid cells
  there; pawns may then fail to reach it.
What happens to the party (fall damage, kinematic Arisen, knocked-down pawns):
[party-in-terrain-mode.md](party-in-terrain-mode.md).

## Live Havok collision (2026-10-02 evening): WORKING, pathing being refined

Idea from SkyCraft (MIT, `skse/src/Collision.cpp`): read Skyrim's own physics instead of
rebuilding it offline.

- **Export** (`src/skse_ghosts/havok_export.cpp`, in DDDAGhosts): every attached exterior
  cell, once per session after 3 s attached, walks the fixed island of the bhkWorld
  (layers static, anim static, transparent, trees, terrain 13, ground 17 = the landscape,
  stair helper) and writes the triangles inside the cell's square (MOPP/BV trees queried
  by the cell box, compressed meshes, lists, transforms; boxes, capsules and convex hulls
  triangulated) to `Data\SKSE\Plugins\DDDA_havok\{world}_{x}_{y}.bin` (format in
  `tools/terrain/havok.py`). Riverwood: 25 cells, 8-30k triangles each, 1-6 ms, 0 faults.
- **Collision** (`stream.py` + `sbcgen.build(triangles=..., keep_cell=...)`): covered
  areas use Skyrim's triangles as they are (faced up); the .esm height grid and the object
  boxes only where no cell was exported. `sbcgen` now splits into groups of <= 20000
  triangles (u16 indices; the game's own tiles have many groups).
- **Invisible ramps** (`Live.ramps`): DDDA's characters step up less than Skyrim's (they
  pushed against a footbridge's plank edge). Every flat **built** floor (not terrain or
  ground layer, normal y >= 0.95: treads, planks, decks, roads) casts a 35-degree cone
  downwards from its cell's edge, down to 70 cm below it; the ramp is the highest cone over
  a lower floor, one quad per 25 cm cell with each corner evaluated at the corner, plus a
  quad on the step cells next to a ramp (the real plank edge lies inside that cell). A
  staircase becomes one continuous ramp. 9-42k ramp triangles per Riverwood tile.
  - 2026-10-03: the first version (one quad per detected lip, kept only where the drop beat
    the slope on both sides) rejected most stair risers, because the next step rises too.
    The pawns bounced off a Riverwood stair (26 cm risers, 50 cm treads) and stayed at the
    first step. With the cones, a check along 13 lines across that stair found no rise
    above 10 cm, and in game the main pawn and a hired pawn climbed it (trail.py; the user:
    "funcionou"). Cones from every floor covered a quarter of a tile (terrain at 25 cm is
    rough: 180-260k triangles), hence built and flat floors only.
  - Not covered: stairs steeper than about 45 degrees keep a small step at each riser;
    drops over 70 cm get no ramp on purpose (deck edges).
- **Navigation**: the node height is the floor **reachable on foot** (`Live.reach`): floor
  levels per 25 cm cell, joined with neighbours within STEP (70 cm, = ramp height) as a
  graph; levels connected to the .esm terrain seed are reachable (bridges from their ends,
  stairs, docks; not roofs, tables, wall tops). Obstacles: points 35-180 cm above that
  floor. Links are dropped where the floor jumps > 75 cm between samples 50 cm apart.
- **Dense graph**: 4 m nodes missed stairs and narrow bridges (the pawns only went up by
  "forcing" a straight line). Now 50 x 50 nodes 2 m apart (the game's graphs have up to
  ~830 nodes; ours 2500). Tested 2026-10-03 in Riverwood: the pawns crossed the narrow
  bridge "quase sem problemas" (the user); DDDA stayed at 57-60 fps. A node whose spot is tight
  (< 1 m from an obstacle or an edge) moves within its cell to the clearest spot
  (`Live.snap`): the middle of a stair, a bridge, a gate. Changing the layout needs every
  tile regenerated (delete `stream_state.json`) with DDDA closed: a new graph's exp links
  into an old-layout neighbour would point at wrong nodes.
- **Limits**: DDDA reads a tile only when it loads, so the first pass through a place can
  still use the .esm version; exported cells persist across sessions. Doors and other
  moving bodies are not exported (fixed island only). Interiors: see below.
- Tools: `py havok.py list`, `py havok.py view M N` (live floor minus .esm terrain; solid
  cells, yellow = not exported).
- `py trail.py record OUT.csv` logs the party's DD global positions at 10 Hz from the
  bridge State; `py trail.py view OUT.csv [CX CZ R]` draws the trails over the reachable
  floor and the waypoint graphs (where a pawn stops on a stair, a deck, a bridge).

## Interiors (2026-10-03, verified in game)

An interior has its own coordinates, unrelated to the world map. Mapped with the world's
mapping it sent the party to ungenerated ground (2026-10-02), so the link used to pause.

- **Export** (DDDAGhosts, `havok_export.cpp`): when the player's cell is an interior and
  has been attached 0.5 s, its whole physics world (fixed island) is written to
  `DDDA_havok/interior/{cell form id}.bin` (same DDHK v1 layout; `worldspace` = the cell's
  form id). `DDDA_havok/current.txt` says `interior {id}` or `exterior`; it is deleted at
  plugin load and on every game load, so a game closed inside an interior does not stop
  the streamer.
- **Arena** (`stream.py`, `Interiors` thread): the interior's middle is mapped to the
  middle of the deepest-inside tile far from the party and from every tile near an
  exported world cell (`plan_arena`). Its tiles get the interior's triangles plus ramps
  (`havok.InteriorLive`: one raster over the whole interior answers every query) over a
  flat catch floor 2 m under its lowest point; the 2 tiles around are catch floor only
  (ring tiles DDDA does not have stay void). Graph nodes on the catch floor are cut off.
  The base puts the lowest point 3 m above the Arisen, so the move goes up (the bridge's
  protection takes the pawns along). A house (6-15k triangles): 25 tiles in 1.9 s.
- **Mapping**: `DDDABridge_interior.ini` (`cell`, `sky`, `dd`). The plugin uses it while
  the player is in that cell; until it exists the link pauses (the party waits). Leaving:
  the plugin goes back to the world's mapping (a leap down, DDDA's own warp brings the
  pawns; immediate). The streamer removes the ini, puts the arena tiles back into its
  "to generate" set and keeps the last arena for a quick re-entry while intact.
- Measured 2026-10-03: entering took 4-6 s, of which the arena 3-6 s while the main
  loop generated a world tile in the same process (Python's interpreter lock). The arena
  is now built in a worker process started with the streamer (it stays, so numpy and
  scipy are loaded once; DDDA's job object kills it with the streamer), and the game's
  original archives are parsed once per worker (`original_part`). Offline, a 15k-triangle
  house: 1.8 s while a world tile is generated (2.4 s before); outputs byte-identical.
  Not measured in game yet.
- Limits: the 2 m graph inside a small house splits into islands around furniture (the
  pawns stay close to the Arisen there anyway); dungeons (several tiles, up to the map's
  depth of 5 tiles from an edge) and cells joined by load doors are not tested.

## Open questions

1. ~~Does Gransys use height fields?~~ No (see above).
2. Which proxy to build: (a) an `rCollisionHeightField` (unused code path, format from
   the loader at `0xFD8xxx`), or (b) an SBC mesh in the same format as the live tiles,
   placed like a tile (proven code path; the format can be checked against live tiles).
   Option (b) is preferred.
3. How SBC meshes are switched off (per-object `Sbc Active`, or remove them from the
   broadphase) so that only our height field remains near the party.
4. Navigation: generate `rAIWayPoint` graphs from Skyrim ground (grid nodes on walkable
   slope), or hook service slot 8 (nearest node) and the path query, so that a goal on
   Skyrim ground always gets a route.

## Plan

1. Done: static map (this file).
2. Runtime read-only dump of height fields, SBCs and navmesh while standing in Gransys.
3. Prototype in a fixed, empty DDDA spot: create one `rCollisionHeightField`
   (64x64 samples, 1 m), flat at first, then filled from DDDAGhosts raycasts, and
   register it. Turn DDDA's own scenery off locally. Test with one pawn and the
   player simulator before the user tests.
4. Navigation hooks.
5. Streaming window: about 128 m (Skyrim only has physics in loaded cells, about ±145 m
   with uGridsToLoad=5, so 500 m is not available).
