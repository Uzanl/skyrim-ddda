"""Read-only live tracker of the Arisen's GLOBAL position in DDDA's open world.

DDDA keeps positions tile-local (100 m tiles); crossing a tile edge shifts the local
coordinates by about 10000. The tracker polls at 20 Hz, follows those jumps from a known
start tile, and writes the state to tracker_state.json (next to this file).

Usage: py tracker.py run M N [SECONDS]   start tile (the save's tile), e.g. 64 54
       py tracker.py where GX GZ          distance and turn hint to a global target
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

STATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tracker_state.json")
PLAYER_VT = 0x15E90D0
CAM_PTR = 0x18D1578  # [DDDA.exe+14D1578]+DF0 = camera position (tile-local)


def find_player(h):
    for b, s in regions(h):
        d = read(h, b, s)
        if not d:
            continue
        a = np.frombuffer(d[:len(d) // 4 * 4], "<u4")
        idx = np.nonzero(a == PLAYER_VT)[0]
        if len(idx):
            return b + int(idx[0]) * 4
    return None


def run(m, n, secs):
    h = open_proc()
    obj = find_player(h)
    last = None
    end = time.time() + secs
    while time.time() < end:
        d = read(h, obj + 0x40, 12) if obj else None
        if not d:
            obj = find_player(h)
            time.sleep(0.5)
            continue
        x, y, z = struct.unpack("<3f", d)
        if last:
            lx, _, lz = last
            if x - lx < -5000:
                n += 1
            elif x - lx > 5000:
                n -= 1
            if z - lz < -5000:
                m += 1
            elif z - lz > 5000:
                m -= 1
        last = (x, y, z)
        cam = None
        p = read(h, CAM_PTR, 4)
        if p:
            c = read(h, struct.unpack("<I", p)[0] + 0xDF0, 12)
            if c:
                cam = struct.unpack("<3f", c)
        gx, gz = x + (n - 50) * 10000, z + (m - 50) * 10000
        state = {"t": time.time(), "tile": [m, n], "local": [x, y, z], "global": [gx, y, gz],
                 "cam_local": cam}
        tmp = STATE + ".tmp"
        json.dump(state, open(tmp, "w"))
        os.replace(tmp, STATE)
        time.sleep(0.05)


def where(tx, tz):
    s = json.load(open(STATE))
    gx, y, gz = s["global"]
    age = time.time() - s["t"]
    dx, dz = tx - gx, tz - gz
    dist = math.hypot(dx, dz) / 100
    out = f"tile {s['tile'][0]}m{s['tile'][1]}n global ({gx:.0f}, {y:.0f}, {gz:.0f}) [{age:.1f}s ago]; alvo a {dist:.0f} m"
    if s["cam_local"]:
        cx, _, cz = s["cam_local"]
        fx, fz = s["local"][0] - cx, s["local"][2] - cz
        ang = math.degrees(math.atan2(fx * dz - fz * dx, fx * dx + fz * dz))
        side = "direita" if (dx * -fz + dz * fx) > 0 else "esquerda"
        out += f"; vire {abs(ang):.0f} graus para a {side} (relativo a camera)"
    print(out)


if __name__ == "__main__":
    if sys.argv[1] == "run":
        run(int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4]) if len(sys.argv) > 4 else 3600)
    else:
        where(float(sys.argv[2]), float(sys.argv[3]))
