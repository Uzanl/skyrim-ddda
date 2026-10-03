"""Synthetic-terrain test: replace the collision ("h" SBC) and waypoint graphs around
the party with ones generated from a single height function, then check that pawns
still follow the Arisen on ground that no longer matches the visible scenery.

Usage: py synth_test.py build X Y Z     (Arisen GLOBAL position from the dump, cm)
       py synth_test.py install         (game closed; backs up originals once)
       py synth_test.py restore

Height function (global coordinates): a flat plane 50 cm under the Arisen's feet with
two smooth hills (4 m and 2.5 m) 20-30 m away, so the party walks up and down slopes.
Collision: 3x3 tiles around the Arisen. Waypoints: every graph whose cover box
(centred on the tile corner, +-50 m) overlaps the inner area, 4 m node spacing, with
cross-tile "exp" links between the replaced graphs.
"""
import json
import math
import os
import shutil
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "recon"))
import arc  # noqa: E402
import sbc  # noqa: E402
import sbcgen  # noqa: E402
import way  # noqa: E402

ROM = r"E:\SteamLibrary\steamapps\common\DDDA\nativePC\rom\stage\stage100"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out_synth")
BACKUP = os.path.join(HERE, "..", "..", "backups", "ddda_arc")
TILE = 10000.0
SBC_TYPE, WAY_TYPE = 0x51FC779F, 0x5F36B659
NODE_STEP = 400.0
COL_CELL = 200.0


