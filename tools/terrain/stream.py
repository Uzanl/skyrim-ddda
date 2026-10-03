"""Terrain streaming: DDDA tiles around the party are generated from Skyrim's terrain
just before DDDA loads them (docs/terrain-proxy.md, "Streaming").

DDDA reads a tile's archives from disk when the tile enters its loaded area (about 2
tiles around the party) and keeps them in memory until it leaves; archives replaced on
disk while the game runs are read the next time (leap_test.py, 2026-10-02). So every
tile within RADIUS (3) of the party is kept generated for the current mapping.

Mapping (fixed for a session, stream_config.json): DD global (gx, gz) <-> Skyrim
(sx, sy) through an anchor pair and scale K = 100/70 (DD x = Skyrim x, DD z = -Skyrim
y); height: Skyrim's lowest terrain (-37032) is 1 m above DDDA's sea (30013), so no
generated ground is ever under DDDA's water.

Waypoint graphs use a FIXED layout: graph (m, n) covers DD global x in
[(n-50)*1e4 - 5000, +10000) and z likewise with m (the game's own graphs are centred on
the tile corner), NODES x NODES nodes NODE_STEP apart in row-major order, so a graph can link to
its neighbours' nodes ("exp" links) before they are generated.

Usage: py stream.py config SKY_X SKY_Y DD_X DD_Z   anchor: Skyrim point = DD global point
       py stream.py tile M N                       generate one tile (collision + graph)
       py stream.py run [M N]                      keep generating around the party
       py stream.py restore                        put every original archive back
"""
import json
import math
import os
import shutil
import sys
import threading
import time

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "recon"))
import arc  # noqa: E402
import havok  # noqa: E402
import obstacles  # noqa: E402
import sbc  # noqa: E402
import sbcgen  # noqa: E402
import spotcheck as sc  # noqa: E402  (Skyrim height grid)
import way  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROM = r"E:\SteamLibrary\steamapps\common\DDDA\nativePC\rom\stage\stage100"
BACKUP = os.path.join(HERE, "..", "..", "backups", "ddda_arc")
CONFIG = os.path.join(HERE, "stream_config.json")
STATE = os.path.join(HERE, "stream_state.json")
SKYRIM_PLUGINS = r"E:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data\SKSE\Plugins"
EXPERIMENT = r"E:\SteamLibrary\steamapps\common\DDDA\ddda_experiment.txt"
TILE = 10000.0
SBC_TYPE, WAY_TYPE = 0x51FC779F, 0x5F36B659
COL_CELL = 200.0
NODE_STEP = 200.0  # 4 m missed stairs and narrow bridges (2026-10-02); the game's graphs have up to ~830 nodes
NODES = 50  # per side of a graph
RADIUS = 3
K = 100.0 / 70.0
DD_SEA = 30013.0
SKY_LOWEST = -37032.0
MAX_SLOPE = 1.0
OBJECTS = True  # Skyrim static objects as boxes (obstacles.py)
LIVE = True     # Skyrim's live collision where DDDAGhosts exported it (havok.py)
LINK_STEP = 75.0  # cm: a link whose ground jumps more than this between samples (50 cm apart) is dropped (ramps cover up to 70)


# --- mapping ---------------------------------------------------------------------------
def load_config():
    return json.load(open(CONFIG))


BASE_MIN = DD_SEA + 100.0 - SKY_LOWEST * K  # Skyrim's lowest terrain 1 m above DDDA's sea
BASE_MAX = 190000.0  # keeps Skyrim's highest terrain (39392) under the graphs' 300000 bound


def base_of(cfg):
    """DD y of Skyrim height 0 (leaps raise it so they always go up)."""
    return cfg.get("base", BASE_MIN)


def make_height(cfg):
    sx0, sy0 = cfg["sky"]
    dx0, dz0 = cfg["dd"]
    base = base_of(cfg)

    def h(gx, gz):
        gx, gz = np.asarray(gx, dtype=np.float64), np.asarray(gz, dtype=np.float64)
        sx = sx0 + (gx - dx0) / K
        sy = sy0 - (gz - dz0) / K
        i = sx / 128 - sc.cx0 * 32
        j = sy / 128 - sc.cy0 * 32
        ny, nx = sc.H.shape
        i = np.clip(i, 0, nx - 1.001)
        j = np.clip(j, 0, ny - 1.001)
        i0, j0 = np.floor(i).astype(int), np.floor(j).astype(int)
        fi, fj = i - i0, j - j0
        H = sc.H
        y = (H[j0, i0] * (1 - fi) * (1 - fj) + H[j0, i0 + 1] * fi * (1 - fj)
             + H[j0 + 1, i0] * (1 - fi) * fj + H[j0 + 1, i0 + 1] * fi * fj)
        y = np.where(np.isnan(y), SKY_LOWEST, y)
        return base + y * K
    h.obs = obstacles.Obstacles(cfg, h, K, base) if OBJECTS else None
    h.live = havok.Live(cfg, K, base) if LIVE else None
    if h.live is not None:
        h.live._ground = h  # floors are reached on foot starting from this terrain
    return h


