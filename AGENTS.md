# Working on this repository (for AI agents)

Dragon's Dogma: Dark Arisen running "inside" Skyrim: start with `README.md`.

## Keep the documentation true

Documentation that lags behind the code has already misled us: a status table claimed the
ground was "not started" when it worked, and an issue nobody had seen in days was still listed
as open. So documentation is part of every change, not a separate task.

- **Same change, same commit:** whenever something is built, changed, verified in game or
  disproved, update the docs it touches before the work counts as done:
  - `README.md` → **Status** table: one row per feature, saying what state it is in.
  - `README.md` → **Where we stopped**: the current state and the next steps in order.
    Keep only the current section. Older ones live in git history.
  - The topic doc under `docs/`: `bridge.md` (the DLL, isolate, protocol), `terrain-proxy.md`
    (tiles, streamer, live collision, navigation), `lighting.md`, `ddda-memory.md`
    (addresses and engine findings), `party-in-terrain-mode.md`, `ghosts.md`,
    `skycraft-notes.md`.
  - `Layout` in the README when files are added or moved.
- **Say how you know:**
  - "built" means it compiles;
  - "verified in game" needs the user's confirmation or a log or screenshot that shows it;
  - "not tested" stays until one of those exists.

  Write what was seen, with dates (YYYY-MM-DD).
- **Remove what is no longer true.** Delete fixed or disproved items instead of piling
  notes on top of them. If the history matters, one line in the topic doc is enough.
- **Commit and push** the docs with the code (`git push` to `origin`, private repo
  `Uzanl/skyrim-ddda`). End commit messages with the Co-Authored-By line.

## Never commit

Game files or anything extracted from them (archives, meshes, `.esm`, `tools/terrain/out*`),
binaries, `backups/`, `staged/`, logs, or local session state (`stream_config.json`,
`stream_state.json`). `.gitignore` covers these; check `git status` before committing.

## Practical rules learned the hard way

- Reply to the user in Brazilian Portuguese.
- **Read the docs before reverse engineering.** Before scanning a game's memory, tracing
  frames or writing a recon script, search `docs/` (above all `ddda-memory.md`) and
  `tools/recon/README.md` for what you need: addresses, offsets, classes and formats
  found earlier are written there. (2026-10-03: a script to find the pawns' names was
  written although `ddda-memory.md` already had them: `[char+0x3DEC]+0x70C`.)
- DLLs that a running game holds cannot be replaced. Build, back up the installed version
  under `backups/<name>/`, copy the new one to `staged/`, then start a watcher as a
  **background** PowerShell command (it waits for the game to exit, installs and checks
  the hash). Tell the user to close the game, and report when the watcher's notification
  says the file is installed.
  ```powershell
  # DDDA: build\x86\dinput8.dll next to DDDA.exe
  while (Get-Process DDDA -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 500 }; Start-Sleep 2
  Copy-Item "<repo>\staged\dinput8.dll" "E:\SteamLibrary\steamapps\common\DDDA\dinput8.dll" -Force
  (Get-FileHash "E:\SteamLibrary\steamapps\common\DDDA\dinput8.dll").Hash -eq (Get-FileHash "<repo>\build\x86\dinput8.dll").Hash
  ```
  For Skyrim, wait on `SkyrimSE` and copy `build\x64\DDDABridge.dll` and
  `build\skse\DDDAGhosts.dll` to `...\Skyrim Special Edition\Data\SKSE\Plugins\`, and
  `build\x64\DDDABridge.addon64` next to `SkyrimSE.exe`. If the game is not running, copy
  right away. Extra steps that need the game closed can go in the same watcher (for
  example deleting `stream_state.json` or editing `ddda_experiment.txt`).
- Changing the waypoint graph layout (`NODES`, `NODE_STEP` in `tools/terrain/stream.py`)
  requires deleting `stream_state.json` and regenerating with **DDDA closed**. Otherwise
  new and old graphs link to the wrong nodes.
- DDDA reads a tile archive only when the tile loads. Changes near the party need a save
  reload in DDDA.
- The streamer starts with DDDA (`ddda_streamer.txt` next to `DDDA.exe`), runs as a single
  instance and logs to `tools/terrain/stream.log`.
- Never write game memory without guards (vtable check, plausible values). Both games run
  while we work.
