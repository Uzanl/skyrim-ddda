"""Generate an SBC collision tile from a height grid (see sbc.py for the format).

build(heights, origin, cell, template) -> bytes
  heights  2D list/array [nz][nx] of Y values (DDDA units, cm, Y up)
  origin   (x0, z0) of sample [0][0] in tile-local coordinates
  cell     spacing between samples (cm)
  template a parsed Sbc whose group 0 (terrain) supplies the opaque fields:
           group id and type, terrain triangle flags, and the surface attribute.

One group, one primitive per triangle, a 4-wide BVH built by median splits.
"""
import struct
from collections import Counter

import sbc

INF = 3.0e38


def _bounds(boxes):
    mn = [min(b[0][k] for b in boxes) for k in range(3)]
    mx = [max(b[1][k] for b in boxes) for k in range(3)]
    return mn, mx


def _bvh(prim_boxes):
    """4-wide BVH over primitive boxes. Returns a list of (flags, child[4], box[4])."""
    nodes = []

    def split4(ids):
        groups = [ids]
        while len(groups) < 4:
            groups.sort(key=len, reverse=True)
            g = groups[0]
            if len(g) < 2:
                break
            mn, mx = _bounds([prim_boxes[i] for i in g])
            ax = max(range(3), key=lambda k: mx[k] - mn[k])
            g = sorted(g, key=lambda i: prim_boxes[i][0][ax] + prim_boxes[i][1][ax])
            h = len(g) // 2
            groups = groups[1:] + [g[:h], g[h:]]
        return groups

    def make(ids):
        idx = len(nodes)
        nodes.append(None)
        flags, child, boxes = 0, [0, 0, 0, 0], []
        for c, g in enumerate(split4(ids)):
            if len(g) == 1:
                flags |= 0x10 << c
                child[c] = g[0]
            else:
                flags |= 1 << c
                child[c] = make(g)
            boxes.append(_bounds([prim_boxes[i] for i in g]))
        while len(boxes) < 4:
            boxes.append(boxes[0])  # unused slots repeat a real box, as the game files do
        nodes[idx] = (flags, child, boxes)
        return idx

    make(list(range(len(prim_boxes))))
    return nodes


def _pack_node(flags, child, boxes):
    out = bytes([flags] * 4) + struct.pack("<4H", *child) + b"\xcd" * 4
    for side in (0, 1):
        for k in range(3):
            out += struct.pack("<4f", *(b[side][k] for b in boxes))
    return out


def _bvhc(nodes, mn, mx):
    hdr = b"BVHC" + struct.pack("<III", sbc.BVHC_HASH, 1, 0)
    hdr += struct.pack("<4f", *mn, 0) + struct.pack("<4f", *mx, 0)
    hdr += struct.pack("<I", len(nodes)) + bytes(12)
    return hdr, [_pack_node(*n) for n in nodes]


# Plain walkable ground, from a normal Gransys terrain tile (63m52n). Some tiles' own
# terrain group is a special surface (attribute 0x0600, triangle flags (0, 1): in
# Gransys they are river/waterfall tiles); copied onto generated ground it held the
# party at a flat height above the slope (2026-10-02, tiles 59m52n/60m52n).
GROUND_ATTR = bytes.fromhex("01000000") + bytes(28)
GROUND_FLAGS = (514, 2)


MAX_GROUP_TRIS = 20000  # per group: vertex indices and BVH child slots are u16


def _normal(p):
    u = [p[1][k] - p[0][k] for k in range(3)]
    w = [p[2][k] - p[0][k] for k in range(3)]
    n = (u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0])
    ln = sum(x * x for x in n) ** 0.5
    return tuple(x / ln for x in n) if ln > 1e-9 else None


