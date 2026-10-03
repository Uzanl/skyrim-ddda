"""Skyrim's static objects as solid ground cells in DDDA's generated tiles.

Sources: out/Tamriel_objects.npz (skyobjects.py: every placed object, its base's OBND
box and model) and out/Tamriel_meshpts.npz (meshcache.py: surface points of each model).

Which objects (candidates()): STAT/MSTT whose EditorID is not walkable or decorative
(EXCLUDE), at least MIN_HEIGHT tall, MIN_SIZE long and at most MAX_SIZE across (bigger
ones are terrain-like: cliffs, mountains); trees (TREE, "Tree...") only as trunks.

How (Obstacles.raster): every candidate's model points are placed in the world (scale,
rotation, position; Skyrim's rotations are clockwise: world = Rz(-z) Ry(-y) Rx(-x) p,
checked on wall chains) and mapped to DDDA. Points between BAND_LO and BAND_HI above the
generated ground mark CELL-sized cells as solid (so an L-shaped house is an L, a porch
roof does not block, a fence is a thin line); the inside of each object's outline is
filled. Objects without a model use their OBND box. Trunks are discs.

Used by stream.py: solid cells become collision boxes (boxes(): rows of cells merged
into rectangles, from 1 m under the ground to the cell's top) and cut the waypoint
nodes (blocked(): solid within a margin).
"""
import os
import re

import numpy as np
import scipy.ndimage as ndi

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
MIN_HEIGHT = 60.0    # Skyrim units (86 cm in DDDA): lower things are stepped over
MIN_SIZE = 40.0      # the longer side
MAX_SIZE = 2600.0    # bigger boxes are terrain-like pieces (cliffs, mountains): skipped
TRUNK = 70.0         # tree trunk diameter, Skyrim units
TRUNK_HEIGHT = 500.0  # trees shorter than this are bushes
CELL = 25.0          # DD cm
BAND_LO = 30.0       # DD cm above the ground: the slice of an object that blocks
BAND_HI = 300.0
WALL_MIN = 100.0     # a solid cell's collision reaches at least this far above the ground
MARGIN = 60.0        # DD cm around solid cells where waypoint nodes are cut
EXCLUDE = re.compile(
    r"^ivy[0-9]|road|walkway(?!.*wall)|bridge|marker|cliff|mountain|rug|plank|dock|stair|floor|ramp|moss|grass|"
    r"fern|shrub|bush|snow|ice|water|light|fog|cloud|mist|dust|decal|path|trim|ground|pebble|"
    r"gravel|scree|lake|river|fx|hay|scatter|vine|leaves|roots?\b|debris|rubble|sawdust|"
    r"blood|sign|banner|rope|chain|cobweb|web|planter|crop|garden|flower|plant|mushroom|"
    r"lichen|kelp|reed|coral|ash|lava|steam|smoke|fire|torch|candle|lantern|noise|sound|"
    r"trigger|collision|navcut|plane|tarp|cloth|curtain|glow|beam|ray|godray|volume|"
    r"sky|horizon|lod|backdrop|distant",
    re.I)

_raw = _cand = _trees = _mesh = None


def raw():
    global _raw
    if _raw is None:
        _raw = dict(np.load(os.path.join(OUT, "Tamriel_objects.npz")))
    return _raw


def candidates():
    """Indices (into raw()) of the STAT/MSTT objects that block."""
    global _cand
    if _cand is None:
        z = raw()
        kinds = [str(k) for k in z["kinds"]]
        size = (z["hi"] - z["lo"]) * z["scale"][:, None]
        stat = (z["kind"] == kinds.index("STAT")) | (z["kind"] == kinds.index("MSTT"))
        keep = (stat & (size[:, 2] >= MIN_HEIGHT) & (np.maximum(size[:, 0], size[:, 1]) >= MIN_SIZE)
                & (np.maximum(size[:, 0], size[:, 1]) <= MAX_SIZE))
        keep &= np.array([not EXCLUDE.search(str(e)) for e in z["edid"]])
        _cand = np.where(keep)[0]
    return _cand


def candidate_models():
    z = raw()
    return [str(z["model"][i]) for i in candidates() if str(z["model"][i])]


def trees():
    global _trees
    if _trees is None:
        z = raw()
        kinds = [str(k) for k in z["kinds"]]
        size = (z["hi"] - z["lo"]) * z["scale"][:, None]
        keep = (z["kind"] == kinds.index("TREE")) & (size[:, 2] >= TRUNK_HEIGHT)
        keep &= np.array([str(e).startswith("Tree") and not re.search(r"stump|log|root|branch|sapling|shrub", str(e), re.I)
                          for e in z["edid"]])
        _trees = np.where(keep)[0]
    return _trees


