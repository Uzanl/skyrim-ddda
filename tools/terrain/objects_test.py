"""Dry run of the obstacle boxes: generate a tile in memory (nothing installed) and check it.

Usage: py objects_test.py M N [png]

Builds the tile's collision and graph with the current stream mapping, validates the
SBC (sbc.validate) and the graph (round trip), prints the boxes and the cut nodes, and
with "png" draws out/objects_M_N.png (ground height, boxes, kept/cut nodes, links).
"""
import os
import sys

import numpy as np

import sbc
import stream
import way


def run(m, n, png=False):
    cfg = stream.load_config()
    H = stream.make_height(cfg)
    got = {}
    real_install = stream.install
    stream.install = lambda path, data: got.__setitem__(path, data)
    try:
        c = stream.gen_collision(m, n, H)
        w = stream.gen_way(m, n, H)
    finally:
        stream.install = real_install
    ox, oz = (n - 50) * stream.TILE, (m - 50) * stream.TILE
    boxes = H.obs.boxes(ox - 200, oz - 200, ox + stream.TILE + 400, oz + stream.TILE + 400)
    names = [str(H.obs.names[i]) for i in H.obs.near(ox - 200, oz - 200, ox + stream.TILE + 400, oz + stream.TILE + 400)]
    print(f"{m}m{n}n: collision {c}, graph {w}; {len(boxes)} boxes: {sorted(set(names))}")
    import arc
    col = got.get(stream.col_arc(m, n))
    if col:
        tmp = "_objects_test.arc"
        open(tmp, "wb").write(col)
        e = next(e for e in arc.entries(tmp) if e[1] == stream.SBC_TYPE and "st100h_" in e[0])
        s = sbc.parse(arc.data(e))
        print("  sbc:", s.nTris, "tris", s.nVerts, "verts; validate:", sbc.validate(s))
        os.remove(tmp)
    g = None
    wp = got.get(stream.way_arc(m, n))
    if wp:
        tmp = "_objects_test_way.arc"
        open(tmp, "wb").write(wp)
        e = next(e for e in arc.entries(tmp) if e[1] == stream.WAY_TYPE)
        raw = arc.data(e)
        g = way.parse(raw)
        os.remove(tmp)
        real = g["nodes"][:stream.NODES ** 2]
        cut = [nd for nd in real if nd["pos"][1] == 0.0]
        links = sum(len(nd["links"]) for nd in real)
        print(f"  graph: {len(cut)} of {len(real)} nodes cut; {links} links; round trip {way.write(g) == raw}")
    if png and g:
        draw(m, n, H, boxes, g)


def draw(m, n, H, boxes, g):
    S = 4  # px per metre
    ox, oz = (n - 50) * stream.TILE, (m - 50) * stream.TILE
    size = int(stream.TILE / 100 * S)
    xs = ox + (np.arange(size) + 0.5) * 100 / S
    zs = oz + (np.arange(size) + 0.5) * 100 / S
    X, Z = np.meshgrid(xs, zs)
    Y = H(X, Z)
    img = np.repeat(((Y - Y.min()) / max(1.0, np.ptp(Y)) * 120 + 60)[..., None], 3, 2).astype(np.uint8)

    def px(x, z):
        return int((x - ox) / 100 * S), int((z - oz) / 100 * S)

    for cs, _, _ in boxes:
        inside = H.obs.blocked(X, Z, margin=0.0)
        img[inside] = (200, 60, 60)
        break
    for nd in g["nodes"][:stream.NODES ** 2]:
        x, y, z = nd["pos"]
        i, j = px(x, z)
        for l in nd["links"]:
            t = g["nodes"][l["target"]]["pos"]
            ti, tj = px(t[0], t[2])
            for f in np.linspace(0, 1, 12):
                a, b = int(i + (ti - i) * f), int(j + (tj - j) * f)
                if 0 <= a < size and 0 <= b < size:
                    img[b, a] = (90, 140, 230)
        if 0 <= i < size and 0 <= j < size:
            img[max(0, j - 1):j + 2, max(0, i - 1):i + 2] = (255, 220, 0) if y == 0.0 else (40, 200, 40)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out", f"objects_{m}_{n}.png")
    write_png(out, img)
    print("  ->", out)


def write_png(path, img):
    import struct
    import zlib
    h, w, _ = img.shape
    raw = b"".join(bytes(1) + img[y].tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    sig = bytes([137, 80, 78, 71, 13, 10, 26, 10])
    open(path, "wb").write(sig + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    run(int(sys.argv[1]), int(sys.argv[2]), len(sys.argv) > 3)