# --- what stands where: live collision where Skyrim exported it, else the .esm sources ----
def covered(H, X, Z):
    if H.live is None:
        return np.zeros(np.shape(X), bool)
    return H.live.covered(X, Z)


def ground_y(H, X, Z):
    """Walkable height: the .esm terrain, raised onto decks and steps where covered."""
    Y = H(X, Z)
    C = covered(H, X, Z)
    if C.any():
        Y = np.where(C, H.live.surface(X, Z, Y), Y)
    return Y


def blocked(H, X, Z, margin=obstacles.MARGIN):
    X, Z = np.asarray(X, float), np.asarray(Z, float)
    out = np.zeros(X.shape, bool)
    C = covered(H, X, Z)
    if (~C).any() and H.obs is not None:
        out[~C] = H.obs.blocked(X[~C], Z[~C], margin=margin)
    if C.any():
        out[C] = H.live.blocked(X[C], Z[C], H, margin=margin)
    return out


def write_plugin_ini(cfg):
    """Skyrim plugin anchor (DDDABridge_terrain.ini): Skyrim (x, y, 0) = DD global (x, y(0), z)."""
    text = ("# Skyrim point = DDDA global point; generated by tools/terrain/stream.py\n"
            f"sky {cfg['sky'][0]:.3f} {cfg['sky'][1]:.3f} 0.000\n"
            f"dd {cfg['dd'][0]:.3f} {base_of(cfg):.3f} {cfg['dd'][1]:.3f}\n")
    path = os.path.join(SKYRIM_PLUGINS, "DDDABridge_terrain.ini")
    open(path + ".tmp", "w").write(text)
    os.replace(path + ".tmp", path)  # the plugin re-reads it live: never a half-written file
    return text


INTERIOR_INI = os.path.join(SKYRIM_PLUGINS, "DDDABridge_interior.ini")


def write_interior_ini(cell, cfg):
    """The interior's mapping onto its arena (DDDABridge_interior.ini): the plugin uses it
    while the player is in that cell."""
    text = (f"# interior {cell:08X} -> arena; generated by tools/terrain/stream.py\n"
            f"cell {cell:08X}\n"
            f"sky {cfg['sky'][0]:.3f} {cfg['sky'][1]:.3f} 0.000\n"
            f"dd {cfg['dd'][0]:.3f} {base_of(cfg):.3f} {cfg['dd'][1]:.3f}\n")
    open(INTERIOR_INI + ".tmp", "w").write(text)
    os.replace(INTERIOR_INI + ".tmp", INTERIOR_INI)


def remove_interior_ini():
    try:
        os.remove(INTERIOR_INI)
    except OSError:
        pass


# --- archives --------------------------------------------------------------------------
def sub(m, n):
    return os.path.join(f"m{m // 10 * 10}", f"n{n // 10 * 10}")


def col_arc(m, n):
    return os.path.join(ROM, "split", sub(m, n), f"st100_{m}m{n}n.arc")


def way_arc(m, n):
    return os.path.join(ROM, "split_way", sub(m, n), f"st100_{m}m{n}n_way.arc")


def original(path):
    """The game's own archive: the backup if we replaced it before, else the file."""
    bak = os.path.join(BACKUP, os.path.basename(path))
    return bak if os.path.exists(bak) else path


def install(path, data):
    os.makedirs(BACKUP, exist_ok=True)
    bak = os.path.join(BACKUP, os.path.basename(path))
    if not os.path.exists(bak):
        shutil.copy2(path, bak)
    tmp = path + ".tmp"
    open(tmp, "wb").write(data)
    os.replace(tmp, path)


def has_way(m, n):
    p = way_arc(m, n)
    return os.path.exists(p) and any(e[1] == WAY_TYPE for e in arc.entries(original(p)))


