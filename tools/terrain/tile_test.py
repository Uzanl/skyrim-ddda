"""In-game proof that DDDA accepts a generated SBC tile.

Rebuilds one st100 tile's character collision ("h" file) from a height grid sampled
from the tile's own terrain, shifted by OFFSET cm, and writes a patched .arc.

Usage: py tile_test.py build COL ROW [OFFSET_CM] [CELL_CM]   -> out/st100_COLmROWn.arc
       py tile_test.py install COL ROW                        backs up and installs it
       py tile_test.py restore COL ROW                        puts the original back
"""
import os
import shutil
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "recon"))
import arc  # noqa: E402
import sbc  # noqa: E402
import sbcgen  # noqa: E402

ROM = r"E:\SteamLibrary\steamapps\common\DDDA\nativePC\rom\stage\stage100\split"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
BACKUP = os.path.join(HERE, "..", "..", "backups", "ddda_arc")
WATER_FLOOR = 29900.0  # below the sea surface (30013) where the tile has no ground


def arc_path(col, row):
    return os.path.join(ROM, f"m{col // 10 * 10}", f"n{row // 10 * 10}", f"st100_{col}m{row}n.arc")


def entry_name(col, row):
    return f"scr\\st100\\collision\\m{col // 10 * 10}\\marge\\st100h_{col}m{row}n_mrg00"


def surface_heights(s, xs, zs):
    """Highest walkable triangle under each (x, z) grid point (NaN where none)."""
    tris = []
    for g in s.groups:
        _, _, _, _, _, t0, tn, v0, _, _, _ = sbc.group_fields(g)
        for ti in range(tn):
            n, v, _, _ = sbc.tri_fields(s.tris[t0 + ti])
            if n[1] < 0.3:  # skip walls and the 800 m invisible boundary walls
                continue
            tris.append([s.verts[v0 + i][:3] for i in v])
    T = np.array(tris, dtype=np.float64)  # (n, 3 verts, xyz)
    X, Z = np.meshgrid(xs, zs)
    P = np.stack([X.ravel(), Z.ravel()], 1)
    best = np.full(len(P), np.nan)
    x1, z1, y1 = T[:, 0, 0], T[:, 0, 2], T[:, 0, 1]
    x2, z2, y2 = T[:, 1, 0], T[:, 1, 2], T[:, 1, 1]
    x3, z3, y3 = T[:, 2, 0], T[:, 2, 2], T[:, 2, 1]
    det = (z2 - z3) * (x1 - x3) + (x3 - x2) * (z1 - z3)
    ok = np.abs(det) > 1e-6
    for k in range(0, len(P), 256):
        px, pz = P[k:k + 256, 0:1], P[k:k + 256, 1:2]
        l1 = ((z2 - z3) * (px - x3) + (x3 - x2) * (pz - z3)) / np.where(ok, det, 1)
        l2 = ((z3 - z1) * (px - x3) + (x1 - x3) * (pz - z3)) / np.where(ok, det, 1)
        l3 = 1 - l1 - l2
        inside = ok & (l1 >= -1e-4) & (l2 >= -1e-4) & (l3 >= -1e-4)
        y = np.where(inside, l1 * y1 + l2 * y2 + l3 * y3, -np.inf)
        m = y.max(1)
        best[k:k + 256] = np.where(np.isfinite(m), m, np.nan)
    return best.reshape(len(zs), len(xs))


def build(col, row, offset, cell):
    name = entry_name(col, row)
    entry = next(e for e in arc.entries(arc_path(col, row)) if e[0] == name)
    orig = sbc.parse(arc.data(entry))
    x0, _, z0, _, x1, _, z1, _ = orig.bbox
    xs = np.arange(np.floor(x0 / cell) * cell, x1 + cell, cell)
    zs = np.arange(np.floor(z0 / cell) * cell, z1 + cell, cell)
    H = surface_heights(orig, xs, zs)
    holes = np.isnan(H).sum()
    H = np.where(np.isnan(H), WATER_FLOOR, H) + offset
    data = sbcgen.build(H.tolist(), (float(xs[0]), float(zs[0])), cell, orig)
    s = sbc.parse(data)
    checks, errors = sbc.validate(s)
    assert errors == 0, errors
    os.makedirs(OUT, exist_ok=True)
    out = os.path.join(OUT, os.path.basename(arc_path(col, row)))
    open(out, "wb").write(arc.rebuild(arc_path(col, row), {name: data}))
    print(f"grid {len(xs)}x{len(zs)} cell {cell} holes {holes} offset {offset}: "
          f"{s.nTris} tris, {len(s.bvhs[0][1])} nodes, {len(data)} bytes, valid ({checks} checks)")
    print(out)


def install(col, row):
    src = os.path.join(OUT, os.path.basename(arc_path(col, row)))
    dst = arc_path(col, row)
    bak = os.path.join(BACKUP, os.path.basename(dst))
    os.makedirs(BACKUP, exist_ok=True)
    if not os.path.exists(bak):
        shutil.copy2(dst, bak)
    shutil.copyfile(src, dst)
    print(f"installed {dst}\nbackup    {bak}")


def restore(col, row):
    dst = arc_path(col, row)
    shutil.copy2(os.path.join(BACKUP, os.path.basename(dst)), dst)
    print(f"restored {dst}")


if __name__ == "__main__":
    cmd, col, row = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    if cmd == "build":
        build(col, row, float(sys.argv[4]) if len(sys.argv) > 4 else -100.0,
              float(sys.argv[5]) if len(sys.argv) > 5 else 200.0)
    elif cmd == "install":
        install(col, row)
    elif cmd == "restore":
        restore(col, row)
