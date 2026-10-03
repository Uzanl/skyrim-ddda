"""Minimal Skyrim SE NIF reader: the triangles of the render meshes in model space.

triangles(data) -> float32 array (n, 3, 3), Skyrim units, model space (z up).

Only what is needed for footprints: the header (block types and sizes), NiNode-like
blocks (transform + children) and BSTriShape / BSLODTriShape / BSMeshLODTriShape
(vertex data with full-precision positions, as in SSE). Other blocks are skipped by
their size. Hidden objects (NiAVObject flag 1) and skinned or alpha-tested shapes are
kept: the caller filters by height.

Transforms: a child's point p goes to the parent as R @ (s * p) + t, with R the 3x3
NiAVObject rotation as stored (row-major). Checked against the ESM's OBND boxes
(mesh_check in meshcache.py).
"""
import struct

import numpy as np

SHAPES = {"BSTriShape", "BSLODTriShape", "BSMeshLODTriShape", "BSDynamicTriShape"}


class Nif:
    def __init__(self, d):
        line_end = d.index(b"\n")
        assert d.startswith(b"Gamebryo File Format"), d[:40]
        o = line_end + 1
        self.version, = struct.unpack_from("<I", d, o)
        o += 4
        o += 1  # endian
        self.user_version, nblocks = struct.unpack_from("<II", d, o)
        o += 8
        self.bs_version, = struct.unpack_from("<I", d, o)
        o += 4
        for _ in range(3):  # author, process script, export script (u8 length strings)
            o += 1 + d[o]
        if self.bs_version >= 130:
            o += 1 + d[o]
        ntypes, = struct.unpack_from("<H", d, o)
        o += 2
        types = []
        for _ in range(ntypes):
            n, = struct.unpack_from("<I", d, o)
            types.append(d[o + 4:o + 4 + n].decode("latin1"))
            o += 4 + n
        idx = struct.unpack_from(f"<{nblocks}H", d, o)
        o += 2 * nblocks
        self.types = [types[i & 0x7FFF] for i in idx]
        sizes = struct.unpack_from(f"<{nblocks}I", d, o)
        o += 4 * nblocks
        nstrings, _ = struct.unpack_from("<II", d, o)
        o += 8
        for _ in range(nstrings):
            n, = struct.unpack_from("<I", d, o)
            o += 4 + n
        ngroups, = struct.unpack_from("<I", d, o)
        o += 4 + 4 * ngroups
        self.offsets = []
        for s in sizes:
            self.offsets.append(o)
            o += s
        self.d = d

    def av(self, b):
        """NiObjectNET + NiAVObject fields of block b: (flags, R, t, s, end offset)."""
        d, o = self.d, self.offsets[b]
        o += 4  # name
        n, = struct.unpack_from("<I", d, o)
        o += 4 + 4 * n + 4  # extra data refs, controller
        flags, = struct.unpack_from("<I", d, o)
        o += 4
        t = np.array(struct.unpack_from("<3f", d, o))
        R = np.array(struct.unpack_from("<9f", d, o + 12)).reshape(3, 3)
        s, = struct.unpack_from("<f", d, o + 48)
        o += 52 + 4  # + collision object ref
        return flags, R, t, s, o

    def children(self, b):
        _, _, _, _, o = self.av(b)
        n, = struct.unpack_from("<I", self.d, o)
        return [c for c in struct.unpack_from(f"<{n}i", self.d, o + 4) if c >= 0]

    def shape(self, b):
        """Model-local triangles of a BSTriShape block (n, 3, 3)."""
        d = self.d
        _, _, _, _, o = self.av(b)
        o += 16 + 12  # bounding sphere, skin, shader property, alpha property
        desc, = struct.unpack_from("<Q", d, o)
        o += 8
        ntri, nvert = struct.unpack_from("<HH", d, o)
        o += 4
        size, = struct.unpack_from("<I", d, o)
        o += 4
        stride = (desc & 0xF) * 4
        if not size or not stride or not nvert or not ntri:
            return np.zeros((0, 3, 3), np.float32)
        attrs = (desc >> 44) & 0xFFF
        if not attrs & 1:  # no positions
            return np.zeros((0, 3, 3), np.float32)
        raw = np.frombuffer(d, np.uint8, nvert * stride, o).reshape(nvert, stride)
        if self.bs_version < 130 or attrs & (1 << 10):  # SSE: full-precision floats
            pos = raw[:, :12].copy().view(np.float32).reshape(nvert, 3)
        else:
            pos = raw[:, :6].copy().view(np.float16).reshape(nvert, 3).astype(np.float32)
        o += nvert * stride
        tris = np.frombuffer(d, np.uint16, ntri * 3, o).reshape(ntri, 3)
        tris = tris[(tris < nvert).all(1)]
        return pos[tris]

    def triangles(self):
        out = []

        def walk(b, M, depth=0):
            if depth > 64:
                return
            t = self.types[b]
            flags, R, tr, s, _ = self.av(b)
            L = np.eye(4)
            L[:3, :3] = R * s
            L[:3, 3] = tr
            W = M @ L
            if t in SHAPES:
                tri = self.shape(b)
                if len(tri):
                    p = tri.reshape(-1, 3) @ W[:3, :3].T + W[:3, 3]
                    out.append(p.reshape(-1, 3, 3).astype(np.float32))
            elif "Node" in t:
                for c in self.children(b):
                    if c < len(self.types):
                        walk(c, W, depth + 1)

        if self.types and "Node" in self.types[0]:
            walk(0, np.eye(4))
        return np.concatenate(out) if out else np.zeros((0, 3, 3), np.float32)


def triangles(data):
    return Nif(data).triangles()
