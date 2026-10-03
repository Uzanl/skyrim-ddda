"""Compare the old position chain against player+0x40, to tell camera from body.

Usage: py campos.py SECONDS PLAYER_HEX
"""
import struct, sys, time
from memscan import open_proc, read

h = open_proc()
player = int(sys.argv[2], 16)
u = lambda a: struct.unpack("<I", read(h, a, 4))[0]
t0, end = time.time(), time.time() + float(sys.argv[1])
while time.time() < end:
    a = struct.unpack("<3f", read(h, u(0x400000 + 0x14D1578) + 0xDF0, 12))
    b = struct.unpack("<3f", read(h, player + 0x40, 12))
    d = sum((a[i] - b[i]) ** 2 for i in range(3)) ** 0.5
    print(f"t={time.time()-t0:5.1f} chain=({a[0]:.0f},{a[1]:.0f},{a[2]:.0f}) body+40=({b[0]:.0f},{b[1]:.0f},{b[2]:.0f}) dist={d:.0f}", flush=True)
    time.sleep(0.5)
