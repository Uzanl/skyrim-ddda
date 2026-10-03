"""DDDA rAIWayPoint ("WAY") waypoint-graph file: parse and write.

Format, from the loader DDDA.exe+0xDFD5C0 (rAIWayPoint vtable 0x14481C4 slot 10).
Little endian and packed (no alignment):

  u32 magic 'WAY\\0', u32 version 0x26, u32 unk (0), u32 nAttrSlots (node attrs + link
  attrs + exp entries), u32 nNodes, u32 nLinks, u32 nNodes, u32 nLinks, u32 nExp
  nodes[nNodes]:
      u16 id, f32 pos[3], u16 nAttr, u16 nLinks, u32 attr[nAttr],
      links[nLinks]: u16 target, u8 nAttr, u32 attr[nAttr], f32 cost, f32 size, f32 height
      u8 geomType: 1 -> 16 bytes, 2 -> 80 bytes of geometry, else none
      u8 nExp, u32 exp[nExp]
  u8 depth, f32 bboxMax[3], f32 bboxMin[3]   (quadtree in x/z, built by +0xD4D5D0;
                                              the game files use depth 4, Y 0..300000)
  cells for levels 0..depth-1 (1 + 4 + 16 + 64 = 85 for depth 4), each:
      u32 n, n x (u16 nodeIndex, u32 0)
  Nodes are stored only in the deepest level; the cell index inside a level is the
  Morton code of (ix, iz) with x in the low bit.

Coordinates are GLOBAL: for tile st100_{m}m{n}n,
  global = tile-local + ((n - 50) * 10000, 0, (m - 50) * 10000)
(fitted against the collision tiles: median height error 9 cm over 1737 nodes).
Link cost = distance / 100. Exp entries are links into the neighbouring tiles'
graphs: u32 = dir << 16 | nodeIndex, dir = (dm + 1) * 3 + (dn + 1) for the
neighbour st100_{m+dm}m{n+dn}n (median 5.5 m between the two linked nodes).
Verified: byte-exact round trip; grid reproduced for 54/54 files given the game bbox.
"""
import struct


class Reader:
    def __init__(self, d):
        self.d, self.o = d, 0

    def take(self, fmt):
        v = struct.unpack_from("<" + fmt, self.d, self.o)
        self.o += struct.calcsize("<" + fmt)
        return v if len(v) > 1 else v[0]


def parse(d):
    r = Reader(d)
    g = {}
    magic, g["version"], g["unk"], g["nAttrSlots"], n = r.take("4sIII I")
    assert magic == b"WAY\0", magic
    g["hdr"] = r.take("4I")
    nodes = []
    for _ in range(n):
        nd = {}
        nd["id"] = r.take("H")
        nd["pos"] = r.take("3f")
        na, nl = r.take("HH")
        nd["attr"] = [r.take("I") for _ in range(na)]
        nd["links"] = []
        for _ in range(nl):
            tgt = r.take("H")
            k = r.take("B")
            attr = [r.take("I") for _ in range(k)]
            cost, size, height = r.take("3f")
            nd["links"].append({"target": tgt, "attr": attr, "cost": cost, "size": size, "height": height})
        t = r.take("B")
        nd["geomType"] = t
        nd["geom"] = d[r.o:r.o + {1: 16, 2: 80}.get(t, 0)]
        r.o += len(nd["geom"])
        ne = r.take("B")
        nd["exp"] = [r.take("I") for _ in range(ne)]
        nodes.append(nd)
    g["nodes"] = nodes
    g["gridDepth"] = r.take("B")
    g["bbox"] = r.take("6f")
    g["gridOffset"] = r.o
    g["grid"] = d[r.o:]
    return g


