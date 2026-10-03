"""Close-up of the generated obstacles, waypoint graph and party around a point.

Usage: py objects_view.py M N            (party position from the running bridge)
       py objects_view.py M N X Z        (tile-local point)
-> out/view.png: 20 px per metre, 40 m across. Red = boxes, green = graph nodes,
yellow = cut nodes, blue = links (read from the INSTALLED graphs), white = Arisen,
cyan = pawns. North (Skyrim) is up.
"""
import ctypes as C
import os
import struct
import sys

import numpy as np


import objects_test
import stream
import arc  # noqa: E402  (tools/recon, put on the path by stream)
import way

S = 20       # px per metre
SPAN = 4000  # cm


def party():
    stream.party_tile()
    raw = C.string_at(stream._state_view, 184)
    tile, = struct.unpack_from("<I", raw, 52)
    out = []
    for r in range(4):
        x, y, z = struct.unpack_from("<3f", raw, 56 + r * 32 + 8)
        out.append(((tile & 0xFFFF) - 50) * stream.TILE + x)
        out[-1] = (out[-1], ((tile >> 16) - 50) * stream.TILE + z)
    return out


def main():
    m, n = int(sys.argv[1]), int(sys.argv[2])
    actors = party()
    if len(sys.argv) > 4:
        cx = (n - 50) * stream.TILE + float(sys.argv[3])
        cz = (m - 50) * stream.TILE + float(sys.argv[4])
    else:
        cx, cz = actors[0]
    H = stream.make_height(stream.load_config())
    size = int(SPAN / 100 * S)
    x0, z0 = cx - SPAN / 2, cz - SPAN / 2
    xs = x0 + (np.arange(size) + 0.5) * 100 / S
    zs = z0 + (np.arange(size) + 0.5) * 100 / S
    X, Z = np.meshgrid(xs, zs)
    Y = H(X, Z)
    img = np.repeat(((Y - Y.min()) / max(1.0, np.ptp(Y)) * 100 + 50)[..., None], 3, 2).astype(np.uint8)
    img[H.obs.blocked(X, Z, margin=0.0)] = (200, 60, 60)

    def px(x, z):
        return int((x - x0) / 100 * S), int((z - z0) / 100 * S)

    def dot(x, z, col, r=2):
        i, j = px(x, z)
        if 0 <= i < size and 0 <= j < size:
            img[max(0, j - r):j + r + 1, max(0, i - r):i + r + 1] = col

    for tm in range(m - 1, m + 2):
        for tn in range(n - 1, n + 2):
            p = stream.way_arc(tm, tn)
            if not os.path.exists(p):
                continue
            e = next((e for e in arc.entries(p) if e[1] == stream.WAY_TYPE), None)
            if not e:
                continue
            g = way.parse(arc.data(e))
            for nd in g["nodes"][:stream.NODES ** 2]:
                x, y, z = nd["pos"]
                for lk in nd["links"]:
                    t = g["nodes"][lk["target"]]["pos"]
                    for f in np.linspace(0, 1, 40):
                        dot(x + (t[0] - x) * f, z + (t[2] - z) * f, (90, 140, 230), 0)
            for nd in g["nodes"][:stream.NODES ** 2]:
                x, y, z = nd["pos"]
                dot(x, z, (255, 220, 0) if y == 0.0 else (40, 200, 40), 2)
    for k, (x, z) in enumerate(actors):
        dot(x, z, (255, 255, 255) if k == 0 else (0, 230, 230), 4)
    img = img[::-1]  # DD +z is Skyrim south: flip so north is up
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out", "view.png")
    objects_test.write_png(out, np.ascontiguousarray(img))
    print("->", out, "centre", cx, cz)


if __name__ == "__main__":
    main()
