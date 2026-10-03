"""Live read-only monitor for the synthetic-terrain test: party positions, height above
the synthetic plane, and distance/direction to the two hills.

Usage: py synth_watch.py [SECONDS] [INTERVAL]
"""
import json
import math
import os
import struct
import sys
import time

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "recon"))
from memscan import open_proc, regions, read  # noqa: E402

PLAYER_VT, PAWN_VT = 0x15E90D0, 0x15E8468
TILE = (64, 54)  # (m, n) of the test start; positions are local to this tile
man = json.load(open(os.path.join(os.path.dirname(__file__), "out_synth", "manifest.json")))
px, py, pz = man["pos"]
BASE = py - 50.0
HILLS = [("morro 4 m", px + 2000, pz + 1000, 400.0, 1200.0),
         ("morro 2,5 m", px - 1500, pz + 2500, 250.0, 900.0)]


def ground(gx, gz):
    return BASE + sum(a * math.exp(-((gx - hx) ** 2 + (gz - hz) ** 2) / (r * r)) for _, hx, hz, a, r in HILLS)


h = open_proc()
objs = []
for b, s in regions(h):
    d = read(h, b, s)
    if not d:
        continue
    a = np.frombuffer(d[:len(d) // 4 * 4], "<u4")
    for i in np.nonzero((a == PLAYER_VT) | (a == PAWN_VT))[0]:
        o = b + int(i) * 4
        objs.append(("Arisen" if a[i] == PLAYER_VT else "pawn", o))
objs.sort()

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 0
step = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
end = time.time() + secs
while True:
    rows = []
    for name, o in objs:
        d = read(h, o + 0x40, 12)
        if not d:
            continue
        x, y, z = struct.unpack("<3f", d)
        gx, gz = x + (TILE[1] - 50) * 10000, z + (TILE[0] - 50) * 10000
        rows.append((name, gx, y, gz))
    arisen = next((r for r in rows if r[0] == "Arisen"), None)
    print(time.strftime("%H:%M:%S"))
    for name, gx, y, gz in rows:
        extra = ""
        if arisen and name != "Arisen":
            extra = f" a {math.hypot(gx - arisen[1], gz - arisen[3]) / 100:5.1f} m do Arisen"
        print(f"  {name:7s} local ({gx - (TILE[1]-50)*10000:7.0f}, {y:7.0f}, {gz - (TILE[0]-50)*10000:7.0f})"
              f"  altura sobre o chao sintetico {(y - ground(gx, gz)) / 100:+5.2f} m"
              f"  (chao ali: {(ground(gx, gz) - BASE) / 100:4.2f} m acima da base){extra}")
    if arisen:
        for hn, hx, hz, a, r in HILLS:
            dx, dz = hx - arisen[1], hz - arisen[3]
            print(f"  {hn}: {math.hypot(dx, dz) / 100:5.1f} m, direcao (dx {dx / 100:+.0f} m, dz {dz / 100:+.0f} m)")
    if time.time() >= end:
        break
    time.sleep(step)


def camera_hint(arisen_gx, arisen_gz):
    """Turn hint from the game camera ([DDDA.exe+14D1578]+DF0) to each hill.
    DD is right-handed with Y up, so right = forward x up."""
    p = struct.unpack("<I", read(h, 0x18D1578, 4))[0]
    cx, _, cz = struct.unpack("<3f", read(h, p + 0xDF0, 12))
    cgx, cgz = cx + (TILE[1] - 50) * 10000, cz + (TILE[0] - 50) * 10000
    fx, fz = arisen_gx - cgx, arisen_gz - cgz
    for hn, hx, hz, _, _ in HILLS:
        tx, tz = hx - arisen_gx, hz - arisen_gz
        ang = math.degrees(math.atan2(fx * tz - fz * tx, fx * tx + fz * tz))
        # right vector = (-fz, fx)?  forward x up = (fx,0,fz) x (0,1,0) = (-fz, 0, fx)
        right = (tx * -fz + tz * fx) > 0
        side = "direita" if right else "esquerda"
        print(f"  {hn}: vire {abs(ang):.0f} graus para a {side} (com a camera olhando para frente)")
