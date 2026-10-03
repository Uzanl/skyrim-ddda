"""Read-only check: does uStageSplitCtrl's "Player Now" tile (N at +0x40, M at +0x44)
change in the same frame as the Arisen's tile-local coordinates jump?

Usage: py tilewatch.py SECONDS        logs every change and any global discontinuity
global = local + ((N - 50) * 10000, 0, (M - 50) * 10000)
"""
import struct
import sys
import time

import numpy as np
from memscan import open_proc, regions, read

SPLIT_VT, PLAYER_VT = 0x160CD58, 0x15E90D0
h = open_proc()
found = {}
for b, s in regions(h):
    d = read(h, b, s)
    if not d:
        continue
    a = np.frombuffer(d[:len(d) // 4 * 4], "<u4")
    for vt in (SPLIT_VT, PLAYER_VT):
        i = np.nonzero(a == vt)[0]
        if len(i) and vt not in found:
            found[vt] = b + int(i[0]) * 4
split, player = found[SPLIT_VT], found[PLAYER_VT]
print(f"uStageSplitCtrl {split:08X}, uPlayer {player:08X}")

end = time.time() + float(sys.argv[1])
last = None
jumps = bad = 0
while time.time() < end:
    n, m = struct.unpack("<ii", read(h, split + 0x40, 8))
    x, y, z = struct.unpack("<3f", read(h, player + 0x40, 12))
    gx, gz = x + (n - 50) * 10000, z + (m - 50) * 10000
    if last:
        ln, lm, lgx, lgz, lx, lz = last
        if (n, m) != (ln, lm) or abs(x - lx) > 5000 or abs(z - lz) > 5000:
            jumps += 1
            step = ((gx - lgx) ** 2 + (gz - lgz) ** 2) ** 0.5
            ok = step < 300
            bad += not ok
            print(f"{time.strftime('%H:%M:%S')} tile {lm}m{ln}n -> {m}m{n}n  local ({lx:.0f},{lz:.0f}) -> ({x:.0f},{z:.0f})"
                  f"  global step {step:.0f} cm {'OK' if ok else 'DISCONTINUITY'}")
    last = (n, m, gx, gz, x, z)
    time.sleep(0.002)
print(f"done: {jumps} tile changes, {bad} discontinuities; last tile {last[1]}m{last[0]}n local ({last[4]:.0f},{last[5]:.0f})")
