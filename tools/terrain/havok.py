"""Skyrim's live collision (DDDAGhosts' havok_export: one file per exterior cell) for the
streamer (docs/terrain-proxy.md, "Live Havok collision").

File: Data/SKSE/Plugins/DDDA_havok/{world:08X}_{x}_{y}.bin
  header 32 B: "DDHK", u32 version 1, u32 worldspace, i32 cell x, i32 cell y,
               u32 triangle count, u32 bodies, u32 reserved
  triangles 40 B each: f32 v[9] (Skyrim units, z up), u32 flags (bit 0 stair helper,
               bits 8..15 collision layer)

Live(cfg, K, base) maps them with the streamer's mapping to DDDA global coordinates
(x = DD x, y up, z = DD z) and answers what stream.py needs:
  covered(x, z)            the Skyrim cell under the point was harvested
  triangles(x0, z0, x1, z1) (n, 3, 3) triangles touching the rectangle, from covered cells
  surface(X, Z, ref)       the walkable surface height near `ref` (terrain, decks, steps)
  blocked(X, Z, ref, margin) something solid between knee and head height

Usage: py havok.py list | cell X Y | view M N [png]   (needs stream_config.json for view)
"""
import glob
import os
import struct
import sys

import numpy as np
import scipy.ndimage as ndi

DIR = r"E:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data\SKSE\Plugins\DDDA_havok"
TAMRIEL = 0x3C
CELL_UNITS = 4096.0
WALKABLE = 0.7       # normal y of a floor (45 degrees)
STEP_UP = 120.0      # DD cm: a floor this far above the reference is still "the ground here"
STEP_DOWN = 150.0
RASTER = 25.0        # DD cm
BAND_LO = 35.0       # DD cm above the walkable surface: what blocks a pawn
BAND_HI = 180.0
SAMPLE = 15.0        # DD cm between sample points on triangles (rasterising)
LEVEL_GAP = 30.0     # DD cm: floor points closer than this in one cell are one floor
STEP = 70.0          # DD cm: the largest step a pawn gets up (with the invisible ramps, RAMP_RISE)
MAX_LEVELS = 8       # floors kept per cell (the highest ones)
RAMP_RISE = 70.0     # DD cm: steps up to this get an invisible ramp in front (DDDA steps up less than Skyrim)
RAMP_SLOPE = 0.7     # rise per run of those ramps (35 degrees)
TERRAIN_LAYERS = (13, 17)  # COL_LAYER kTerrain, kGround: ramps start only at built floors (steps, decks, planks)
BUILT_FLAT = 0.95    # normal y of a built floor (18 degrees): treads, planks, decks; not rock tops
RAMP_RUN = 4         # raster cells (1 m) a ramp reaches out from its step


def read_cell(path):
    raw = open(path, "rb").read()
    magic, ver, world, cx, cy, count, bodies, _ = struct.unpack_from("<4sIIiiIII", raw, 0)
    if magic != b"DDHK" or ver != 1:
        raise ValueError(f"{path}: not a DDHK v1 file")
    t = np.frombuffer(raw, dtype=np.dtype([("v", "<f4", (9,)), ("flags", "<u4")]), count=count, offset=32)
    return {"world": world, "x": cx, "y": cy, "bodies": bodies, "v": t["v"].reshape(-1, 3, 3), "flags": t["flags"]}


def cells(world=TAMRIEL):
    """{(cell x, cell y): path} of every harvested cell."""
    out = {}
    for p in glob.glob(os.path.join(DIR, f"{world:08X}_*.bin")):
        _, x, y = os.path.basename(p)[:-4].split("_")
        out[(int(x), int(y))] = p
    return out


