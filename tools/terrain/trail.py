"""Party trails over the generated ground: where pawns get stuck on stairs, decks, bridges.

record: reads the bridge State (Local\\DDDA_SkyrimBridge_v2, bridge_shared.h) 10 times a
        second and appends DD global positions to a CSV (t, role, x, y, z), until Ctrl+C
        or SECONDS.
view:   draws the floor reached on foot (havok.Live.reach, colour = height), the waypoint
        graphs (white: nodes with links, black: cut off) and the trails (A white, M red,
        H1 green, H2 blue; a ring every 5 s) around a point, default the trails' middle.

Usage: py trail.py record OUT.csv [SECONDS]
       py trail.py view TRAIL.csv [CX CZ RADIUS] [OUT.png]
"""
import csv
import ctypes as C
import json
import os
import struct
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "recon"))
ROLES = ["A", "M", "H1", "H2"]
COLORS = {"A": (255, 255, 255), "M": (60, 60, 255), "H1": (60, 255, 60), "H2": (255, 120, 40)}  # BGR


def record(out, seconds):
    k = C.WinDLL("kernel32", use_last_error=True)
    k.OpenFileMappingW.restype = C.c_void_p
    k.OpenFileMappingW.argtypes = [C.c_uint32, C.c_int, C.c_wchar_p]
    k.MapViewOfFile.restype = C.c_void_p
    k.MapViewOfFile.argtypes = [C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32, C.c_size_t]
    h = k.OpenFileMappingW(4, False, r"Local\DDDA_SkyrimBridge_v2")
    if not h:
        raise SystemExit("bridge State not found (DDDA with the bridge not running)")
    v = k.MapViewOfFile(h, 4, 0, 0, 184)
    f = open(out, "a", newline="")
    w = csv.writer(f)
    t0 = time.time()
    n = 0
    print(f"recording to {out}, Ctrl+C to stop", flush=True)
    try:
        while time.time() - t0 < seconds:
            for _ in range(5):  # seqlock
                seq = struct.unpack_from("<I", C.string_at(v + 8, 4))[0]
                raw = C.string_at(v, 184)
                if not seq & 1 and struct.unpack_from("<I", raw, 8)[0] == seq:
                    break
            tile = struct.unpack_from("<I", raw, 52)[0]
            if tile:
                ox, oz = ((tile & 0xFFFF) - 50) * 10000.0, ((tile >> 16) - 50) * 10000.0
                now = time.time()
                for i, role in enumerate(ROLES):
                    flags, _, x, y, z = struct.unpack_from("<2I3f", raw, 56 + 32 * i)
                    if flags & 1:
                        w.writerow([f"{now:.2f}", role, f"{x + ox:.0f}", f"{y:.0f}", f"{z + oz:.0f}"])
                n += 1
                if n % 50 == 0:
                    f.flush()
                    print(f"  {n / 10:.0f} s, tile {tile >> 16}m{tile & 0xFFFF}n", flush=True)
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    f.close()


def view(path, cx=None, cz=None, R=None, out=None):
    import cv2
    import arc
    import havok
    import overlay
    import stream
    import way
    rows = [(float(t), r, float(x), float(y), float(z)) for t, r, x, y, z in csv.reader(open(path))]
    P = np.array([(x, z) for _, _, x, _, z in rows])
    if cx is None:
        cx, cz = (P.min(0) + P.max(0)) / 2
        R = max(600.0, float((P.max(0) - P.min(0)).max()) / 2 + 300)
    cfg = json.load(open(stream.CONFIG))
    H = stream.make_height(cfg)
    L = H.live
    r = L.raster(cx - R, cz - R, cx + R, cz + R)
    Rch = L.reach(r, H).reshape(r["nz"], r["nx"])
    S = 8  # px per raster cell

    def px(x, z):
        return (int((x - r["x0"]) / havok.RASTER * S),
                int(r["nz"] * S - 1 - (z - r["z0"]) / havok.RASTER * S))

    lo = np.nanpercentile(Rch, 2)
    hi = np.nanpercentile(Rch, 98)
    t = np.clip((np.nan_to_num(Rch, nan=lo) - lo) / max(hi - lo, 1.0), 0, 1)
    img = (cv2.applyColorMap((t * 255).astype(np.uint8), cv2.COLORMAP_JET) * 0.6).astype(np.uint8)
    img[~np.isfinite(Rch)] = 0
    img = cv2.resize(img[::-1], (r["nx"] * S, r["nz"] * S), interpolation=cv2.INTER_NEAREST)
    m0, n0 = int(np.floor((cz + 5000) / 10000)) + 50, int(np.floor((cx + 5000) / 10000)) + 50
    for mm in (m0 - 1, m0, m0 + 1):
        for nn in (n0 - 1, n0, n0 + 1):
            p = overlay.current(stream.way_arc(mm, nn))
            if not os.path.exists(p):
                continue
            e = next((e for e in arc.entries(p) if e[1] == stream.WAY_TYPE), None)
            if not e:
                continue
            nodes = way.parse(arc.data(e))["nodes"]
            for nd in nodes:
                x, y, z = nd["pos"]
                if abs(x - cx) > R or abs(z - cz) > R:
                    continue
                for li in nd["links"]:
                    q = nodes[li["target"]]["pos"]
                    cv2.line(img, px(x, z), px(q[0], q[2]), (200, 200, 200), 1)
            for nd in nodes:
                x, y, z = nd["pos"]
                if abs(x - cx) <= R and abs(z - cz) <= R:
                    cv2.circle(img, px(x, z), 3, (255, 255, 255) if nd["links"] else (0, 0, 0), -1)
    t0 = rows[0][0] if rows else 0
    for role in ROLES:
        pts = [(t, x, z) for t, rr, x, _, z in rows if rr == role]
        for (ta, xa, za), (tb, xb, zb) in zip(pts, pts[1:]):
            if tb - ta < 1.0:
                cv2.line(img, px(xa, za), px(xb, zb), COLORS[role], 2)
        last = -1e9
        for t, x, z in pts:
            if t - last >= 5.0:
                cv2.circle(img, px(x, z), 6, COLORS[role], 1)
                cv2.putText(img, f"{t - t0:.0f}", (px(x, z)[0] + 7, px(x, z)[1]), 0, 0.35, COLORS[role], 1)
                last = t
    cv2.putText(img, f"floor {lo:.0f}..{hi:.0f} (blue..red)  A white  M red  H1 green  H2 blue",
                (8, 18), 0, 0.5, (255, 255, 255), 1)
    out = out or os.path.splitext(path)[0] + ".png"
    cv2.imwrite(out, img)
    print("wrote", out, f"| centre {cx:.0f} {cz:.0f} radius {R:.0f}")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "record":
        record(sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 1e9)
    elif cmd == "view":
        a = sys.argv[3:]
        if len(a) >= 3:
            view(sys.argv[2], float(a[0]), float(a[1]), float(a[2]), a[3] if len(a) > 3 else None)
        else:
            view(sys.argv[2], out=a[0] if a else None)
    else:
        print(__doc__)
