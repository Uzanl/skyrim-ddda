"""Read-only: logs the party pawns' uModel mTransparency (+0x158) and mDissolveEnable
neighbourhood whenever a pawn is not fully opaque (DDDA's camera-occlusion fade?).

Usage: py fadewatch.py SECONDS
"""
import struct
import sys
import time

import numpy as np
from memscan import open_proc, regions, read

PAWN_VT = 0x15E8468
h = open_proc()
pawns = []
for b, s in regions(h):
    d = read(h, b, s)
    if not d:
        continue
    a = np.frombuffer(d[:len(d) // 4 * 4], "<u4")
    pawns += [b + int(i) * 4 for i in np.nonzero(a == PAWN_VT)[0]]
print("pawns:", " ".join(f"{p:08X}" for p in pawns), flush=True)
end = time.time() + float(sys.argv[1])
last = {}
changes = 0
while time.time() < end:
    for p in pawns:
        d = read(h, p + 0x158, 4)
        if not d:
            continue
        t, = struct.unpack("<f", d)
        if last.get(p) is None or abs(t - last[p]) > 0.02:
            print(f"{time.strftime('%H:%M:%S')} pawn {p:08X} mTransparency {t:.3f}", flush=True)
            last[p] = t
            changes += 1
    time.sleep(0.01)
print("changes:", changes)