# --- generation ------------------------------------------------------------------------
def gen_collision(m, n, H):
    path = col_arc(m, n)
    if not os.path.exists(path):
        return False
    src = original(path)
    name, entry = next((e[0], e) for e in arc.entries(src) if e[1] == SBC_TYPE and "st100h_" in e[0])
    template = sbc.parse(arc.data(entry))
    ox, oz = (n - 50) * TILE, (m - 50) * TILE
    ls = np.arange(-COL_CELL, TILE + 2 * COL_CELL, COL_CELL)
    LX, LZ = np.meshgrid(ls, ls)
    a0, a1 = ls[0], ls[-1]
    live_cells = H.live is not None and H.live._keys_for(ox + a0, oz + a0, ox + a1, oz + a1)
    keep = (lambda x, z: not covered(H, x + ox, z + oz)) if live_cells else None
    solids = []
    if H.obs is not None:  # Skyrim's houses, walls, rocks, trunks (obstacles.py), where not live
        for cs, y0, y1 in H.obs.boxes(ox + a0, oz + a0, ox + a1, oz + a1):
            if keep and not keep(float(cs[:, 0].mean()) - ox, float(cs[:, 1].mean()) - oz):
                continue
            solids.append(([(x - ox, z - oz) for x, z in cs], y0, y1))
    tris = []
    if live_cells:  # Skyrim's own collision triangles: terrain, houses, decks, stairs
        T, _ = H.live.triangles(ox + a0, oz + a0, ox + a1, oz + a1)
        Rp = H.live.ramps(ox + a0, oz + a0, ox + a1, oz + a1)  # invisible ramps at plank edges, lips
        c = Rp.mean(1)
        Rp = Rp[(c[:, 0] >= ox + a0) & (c[:, 0] <= ox + a1) & (c[:, 2] >= oz + a0) & (c[:, 2] <= oz + a1)]
        tris = (np.concatenate([T, Rp]) - np.array([ox, 0.0, oz])).tolist()
    data = sbcgen.build(H(LX + ox, LZ + oz).tolist(), (float(ls[0]), float(ls[0])), COL_CELL, template,
                        solids=solids, triangles=tris, keep_cell=keep)
    install(path, arc.rebuild(src, {name: data}))
    return True


def node_xz(m, n, i, j):
    x0 = (n - 50) * TILE - TILE / 2 + NODE_STEP / 2
    z0 = (m - 50) * TILE - TILE / 2 + NODE_STEP / 2
    return x0 + i * NODE_STEP, z0 + j * NODE_STEP


def gen_way(m, n, H):
    path = way_arc(m, n)
    if not has_way(m, n):
        return False
    src = original(path)
    name, entry = next((e[0], e) for e in arc.entries(src) if e[1] == WAY_TYPE)
    orig_count = len(way.parse(arc.data(entry))["nodes"])
    # heights of this graph's nodes plus a 1-node border (the neighbours' edge nodes)
    ii = np.arange(-1, NODES + 1)
    X = np.array([[node_xz(m, n, i, j)[0] for i in ii] for j in ii])
    Z = np.array([[node_xz(m, n, i, j)[1] for i in ii] for j in ii])
    if H.live is not None:  # tight spots: the node moves to the middle of the passage
        C = covered(H, X, Z)
        if C.any():
            sx, sz = H.live.snap(X[C], Z[C], H, NODE_STEP / 2 - 25.0)
            X, Z = X.copy(), Z.copy()
            X[C], Z[C] = sx, sz
    Y = ground_y(H, X, Z)
    # nodes inside an obstacle are cut off (no links, parked at y 0 like the padding nodes)
    B = blocked(H, X, Z)
    if getattr(H, "catch", None) is not None:  # an interior's arena: nothing to walk on the catch floor
        B |= Y <= H.catch + 1.0
    # CROSS[(di, dj)][j, i]: the link from node (i, j) towards (i + di, j + dj) passes
    # through a box (7 points along it, all links of a direction in one go)
    CROSS = {}
    if H.obs is not None or H.live is not None:
        t = np.linspace(0.0, 1.0, 9)[1:-1, None, None]
        tt = np.linspace(0.0, 1.0, 9)[:, None, None]
        for dj in (-1, 0, 1):
            for di in (-1, 0, 1):
                if di or dj:
                    xa, za = X[1:-1, 1:-1], Z[1:-1, 1:-1]
                    xb, zb = X[1 + dj:NODES + 1 + dj, 1 + di:NODES + 1 + di], Z[1 + dj:NODES + 1 + dj, 1 + di:NODES + 1 + di]
                    cross = blocked(H, xa + (xb - xa) * t, za + (zb - za) * t, margin=25.0).any(0)
                    if H.live is not None:  # no step taller than LINK_STEP along the link (bridge edges, decks)
                        xs, zs = xa + (xb - xa) * tt, za + (zb - za) * tt
                        C = covered(H, xs, zs)
                        if C.any():
                            ys = ground_y(H, xs, zs)
                            cross |= (np.abs(np.diff(ys, axis=0)) > LINK_STEP).any(0) & C.any(0)
                    CROSS[(di, dj)] = cross
    nodes = []
    for j in range(NODES):
        for i in range(NODES):
            nodes.append({"id": len(nodes), "pos": (float(X[j + 1, i + 1]),
                                                     0.0 if B[j + 1, i + 1] else float(Y[j + 1, i + 1]),
                                                     float(Z[j + 1, i + 1])),
                          "attr": [0], "links": [], "geomType": 0, "geom": b"", "exp": []})
    for j in range(NODES):
        for i in range(NODES):
            a = nodes[j * NODES + i]
            pa = a["pos"]
            if B[j + 1, i + 1]:
                continue
            for dj in (-1, 0, 1):
                for di in (-1, 0, 1):
                    if di == 0 and dj == 0:
                        continue
                    pb = (X[j + 1 + dj, i + 1 + di], Y[j + 1 + dj, i + 1 + di], Z[j + 1 + dj, i + 1 + di])
                    flat = math.hypot(pa[0] - pb[0], pa[2] - pb[2])
                    if abs(pa[1] - pb[1]) > MAX_SLOPE * flat:
                        continue
                    if B[j + 1 + dj, i + 1 + di] or (CROSS and CROSS[(di, dj)][j, i]):
                        continue
                    ti, tj = i + di, j + dj
                    dn = -1 if ti < 0 else 1 if ti >= NODES else 0
                    dm = -1 if tj < 0 else 1 if tj >= NODES else 0
                    idx = (tj % NODES) * NODES + (ti % NODES)
                    if dm == 0 and dn == 0:
                        a["links"].append({"target": idx, "attr": [0], "cost": math.dist(pa, pb) / 100.0,
                                           "size": 0.0, "height": 0.0})
                    elif has_way(m + dm, n + dn):
                        a["exp"].append(((dm + 1) * 3 + (dn + 1)) << 16 | idx)
    x, _, z = nodes[0]["pos"]
    while len(nodes) < orig_count:  # other data may hold indices of the game's graph
        nodes.append({"id": len(nodes), "pos": (x, 0.0, z), "attr": [0], "links": [], "geomType": 0,
                      "geom": b"", "exp": []})
    g = way.finalize({"version": 0x26, "unk": 0, "nodes": nodes})
    install(path, arc.rebuild(src, {name: way.write(g)}))
    return True


