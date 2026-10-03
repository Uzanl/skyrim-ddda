"""DDDA frame rate and hitches, read from the frame transport counter (frame_shared.h).

The bridge publishes one frame per DDDA frame while Skyrim reads them, so the header's
frame count is DDDA's frame rate. Polls every 1 ms; prints one line per INTERVAL with
the frame rate and the longest gap between frames, and every gap over HITCH ms.
Also appends to ddfps.log next to this script.

Usage: py ddfps.py [SECONDS] [INTERVAL=5] [HITCH=100]
"""
import ctypes as C
import os
import struct
import sys
import time

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 1e9
interval = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
hitch = float(sys.argv[3]) if len(sys.argv) > 3 else 100.0

k = C.WinDLL("kernel32", use_last_error=True)
k.OpenFileMappingW.restype = C.c_void_p
k.MapViewOfFile.restype = C.c_void_p
k.MapViewOfFile.argtypes = [C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32, C.c_size_t]
k.OpenFileMappingW.argtypes = [C.c_uint32, C.c_int, C.c_wchar_p]
h = None
for _ in range(600):  # DDDA may still be starting: wait up to 10 min
    h = k.OpenFileMappingW(4, False, r"Local\DDDA_SkyrimBridge_frame_v3")
    if h:
        break
    time.sleep(1)
if not h:
    raise SystemExit("frame mapping not found (DDDA with the bridge not running)")
v = k.MapViewOfFile(h, 4, 0, 0, 4096)
k.timeBeginPeriod = C.WinDLL("winmm").timeBeginPeriod
k.timeBeginPeriod(1)

log = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ddfps.log"), "a", buffering=1)


def out(s):
    line = f"{time.strftime('%H:%M:%S')} {s}"
    print(line, flush=True)
    log.write(line + "\n")


def frames():
    return struct.unpack_from("<Q", C.string_at(v + 24, 8))[0]


out(f"=== ddfps start (interval {interval}s, hitch > {hitch:.0f} ms)")
start = time.perf_counter()
last_n, last_t = frames(), start
win_t, win_n, win_gap = start, last_n, 0.0
while time.perf_counter() - start < seconds:
    time.sleep(0.001)
    now = time.perf_counter()
    n = frames()
    if n != last_n:
        gap = (now - last_t) * 1000
        if gap > hitch and n - last_n == 1:
            out(f"  hitch {gap:.0f} ms")
        win_gap = max(win_gap, gap)
        last_n, last_t = n, now
    if now - win_t >= interval:
        fps = (last_n - win_n) / (now - win_t)
        idle = (now - last_t) * 1000
        out(f"fps {fps:5.1f}  max gap {win_gap:5.0f} ms" + (f"  (no frame for {idle:.0f} ms)" if idle > 1000 else ""))
        win_t, win_n, win_gap = now, last_n, 0.0
