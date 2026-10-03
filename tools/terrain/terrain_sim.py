"""Player simulator for terrain mode: plays Skyrim's side of the bridge without Skyrim.

Drives a fake player through the generated Riverwood area in DDDA GLOBAL coordinates
(CameraCmd with kCamOverride | kMoveArisen | kGlobalCoords), the camera 4 m behind and
2 m above it. The path starts exactly at the Arisen (no teleport), visits the points
below and comes back. Every 0.25 s it logs each pawn's distance to the Arisen, the
Arisen's height against the generated ground, and the tile the bridge reports.
At the end (or Ctrl+C) the override is released.

Usage: py terrain_sim.py [SPEED_M_S] [LOG] [stream [loop|east]]
In stream mode it behaves like the Skyrim plugin: the route is a path in SKYRIM
coordinates, mapped to DDDA with the streamer's current mapping, re-read when
stream_config.json changes (a leap moves the mapping). "east" walks 450 m east of the
start, past the edge of DDDA's map, to test leaps.
Run with DDDA in the world (Riverwood archives and the terrain-mode bridge installed)
and Skyrim closed.
"""
import ctypes as C
import math
import os
import struct
import sys
import time

import skyterrain

k = C.WinDLL("kernel32", use_last_error=True)
k.CreateFileMappingW.restype = C.c_void_p
k.CreateFileMappingW.argtypes = [C.c_void_p, C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32, C.c_wchar_p]
k.OpenFileMappingW.restype = C.c_void_p
k.OpenFileMappingW.argtypes = [C.c_uint32, C.c_int, C.c_wchar_p]
k.MapViewOfFile.restype = C.c_void_p
k.MapViewOfFile.argtypes = [C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32, C.c_size_t]

ANCHOR = (22165.2, 32319.1, 132978.9)  # the Riverwood archives were generated for this save
# With tools/terrain/stream.py configured, its mapping and a long loop (relative to the
# start: 300 m north, 200 m west, back) that leaves the old Riverwood block.
STREAM = len(sys.argv) > 3 and sys.argv[3] == "stream"
ROUTE = sys.argv[4] if len(sys.argv) > 4 else "loop"
if STREAM:
    import stream
    H = stream.make_height(stream.load_config())
    WAYPOINTS = None  # built from the start position in main()
else:
    H = skyterrain.make_height(*ANCHOR)
    # Route (DDDA global x, z): Riverwood centre, east along the valley, north, back.
    WAYPOINTS = [(20000, 130000), (26000, 131000), (27000, 126000), (21000, 125000), (20000, 130000)]
FLAGS = 1 | 2 | 4  # kCamOverride | kMoveArisen | kGlobalCoords


def mapping(name, size, create):
    if create:
        h = k.CreateFileMappingW(C.c_void_p(-1), None, 4, 0, size, name)
    else:
        h = k.OpenFileMappingW(0xF001F, False, name)
    if not h:
        raise SystemExit(f"{name}: not available (is DDDA running with the bridge?)")
    return k.MapViewOfFile(h, 0xF001F, 0, 0, size)


state = mapping(r"Local\DDDA_SkyrimBridge_v2", 184, False)
cam = mapping(r"Local\DDDA_SkyrimBridge_cam_v3", 152, True)


def read_state():
    raw = C.string_at(state, 184)
    tile, = struct.unpack_from("<I", raw, 52)
    actors = []
    for r in range(4):
        fl, _, x, y, z, _, _, _ = struct.unpack_from("<II3fffI", raw, 56 + 32 * r)
        actors.append((fl & 1, (x, y, z)))
    return tile, actors


def to_global(tile, p):
    n, m = tile & 0xFFFF, tile >> 16
    return (p[0] + (n - 50) * 10000, p[1], p[2] + (m - 50) * 10000)


seq = C.c_uint32.from_address(cam + 8)
updates = C.c_uint64.from_address(cam + 16)


def write_cmd(flags, pos, target, body):
    seq.value += 1
    struct.pack_into("<I", (C.c_char * 4).from_address(cam + 12), 0, flags)
    buf = (C.c_char * 152).from_address(cam)
    struct.pack_into("<I", buf, 0, 0x43424444)
    struct.pack_into("<I", buf, 4, 3)  # CameraCmd v3; skyPose stays zero (no Skyrim)
    struct.pack_into("<f3f3f3ff3ff", buf, 28, 0.0, *pos, *target, 0.0, 1.0, 0.0, 0.0, *body, 0.0)
    updates.value += 1
    seq.value += 1