class Live:
    def __init__(self, cfg, K, base, world=TAMRIEL):
        self.sx0, self.sy0 = cfg["sky"]
        self.dx0, self.dz0 = cfg["dd"]
        self.K, self.base = K, base
        self.world = world
        self._ground = None  # the .esm terrain height function (set by the streamer)
        self.refresh()

    def refresh(self):
        """Re-scans the folder; returns the cells that are new (or rewritten) since last time."""
        now = {k: (p, os.path.getmtime(p)) for k, p in cells(self.world).items()}
        old = getattr(self, "_files", {})
        new = [k for k, v in now.items() if old.get(k, (None, None))[1] != v[1]]
        self._files = now
        self._cache = {k: v for k, v in getattr(self, "_cache", {}).items() if k in now and k not in new}
        if new:
            self._r = None
        return new

    # --- mapping ---
    def to_sky(self, x, z):
        return self.sx0 + (np.asarray(x) - self.dx0) / self.K, self.sy0 - (np.asarray(z) - self.dz0) / self.K

    def to_dd(self, v):
        """(..., 3) Skyrim points -> DD global (x, y, z)."""
        x = self.dx0 + (v[..., 0] - self.sx0) * self.K
        z = self.dz0 - (v[..., 1] - self.sy0) * self.K
        y = self.base + v[..., 2] * self.K
        return np.stack([x, y, z], -1)

    def cell_of(self, x, z):
        sx, sy = self.to_sky(x, z)
        return np.floor(sx / CELL_UNITS).astype(int), np.floor(sy / CELL_UNITS).astype(int)

    def covered(self, x, z):
        cx, cy = self.cell_of(x, z)
        f = np.vectorize(lambda a, b: (int(a), int(b)) in self._files, otypes=[bool])
        return f(cx, cy)

    def _cell(self, key):
        if key not in self._cache:
            c = read_cell(self._files[key][0])
            tri = self.to_dd(c["v"].astype(np.float64))
            # with z = -y the mapping mirrors: normals are recomputed, facing up
            n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
            ln = np.linalg.norm(n, axis=1)
            ok = ln > 1e-6
            tri, n, ln, flags = tri[ok], n[ok], ln[ok], c["flags"][ok]
            n = n / ln[:, None]
            flip = n[:, 1] < 0
            n[flip] *= -1
            tri[flip] = tri[flip][:, [0, 2, 1]]
            self._cache[key] = {"tri": tri, "n": n, "flags": flags,
                                "lo": tri.min(1), "hi": tri.max(1)}
        return self._cache[key]

    def _keys_for(self, x0, z0, x1, z1):
        sxa, sya = self.to_sky(x0, z1)
        sxb, syb = self.to_sky(x1, z0)
        out = []
        for cx in range(int(np.floor(sxa / CELL_UNITS)), int(np.floor(sxb / CELL_UNITS)) + 1):
            for cy in range(int(np.floor(sya / CELL_UNITS)), int(np.floor(syb / CELL_UNITS)) + 1):
                if (cx, cy) in self._files:
                    out.append((cx, cy))
        return out

    def triangles(self, x0, z0, x1, z1, flags=False):
        """(tri (n, 3, 3), normals (n, 3)[, flags (n,)]) touching the rectangle (DD global)."""
        tris, ns, fs = [], [], []
        for k in self._keys_for(x0, z0, x1, z1):
            c = self._cell(k)
            m = (c["hi"][:, 0] >= x0) & (c["lo"][:, 0] <= x1) & (c["hi"][:, 2] >= z0) & (c["lo"][:, 2] <= z1)
            tris.append(c["tri"][m])
            ns.append(c["n"][m])
            fs.append(c["flags"][m])
        if not tris:
            out = np.zeros((0, 3, 3)), np.zeros((0, 3)), np.zeros(0, np.uint32)
        else:
            out = np.concatenate(tris), np.concatenate(ns), np.concatenate(fs)
        return out if flags else out[:2]

    # --- rasters over an area (cached per area) ---
    def raster(self, x0, z0, x1, z1):
        """Every sampled surface point of the area, by RASTER cell (cell index, height,
        walkable). One raster serves every query inside it (built with a 5 m border)."""
        r = getattr(self, "_r", None)
        if r is not None and x0 >= r["x0"] and z0 >= r["z0"] and x1 <= r["x1"] and z1 <= r["z1"]:
            return r
        x0, z0 = np.floor((x0 - 500.0) / RASTER) * RASTER, np.floor((z0 - 500.0) / RASTER) * RASTER
        x1, z1 = x1 + 500.0, z1 + 500.0
        nx, nz = int(np.ceil((x1 - x0) / RASTER)) + 1, int(np.ceil((z1 - z0) / RASTER)) + 1
        tri, n, fl = self.triangles(x0, z0, x0 + nx * RASTER, z0 + nz * RASTER, flags=True)
        layer = (fl >> 8) & 0xFF
        pts, walk, built = sample(tri, n, ~np.isin(layer, TERRAIN_LAYERS) & (n[:, 1] >= BUILT_FLAT))
        i = np.floor((pts[:, 0] - x0) / RASTER).astype(int)
        j = np.floor((pts[:, 2] - z0) / RASTER).astype(int)
        ok = (i >= 0) & (j >= 0) & (i < nx) & (j < nz)
        r = {"x0": x0, "z0": z0, "x1": x0 + (nx - 1) * RASTER, "z1": z0 + (nz - 1) * RASTER, "nx": nx, "nz": nz,
             "cell": (j[ok] * nx + i[ok]), "y": pts[ok, 1], "walk": walk[ok], "built": built[ok], "occ": {}}
        self._r = r
        return r

    def _ij(self, r, X, Z):
        i = np.floor((np.asarray(X, float) - r["x0"]) / RASTER).astype(int)
        j = np.floor((np.asarray(Z, float) - r["z0"]) / RASTER).astype(int)
        return np.clip(i, 0, r["nx"] - 1), np.clip(j, 0, r["nz"] - 1)

    def reach(self, r, ground):
        """The floor a pawn reaches on foot in every raster cell (NaN: none), cached in r.

        Floors are the walkable points of a cell clustered into levels. Seeds: the level
        nearest the .esm terrain (within STEP_UP/STEP_DOWN). Then the reachable floor
        spreads to neighbouring cells' levels within STEP (stairs step by step, a bridge
        from its ends, a dock from the bank), keeping the highest one reached per cell. A
        roof, a table or a wall top starts higher than a step above anything around it, so
        it is never reached."""
        if "reach" in r:
            return r["reach"]
        nx, nz = r["nx"], r["nz"]
        nc = nx * nz
        w = r["walk"]
        cell, y = r["cell"][w], r["y"][w]
        order = np.lexsort((y, cell))
        cell, y = cell[order], y[order]
        Lv = np.full((nc, MAX_LEVELS), np.nan)
        if len(y):
            new = np.ones(len(y), bool)
            new[1:] = (cell[1:] != cell[:-1]) | (y[1:] - y[:-1] > LEVEL_GAP)
            last = np.append(new[1:], True)  # the top point of each level
            lc, ly = cell[last], y[last]
            # rank of each level from the top of its cell (levels are sorted by height)
            rank = np.searchsorted(lc, lc, side="right") - 1 - np.arange(len(lc))
            keep = rank < MAX_LEVELS
            Lv[lc[keep], rank[keep]] = ly[keep]
        jj, ii = np.mgrid[0:nz, 0:nx]
        G = ground(r["x0"] + (ii + 0.5) * RASTER, r["z0"] + (jj + 0.5) * RASTER).ravel()
        d = Lv - G[:, None]
        ok = (d >= -STEP_DOWN) & (d <= STEP_UP)
        near = np.where(ok, np.abs(d), np.inf)
        k = near.argmin(1)
        seed = np.isfinite(near.min(1))
        # graph over (cell, level): neighbouring cells' levels within STEP are joined; every
        # level in a component that holds a seed is reachable
        from scipy.sparse import coo_matrix
        from scipy.sparse.csgraph import connected_components
        L3 = Lv.reshape(nz, nx, MAX_LEVELS)
        node = np.arange(nc * MAX_LEVELS).reshape(nz, nx, MAX_LEVELS)
        ea, eb = [], []
        for dj, di in ((1, 0), (0, 1)):
            A = L3[:nz - dj, :nx - di]
            B = L3[dj:, di:]
            for k1 in range(MAX_LEVELS):
                for k2 in range(MAX_LEVELS):
                    m = np.abs(A[..., k1] - B[..., k2]) <= STEP
                    if m.any():
                        ea.append(node[:nz - dj, :nx - di, k1][m])
                        eb.append(node[dj:, di:, k2][m])
        n_nodes = nc * MAX_LEVELS
        if ea:
            ea, eb = np.concatenate(ea), np.concatenate(eb)
        else:
            ea = eb = np.zeros(0, int)
        g = coo_matrix((np.ones(len(ea), np.int8), (ea, eb)), shape=(n_nodes, n_nodes))
        _, label = connected_components(g, directed=False)
        good = np.zeros(label.max() + 1, bool)
        good[label[np.arange(nc)[seed] * MAX_LEVELS + k[seed]]] = True
        reach = good[label].reshape(nc, MAX_LEVELS) & np.isfinite(Lv)
        R = np.where(reach, Lv, -np.inf).max(1)
        R = np.where(np.isfinite(R), R, np.nan)
        r["reach"] = R.ravel()
        return r["reach"]

    def ramps(self, x0, z0, x1, z1):
        """Invisible ramps (n, 3, 3) in front of every reachable step up to RAMP_RISE in the
        area, like Skyrim's stair helpers: DDDA's characters step up less than Skyrim's, so a
        bridge's plank edge or a deck's lip stopped them (2026-10-02).

        Every reachable floor casts a RAMP_SLOPE cone downwards from its cell's edge, down
        to RAMP_RISE below it; the ramp surface is the highest cone over a lower floor. A
        staircase becomes one continuous sawtooth of ramps (the first version made one quad
        per detected lip, and its "not a hillside" test rejected most stair risers, since the
        next step also rises: pawns bounced off a Riverwood stair, 2026-10-03). Hillsides
        gentler than RAMP_SLOPE are left alone; drops over RAMP_RISE (deck edges) too."""
        r = self.raster(x0, z0, x1, z1)
        nx, nz = r["nx"], r["nz"]
        R = self.reach(r, self._ground).reshape(nz, nx)
        F = np.where(np.isfinite(R), R, -np.inf)

        def shifted(A, dj, di, fill=-np.inf):
            """N[j, i] = A[j + dj, i + di]."""
            N = np.full((nz, nx), fill)
            N[max(-dj, 0):nz + min(-dj, 0), max(-di, 0):nx + min(-di, 0)] = \
                A[max(dj, 0):nz + min(dj, 0), max(di, 0):nx + min(di, 0)]
            return N

        # Cones start only at flat built floors (steps, planks, decks): Skyrim's terrain
        # collision is rough at 25 cm and got ramps over a quarter of a tile (180-260k
        # triangles); rocks are static too, but their tops are not flat.
        w = r["walk"] & r["built"]
        top = np.full(nz * nx, -np.inf)
        np.maximum.at(top, r["cell"][w], r["y"][w])
        top = top.reshape(nz, nx)
        with np.errstate(invalid="ignore"):
            B = np.where(np.isfinite(F) & (np.abs(top - F) <= LEVEL_GAP), F, -np.inf)
        E = F.copy()
        k = int(np.ceil(RAMP_RISE / RAMP_SLOPE / RASTER)) + 1
        with np.errstate(invalid="ignore"):
            for dj in range(-k, k + 1):
                for di in range(-k, k + 1):
                    if not (di or dj):
                        continue
                    run = max(0.0, (np.hypot(di, dj) - 0.5) * RASTER)  # from the higher cell's edge
                    Q = shifted(B, dj, di)
                    cone = Q - RAMP_SLOPE * run
                    ok = (Q - F <= RAMP_RISE) & (cone > E)
                    E = np.where(ok, cone, E)
        ramp = np.isfinite(F) & (E > F + 3.0)
        if not ramp.any():
            return np.zeros((0, 3, 3))
        # A cell is a step's top when any of its samples is: the real edge lies inside it, so a
        # ramp ending at the cell's edge left a gap down to the lower tread. The step cells
        # next to a ramp get a quad too (flat at their own height, or rising to the next step).
        ramp |= np.isfinite(B) & ndi.binary_dilation(ramp, np.ones((3, 3), bool))
        # One quad per ramp cell. Each corner is the same cone envelope evaluated at the corner
        # itself (distance from the corner to each higher cell), over this cell's own floor, so
        # the quads reach the step above at its edge and come down to the floor at the foot.
        corner = {}
        with np.errstate(invalid="ignore"):
            for cj, ci in ((0, 0), (0, 1), (1, 0), (1, 1)):
                best = F.copy()
                for dj in range(-k, k + 1):
                    for di in range(-k, k + 1):
                        run = np.hypot(max(0, di - ci, ci - di - 1), max(0, dj - cj, cj - dj - 1)) * RASTER
                        Q = shifted(B, dj, di)
                        cone = Q - RAMP_SLOPE * run
                        best = np.where((Q - F <= RAMP_RISE) & (cone > best), cone, best)
                corner[(cj, ci)] = best
        lj, li = np.nonzero(ramp)
        x0c, z0c = r["x0"] + li * RASTER, r["z0"] + lj * RASTER
        P = {key: np.stack([x0c + key[1] * RASTER, v[lj, li], z0c + key[0] * RASTER], 1)
             for key, v in corner.items()}
        a, b, c, d = P[(0, 0)], P[(0, 1)], P[(1, 1)], P[(1, 0)]
        return np.concatenate([np.stack([a, b, c], 1), np.stack([a, c, d], 1)])

    def surface(self, X, Z, ref, r=None):
        """Walkable surface height at each point: the floor reached on foot (reach()), else
        the highest floor between ref - STEP_DOWN and ref + STEP_UP; `ref` where there is
        none (e.g. no data)."""
        X, Z, ref = (np.asarray(a, float) for a in np.broadcast_arrays(X, Z, ref))
        if r is None:
            r = self.raster(X.min(), Z.min(), X.max(), Z.max())
        i, j = self._ij(r, X, Z)
        q = (j * r["nx"] + i).ravel()
        if self._ground is not None:
            R = self.reach(r, self._ground)[q]
            if np.isfinite(R).all():
                return R.reshape(X.shape)
        else:
            R = np.full(len(q), np.nan)
        refcell = np.full(r["nx"] * r["nz"], np.nan)
        refcell[q] = ref.ravel()
        rp = refcell[r["cell"]]
        m = r["walk"] & (r["y"] >= rp - STEP_DOWN) & (r["y"] <= rp + STEP_UP)
        best = np.full(r["nx"] * r["nz"], -np.inf)
        np.maximum.at(best, r["cell"][m], r["y"][m])
        out = np.where(np.isfinite(R), R, best[q])
        return np.where(np.isfinite(out), out, ref.ravel()).reshape(X.shape)

    def solid(self, x0, z0, x1, z1, ground):
        """occ[j, i] over the area's raster: a surface point between BAND_LO and BAND_HI above
        the walkable surface of that cell (`ground(X, Z)` gives the reference height)."""
        r = self.raster(x0, z0, x1, z1)
        if "solid" not in r:
            nx, nz = r["nx"], r["nz"]
            jj, ii = np.mgrid[0:nz, 0:nx]
            X = r["x0"] + (ii + 0.5) * RASTER
            Z = r["z0"] + (jj + 0.5) * RASTER
            S = self.surface(X, Z, ground(X, Z), r)
            occ = np.zeros((nz, nx), bool)
            h = r["y"] - S.ravel()[r["cell"]]
            hit = (h > BAND_LO) & (h < BAND_HI)
            occ.ravel()[np.unique(r["cell"][hit])] = True
            r["solid"] = (occ, S)
        return (r,) + r["solid"]

    def clearance(self, x0, z0, x1, z1, ground):
        """(raster, cm to the nearest obstacle or edge per cell): obstacles are solid cells
        (solid()), edges are cells whose reachable floor differs from a neighbour's by more
        than STEP (the sides of a stair or a bridge, a deck's drop) or that have none."""
        r, occ, _ = self.solid(x0, z0, x1, z1, ground)
        if "clear" not in r:
            nx, nz = r["nx"], r["nz"]
            R = self.reach(r, ground).reshape(nz, nx)
            edge = ~np.isfinite(R)
            with np.errstate(invalid="ignore"):
                for dj, di in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    N = np.full((nz, nx), np.nan)
                    N[max(dj, 0):nz + min(dj, 0), max(di, 0):nx + min(di, 0)] =                         R[max(-dj, 0):nz + min(-dj, 0), max(-di, 0):nx + min(-di, 0)]
                    edge |= ~(np.abs(N - R) <= STEP)
            r["clear"] = ndi.distance_transform_edt(~(occ | edge)) * RASTER
        return r, r["clear"]

    def snap(self, X, Z, ground, reach_cm, good_cm=100.0):
        """Waypoint positions moved (up to reach_cm on each axis) to the clearest spot around
        them where their own spot is tight (clearance under good_cm): a node lands in the
        middle of a stair, a narrow bridge, a gate."""
        X, Z = np.asarray(X, float), np.asarray(Z, float)
        r, clear = self.clearance(X.min() - reach_cm, Z.min() - reach_cm, X.max() + reach_cm, Z.max() + reach_cm, ground)
        i, j = self._ij(r, X, Z)
        here = clear[j, i]
        k = int(reach_cm // RASTER)
        best, bi, bj = here.copy(), i.copy(), j.copy()
        for dj in range(-k, k + 1):
            for di in range(-k, k + 1):
                ci = np.clip(i + di, 0, r["nx"] - 1)
                cj = np.clip(j + dj, 0, r["nz"] - 1)
                c = clear[cj, ci] - 0.01 * np.hypot(di, dj)  # ties: the nearest
                better = c > best
                best, bi, bj = np.where(better, c, best), np.where(better, ci, bi), np.where(better, cj, bj)
        move = here < good_cm
        return (np.where(move, r["x0"] + (bi + 0.5) * RASTER, X), np.where(move, r["z0"] + (bj + 0.5) * RASTER, Z))

    def blocked(self, X, Z, ground, margin=60.0):
        X, Z = np.asarray(X, float), np.asarray(Z, float)
        pad = margin + 2 * RASTER
        r, occ, _ = self.solid(X.min() - pad, Z.min() - pad, X.max() + pad, Z.max() + pad, ground)
        k = int(np.ceil(margin / RASTER))
        if k not in r["occ"]:
            r["occ"][k] = ndi.binary_dilation(occ, np.ones((2 * k + 1, 2 * k + 1), bool)) if k else occ
        i, j = self._ij(r, X, Z)
        return r["occ"][k][j, i]


INTERIOR_DIR = os.path.join(DIR, "interior")
CURRENT = os.path.join(DIR, "current.txt")


def current_cell():
    """DDDAGhosts' report of where the player is: an interior cell's form id, 0 outside, or
    None (no report yet)."""
    try:
        words = open(CURRENT).read().split()
    except OSError:
        return None
    if words[:1] == ["interior"] and len(words) > 1:
        return int(words[1], 16)
    return 0 if words[:1] == ["exterior"] else None


def interior_path(cell):
    return os.path.join(INTERIOR_DIR, f"{cell:08X}.bin")


class InteriorLive(Live):
    """An interior cell's collision (one file, its own coordinates) mapped into an arena of
    DDDA's world: the same queries as Live, `covered` is the interior's footprint."""

    def __init__(self, cell, cfg, K, base):
        self.key = ("interior", cell)
        self.path = interior_path(cell)
        super().__init__(cfg, K, base)
        tri = self._cell(self.key)["tri"]
        self.lo, self.hi = tri.min((0, 1)), tri.max((0, 1))

    def refresh(self):
        self._files = {self.key: (self.path, os.path.getmtime(self.path))}
        self._cache = getattr(self, "_cache", {})
        return []

    def _keys_for(self, x0, z0, x1, z1):
        return [self.key]

    def raster(self, x0, z0, x1, z1):
        """One raster over the whole interior answers every query (nothing lies outside it):
        rebuilt per query area, an arena took 14 s (77 rasters, 71 reach graphs)."""
        r = getattr(self, "_r", None)
        if r is None:
            r = Live.raster(self, self.lo[0] - 1000, self.lo[2] - 1000, self.hi[0] + 1000, self.hi[2] + 1000)
        return r

    def covered(self, x, z):
        x, z = np.asarray(x, float), np.asarray(z, float)
        return (x >= self.lo[0] - 200) & (x <= self.hi[0] + 200) & (z >= self.lo[2] - 200) & (z <= self.hi[2] + 200)

    def lowest_floor(self, X, Z):
        """The lowest walkable surface at each point (NaN: none): where the reachable floors
        start, as the .esm terrain does outside."""
        X, Z = np.asarray(X, float), np.asarray(Z, float)
        r = self.raster(X.min(), Z.min(), X.max(), Z.max())
        if "low" not in r:
            low = np.full(r["nx"] * r["nz"], np.inf)
            np.minimum.at(low, r["cell"][r["walk"]], r["y"][r["walk"]])
            r["low"] = np.where(np.isfinite(low), low, np.nan)
        i, j = self._ij(r, X, Z)
        return r["low"][j * r["nx"] + i]


def sample(tri, n, tag=None, step=SAMPLE):
    """Points on every triangle about `step` apart (vertices and centroids included),
    whether each point lies on a walkable (floor) triangle, and (with `tag`, one bool per
    triangle) each point's triangle tag."""
    if tag is None:
        tag = np.zeros(len(tri), bool)
    if not len(tri):
        return np.zeros((0, 3)), np.zeros(0, bool), np.zeros(0, bool)
    e1 = np.linalg.norm(tri[:, 1] - tri[:, 0], axis=1)
    e2 = np.linalg.norm(tri[:, 2] - tri[:, 0], axis=1)
    e3 = np.linalg.norm(tri[:, 2] - tri[:, 1], axis=1)
    m = np.clip(np.ceil(np.maximum(np.maximum(e1, e2), e3) / step), 1, 200).astype(int)
    pts, walk, tags = [], [], []
    for k in np.unique(m):
        sel = np.where(m == k)[0]
        a, b = np.mgrid[0:k + 1, 0:k + 1]
        keep = a + b <= k
        u, v = a[keep] / k, b[keep] / k
        u = np.append(u, 1 / 3)
        v = np.append(v, 1 / 3)
        T = tri[sel]
        P = T[:, None, 0] + u[None, :, None] * (T[:, None, 1] - T[:, None, 0]) + v[None, :, None] * (T[:, None, 2] - T[:, None, 0])
        pts.append(P.reshape(-1, 3))
        walk.append(np.repeat(n[sel, 1] >= WALKABLE, len(u)))
        tags.append(np.repeat(tag[sel], len(u)))
    return np.concatenate(pts), np.concatenate(walk), np.concatenate(tags)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    if cmd == "list":
        cs = cells()
        print(f"{len(cs)} cells in {DIR}")
        for k in sorted(cs):
            c = read_cell(cs[k])
            layers = np.bincount((c["flags"] >> 8) & 0xFF, minlength=64)
            print(f"  {k}: {len(c['v'])} triangles, {c['bodies']} bodies, stair helper "
                  f"{int((c['flags'] & 1).sum())}, layers {[(i, int(v)) for i, v in enumerate(layers) if v]}")
    elif cmd == "cell":
        c = read_cell(cells()[(int(sys.argv[2]), int(sys.argv[3]))])
        v = c["v"]
        print(len(v), "triangles; bounds", v.reshape(-1, 3).min(0), v.reshape(-1, 3).max(0))
    elif cmd == "view":
        import json
        here = os.path.dirname(os.path.abspath(__file__))
        sys.path.insert(0, here)
        import stream
        cfg = json.load(open(os.path.join(here, "stream_config.json")))
        H = stream.make_height(cfg)
        L = H.live
        m, n = int(sys.argv[2]), int(sys.argv[3])
        x0, z0 = (n - 50) * 10000.0, (m - 50) * 10000.0
        r, occ, S = L.solid(x0, z0, x0 + 10000, z0 + 10000, H)
        jj, ii = np.mgrid[0:r["nz"], 0:r["nx"]]
        X, Z = r["x0"] + (ii + 0.5) * RASTER, r["z0"] + (jj + 0.5) * RASTER
        D = S - H(X, Z)
        # left: live floor minus .esm terrain (blue below, red above, +-100 cm); right: solid
        t = np.clip(D / 100.0, -1, 1)
        left = np.stack([np.where(t > 0, 255, 255 * (1 + t)), 255 * (1 - np.abs(t)), np.where(t < 0, 255, 255 * (1 - t))], -1)
        right = np.where(occ[..., None], 0, 255) * np.ones(3)
        right[~L.covered(X, Z)] = (255, 230, 150)  # not exported: yellow
        img = np.concatenate([left, np.full((left.shape[0], 8, 3), 128), right], 1)[::-1].astype(np.uint8)
        out = sys.argv[4] if len(sys.argv) > 4 else os.path.join(here, "out", f"havok_{m}m{n}n.png")
        write_png(out, img)
        print("wrote", out, "| cells", L._keys_for(x0, z0, x0 + 10000, z0 + 10000),
              f"| floor above terrain >20 cm: {(D > 20).mean() * 100:.1f}% | solid {occ.mean() * 100:.1f}%")


def write_png(path, img):
    import zlib
    h, w, _ = img.shape
    raw = b"".join(bytes(1) + img[y].tobytes() for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    signature = bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])
    open(path, "wb").write(signature + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

if __name__ == "__main__":
    main()