def meshes():
    """{model (lower case): int16 points (n, 3)} (empty dict without meshcache.py's file)."""
    global _mesh
    if _mesh is None:
        _mesh = {}
        path = os.path.join(OUT, "Tamriel_meshpts.npz")
        if os.path.exists(path):
            z = np.load(path)
            pts = z["pts"]
            for m, s, c in zip(z["model"], z["start"], z["count"]):
                _mesh[str(m)] = pts[s:s + c]
    return _mesh


def rotation(r):
    """Skyrim reference rotation (radians, clockwise) as a 3x3 matrix."""
    x, y, zr = -r[0], -r[1], -r[2]
    cx, sx, cy, sy, cz, sz = np.cos(x), np.sin(x), np.cos(y), np.sin(y), np.cos(zr), np.sin(zr)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Rz @ Ry @ Rx


def obnd_points(lo, hi, step=12.0):
    """Points on the walls of an OBND box (objects without a model)."""
    xs = np.arange(lo[0], hi[0] + step, step)
    ys = np.arange(lo[1], hi[1] + step, step)
    zs = np.arange(lo[2], hi[2] + step, step * 4)
    out = []
    for x in (lo[0], hi[0]):
        Y, Z = np.meshgrid(ys, zs)
        out.append(np.stack([np.full(Y.size, x), Y.ravel(), Z.ravel()], 1))
    for y in (lo[1], hi[1]):
        X, Z = np.meshgrid(xs, zs)
        out.append(np.stack([X.ravel(), np.full(X.size, y), Z.ravel()], 1))
    return np.concatenate(out)


class Raster:
    """Solid cells of an area: occ[j, i] for the cell at (x0 + i CELL, z0 + j CELL), top = DD y."""

    def __init__(self, x0, z0, nx, nz):
        self.x0, self.z0 = x0, z0
        self.occ = np.zeros((nz, nx), bool)
        self.top = np.full((nz, nx), -1e9, np.float32)
        self._grown = {}

    def grown(self, margin):
        k = int(np.ceil(margin / CELL))
        if k not in self._grown:
            self._grown[k] = ndi.binary_dilation(self.occ, np.ones((2 * k + 1, 2 * k + 1), bool)) if k else self.occ
        return self._grown[k]

    def lookup(self, X, Z, margin=0.0):
        g = self.grown(margin)
        i = np.floor((np.asarray(X) - self.x0) / CELL).astype(int)
        j = np.floor((np.asarray(Z) - self.z0) / CELL).astype(int)
        ok = (i >= 0) & (j >= 0) & (i < g.shape[1]) & (j < g.shape[0])
        out = np.zeros(np.shape(X), bool)
        out[ok] = g[j[ok], i[ok]]
        return out


