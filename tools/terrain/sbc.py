"""DDDA (MT Framework) SBC stage-collision file: parse and write.

Layout (all little endian), worked out from st100 tiles (docs/terrain-proxy.md):

  0x00  "SBC\\xFF", u32 version hash (0x77DF2114), u32 0, u32 0
  0x10  u16 nGroups, u16 nAttrs, u32 nAdj, u32 nTris, u32 nVerts
  0x20  16 bytes (u32 count, nonzero in "e" files; kept raw)
  0x30  bbox min f32x4, bbox max f32x4
  0x50  u32 unknown (kept)
  0x54  groups[nGroups], 0x50 bytes each:
          min f32x3, u32, max f32x3, u32, u32 id, 8 bytes (to +0x2C),
          u32 adjStart, u32 adjCount, u32 triStart, u32 triCount,
          u32 vtxStart, u32 vtxCount, u32 unk, u32 unk2
  then  nGroups + 1 BVHC blocks (one per group, then a top level over groups):
          "BVHC", u32 hash (0x77B17B24), u32 1, u32 0, min f32x4, max f32x4,
          u32 nNodes, 12 bytes zero, nodes[nNodes] of 0x70 bytes:
            u8 flags x4 (same byte; bit i = child i is a node, bit 4+i = leaf),
            u16 child[4], u32 pad, f32 minX[4] minY[4] minZ[4] maxX[4] maxY[4] maxZ[4]
  then  tris[nTris], 32 bytes: normal f32x3, u16 v[3], u16 0, u32 0, u16 a, u16 b, u32 0
        (vertex indices are group-relative)
  then  verts[nVerts] f32x4 (w = 0)
  then  attrs[nAttrs], 32 bytes (kept as raw)
  then  adj[nAdj], 10 bytes (kept as raw)
"""
import struct

BVHC_HASH = 0x77B17B24


class Sbc:
    pass


def parse(d):
    s = Sbc()
    assert d[:4] == b"SBC\xff", d[:4]
    s.version, = struct.unpack_from("<I", d, 4)
    s.nGroups, s.nAttrs, s.nAdj, s.nTris, s.nVerts = struct.unpack_from("<HHIII", d, 0x10)
    s.hdr20 = d[0x20:0x30]
    s.bbox = struct.unpack_from("<8f", d, 0x30)
    s.unk50, = struct.unpack_from("<I", d, 0x50)
    o = 0x54
    s.groups = []
    for _ in range(s.nGroups):
        s.groups.append(d[o:o + 0x50])
        o += 0x50
    s.bvhs = []
    for _ in range(s.nGroups + 1):
        assert d[o:o + 4] == b"BVHC", hex(o)
        n, = struct.unpack_from("<I", d, o + 0x30)
        hdr = d[o:o + 0x40]
        nodes = [d[o + 0x40 + i * 0x70:o + 0x40 + (i + 1) * 0x70] for i in range(n)]
        s.bvhs.append((hdr, nodes))
        o += 0x40 + n * 0x70
    s.tris = [d[o + i * 32:o + (i + 1) * 32] for i in range(s.nTris)]
    o += 32 * s.nTris
    s.verts = [struct.unpack_from("<4f", d, o + i * 16) for i in range(s.nVerts)]
    o += 16 * s.nVerts
    s.attrs = d[o:o + 32 * s.nAttrs]
    o += 32 * s.nAttrs
    s.adj = d[o:o + 10 * s.nAdj]
    o += 10 * s.nAdj
    s.tail = d[o:]
    return s


def write(s):
    out = bytearray(b"SBC\xff")
    out += struct.pack("<III", s.version, 0, 0)
    out += struct.pack("<HHIII", len(s.groups), len(s.attrs) // 32, len(s.adj) // 10,
                       len(s.tris), len(s.verts))
    out += s.hdr20
    out += struct.pack("<8f", *s.bbox)
    out += struct.pack("<I", s.unk50)
    for g in s.groups:
        out += g
    for hdr, nodes in s.bvhs:
        out += hdr
        for n in nodes:
            out += n
    for t in s.tris:
        out += t
    for v in s.verts:
        out += struct.pack("<4f", *v)
    out += s.attrs + s.adj + s.tail
    return bytes(out)


def group_fields(g):
    """(min, max, id, adjStart, adjCount, triStart, triCount, vtxStart, vtxCount, unk, unk2)"""
    mn = struct.unpack_from("<3f", g, 0)
    mx = struct.unpack_from("<3f", g, 0x10)
    gid, = struct.unpack_from("<I", g, 0x20)
    rest = struct.unpack_from("<8I", g, 0x2C)
    return (mn, mx, gid) + rest


def tri_fields(t):
    """(normal, (v0, v1, v2), a, b)"""
    n = struct.unpack_from("<3f", t, 0)
    v = struct.unpack_from("<3H", t, 12)
    a, b = struct.unpack_from("<HH", t, 0x18)
    return n, v, a, b


if __name__ == "__main__":
    import sys
    for p in sys.argv[1:]:
        d = open(p, "rb").read()
        s = parse(d)
        ok = write(s) == d
        print(f"{'OK ' if ok else 'BAD'} groups {s.nGroups:3d} tris {s.nTris:6d} verts {s.nVerts:6d} "
              f"adj {s.nAdj:6d} attrs {s.nAttrs} tail {len(s.tail)}  {p}")


def validate(s):
    """Check every BVH leaf -> primitive -> triangle -> vertex lies in the leaf box and
    that every primitive and triangle of each group is reachable. Returns (checks, errors)."""
    checks = errors = 0
    for gi, g in enumerate(s.groups):
        _, _, _, a0, an, t0, tn, v0, vn, _, _ = group_fields(g)
        _, nodes = s.bvhs[gi]
        prims, tris = set(), set()
        for nd in nodes:
            fl = nd[0]
            ch = struct.unpack_from("<4H", nd, 4)
            f = struct.unpack_from("<24f", nd, 16)
            for c in range(4):
                if fl >> (4 + c) & 1:
                    prims.add(ch[c])
                    pa, pb = struct.unpack_from("<2H", s.adj, (a0 + ch[c]) * 10)
                    for ti in (pa, pb):
                        if ti == 0xFFFF:
                            continue
                        tris.add(ti)
                        for vi in tri_fields(s.tris[t0 + ti])[1]:
                            p = s.verts[v0 + vi]
                            checks += 1
                            if not all(f[k * 4 + c] - 0.5 <= p[k] <= f[(k + 3) * 4 + c] + 0.5 for k in range(3)):
                                errors += 1
        if prims != set(range(an)) or tris != set(range(tn)):
            errors += 1
    return checks, errors