# --- interiors (docs/terrain-proxy.md, "Interiors") -------------------------------------
# An interior has its own coordinates. Its collision (DDDAGhosts: DDDA_havok/interior/) is
# mapped into an "arena": tiles deep inside DDDA's map, far from the party, whose ground is
# that interior only, over a flat catch floor ARENA_CATCH below its lowest point (a hole
# in the collision must not drop a pawn into the void). The tiles ARENA_RING around it
# are catch floor only (DDDA loads about 2 tiles around the party; older generated ground
# there could stick into the arena; ring tiles DDDA does not have are void, which is fine).
# The mapping goes up, like the leaps.
ARENA_CATCH = 200.0  # cm under the interior's lowest point
ARENA_RING = 2       # tiles of catch floor around the interior's tiles


def arena_height(cell, cfg):
    """Height function for an arena: the interior's lowest floor where it has one (the
    reachable floors start there, as the .esm terrain does outside), else the catch floor."""
    live = havok.InteriorLive(cell, cfg, K, base_of(cfg))
    catch = float(live.lo[1]) - ARENA_CATCH

    def h(gx, gz):
        gx, gz = np.broadcast_arrays(np.asarray(gx, dtype=np.float64), np.asarray(gz, dtype=np.float64))
        out = np.full(gx.shape, catch)
        C = live.covered(gx, gz)
        if C.any():
            low = live.lowest_floor(gx[C], gz[C])
            out[C] = np.where(np.isfinite(low), low, catch)
        return out
    h.obs, h.live, h.catch = None, live, catch
    live._ground = h
    return h


def plan_arena(cell, here, ay, world=()):
    """(cfg, tiles) for an interior: the deepest-inside tile far enough from `here` and from
    every tile of the world's generated ground (`world`) takes its middle; the base puts its
    lowest point LEAP_UP above the Arisen (ay). `here` alone is not enough: a streamer started
    while the player was inside saw the old arena as the party's tile and put the new arena
    on Riverwood's ground (2026-10-03)."""
    v = havok.read_cell(havok.interior_path(cell))["v"].reshape(-1, 3)
    lo, hi = v.min(0), v.max(0)
    half = int(math.ceil(max(hi[0] - lo[0], hi[1] - lo[1]) * K / 2 / TILE))
    best = None
    for (tm, tn), d in interior().items():
        away = max(abs(tm - here[0]), abs(tn - here[1]))
        # the interior's own tiles must all exist (ring tiles that do not are void: fine)
        if d < half + 1 or away < RADIUS + half + ARENA_RING + 1 or not has_way(tm, tn):
            continue
        if any(max(abs(tm - wm), abs(tn - wn)) < half + ARENA_RING + 2 for wm, wn in world):
            continue
        key = (d, -away)
        if best is None or key > best[0]:
            best = (key, (tm, tn))
    if best is None:
        return None
    m, n = best[1]
    gx, gz = (n - 50) * TILE + TILE / 2, (m - 50) * TILE + TILE / 2
    base = max(BASE_MIN, ay + LEAP_UP - lo[2] * K)
    base = min(base, BASE_MAX, 290000.0 - hi[2] * K)  # the graphs' 300000 bound
    cfg = {"sky": [float(lo[0] + hi[0]) / 2, float(lo[1] + hi[1]) / 2], "dd": [gx, gz], "base": float(base)}
    # tiles the interior touches (with the collision margin), then the ring
    x0, x1 = gx - (hi[0] - lo[0]) * K / 2 - 2 * COL_CELL, gx + (hi[0] - lo[0]) * K / 2 + 2 * COL_CELL
    z0, z1 = gz - (hi[1] - lo[1]) * K / 2 - 2 * COL_CELL, gz + (hi[1] - lo[1]) * K / 2 + 2 * COL_CELL
    ns = range(int(math.floor(x0 / TILE)) + 50 - ARENA_RING, int(math.floor(x1 / TILE)) + 51 + ARENA_RING)
    ms = range(int(math.floor(z0 / TILE)) + 50 - ARENA_RING, int(math.floor(z1 / TILE)) + 51 + ARENA_RING)
    tiles = sorted(((tm, tn) for tm in ms for tn in ns if os.path.exists(col_arc(tm, tn))),
                   key=lambda t: max(abs(t[0] - m), abs(t[1] - n)))
    return cfg, tiles