def build(heights, origin, cell, template, ground=True, solids=(), triangles=(), keep_cell=None):
    """ground=True: plain ground surface (GROUND_ATTR/FLAGS) instead of the template's.
    solids: boxes [(corners (4 x (x, z)) in order around, y bottom, y top)] in the same
    coordinates, added as 4 walls + a top (DDDA tells walls from floors by the normal).
    triangles: extra triangles [((x, y, z) x 3)] as they are (Skyrim's live collision),
    turned to face up. keep_cell(x, z): False drops the height grid's cell there (the area
    is covered by `triangles`)."""
    nz, nx = len(heights), len(heights[0])
    x0, z0 = origin

    t = template
    g0 = sbc.group_fields(t.groups[0])
    _, _, _, a0, an, t0, tn, _, _, _, _ = g0
    flags = Counter(sbc.tri_fields(t.tris[t0 + i])[2:] for i in range(tn)).most_common(1)[0][0]
    attr = Counter(struct.unpack_from("<5H", t.adj, (a0 + i) * 10)[4] for i in range(an)).most_common(1)[0][0]
    if ground:
        flags = GROUND_FLAGS

    faces = []  # (p0, p1, p2), normal
    for j in range(nz - 1):
        for i in range(nx - 1):
            if keep_cell and not keep_cell(x0 + (i + 0.5) * cell, z0 + (j + 0.5) * cell):
                continue
            q = [(x0 + (i + a) * cell, float(heights[j + b][i + a]), z0 + (j + b) * cell)
                 for a, b in ((0, 0), (1, 0), (0, 1), (1, 1))]
            for p in ((q[0], q[2], q[1]), (q[1], q[2], q[3])):  # counter-clockwise seen from +Y
                n = _normal(p)
                if n[1] < 0:
                    p, n = (p[0], p[2], p[1]), tuple(-x for x in n)
                faces.append((p, n))

    for corners, y0, y1 in solids:
        v = [(float(x), float(y0), float(z)) for x, z in corners] + [(float(x), float(y1), float(z)) for x, z in corners]
        mid = [sum(c[a] for c in v) / 8 for a in range(3)]
        quads = [(k, (k + 1) % 4, 4 + (k + 1) % 4, 4 + k) for k in range(4)] + [(4, 5, 6, 7)]
        for qd in quads:
            for tri in ((qd[0], qd[1], qd[2]), (qd[0], qd[2], qd[3])):
                p = tuple(v[k] for k in tri)
                n = _normal(p)
                out = [sum(c[k] for c in p) / 3 - mid[k] for k in range(3)]
                if sum(n[k] * out[k] for k in range(3)) < 0:  # face outwards
                    p, n = (p[0], p[2], p[1]), tuple(-x for x in n)
                faces.append((p, n))

    for p in triangles:
        p = tuple(tuple(float(c) for c in v) for v in p)
        n = _normal(p)
        if n is None:
            continue
        if n[1] < 0:
            p, n = (p[0], p[2], p[1]), tuple(-x for x in n)
        faces.append((p, n))

    # Groups: spatial chunks of at most MAX_GROUP_TRIS faces (median splits on x/z).
    def chunks(ids):
        if len(ids) <= MAX_GROUP_TRIS:
            return [ids]
        cx = [sum(c[0] for c in faces[i][0]) for i in ids]
        cz = [sum(c[2] for c in faces[i][0]) for i in ids]
        ax = cx if max(cx) - min(cx) >= max(cz) - min(cz) else cz
        order = [i for _, i in sorted(zip(ax, ids))]
        h = len(order) // 2
        return chunks(order[:h]) + chunks(order[h:])

    groups = chunks(list(range(len(faces))))
    tg = t.groups[0]
    s = sbc.Sbc()
    s.version = t.version
    s.hdr20 = bytes(16)
    s.groups, s.bvhs, s.tris, s.verts, prims = [], [], [], [], []
    gboxes, nnodes = [], 0
    for ids in groups:
        vidx, gverts, gtris, gprims, boxes = {}, [], [], [], []
        for fi in ids:
            p, n = faces[fi]
            tv = []
            for c in p:
                key = (round(c[0], 1), round(c[1], 1), round(c[2], 1))
                if key not in vidx:
                    vidx[key] = len(gverts)
                    gverts.append((c[0], c[1], c[2], 0.0))
                tv.append(vidx[key])
            if len(set(tv)) < 3:
                continue
            ti = len(gtris)
            gtris.append(struct.pack("<3f3HHIHHI", *n, *tv, 0, 0, flags[0], flags[1], 0))
            gprims.append(struct.pack("<5H", ti, 0xFFFF, 0xFFFF, 0xFFFF, 0))
            boxes.append(([min(q[k] for q in p) for k in range(3)], [max(q[k] for q in p) for k in range(3)]))
        if not boxes:
            continue
        assert len(gverts) <= 0xFFFF and len(gprims) <= 0xFFFF, (len(gverts), len(gprims))
        mn, mx = _bounds(boxes)
        nodes = _bvh(boxes)
        assert len(nodes) <= 0xFFFF
        grp = struct.pack("<3fI3fI", *mn, 0, *mx, 0) + tg[0x20:0x2C]
        grp += struct.pack("<8I", len(prims), len(gprims), len(s.tris), len(gtris), len(s.verts), len(gverts),
                           g0[9], g0[10]) + tg[0x4C:0x50]
        s.groups.append(grp)
        s.bvhs.append(_bvhc(nodes, mn, mx))
        nnodes += len(nodes)
        s.tris += gtris
        s.verts += gverts
        prims += gprims
        gboxes.append((mn, mx))
    mn, mx = _bounds(gboxes)
    top = _bvh(gboxes) if len(gboxes) > 1 else [(0x10, [0, 0, 0, 0], [(mn, mx)] * 4)]
    s.bvhs.append(_bvhc(top, mn, mx))
    s.bbox = (*mn, 0.0, *mx, 0.0)
    s.unk50 = 0x70 * (nnodes + len(top))
    s.attrs = GROUND_ATTR if ground else t.attrs[attr * 32:attr * 32 + 32]
    s.adj = b"".join(prims)
    s.tail = b""
    return sbc.write(s)
