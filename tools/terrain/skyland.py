"""Skyrim SE terrain heights from the master files (LAND / VHGT records).

Usage: py skyland.py [WORLD_EDID]      (default Tamriel) -> out/<world>_heights.npz

A worldspace cell is 4096 units with 33 x 33 height samples (128 units apart, edges
shared). VHGT = f32 offset, 33 x 33 signed byte deltas, 3 bytes padding:
the first value of each row accumulates from the previous row's first value, the rest
accumulate along the row, and height = value * 8 (Skyrim units).
Update.esm is applied after Skyrim.esm (later masters override the same cell).

The result is a single grid H[j][i] in Skyrim units: sample (i, j) is at
x = (cellMinX * 32 + i) * 128, y = (cellMinY * 32 + j) * 128 (Skyrim: x east, y north,
z up). Cells without LAND are NaN.
"""
import os
import struct
import sys
import zlib

import numpy as np

DATA = r"E:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data"
MASTERS = ["Skyrim.esm", "Update.esm"]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
COMPRESSED = 0x00040000


def subrecords(data):
    o, big = 0, None
    while o < len(data):
        t = data[o:o + 4]
        n, = struct.unpack_from("<H", data, o + 4)
        o += 6
        if t == b"XXXX":
            big, = struct.unpack_from("<I", data, o)
            o += 4
            continue
        if big is not None:
            n, big = big, None
        yield t, data[o:o + n]
        o += n


def record_data(d, o):
    size, flags = struct.unpack_from("<II", d, o + 4)
    raw = d[o + 24:o + 24 + size]
    if flags & COMPRESSED:
        raw = zlib.decompress(raw[4:])
    return raw


def decode_vhgt(v):
    off, = struct.unpack_from("<f", v, 0)
    deltas = np.frombuffer(v, dtype=np.int8, count=33 * 33, offset=4).reshape(33, 33).astype(np.float64)
    out = np.empty((33, 33))
    row_start = off
    for r in range(33):
        row_start += deltas[r, 0]
        out[r] = row_start + np.concatenate(([0.0], np.cumsum(deltas[r, 1:])))
    return out * 8.0


def world_cells(path, world_edid):
    """{(cx, cy): 33x33 heights} for one worldspace in one plugin."""
    d = open(path, "rb").read()
    cells = {}
    end = len(d)
    o = struct.unpack_from("<I", d, 4)[0] + 24  # skip TES4
    while o < end:
        assert d[o:o + 4] == b"GRUP", (path, hex(o))
        gsize, label = struct.unpack_from("<I4s", d, o + 4)
        if label != b"WRLD":
            o += gsize
            continue
        g_end = o + gsize
        p = o + 24
        target = None
        while p < g_end:
            if d[p:p + 4] == b"WRLD":
                size, = struct.unpack_from("<I", d, p + 4)
                fid, = struct.unpack_from("<I", d, p + 12)
                edid = next((v for t, v in subrecords(record_data(d, p)) if t == b"EDID"), b"")
                if edid.rstrip(b"\0").decode("latin1") == world_edid:
                    target = fid
                p += 24 + size
            elif d[p:p + 4] == b"GRUP":
                sz, lab, gtype = struct.unpack_from("<I4sI", d, p + 4)
                if gtype == 1 and struct.unpack("<I", lab)[0] == target:
                    walk_world(d, p + 24, p + sz, cells)
                p += sz
            else:
                raise ValueError(hex(p))
        o += gsize
    return cells


def walk_world(d, p, end, cells):
    cell = None
    while p < end:
        tag = d[p:p + 4]
        if tag == b"GRUP":
            sz, gtype = struct.unpack_from("<I", d, p + 4)[0], struct.unpack_from("<I", d, p + 12)[0]
            if gtype in (4, 5, 6, 9):  # blocks, sub-blocks, cell children, temporary children
                walk_world(d, p + 24, p + sz, cells)
            p += sz
            continue
        size, = struct.unpack_from("<I", d, p + 4)
        if tag == b"CELL":
            cell = None
            for t, v in subrecords(record_data(d, p)):
                if t == b"XCLC":
                    cell = struct.unpack_from("<ii", v, 0)
            walk_world.cell = cell
        elif tag == b"LAND":
            c = getattr(walk_world, "cell", None)
            for t, v in subrecords(record_data(d, p)):
                if t == b"VHGT" and c is not None:
                    cells[c] = decode_vhgt(v)
        p += 24 + size


def build(world="Tamriel"):
    cells = {}
    for m in MASTERS:
        path = os.path.join(DATA, m)
        if os.path.exists(path):
            got = world_cells(path, world)
            print(f"{m}: {len(got)} cells with heights")
            cells.update(got)
    xs = [c[0] for c in cells]
    ys = [c[1] for c in cells]
    cx0, cy0 = min(xs), min(ys)
    nx, ny = max(xs) - cx0 + 1, max(ys) - cy0 + 1
    H = np.full((ny * 32 + 1, nx * 32 + 1), np.nan, dtype=np.float32)
    for (cx, cy), h in cells.items():
        i, j = (cx - cx0) * 32, (cy - cy0) * 32
        H[j:j + 33, i:i + 33] = h
    os.makedirs(OUT, exist_ok=True)
    out = os.path.join(OUT, f"{world}_heights.npz")
    np.savez_compressed(out, H=H, cell_min=np.array([cx0, cy0]), spacing=128.0)
    print(f"{world}: cells x {cx0}..{cx0 + nx - 1}, y {cy0}..{cy0 + ny - 1}; grid {H.shape}; "
          f"heights {np.nanmin(H):.0f}..{np.nanmax(H):.0f}  -> {out}")
    return out


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "Tamriel")