def gen_arena_collision(m, n, H):
    path = col_arc(m, n)
    if not os.path.exists(path):
        return False
    src = original(path)
    name, entry = next((e[0], e) for e in arc.entries(src) if e[1] == SBC_TYPE and "st100h_" in e[0])
    template = sbc.parse(arc.data(entry))
    ox, oz = (n - 50) * TILE, (m - 50) * TILE
    ls = np.arange(-COL_CELL, TILE + 2 * COL_CELL, COL_CELL)
    a0, a1 = ls[0], ls[-1]
    tris = []
    lo, hi = H.live.lo, H.live.hi
    if lo[0] <= ox + a1 and hi[0] >= ox + a0 and lo[2] <= oz + a1 and hi[2] >= oz + a0:  # the interior is here
        T, _ = H.live.triangles(ox + a0, oz + a0, ox + a1, oz + a1)
        Rp = H.live.ramps(ox + a0, oz + a0, ox + a1, oz + a1)
        c = Rp.mean(1) if len(Rp) else np.zeros((0, 3))
        Rp = Rp[(c[:, 0] >= ox + a0) & (c[:, 0] <= ox + a1) & (c[:, 2] >= oz + a0) & (c[:, 2] <= oz + a1)]
        T = T[(T[:, :, 0].max(1) >= ox + a0) & (T[:, :, 0].min(1) <= ox + a1)
              & (T[:, :, 2].max(1) >= oz + a0) & (T[:, :, 2].min(1) <= oz + a1)]
        tris = (np.concatenate([T, Rp]) - np.array([ox, 0.0, oz])).tolist()
    # the catch floor is one flat quad (a 52 x 52 grid took most of an arena's time)
    flat = [[H.catch, H.catch], [H.catch, H.catch]]
    data = sbcgen.build(flat, (float(a0), float(a0)), float(a1 - a0), template, triangles=tris)
    install(path, arc.rebuild(src, {name: data}))
    return True


def build_arena(cell, cfg, tiles):
    t0 = time.time()
    H = arena_height(cell, cfg)
    for (m, n) in tiles:
        gen_arena_collision(m, n, H)
        gen_way(m, n, H)
    return time.time() - t0


def gen_tile(m, n, H):
    t0 = time.time()
    c = gen_collision(m, n, H)
    w = gen_way(m, n, H)
    return c, w, time.time() - t0


def set_hold(on):
    """The bridge's "hold" line: while Skyrim is not linked, the party floats where it
    is instead of falling (a save made before the generated ground was there)."""
    try:
        lines = [l for l in open(EXPERIMENT).read().splitlines() if l.strip() and l.strip() != "hold"]
    except OSError:
        lines = ["off"]
    if on:
        lines.append("hold")
    open(EXPERIMENT, "w").write("\n".join(lines) + "\n")


# --- state and the loop -----------------------------------------------------------------
def load_state(cfg):
    try:
        st = json.load(open(STATE))
        if st.get("config") == cfg:
            return st
    except (OSError, ValueError):
        pass
    return {"config": cfg, "done": []}


def save_state(st):
    tmp = STATE + ".tmp"
    json.dump(st, open(tmp, "w"))
    os.replace(tmp, STATE)


_state_view = None


def party_tile():
    """(m, n) of the party from the bridge State, or None (DDDA not running yet)."""
    global _state_view
    import ctypes as C
    import struct
    if _state_view is None:
        k = C.WinDLL("kernel32", use_last_error=True)
        k.OpenFileMappingW.restype = C.c_void_p
        k.OpenFileMappingW.argtypes = [C.c_uint32, C.c_int, C.c_wchar_p]
        k.MapViewOfFile.restype = C.c_void_p
        k.MapViewOfFile.argtypes = [C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32, C.c_size_t]
        h = k.OpenFileMappingW(0x0004, False, r"Local\DDDA_SkyrimBridge_v2")
        if not h:
            return None
        _state_view = k.MapViewOfFile(h, 0x0004, 0, 0, 184)
        if not _state_view:
            _state_view = None
            return None
    raw = C.string_at(_state_view, 184)
    tile, = struct.unpack_from("<I", raw, 52)
    present = struct.unpack_from("<I", raw, 56)[0] & 1
    if not tile or not present:
        return None
    return tile >> 16, tile & 0xFFFF


