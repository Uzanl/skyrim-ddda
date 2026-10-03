# Recon scripts

Python 3 (64-bit) scripts that read a running `DDDA.exe` (32-bit). Run them from
this folder (they import `memscan.py`). Dependencies: `numpy`, and `capstone`
for `disasm.py`. Findings are written up in [../../docs/ddda-memory.md](../../docs/ddda-memory.md).

Read-only unless noted. **Debugger** = attaches as a debugger. These use hardware
breakpoints and detach through `hwbp.safe_detach` (suspend threads, clear DRs,
drain events). An earlier detach without that crashed the game once.

## Current, useful

| Script | What it does |
|---|---|
| `memscan.py` | Core helpers (`open_proc`, `read`, `regions`). It is also a snapshot-diff float scanner (snapshots in `_data/`, about 900 MB each) |
| `chain.py ADDR...` | 1–2 level static pointer chains to an address (many false hits with odd offsets; ignore those) |
| `findvt.py [VT]` | Finds objects by vtable (default: the status object) and shows HP |
| `dumpobj.py ADDR START END` | Annotated dword dump (pointers to vtable objects, floats) |
| `disasm.py ADDR [LEN]` / `disasm.py func ADDR` | Disassembles live code (`+RVA` accepted) |
| `vtfind.py CODE VT...` | Finds the function containing CODE and whether it is a vtable slot |
| `hwbp.py SECS ADDR[=label]...` | **Debugger.** Write watchpoints (up to 4) and logs the writing EIP plus registers |
| `watchparty.py SECS MAXHP=label...` | **Debugger.** Finds party status objects by max HP and watches their HP writes |
| `hwexec.py SECS CODE [N]` | **Debugger.** Execute breakpoint, register values and a heuristic return-address chain |
| `stacksample.py SECS` | Suspend/resume sampling of the window thread's stack, grouped by focus (not a debugger) |
| `lines.py` | sUnit move-line flags (bit 1 = paused line) |
| `focusdiff.py SECS` / `focusdiff2.py` | Memory that tracks focus (static `.data` / all memory). The all-memory version is noisy |
| `fsmonitor.py SECS` | Logs changes to monitor display modes, the game window (rect/style/minimized/foreground) and game-loop activity. Written for the fullscreen black-screen issue: it predates the bridge, comes from the hybrid AMD+NVIDIA GPU, and was dropped because the project needs windowed mode anyway |
| `campos.py SECS PLAYER` | Camera chain vs body position (proved the old "position" chain is the camera) |
| `charlayout.py CHAR=label...` | Status pointers and position-like fields inside character objects |
| `names.py PAWN=Name...` | How pawn objects reach their name/save record (`+0x3DEC`, name at `+0x70C`) |
| `pawnheight.py` | Party members' body height: scale floats in the character objects and record fields that differ (found `char+0x64`) |
| `pawndiff.py MAINMAX` | Field diff main vs hired pawn (found nothing inside the object; kept for reference) |
| `camscan.py` | Every copy of the camera position, with the owning object (found the `uCameraCtrl`) |
| `propoff.py NAME...` / `proplist.py START END` | MtDTI property registrations: field offsets by property name |
| `dtiname.py VT...` | Class name of an MT object by vtable (slot 4 returns its MtDTI) |
| `findclass.py REGEX` / `ownedby.py OWNER REGEX` | Live objects by class name / models carried by an owner (`+0x30`) |
| `objrefs.py OBJ SIZE REGEX [DEPTH]` | Pointers from an object to MT objects of matching classes |
| `nearmodels.py CHAR [RADIUS] [--all]` | Objects positioned near a character |
| `poke.py OBJ VT OFF f\|u VALUE` | **Writes.** One guarded write (only if the object's vtable matches) |
| `framestat.py` | Frame transport header: frames, slots, reader heartbeat |
| `ddfps.py [SECONDS] [INTERVAL] [HITCH_MS]` | DDDA frame rate and gaps between published frames, from the frame counter (also `ddfps.log`) |
| `tracepasses.py [TRACE]` / `drawclass.py TRACE VBS` | Summarise a one-frame D3D9 trace into passes |
| `partyvbs.py` | **Writes.** Hides the pawns for two traces and diffs the vertex buffers (party meshes) |
| `sdis.py ADDR\|func\|calls\|xref\|vtref` | **Static** (reads `DDDA.exe` from disk, game not needed): disassembly, a function's calls, call/jmp xrefs, dword refs |
| `sdti.py REGEX` | **Static.** Class name to MtDTI to vtables |
| `hfdump.py` / `sbcscan.py` | Live sCollision height-field list / census of loaded collision tiles, waypoint graphs and navmeshes (read-only) |
| `sprops.py VTABLE` | **Static.** MtDTI property names and field offsets (vtable slot 3) |
| `skyrim/addrlib.py ID...` / `skyrim/sky.py` | Skyrim 1.7.104 Address Library (format 5) lookups; live Skyrim reads |

Note: `gbufstat.py`, `depthfit.py` and `../follow_sim.py` read the v2 frame/camera
mappings; since CameraCmd v3 and frame v3 (camera pose for reprojection) they no longer
find them and need updating before use. `framestat.py` was updated to frame v3 on 2026-10-03.

## Obsolete or wrong (kept as history; do not trust their output)

| Script | Problem |
|---|---|
| `readstate.py`, `chains.py` | Read `[DDDA.exe+14D1578]+DF0` as the Arisen's position, but that is the **camera**. `readstate.py` also uses the wrong HP chain `[[+14D0380]+820]` |
| `hpwatch.py`, `hplink.py` | Compare HP chain candidates, including the wrong ones. B1 (`[[[+14D09E0]+8FC]+8]`) reads an engine list, not a stable link |
| `findchars.py`, `charvt.py`, `charrefs.py`, `statusrefs.py`, `backref.py`, `blockrefs.py`, `hprefs.py` | Dead ends while looking for pawn objects. They assumed `[+14D09E0]` was the Arisen (it is sUnit) |
| `party.py`, `unitlines.py` | Tried to use `[DDDA.exe+122221C]` lists as a party list. They are allocator/engine lists that change per area |
