"""Inspect character objects: vtable, fields pointing at the status object / HP
struct, and float triples near the Arisen's position.

Usage: py charlayout.py CHAR_HEX=label ...
"""
import math, struct, sys
import numpy as np
from memscan import open_proc, regions, read

MB = 0x400000
VT_STATUS = 0x159F198
h = open_proc()

def u32(a):
    d = read(h, a, 4) if a else None
    return struct.unpack("<I", d)[0] if d else 0

ap = struct.unpack("<3f", read(h, u32(MB + 0x14D1578) + 0xDF0, 12))
print("arisen pos (known chain)", tuple(round(x, 1) for x in ap))

statuses = []
for base, size in regions(h):
    d = read(h, base, size)
    if d:
        a = np.frombuffer(d[: len(d) // 4 * 4], dtype="<u4")
        statuses += [base + int(i) * 4 for i in np.nonzero(a == VT_STATUS)[0]]
smax = {s: struct.unpack("<f", read(h, s + 0xECC, 4))[0] for s in statuses}

SIZE = 0x8000
for arg in sys.argv[1:]:
    c, _, label = arg.partition("=")
    c = int(c, 16)
    d = read(h, c, SIZE) or b""
    w = struct.unpack(f"<{len(d)//4}I", d[: len(d) // 4 * 4])
    f = struct.unpack(f"<{len(d)//4}f", d[: len(d) // 4 * 4])
    print(f"== {label} char {c:08X} vt {w[0]:08X}")
    for i, v in enumerate(w):
        for s, mx in smax.items():
            if mx >= 100 and s <= v < s + 0x1000:
                print(f"   +{i*4:X} -> status {s:08X}+{v - s:X} (max {mx:.0f})")
    for i in range(len(f) - 2):
        x, y, z = f[i], f[i + 1], f[i + 2]
        if all(math.isfinite(t) for t in (x, y, z)) and abs(x) > 1 and abs(z) > 1:
            if math.dist((x, y, z), ap) < 1500:
                print(f"   +{i*4:X} pos? ({x:.1f}, {y:.1f}, {z:.1f})")
