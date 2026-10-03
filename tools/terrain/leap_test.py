"""Feasibility test for terrain streaming ("arenas" + leaps), DDDA only (Skyrim closed).

A. Does DDDA read a tile archive replaced on disk WHILE it runs, if the tile was not
   loaded yet?  The test block gets a flat floor at a height nothing else has
   (FLOOR_Y); landing at that height proves the new file was read.
B. How does the party take a long leap? The Arisen is moved ~4 tiles away (DDDA should
   warp the pawns to it); pawn distances are logged while it waits there.

Usage: py leap_test.py build | install | restore     (install works with DDDA running)
       py leap_test.py run [LOG]                     terrain-mode bridge + save loaded
"""
import math
import os
import sys
import time

import synth_test
import terrain_sim as sim

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out_leap")
MS, NS = range(66, 69), range(44, 47)       # 3x3 tiles, all with collision and waypoints
FLOOR_Y = 41234.0
TARGET = (-45000.0, 175000.0)               # global centre of tile 67m45n


def flat(gx, gz):
    import numpy as np
    return np.full(np.shape(gx), FLOOR_Y)


def hold(body, seconds, log, label):
    t0 = time.time()
    last = 0.0
    while time.time() - t0 < seconds:
        gx, gy, gz = body
        sim.write_cmd(sim.FLAGS, (gx - 400, gy + 200, gz), (gx + 400, gy + 100, gz), body)
        now = time.time()
        if now - last >= 0.5:
            last = now
            tile, actors = sim.read_state()
            if tile and actors[0][0]:
                a = sim.to_global(tile, actors[0][1])
                d = []
                for present, p in actors[1:]:
                    if present:
                        g = sim.to_global(tile, p)
                        d.append(f"{math.hypot(g[0] - a[0], g[2] - a[2]) / 100:6.1f}")
                print(f"{label} {now - t0:5.1f}s tile {tile >> 16}m{tile & 0xFFFF}n Arisen y {a[1]:.0f} "
                      f"(target {gy:.0f}, off {(a[1] - gy) / 100:+.2f} m) xz-off "
                      f"{math.hypot(a[0] - gx, a[2] - gz) / 100:.1f} m | pawns {' '.join(d)} m",
                      file=log, flush=True)
        time.sleep(0.004)


def run(log):
    tile, actors = sim.read_state()
    if not tile or not actors[0][0]:
        raise SystemExit("no tile or no Arisen: terrain-mode bridge installed and save loaded?")
    start = sim.to_global(tile, actors[0][1])
    try:
        hold(start, 3, log, "link ")
        hold((TARGET[0], FLOOR_Y, TARGET[1]), 25, log, "leap ")
        hold(start, 12, log, "back ")
    finally:
        sim.write_cmd(0, (0, 0, 0), (0, 0, 1), (0, 0, 0))
        print("override released", file=log, flush=True)


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "build":
        synth_test.build_region(flat, MS, NS, OUT, {"floor": FLOOR_Y})
    elif cmd == "install":
        synth_test.install(OUT)
    elif cmd == "restore":
        synth_test.restore(OUT)
    elif cmd == "run":
        run(open(sys.argv[2], "w") if len(sys.argv) > 2 else sys.stdout)