# --- leaps at the edges of DDDA's map ---------------------------------------------------
# DDDA's open world ends (and has holes); Skyrim goes on. When a tile without collision
# is within LEAP_EDGE of the party, the mapping moves: the player's current Skyrim spot
# is mapped to the middle of a deep-inside tile far away (not loaded, so DDDA reads the
# freshly generated archives), whose ground is generated first. The vertical offset is
# raised if needed so the party goes UP (DDDA's fall damage, docs/party-in-terrain-mode.md).
# The plugin follows the ini live; the bridge sees the jump and brings the pawns.
LEAP_EDGE = 2         # tiles (DDDA loads about 2 around the party: leap before the void loads)
LEAP_FIRST = 2        # destination tiles generated before the leap (radius; ~0.3 s each)
LEAP_MIN_AWAY = 6     # destination at least this far from the party (never loaded)
LEAP_COOLDOWN = 20.0  # s
LEAP_UP = 300.0       # cm the destination ground is above the Arisen at least

_interior = None


def interior():
    """Chebyshev distance of every existing tile to the nearest tile without collision."""
    global _interior
    if _interior is None:
        have = {(m, n) for m in range(20, 85) for n in range(30, 70) if os.path.exists(col_arc(m, n))}
        _interior = {}
        for (m, n) in have:
            d = 1
            while d < 12 and all((m + a, n + b) in have for a in range(-d, d + 1) for b in range(-d, d + 1)):
                d += 1
            _interior[(m, n)] = d
    return _interior


def needs_leap(m, n):
    return interior().get((m, n), 0) <= LEAP_EDGE


def leap_target(m, n):
    """The deepest-inside tile at least LEAP_MIN_AWAY from (m, n)."""
    best = None
    for (tm, tn), d in interior().items():
        away = max(abs(tm - m), abs(tn - n))
        if away < LEAP_MIN_AWAY or not has_way(tm, tn):
            continue
        key = (d, -away)
        if best is None or key > best[0]:
            best = (key, (tm, tn))
    return best[1] if best else None


def party_global():
    """Arisen's DD global position from the bridge State, or None."""
    import ctypes as C
    import struct
    if party_tile() is None:
        return None
    raw = C.string_at(_state_view, 184)
    tile, = struct.unpack_from("<I", raw, 52)
    x, y, z = struct.unpack_from("<3f", raw, 56 + 8)
    return x + ((tile & 0xFFFF) - 50) * TILE, y, z + ((tile >> 16) - 50) * TILE


def leap(cfg, dest):
    """New mapping: the player's Skyrim spot -> the middle of tile dest, higher than now."""
    g = party_global()
    if g is None:
        return None
    sx0, sy0 = cfg["sky"]
    dx0, dz0 = cfg["dd"]
    sx = sx0 + (g[0] - dx0) / K  # the player's Skyrim position (the Arisen stands at its feet)
    sy = sy0 - (g[2] - dz0) / K
    m, n = dest
    gx, gz = (n - 50) * TILE + TILE / 2, (m - 50) * TILE + TILE / 2
    probe = {"sky": [sx, sy], "dd": [gx, gz], "base": 0.0}
    sky_h = float(make_height(probe)(gx, gz))  # Skyrim height there, in DD units (base 0)
    base = max(BASE_MIN, g[1] + LEAP_UP - sky_h)
    if base > BASE_MAX:
        print(f"leap: needed base {base:.0f} is over {BASE_MAX:.0f}; capped (the party may go down)", flush=True)
        base = BASE_MAX
    return {"sky": [sx, sy], "dd": [gx, gz], "base": base}