def to_sky(cfg, gx, gz):
    return cfg["sky"][0] + (gx - cfg["dd"][0]) / stream.K, cfg["sky"][1] - (gz - cfg["dd"][1]) / stream.K


def to_dd(cfg, sx, sy):
    return cfg["dd"][0] + (sx - cfg["sky"][0]) * stream.K, cfg["dd"][1] - (sy - cfg["sky"][1]) * stream.K


def main():
    speed = float(sys.argv[1]) * 100 if len(sys.argv) > 1 else 300.0
    log = open(sys.argv[2], "w") if len(sys.argv) > 2 else sys.stdout
    tile, actors = read_state()
    if not tile or not actors[0][0]:
        raise SystemExit("no tile or no Arisen yet: is the terrain-mode bridge installed and the save loaded?")
    start = to_global(tile, actors[0][1])
    global H
    cfg = cfg_time = None
    if WAYPOINTS is None:
        x, z = start[0], start[2]
        if ROUTE == "east":  # past the edge of DDDA's map: a leap must happen
            route = [(x, z), (x + 45000, z)]
        else:  # north (DD -z) 300 m, west 200 m, back: towards Whiterun's plain, inside DDDA's map
            route = [(x, z), (x, z - 30000), (x - 20000, z - 30000), (x - 20000, z), (x, z)]
        # the route lives in Skyrim coordinates, like the player
        cfg = stream.load_config()
        cfg_time = os.path.getmtime(stream.CONFIG)
        route = [to_sky(cfg, gx, gz) for gx, gz in route]
    else:
        route = [(start[0], start[2])] + WAYPOINTS
    print(f"start at global ({start[0]:.0f}, {start[1]:.0f}, {start[2]:.0f}), tile {tile >> 16}m{tile & 0xFFFF}n; "
          f"ground there {float(H(start[0], start[2])):.0f}", file=log, flush=True)
    seg, along = 0, 0.0
    heading = (1.0, 0.0)
    last_log = 0.0
    t0 = time.time()
    prev = time.time()
    worst = 0.0
    try:
        while seg < len(route) - 1:
            now = time.time()
            dt, prev = now - prev, now
            (ax, az), (bx, bz) = route[seg], route[seg + 1]
            length = math.hypot(bx - ax, bz - az)
            along += (speed / stream.K if cfg else speed) * dt
            if along >= length:
                seg, along = seg + 1, along - length
                continue
            f = along / length
            gx, gz = ax + (bx - ax) * f, az + (bz - az) * f
            heading = ((bx - ax) / length, (bz - az) / length)
            if cfg:  # Skyrim point -> DDDA with the current mapping (a leap changes it)
                t_cfg = os.path.getmtime(stream.CONFIG)
                if t_cfg != cfg_time:
                    cfg_time = t_cfg
                    cfg = stream.load_config()
                    H = stream.make_height(cfg)
                    print(f"{now - t0:6.1f}s mapping changed: dd {cfg['dd']} base {cfg.get('base')}", file=log, flush=True)
                gx, gz = to_dd(cfg, gx, gz)
                heading = (heading[0], -heading[1])  # Skyrim y north = DD -z
            gy = float(H(gx, gz))
            body = (gx, gy, gz)
            pos = (gx - heading[0] * 400, gy + 200, gz - heading[1] * 400)
            target = (gx + heading[0] * 400, gy + 100, gz + heading[1] * 400)
            write_cmd(FLAGS, pos, target, body)
            if now - last_log >= 0.25:
                last_log = now
                tile, actors = read_state()
                if tile and actors[0][0]:
                    a = to_global(tile, actors[0][1])
                    ground = float(H(a[0], a[2]))
                    d = []
                    for present, p in actors[1:]:
                        if present:
                            g = to_global(tile, p)
                            d.append(math.hypot(g[0] - a[0], g[2] - a[2]) / 100)
                    worst = max([worst] + d)
                    print(f"{now - t0:6.1f}s tile {tile >> 16}m{tile & 0xFFFF}n target ({gx:.0f},{gz:.0f}) "
                          f"Arisen ({a[0]:.0f},{a[1]:.0f},{a[2]:.0f}) off-target {math.hypot(a[0] - gx, a[2] - gz) / 100:5.1f} m "
                          f"above ground {(a[1] - ground) / 100:+5.2f} m | pawns " +
                          " ".join(f"{x:5.1f}" for x in d) + " m", file=log, flush=True)
            time.sleep(0.004)
    finally:
        write_cmd(0, (0, 0, 0), (0, 0, 1), (0, 0, 0))
        print(f"done; worst pawn distance {worst:.1f} m; override released", file=log, flush=True)


if __name__ == "__main__":
    main()