def write(g):
    out = bytearray(struct.pack("<4sIIII", b"WAY\0", g["version"], g["unk"], g["nAttrSlots"], len(g["nodes"])))
    out += struct.pack("<4I", *g["hdr"])
    for nd in g["nodes"]:
        out += struct.pack("<H3fHH", nd["id"], *nd["pos"], len(nd["attr"]), len(nd["links"]))
        out += b"".join(struct.pack("<I", a) for a in nd["attr"])
        for l in nd["links"]:
            out += struct.pack("<HB", l["target"], len(l["attr"]))
            out += b"".join(struct.pack("<I", a) for a in l["attr"])
            out += struct.pack("<3f", l["cost"], l["size"], l["height"])
        out += struct.pack("<B", nd["geomType"]) + nd["geom"]
        out += struct.pack("<B", len(nd["exp"])) + b"".join(struct.pack("<I", e) for e in nd["exp"])
    out += struct.pack("<B6f", g["gridDepth"], *g["bbox"])
    out += g["grid"]
    return bytes(out)


def morton(ix, iz, bits):
    r = 0
    for k in range(bits):
        r |= ((ix >> k) & 1) << (2 * k) | ((iz >> k) & 1) << (2 * k + 1)
    return r


def build_grid(nodes, depth=4, ymin=0.0, ymax=300000.0, bbox=None):
    """Quadtree bbox and cell bytes in the game's layout. The game's own bbox is not
    the node extent (nodes outside it are clamped to edge cells); by default ours is."""
    if bbox:
        mx, mn = tuple(bbox[:3]), tuple(bbox[3:])
    else:
        xs = [n["pos"][0] for n in nodes]
        zs = [n["pos"][2] for n in nodes]
        mn = (min(xs), ymin, min(zs))
        mx = (max(xs), ymax, max(zs))
    side = 1 << (depth - 1)
    leaves = [[] for _ in range(side * side)]
    for i, n in enumerate(nodes):
        x, _, z = n["pos"]
        ix = max(0, min(side - 1, int((x - mn[0]) / max(mx[0] - mn[0], 1e-6) * side)))
        iz = max(0, min(side - 1, int((z - mn[2]) / max(mx[2] - mn[2], 1e-6) * side)))
        leaves[morton(ix, iz, depth - 1)].append(i)
    out = bytearray()
    for level in range(depth - 1):
        out += struct.pack("<I", 0) * (4 ** level)
    for cell in leaves:
        out += struct.pack("<I", len(cell)) + b"".join(struct.pack("<HI", i, 0) for i in cell)
    return mx + mn, bytes(out)


def finalize(g):
    """Recompute header counts and the quadtree from g['nodes']."""
    nodes = g["nodes"]
    nl = sum(len(n["links"]) for n in nodes)
    ne = sum(len(n["exp"]) for n in nodes)
    g["nAttrSlots"] = sum(len(n["attr"]) + len(n["exp"]) + sum(len(l["attr"]) for l in n["links"]) for n in nodes)
    g["hdr"] = (nl, len(nodes), nl, ne)
    g["gridDepth"] = 4
    g["bbox"], g["grid"] = build_grid(nodes, 4)
    return g


def from_heights(heights, origin, cell, max_slope=0.9, max_step=60.0):
    """Waypoint graph on a height grid (global coordinates): one node per sample,
    8-neighbour links where the slope and the step are walkable. NaN = no node."""
    nz, nx = len(heights), len(heights[0])
    x0, z0 = origin
    index, nodes = {}, []
    for j in range(nz):
        for i in range(nx):
            h = heights[j][i]
            if h != h:  # NaN
                continue
            index[(i, j)] = len(nodes)
            nodes.append({"id": len(nodes), "pos": (x0 + i * cell, float(h), z0 + j * cell),
                          "attr": [0], "links": [], "geomType": 0, "geom": b"", "exp": []})
    for (i, j), a in index.items():
        pa = nodes[a]["pos"]
        for di in (-1, 0, 1):
            for dj in (-1, 0, 1):
                b = index.get((i + di, j + dj))
                if b is None or b == a:
                    continue
                pb = nodes[b]["pos"]
                flat = ((pa[0] - pb[0]) ** 2 + (pa[2] - pb[2]) ** 2) ** 0.5
                dy = abs(pa[1] - pb[1])
                if dy / flat > max_slope and dy > max_step:
                    continue
                dist = (flat * flat + dy * dy) ** 0.5
                nodes[a]["links"].append({"target": b, "attr": [0], "cost": dist / 100.0,
                                          "size": 0.0, "height": 0.0})
    g = {"version": 0x26, "unk": 0, "nodes": nodes}
    return finalize(g)