class Obstacles:
    """The obstacles mapped to DDDA global coordinates with the streamer's mapping."""

    def __init__(self, cfg, H, K, base):
        self.sx0, self.sy0 = cfg["sky"]
        self.dx0, self.dz0 = cfg["dd"]
        self.K, self.base, self.H = K, base, H
        z = raw()
        self.z = z
        self.ids = np.concatenate([candidates(), trees()])
        self.is_tree = np.concatenate([np.zeros(len(candidates()), bool), np.ones(len(trees()), bool)])
        pos = z["pos"][self.ids]
        self.c = np.stack(self.to_dd(pos[:, 0], pos[:, 1]), 1)
        ext = np.maximum(np.abs(z["lo"][self.ids]), np.abs(z["hi"][self.ids])) * z["scale"][self.ids, None]
        self.reach = np.hypot(ext[:, 0], ext[:, 1]) * K
        self.names = z["edid"][self.ids]
        self._rasters = {}

    def to_dd(self, sx, sy):
        return self.dx0 + (sx - self.sx0) * self.K, self.dz0 - (sy - self.sy0) * self.K

    def near(self, x0, z0, x1, z1, pad=0.0):
        """Indices (into self.ids) of objects that may touch the rectangle."""
        r = self.reach + pad
        c = self.c
        return np.where((c[:, 0] + r >= x0) & (c[:, 0] - r <= x1) & (c[:, 1] + r >= z0) & (c[:, 1] - r <= z1))[0]

    def world_points(self, k):
        """DD global points (n, 3) of object k (index into self.ids)."""
        i = self.ids[k]
        z = self.z
        p = meshes().get(str(z["model"][i]).lower())
        if p is None or not len(p):
            p = obnd_points(z["lo"][i], z["hi"][i])
        w = (p.astype(np.float64) * z["scale"][i]) @ rotation(z["rot"][i]).T + z["pos"][i]
        gx, gz = self.to_dd(w[:, 0], w[:, 1])
        return np.stack([gx, self.base + w[:, 2] * self.K, gz], 1)

    def raster(self, x0, z0, x1, z1):
        key = (round(x0), round(z0), round(x1), round(z1))
        if key in self._rasters:
            return self._rasters[key]
        x0, z0 = np.floor(x0 / CELL) * CELL, np.floor(z0 / CELL) * CELL
        nx, nz = int(np.ceil((x1 - x0) / CELL)) + 1, int(np.ceil((z1 - z0) / CELL)) + 1
        R = Raster(x0, z0, nx, nz)
        for k in self.near(x0, z0, x0 + nx * CELL, z0 + nz * CELL, MARGIN):
            if self.is_tree[k]:
                self._trunk(R, k)
                continue
            P = self.world_points(k)
            g = self.H(P[:, 0], P[:, 2])
            h = P[:, 1] - g
            band = (h >= BAND_LO) & (h <= BAND_HI)
            if not band.any():
                continue
            P, g = P[band], g[band]
            i = np.floor((P[:, 0] - x0) / CELL).astype(int)
            j = np.floor((P[:, 2] - z0) / CELL).astype(int)
            # the object's own small grid: fill the inside of its outline, then paste it
            i0, j0 = i.min() - 2, j.min() - 2
            sub = np.zeros((j.max() - j0 + 3, i.max() - i0 + 3), bool)
            sub[j - j0, i - i0] = True
            sub = ndi.binary_fill_holes(ndi.binary_closing(sub, np.ones((3, 3), bool)) | sub)
            top = np.full(sub.shape, -1e9, np.float32)
            np.maximum.at(top, (j - j0, i - i0), np.maximum(P[:, 1], g + WALL_MIN).astype(np.float32))
            top = np.where(sub, ndi.maximum_filter(top, 5), -1e9).astype(np.float32)
            self._paste(R, sub, top, i0, j0)
        self._rasters[key] = R
        return R

    def _trunk(self, R, k):
        i = self.ids[k]
        gx, gz = self.c[k]
        r = TRUNK / 2 * self.z["scale"][i] * self.K
        n = int(np.ceil(r / CELL)) + 1
        ci, cj = int((gx - R.x0) // CELL), int((gz - R.z0) // CELL)
        jj, ii = np.mgrid[-n:n + 1, -n:n + 1]
        sub = (np.hypot((ci + ii + 0.5) * CELL + R.x0 - gx, (cj + jj + 0.5) * CELL + R.z0 - gz) <= r)
        sub[n, n] = True
        top = np.where(sub, float(self.H(gx, gz)) + BAND_HI, -1e9).astype(np.float32)
        self._paste(R, sub, top, ci - n, cj - n)

    @staticmethod
    def _paste(R, sub, top, i0, j0):
        nz, nx = R.occ.shape
        a0, b0 = max(0, -j0), max(0, -i0)
        a1, b1 = min(sub.shape[0], nz - j0), min(sub.shape[1], nx - i0)
        if a0 >= a1 or b0 >= b1:
            return
        win = (slice(j0 + a0, j0 + a1), slice(i0 + b0, i0 + b1))
        R.occ[win] |= sub[a0:a1, b0:b1]
        R.top[win] = np.maximum(R.top[win], top[a0:a1, b0:b1])

    def boxes(self, x0, z0, x1, z1):
        """[(corners xz (4, 2), y bottom, y top)]: the solid cells of the area merged into
        rectangles (runs along x, then equal runs along z, at most 10 m long)."""
        R = self.raster(x0, z0, x1, z1)
        occ = R.occ
        open_runs = {}  # (i0, i1) -> [j0, j1, top]
        rects = []
        for j in range(occ.shape[0] + 1):
            runs = set()
            if j < occ.shape[0]:
                row = np.concatenate([[False], occ[j], [False]])
                d = np.flatnonzero(np.diff(row.astype(np.int8)))
                runs = set(zip(d[::2].tolist(), d[1::2].tolist()))
            for r in list(open_runs):
                if r not in runs or open_runs[r][1] - open_runs[r][0] >= 40:
                    rects.append((r, *open_runs.pop(r)))
            for r in runs:
                t = float(R.top[j, r[0]:r[1]].max())
                if r in open_runs:
                    open_runs[r][1] = j + 1
                    open_runs[r][2] = max(open_runs[r][2], t)
                else:
                    open_runs[r] = [j, j + 1, t]
        out = []
        for (i0, i1), j0, j1, top in rects:
            ax, bx = R.x0 + i0 * CELL, R.x0 + i1 * CELL
            az, bz = R.z0 + j0 * CELL, R.z0 + j1 * CELL
            cs = np.array([(ax, az), (bx, az), (bx, bz), (ax, bz)])
            g = self.H(np.append(cs[:, 0], (ax + bx) / 2), np.append(cs[:, 1], (az + bz) / 2))
            out.append((cs, float(g.min()) - 100.0, max(top, float(g.max()) + WALL_MIN)))
        return out

    def blocked(self, X, Z, margin=MARGIN):
        """Boolean array: points within `margin` of a solid cell."""
        X, Z = np.asarray(X, float), np.asarray(Z, float)
        pad = margin + CELL
        R = self.raster(X.min() - pad, Z.min() - pad, X.max() + pad, Z.max() + pad)
        return R.lookup(X, Z, margin)
