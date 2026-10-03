"""Find back-pointers from status objects to their character objects.

For the Arisen, check which offsets inside its status object (and the companion
object at status-0x5F0, vt 015EA7E0) point at the Arisen char or one hop away.
Then apply the same offsets to the pawn statuses and print what they lead to.

Usage: py backref.py
"""
import struct
import numpy as np
from memscan import open_proc, regions, read

MB, ME = 0x400000, 0x400000 + 0x160C000
VT = 0x159F198
h = open_proc()

def u32(a):
    d = read(h, a, 4) if a else None
    return struct.unpack("<I", d)[0] if d else 0

def words(a, n):
    d = read(h, a, n) or b""
    return struct.unpack(f"<{len(d)//4}I", d[: len(d) // 4 * 4])

arisen = u32(MB + 0x14D09E0)
statuses = []
for base, size in regions(h):
    d = read(h, base, size)
    if d:
        a = np.frombuffer(d[: len(d) // 4 * 4], dtype="<u4")
        statuses += [base + int(i) * 4 for i in np.nonzero(a == VT)[0]]
hpof = {s: struct.unpack("<2f", read(h, s + 0xEC8, 8)) for s in statuses}
ari_s = next(s for s in statuses if s == u32(u32(arisen + 0x8FC) + 0x8))
print(f"arisen char {arisen:08X}, status {ari_s:08X}")

# Search a window around the status object (both directions) for direct/1-hop refs.
paths = []
for start_off in (-0x1000,):
    w = words(ari_s + start_off, 0x2000)
    for i, v in enumerate(w):
        off = start_off + i * 4
        if v == arisen:
            paths.append((off, None))
        elif 0x10000 <= v < 0x80000000:
            for j, v2 in enumerate(words(v, 0x400)):
                if v2 == arisen:
                    paths.append((off, j * 4))
print("paths from status to arisen char:", [(hex(a), b if b is None else hex(b)) for a, b in paths])

def pos(c):
    d = read(h, c + 0xEC4, 12)
    return tuple(round(x, 1) for x in struct.unpack("<3f", d)) if d else None

for s in statuses:
    cur, mx = hpof[s]
    if mx < 100 or s == ari_s:
        continue
    print(f"pawn? status {s:08X} hp={cur:.0f}/{mx:.0f}")
    for a, b in paths:
        c = u32(s + a) if b is None else u32(u32(s + a) + b)
        print(f"   via {hex(a)}{'' if b is None else '->' + hex(b)}: obj {c:08X} vt {u32(c):08X} pos@EC4={pos(c)}")