def tiles_of_cell(cfg, key):
    """DD tiles (m, n) that a Skyrim exterior cell overlaps under the mapping."""
    sx0, sy0 = cfg["sky"]
    dx0, dz0 = cfg["dd"]
    xs = [dx0 + (key[0] * havok.CELL_UNITS + a - sx0) * K for a in (0, havok.CELL_UNITS)]
    zs = [dz0 - (key[1] * havok.CELL_UNITS + a - sy0) * K for a in (0, havok.CELL_UNITS)]
    pad = 2 * COL_CELL
    ns = range(int((min(xs) - pad) // TILE) + 50, int((max(xs) + pad) // TILE) + 51)
    ms = range(int((min(zs) - pad) // TILE) + 50, int((max(zs) + pad) // TILE) + 51)
    return [(m, n) for m in ms for n in ns]


def wanted(m, n):
    out = [(m + dm, n + dn) for dm in range(-RADIUS, RADIUS + 1) for dn in range(-RADIUS, RADIUS + 1)]
    return sorted(out, key=lambda t: max(abs(t[0] - m), abs(t[1] - n)))


_instance = None


def single_instance():
    """False if another streamer already runs (two of them once wrote the same tiles)."""
    global _instance
    import ctypes as C
    k = C.WinDLL("kernel32", use_last_error=True)
    k.CreateMutexW.restype = C.c_void_p
    _instance = k.CreateMutexW(None, False, r"Local\DDDA_Streamer")
    return C.get_last_error() != 183  # ERROR_ALREADY_EXISTS


class Interiors(threading.Thread):
    """Builds an interior's arena as soon as the player enters it (DDDAGhosts' current.txt)
    and writes its mapping; removes the mapping on the way out. The main loop reads the
    arenas built and left (take_built, take_left) to keep its `done` set true, and tells
    which tiles it regenerates (overwritten), so a kept arena is reused only while intact."""

    def __init__(self, start):
        super().__init__(daemon=True)
        self.start_tile = start
        self.world = frozenset()  # tiles of the world's ground (set by the main loop)
        self.lock = threading.Lock()
        self.built, self.left = [], []
        self.last = (None, None, frozenset())  # the last arena (cell, cfg, tiles) while intact

    def take_built(self):
        with self.lock:
            out, self.built = self.built, []
        return out

    def take_left(self):
        with self.lock:
            out, self.left = self.left, []
        return out

    def overwritten(self, tile):
        with self.lock:
            if tile in self.last[2]:
                self.last = (None, None, frozenset())

    def run(self):
        inside = None  # (cell, cfg or None if it failed, tiles)
        waiting = None
        while True:
            time.sleep(0.1)
            cell = havok.current_cell()
            if not cell:
                if inside is not None:
                    remove_interior_ini()
                    with self.lock:
                        self.left.append(frozenset(inside[2]))
                    print(f"{time.strftime('%H:%M:%S')} left interior {inside[0]:08X}", flush=True)
                    inside = waiting = None
                continue
            if inside is not None and inside[0] == cell:
                continue
            try:
                with self.lock:
                    last = self.last
                if cell == last[0]:  # its arena is still on disk: only the mapping
                    write_interior_ini(cell, last[1])
                    inside = (cell, last[1], last[2])
                    print(f"{time.strftime('%H:%M:%S')} interior {cell:08X}: arena reused; mapping written", flush=True)
                    continue
                here, g = party_tile() or self.start_tile, party_global()
                if not os.path.exists(havok.interior_path(cell)) or g is None or here is None:
                    if waiting != cell:
                        print(f"{time.strftime('%H:%M:%S')} interior {cell:08X}: waiting for its collision export",
                              flush=True)
                        waiting = cell
                    continue
                plan = plan_arena(cell, here, g[1], self.world)
                if plan is None:
                    raise RuntimeError("no tile deep enough inside DDDA's map for this interior")
                acfg, tiles = plan
                with self.lock:
                    self.built.append(frozenset(tiles))
                dt = build_arena(cell, acfg, tiles)
                write_interior_ini(cell, acfg)
                inside = (cell, acfg, tiles)
                with self.lock:
                    self.last = (cell, acfg, frozenset(tiles))
                print(f"{time.strftime('%H:%M:%S')} interior {cell:08X}: arena around {tiles[0][0]}m{tiles[0][1]}n, "
                      f"{len(tiles)} tiles in {dt:.1f}s, base {acfg['base']:.0f}; mapping written", flush=True)
            except Exception as e:  # noqa: BLE001 (the streamer must keep running)
                import traceback
                traceback.print_exc()
                print(f"{time.strftime('%H:%M:%S')} interior {cell:08X}: arena FAILED ({e}); the party waits outside",
                      flush=True)
                inside = (cell, None, [])


def run(start=None):
    if not single_instance():
        print("another streamer is already running", flush=True)
        return
    if sys.stdout is None or not sys.stdout.isatty():  # started by the DDDA bridge (no console)
        sys.stdout = sys.stderr = open(os.path.join(HERE, "stream.log"), "a", buffering=1, encoding="utf-8")
        print("", flush=True)
        print(f"=== {time.strftime('%Y-%m-%d %H:%M:%S')} streamer started", flush=True)
    cfg = load_config()
    H = make_height(cfg)
    st = load_state(cfg)
    done = {tuple(t) for t in st["done"]}
    if H.live is not None:  # tiles of exported cells may predate their export (or this code)
        for key in H.live._files:
            for t in tiles_of_cell(cfg, key):
                done.discard(t)
    print(write_plugin_ini(cfg), flush=True)
    remove_interior_ini()  # an arena from an earlier session may have been overwritten since
    set_hold(True)
    last = None
    last_leap = 0.0
    last_live = 0.0
    leap_dest = None
    left_arena = None  # (arena tiles, time) after leaving an interior
    rooms = Interiors(start)
    rooms.world = frozenset(done)
    rooms.start()
    while True:
        here = party_tile() or start
        if not here:
            time.sleep(0.5)
            continue
        # Interiors (their own thread builds the arena, so a world tile being generated does
        # not delay it): while the player is inside, the world's streaming stops (the
        # party's tile is the arena's).
        for tiles in rooms.take_built():  # the world's ground there is gone
            done.difference_update(tiles)
            st["done"] = sorted(done)
            save_state(st)
        for tiles in rooms.take_left():  # it must come back if the party ever goes there
            done.difference_update(tiles)
            st["done"] = sorted(done)
            save_state(st)
            left_arena = (set(tiles), time.time())
        world = set(done)
        if H.live is not None:  # everywhere the player has been, even if not generated now
            for key in H.live._files:
                world.update(tiles_of_cell(cfg, key))
        rooms.world = frozenset(world)
        if havok.current_cell():
            time.sleep(0.25)
            continue
        if left_arena and here in left_arena[0] and time.time() - left_arena[1] < 15.0:
            time.sleep(0.25)  # the bridge still reports the arena's tile: wait for the way out
            continue
        if here != last:
            print(f"{time.strftime('%H:%M:%S')} party in {here[0]}m{here[1]}n", flush=True)
            last = here
        if needs_leap(*here) and party_tile() and time.time() - last_leap > LEAP_COOLDOWN:
            dest = leap_target(*here)
            new = leap(cfg, dest) if dest else None
            if new:
                t0 = time.time()
                Hn = make_height(new)
                first = [t for t in wanted(*dest)
                         if max(abs(t[0] - dest[0]), abs(t[1] - dest[1])) <= LEAP_FIRST]
                for (m, n) in first:  # the destination's ground first (not loaded); the loop does the rest
                    gen_tile(m, n, Hn)
                cfg, H = new, Hn
                json.dump(cfg, open(CONFIG, "w"), indent=1)
                st = {"config": cfg, "done": [list(t) for t in first]}
                done = {tuple(t) for t in st["done"]}
                save_state(st)
                write_plugin_ini(cfg)  # the plugin picks it up within 250 ms: the leap happens
                last_leap = time.time()
                leap_dest = dest
                print(f"{time.strftime('%H:%M:%S')} LEAP {here[0]}m{here[1]}n -> {dest[0]}m{dest[1]}n "
                      f"(edge within {LEAP_EDGE}; ground generated in {time.time() - t0:.1f}s; "
                      f"base {cfg['base']:.0f})", flush=True)
                continue
        if leap_dest and max(abs(here[0] - leap_dest[0]), abs(here[1] - leap_dest[1])) <= 2:
            leap_dest = None  # arrived
        if leap_dest and time.time() - last_leap < LEAP_COOLDOWN:
            here = leap_dest  # the bridge still reports the old tile: keep filling the destination
        if H.live is not None and time.time() - last_live > 2.0:
            last_live = time.time()
            for key in H.live.refresh():  # a Skyrim cell was exported: its DD tiles again
                for t in tiles_of_cell(cfg, key):
                    done.discard(t)
        todo = [t for t in wanted(*here) if t not in done]
        if not todo:
            time.sleep(0.5)
            continue
        m, n = todo[0]
        rooms.overwritten((m, n))
        t0 = time.time()
        c = gen_collision(m, n, H)
        if havok.current_cell():  # the player went inside meanwhile: the interior first, this tile later
            print(f"{time.strftime('%H:%M:%S')} {m}m{n}n graph postponed (interior)", flush=True)
            continue
        w = gen_way(m, n, H)
        dt = time.time() - t0
        done.add((m, n))
        st["done"] = sorted(done)
        save_state(st)
        print(f"{time.strftime('%H:%M:%S')} {m}m{n}n collision {'ok' if c else '--'} graph {'ok' if w else '--'} "
              f"{dt:.1f}s (distance {max(abs(m - here[0]), abs(n - here[1]))}, {len(todo) - 1} left)",
              flush=True)


def restore():
    n = 0
    for f in os.listdir(BACKUP):
        if not f.startswith("st100_"):
            continue
        is_way = f.endswith("_way.arc")
        mm, nn = f[6:].split("m")[0], f[6:].split("m")[1].split("n")[0]
        dst = way_arc(int(mm), int(nn)) if is_way else col_arc(int(mm), int(nn))
        shutil.copy2(os.path.join(BACKUP, f), dst)
        n += 1
    if os.path.exists(STATE):
        os.remove(STATE)
    set_hold(False)
    print(f"restored {n} archives")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "config":
        cfg = {"sky": [float(sys.argv[2]), float(sys.argv[3])], "dd": [float(sys.argv[4]), float(sys.argv[5])]}
        json.dump(cfg, open(CONFIG, "w"), indent=1)
        print(cfg)
    elif cmd == "tile":
        c, w, dt = gen_tile(int(sys.argv[2]), int(sys.argv[3]), make_height(load_config()))
        print(f"collision {c} graph {w} {dt:.1f}s")
    elif cmd == "run":
        run((int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else None)
    elif cmd == "restore":
        restore()