def tile_of(gx, gz):
    return int(gz // TILE) + 50, int(gx // TILE) + 50  # (m, n)


def sub(m, n):
    return os.path.join(f"m{m // 10 * 10}", f"n{n // 10 * 10}")


def col_arc(m, n):
    return os.path.join(ROM, "split", sub(m, n), f"st100_{m}m{n}n.arc")


def way_arc(m, n):
    return os.path.join(ROM, "split_way", sub(m, n), f"st100_{m}m{n}n_way.arc")


def make_height(px, py, pz):
    base = py - 50.0
    hills = [(px + 2000, pz + 1000, 400.0, 1200.0), (px - 1500, pz + 2500, 250.0, 900.0)]

    def h(gx, gz):
        y = np.full(np.shape(gx), base, dtype=np.float64)
        for hx, hz, amp, rad in hills:
            y = y + amp * np.exp(-(((gx - hx) ** 2 + (gz - hz) ** 2) / (rad * rad)))
        return y
    return h


def build(px, py, pz):
    pm, pn = tile_of(px, pz)
    build_region(make_height(px, py, pz), range(pm - 1, pm + 2), range(pn - 1, pn + 2), OUT,
                 {"pos": [px, py, pz]})


def build_region(H, ms, ns, out_dir, meta, max_slope=None):
    """Generate collision for tiles ms x ns (rows m = z, columns n = x) and waypoint graphs
    for the area, from H(gx, gz) -> y in DDDA global coordinates. Writes patched archives
    and manifest.json into out_dir. max_slope (rise/run): links steeper than this are
    not created (None = link everything, as in the synthetic test)."""
    os.makedirs(out_dir, exist_ok=True)
    written = []
    ms, ns = list(ms), list(ns)
    area = [TILE * (ns[0] - 50), TILE * (ms[0] - 50), TILE * (ns[-1] + 1 - 50), TILE * (ms[-1] + 1 - 50)]
    for m in ms:
        for n in ns:
            path = col_arc(m, n)
            if not os.path.exists(path):
                print("no collision arc", m, n)
                continue
            name, entry = next((e[0], e) for e in arc.entries(path) if e[1] == SBC_TYPE and "st100h_" in e[0])
            template = sbc.parse(arc.data(entry))
            ox, oz = (n - 50) * TILE, (m - 50) * TILE
            ls = np.arange(-COL_CELL, TILE + 2 * COL_CELL, COL_CELL)
            LX, LZ = np.meshgrid(ls, ls)
            heights = H(LX + ox, LZ + oz)
            data = sbcgen.build(heights.tolist(), (float(ls[0]), float(ls[0])), COL_CELL, template)
            assert sbc.validate(sbc.parse(data))[1] == 0
            out = os.path.join(out_dir, os.path.basename(path))
            open(out, "wb").write(arc.rebuild(path, {name: data}))
            written.append(path)

    # waypoints: global node grid over the inner area (inset 5 m from the collision area)
    x0, z0, x1, z1 = area[0] + 500, area[1] + 500, area[2] - 500, area[3] - 500
    xs = np.arange(math.ceil(x0 / NODE_STEP) * NODE_STEP, x1, NODE_STEP)
    zs = np.arange(math.ceil(z0 / NODE_STEP) * NODE_STEP, z1, NODE_STEP)

    def way_file_of(gx, gz):  # graph (m, n) covers its tile corner +-5000
        return int(math.floor((gz + 5000) / TILE)) + 50, int(math.floor((gx + 5000) / TILE)) + 50

    files = {}
    for gz in zs:
        for gx in xs:
            key = way_file_of(gx, gz)
            files.setdefault(key, [])
    files = {k: v for k, v in files.items()
             if os.path.exists(way_arc(*k)) and any(e[1] == WAY_TYPE for e in arc.entries(way_arc(*k)))}
    graphs, where = {}, {}
    for (m, n) in files:
        graphs[(m, n)] = {"version": 0x26, "unk": 0, "nodes": []}
    for j, gz in enumerate(zs):
        for i, gx in enumerate(xs):
            key = way_file_of(gx, gz)
            if key not in graphs:
                continue
            nodes = graphs[key]["nodes"]
            where[(i, j)] = (key, len(nodes))
            nodes.append({"id": len(nodes), "pos": (float(gx), float(H(gx, gz)), float(gz)),
                          "attr": [0], "links": [], "geomType": 0, "geom": b"", "exp": []})
    for (i, j), (key, a) in where.items():
        na = graphs[key]["nodes"][a]
        for di in (-1, 0, 1):
            for dj in (-1, 0, 1):
                if (di, dj) == (0, 0) or (i + di, j + dj) not in where:
                    continue
                key2, b = where[(i + di, j + dj)]
                if max_slope is not None:
                    pa, pb = na["pos"], graphs[key2]["nodes"][b]["pos"]
                    if abs(pa[1] - pb[1]) > max_slope * math.hypot(pa[0] - pb[0], pa[2] - pb[2]):
                        continue
                if key2 == key:
                    pb = graphs[key2]["nodes"][b]["pos"]
                    na["links"].append({"target": b, "attr": [0], "cost": math.dist(na["pos"], pb) / 100.0,
                                        "size": 0.0, "height": 0.0})
                else:
                    dm, dn = key2[0] - key[0], key2[1] - key[1]
                    na["exp"].append(((dm + 1) * 3 + (dn + 1)) << 16 | b)
    for (m, n), g in graphs.items():
        if not g["nodes"]:
            continue
        path = way_arc(m, n)
        name, entry = next((e[0], e) for e in arc.entries(path) if e[1] == WAY_TYPE)
        # Other data may hold node indices of the original graph: pad with isolated
        # nodes at the bottom of the world (never the nearest node) up to its count.
        orig_n = len(way.parse(arc.data(entry))["nodes"])
        x, _, z = g["nodes"][0]["pos"]
        while len(g["nodes"]) < orig_n:
            g["nodes"].append({"id": len(g["nodes"]), "pos": (x, 0.0, z), "attr": [0], "links": [],
                               "geomType": 0, "geom": b"", "exp": []})
        way.finalize(g)
        data = way.write(g)
        assert way.write(way.parse(data)) == data
        out = os.path.join(out_dir, os.path.basename(path))
        open(out, "wb").write(arc.rebuild(path, {name: data}))
        written.append(path)
        print(f"way {m}m{n}n: {len(g['nodes'])} nodes, {g['hdr'][0]} links, {g['hdr'][3]} cross-tile")
    # Original graphs around the replaced ones hold "exp" links (node indices) into
    # them; those indices are now meaningless, so strip them.
    replaced = {k for k, g in graphs.items() if g["nodes"]}
    ring = {(m + dm, n + dn) for (m, n) in replaced for dm in (-1, 0, 1) for dn in (-1, 0, 1)} - replaced
    for (m, n) in sorted(ring):
        path = way_arc(m, n)
        if not os.path.exists(path):
            continue
        entry = next((e for e in arc.entries(path) if e[1] == WAY_TYPE), None)
        if entry is None:
            continue
        g = way.parse(arc.data(entry))
        removed = 0
        for nd in g["nodes"]:
            keep = []
            for v in nd["exp"]:
                d = v >> 16
                if (m + d // 3 - 1, n + d % 3 - 1) in replaced:
                    removed += 1
                else:
                    keep.append(v)
            nd["exp"] = keep
        if not removed:
            continue
        bbox = g["bbox"]
        way.finalize(g)
        g["bbox"], g["grid"] = way.build_grid(g["nodes"], 4, bbox=bbox)  # keep the game's bbox
        data = way.write(g)
        out = os.path.join(out_dir, os.path.basename(path))
        open(out, "wb").write(arc.rebuild(path, {entry[0]: data}))
        written.append(path)
        print(f"way {m}m{n}n (original): removed {removed} cross-tile links into replaced graphs")
    json.dump(dict(meta, files=written), open(os.path.join(out_dir, "manifest.json"), "w"), indent=1)
    print(f"{len(written)} archives written to {out_dir}")


def install(out_dir=OUT):
    man = json.load(open(os.path.join(out_dir, "manifest.json")))
    os.makedirs(BACKUP, exist_ok=True)
    for dst in man["files"]:
        bak = os.path.join(BACKUP, os.path.basename(dst))
        if not os.path.exists(bak):
            shutil.copy2(dst, bak)
        shutil.copyfile(os.path.join(out_dir, os.path.basename(dst)), dst)
    print(f"installed {len(man['files'])} archives (originals in {os.path.abspath(BACKUP)})")


def restore(out_dir=OUT):
    man = json.load(open(os.path.join(out_dir, "manifest.json")))
    for dst in man["files"]:
        shutil.copy2(os.path.join(BACKUP, os.path.basename(dst)), dst)
    print(f"restored {len(man['files'])} archives")


if __name__ == "__main__":
    if sys.argv[1] == "build":
        build(*map(float, sys.argv[2:5]))
    elif sys.argv[1] == "install":
        install()
    elif sys.argv[1] == "restore":
        restore()
