"""Player simulator for testing the party follow without Skyrim.

Pretends to be Skyrim's side of the bridge: drives a fake player along a scripted
path in DDDA space (CameraCmd with kMoveArisen), publishes flat Skyrim ground for
the pawns (Ground mapping), echoes DDDA's treadmill shift, and switches following
on through ddda_experiment.txt. Every 0.25 s it measures each pawn's distance to
the fake player and flags gluing (a pawn on the Arisen) and falls. Following is
switched off again at the end (safe mode).

Usage: py follow_sim.py [PATH] [SPEED_M_S]
  PATH: line (default) | circle | zigzag | stop
Run with DDDA in the world and Skyrim closed.
"""
import ctypes as C, math, os, struct, sys, time

GAME = r"E:\SteamLibrary\steamapps\common\DDDA"
k = C.WinDLL("kernel32", use_last_error=True)
k.CreateFileMappingW.restype = C.c_void_p
k.CreateFileMappingW.argtypes = [C.c_void_p, C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32, C.c_wchar_p]
k.OpenFileMappingW.restype = C.c_void_p
k.MapViewOfFile.restype = C.c_void_p
k.MapViewOfFile.argtypes = [C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32, C.c_size_t]


def mapping(name, size, create):
    h = k.CreateFileMappingW(C.c_void_p(-1), None, 4, 0, size, name) if create else k.OpenFileMappingW(0xF001F, False, name)
    if not h:
        raise SystemExit(f"{name}: not available (is DDDA running with the bridge?)")
    return k.MapViewOfFile(h, 0xF001F, 0, 0, size)


state = mapping(r"Local\DDDA_SkyrimBridge_v2", 184, False)
cam = mapping(r"Local\DDDA_SkyrimBridge_cam_v2", 88, True)
ground = mapping(r"Local\DDDA_SkyrimBridge_ground_v1", 40, True)
shift = mapping(r"Local\DDDA_SkyrimBridge_shift_v1", 24, True)


def rd(addr, fmt, off=0):
    return struct.unpack_from(fmt, C.string_at(addr + off, struct.calcsize(fmt)))


def wr(addr, off, fmt, *vals):
    b = struct.pack(fmt, *vals)
    C.memmove(addr + off, b, len(b))


def bump(addr, off):  # seqlock counter += 1
    v = rd(addr, "<I", off)[0]
    wr(addr, off, "<I", (v + 1) & 0xFFFFFFFF)


def party():
    out = []
    for r in range(4):
        flags, ms, x, y, z, hp, hpmax, _ = rd(state, "<2I3f2fI", 56 + 32 * r)
        out.append(((x, y, z), bool(flags & 1), hp))
    return out


def read_shift():
    magic = rd(shift, "<I")[0]
    if magic != 0x53424444:
        return 0.0, 0.0
    return rd(shift, "<2f", 16)


wr(cam, 0, "<2I", 0x43424444, 2)
wr(cam, 20, "<I", os.getpid())
wr(ground, 0, "<2I", 0x47424444, 1)
updates = [0]
gupdates = [0]


def send_cam(flags, pos, target, body, sx, sz):
    bump(cam, 8)
    wr(cam, 12, "<I", flags)
    wr(cam, 24, "<f", 0.0)
    wr(cam, 28, "<3f", *pos)
    wr(cam, 40, "<3f", *target)
    wr(cam, 52, "<3f", 0.0, 1.0, 0.0)
    wr(cam, 64, "<f", sx)
    wr(cam, 68, "<3f", *body)
    wr(cam, 80, "<f", sz)
    updates[0] += 1
    wr(cam, 16, "<Q", updates[0])
    bump(cam, 8)


def send_ground(y):
    bump(ground, 8)
    wr(ground, 12, "<I", 0b1110)
    gupdates[0] += 1
    wr(ground, 16, "<Q", gupdates[0])
    wr(ground, 24, "<4f", y, y, y, y)
    bump(ground, 8)


def path(kind, t, v):
    """Player offset (x, z) in cm from the start, and facing angle, at time t."""
    if t < 3:
        return 0.0, 0.0, 0.0
    t -= 3
    if kind == "line":
        return v * t, 0.0, 0.0
    if kind == "circle":
        r = 1500.0
        a = v * t / r
        return r * math.sin(a), r - r * math.cos(a), a
    if kind == "zigzag":
        seg = 8.0
        n = int(t // seg)
        tt = t - n * seg
        x = v * t
        z = (v * 0.6) * (tt if n % 2 == 0 else seg - tt) - v * 0.6 * seg / 2
        return x, z, math.atan2(0.6 if n % 2 == 0 else -0.6, 1.0)
    return 0.0, 0.0, 0.0


kind = sys.argv[1] if len(sys.argv) > 1 else "line"
speed = float(sys.argv[2]) * 100.0 if len(sys.argv) > 2 else 500.0
duration = 3 + (60 if kind != "stop" else 20)

p0 = party()
if not p0[0][1]:
    raise SystemExit("the Arisen is not present")
anchor = p0[0][0]
print(f"anchor (Arisen) at ({anchor[0]:.0f}, {anchor[1]:.0f}, {anchor[2]:.0f}); path {kind} at {speed / 100:.1f} m/s")
exp = os.path.join(GAME, "ddda_experiment.txt")
open(exp, "w").write("follow on\n")
t0 = time.time()
last_report = -1
worst = 0.0
glue_samples = 0
samples = 0
try:
    while True:
        t = time.time() - t0
        if t > duration:
            break
        ox, oz, ang = path(kind, t, speed)
        sx, sz = read_shift()
        body = (anchor[0] + sx + ox, anchor[1], anchor[2] + sz + oz)
        fx, fz = math.cos(ang), math.sin(ang)
        pos = (body[0] - fx * 350, body[1] + 220, body[2] - fz * 350)
        target = (body[0] + fx * 100, body[1] + 150, body[2] + fz * 100)
        send_cam(0b11, pos, target, body, sx, sz)
        send_ground(anchor[1])
        pt = party()
        if t > 3:
            samples += 1
            ar = pt[0][0]
            dists = []
            glued = False
            for r in (1, 2, 3):
                pp = pt[r][0]
                d = math.hypot(pp[0] - body[0], pp[2] - body[2]) / 100
                dists.append(d)
                if math.hypot(pp[0] - ar[0], pp[2] - ar[2]) < 5:
                    glued = True
            glue_samples += glued
            worst = max(worst, max(dists))
            if int(t) // 2 != last_report:
                last_report = int(t) // 2
                print(f"t={t:5.1f}s pawn dist to player (m): {dists[0]:5.1f} {dists[1]:5.1f} {dists[2]:5.1f}"
                      f"  Arisen dy={ar[1] - anchor[1]:7.0f}  shift=({sx:.0f},{sz:.0f}){'  GLUED' if glued else ''}",
                      flush=True)
        time.sleep(1 / 60)
finally:
    send_cam(0, (0, 0, 0), (0, 0, 1), (0, 0, 0), 0, 0)
    open(exp, "w").write("off\n")
print(f"done: worst pawn distance {worst:.1f} m, glued in {glue_samples}/{samples} samples")
