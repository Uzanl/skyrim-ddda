"""Relate DDDA's G-buffer value to real view depth. For SECONDS, once per new
frame: project each visible party actor (chest height) to the screen with the
camera Skyrim sends, read the G-buffer around that pixel as a 24-bit number
(R high, G, B low), and print it next to the actor's view-space depth.

Usage: py depthfit.py [SECONDS]
"""
import ctypes as C, math, struct, sys, time
import numpy as np

k = C.WinDLL("kernel32", use_last_error=True)
k.OpenFileMappingW.restype = C.c_void_p
k.MapViewOfFile.restype = C.c_void_p
k.MapViewOfFile.argtypes = [C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32, C.c_size_t]


def view(name, size):
    h = k.OpenFileMappingW(4, False, name)
    if not h:
        raise SystemExit(f"{name} not found")
    return k.MapViewOfFile(h, 4, 0, 0, size)


HDR, PLANE = 4096, 1920 * 1080 * 4
fv = view(r"Local\DDDA_SkyrimBridge_frame_v2", HDR + 6 * PLANE)
sv = view(r"Local\DDDA_SkyrimBridge_v2", 184)
cv = view(r"Local\DDDA_SkyrimBridge_cam_v2", 88)

sub = lambda a, b: [a[i] - b[i] for i in range(3)]
dot = lambda a, b: sum(a[i] * b[i] for i in range(3))
cross = lambda a, b: [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
norm = lambda a: [x / math.sqrt(dot(a, a)) for x in a]

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 60
end, last, rows = time.time() + seconds, None, []
while time.time() < end:
    latest = struct.unpack("<I", C.string_at(fv + 16, 4))[0]
    seq, w, hh, pitch, fid, qpc, flags = struct.unpack("<4I2QI", C.string_at(fv + 32 + 48 * latest, 36))
    if fid == last or not (flags & 1):
        time.sleep(0.05)
        continue
    last = fid
    st = C.string_at(sv, 184)
    cm = C.string_at(cv, 88)
    fov = struct.unpack_from("<f", cm, 28)[0]
    pos = list(struct.unpack_from("<3f", cm, 32))
    tgt = list(struct.unpack_from("<3f", cm, 44))
    up = list(struct.unpack_from("<3f", cm, 56))
    body = list(struct.unpack_from("<3f", cm, 72))
    actors = [struct.unpack_from("<2I3f2fI", st, 56 + 32 * r) for r in range(4)]
    dy = actors[0][3] - body[1]  # the bridge raises the camera to the Arisen's real feet
    pos[1] += dy
    tgt[1] += dy
    f = norm(sub(tgt, pos))
    r = norm(cross(f, up))
    u = cross(r, f)
    t = math.tan(math.radians(fov) / 2)
    m = np.frombuffer(C.string_at(fv + HDR + latest * 2 * PLANE + PLANE, pitch * hh), np.uint8)
    m = m.reshape(hh, pitch // 4, 4)[:, :w].astype(np.int64)
    d24 = (m[..., 2] << 16) | (m[..., 1] << 8) | m[..., 0]
    for role in (1, 2, 3):
        a = actors[role]
        if not (a[0] & 1):
            continue
        p = [a[2], a[3] + 120.0, a[4]]
        v = sub(p, pos)
        z = dot(v, f)
        if z < 50:
            continue
        for sign in (1, -1):  # handedness of "right" is not known yet
            sx = sign * dot(v, r) / z / (t * w / hh)
            sy = dot(v, u) / z / t
            px, py = int((sx * 0.5 + 0.5) * w), int((0.5 - sy * 0.5) * hh)
            if 2 <= px < w - 2 and 2 <= py < hh - 2:
                patch = d24[py - 2:py + 3, px - 2:px + 3]
                vals = patch[patch != 0xFFFFFF]
                if len(vals):
                    med = int(np.median(vals))
                    rows.append((z, med))
                    print(f"frame {fid} role {role} sign {sign:+d} px ({px},{py}) viewZ {z:7.1f}  "
                          f"d24 {med:#08x} = {med / 0xFFFFFF:.6f}  R={med >> 16}", flush=True)
    time.sleep(0.25)
if len(rows) >= 3:
    z = np.array([r[0] for r in rows]); d = np.array([r[1] / 0xFFFFFF for r in rows])
    for name, x in (("linear d=a+b*z", z), ("perspective d=a+b/z", 1 / z)):
        A = np.vstack([np.ones_like(x), x]).T
        coef, res, *_ = np.linalg.lstsq(A, d, rcond=None)
        err = np.abs(A @ coef - d).max()
        print(f"{name}: a={coef[0]:.6f} b={coef[1]:.6f} max err {err:.6f}")
