"""Skyrim SE placed objects (REFR) of a worldspace with their bounds, from the masters.

Usage: py skyobjects.py [WORLD_EDID]      (default Tamriel) -> out/<world>_objects.npz

A REFR places a base object (NAME) at DATA = pos (3 f32) + rot (3 f32, radians, applied
as Z then Y then X... see box_corners) with an optional scale XSCL. The base object's
OBND is its local bounding box (6 int16: x1 y1 z1 x2 y2 z2, Skyrim units). Records with
the deleted flag (0x20) are skipped; Update.esm overrides Skyrim.esm by form id.

Saved arrays (one row per reference): ref, base (form ids), kind (index in KINDS),
pos (n, 3), rot (n, 3), scale (n), lo/hi (n, 3) = OBND, edid (base EditorID), model (the
base's MODL path, under meshes).
"""
import os
import struct
import sys

import numpy as np

from skyland import DATA, MASTERS, OUT, record_data, subrecords

KINDS = [b"STAT", b"MSTT", b"TREE", b"ACTI", b"FURN", b"CONT", b"DOOR", b"FLOR", b"MISC", b"LIGH"]
DELETED = 0x20


def bases(d, found):
    """Base objects with OBND from the top-level groups of KINDS."""
    o = struct.unpack_from("<I", d, 4)[0] + 24
    while o < len(d):
        gsize, label = struct.unpack_from("<I4s", d, o + 4)
        if label in KINDS:
            p, end = o + 24, o + gsize
            while p < end:
                size, flags, fid = struct.unpack_from("<III", d, p + 4)
                if d[p:p + 4] == label and not flags & DELETED:
                    edid, ob, model = b"", None, b""
                    for t, v in subrecords(record_data(d, p)):
                        if t == b"EDID":
                            edid = v.rstrip(b"\0")
                        elif t == b"OBND":
                            ob = struct.unpack_from("<6h", v)
                        elif t == b"MODL":
                            model = v.rstrip(b"\0")
                    if ob:
                        found[fid] = (KINDS.index(label), ob, edid.decode("latin1"), model.decode("latin1"))
                p += 24 + size
        o += gsize


def refs(d, world_edid, found):
    o = struct.unpack_from("<I", d, 4)[0] + 24
    while o < len(d):
        gsize, label = struct.unpack_from("<I4s", d, o + 4)
        if label == b"WRLD":
            p, end, target = o + 24, o + gsize, None
            while p < end:
                if d[p:p + 4] == b"WRLD":
                    size, = struct.unpack_from("<I", d, p + 4)
                    fid, = struct.unpack_from("<I", d, p + 12)
                    edid = next((v for t, v in subrecords(record_data(d, p)) if t == b"EDID"), b"")
                    if edid.rstrip(b"\0").decode("latin1") == world_edid:
                        target = fid
                    p += 24 + size
                else:
                    sz, lab, gtype = struct.unpack_from("<I4sI", d, p + 4)
                    if gtype == 1 and struct.unpack("<I", lab)[0] == target:
                        walk(d, p + 24, p + sz, found)
                    p += sz
        o += gsize


def walk(d, p, end, found):
    while p < end:
        tag = d[p:p + 4]
        if tag == b"GRUP":
            sz, = struct.unpack_from("<I", d, p + 4)
            walk(d, p + 24, p + sz, found)
            p += sz
            continue
        size, flags, fid = struct.unpack_from("<III", d, p + 4)
        if tag == b"REFR":
            if flags & DELETED:
                found[fid] = None
            else:
                base, pos, scale = None, None, 1.0
                for t, v in subrecords(record_data(d, p)):
                    if t == b"NAME":
                        base, = struct.unpack_from("<I", v)
                    elif t == b"DATA":
                        pos = struct.unpack_from("<6f", v)
                    elif t == b"XSCL":
                        scale, = struct.unpack_from("<f", v)
                if base is not None and pos is not None:
                    found[fid] = (base, pos, scale, flags)
        p += 24 + size


def build(world="Tamriel"):
    base_info, placed = {}, {}
    for m in MASTERS:
        path = os.path.join(DATA, m)
        if not os.path.exists(path):
            continue
        d = open(path, "rb").read()
        nb, nr = len(base_info), len(placed)
        bases(d, base_info)
        refs(d, world, placed)
        print(f"{m}: {len(base_info) - nb} bases, {len(placed) - nr} new refs ({len(placed)} total)")
    rows = []
    for fid, r in placed.items():
        if r is None or r[0] not in base_info:
            continue
        kind, ob, edid, model = base_info[r[0]]
        rows.append((fid, r[0], kind, r[1][:3], r[1][3:], r[2], ob[:3], ob[3:], edid, r[3], model))
    print(f"{world}: {len(rows)} placed objects with bounds")
    out = os.path.join(OUT, f"{world}_objects.npz")
    np.savez_compressed(
        out,
        ref=np.array([r[0] for r in rows], dtype=np.uint32),
        base=np.array([r[1] for r in rows], dtype=np.uint32),
        kind=np.array([r[2] for r in rows], dtype=np.uint8),
        pos=np.array([r[3] for r in rows], dtype=np.float32),
        rot=np.array([r[4] for r in rows], dtype=np.float32),
        scale=np.array([r[5] for r in rows], dtype=np.float32),
        lo=np.array([r[6] for r in rows], dtype=np.float32),
        hi=np.array([r[7] for r in rows], dtype=np.float32),
        flags=np.array([r[9] for r in rows], dtype=np.uint32),
        edid=np.array([r[8] for r in rows]),
        model=np.array([r[10] for r in rows]),
        kinds=np.array([k.decode() for k in KINDS]))
    print("->", out)
    return out


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "Tamriel")
