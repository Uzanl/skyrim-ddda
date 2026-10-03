"""Find where the Arisen's char object is referenced, and list neighbouring pointers
that lead to objects with a plausible position at +EC4 (candidate pawn chars).

Usage: py charrefs.py
"""
import struct
import numpy as np
from memscan import open_proc, regions, read

MB, ME = 0x400000, 0x400000 + 0x160C000
h = open_proc()

def u32(a):
    d = read(h, a, 4) if a else None
    return struct.unpack("<I", d)[0] if d else 0

def pos(c):
    d = read(h, c + 0xEC4, 12)
    return struct.unpack("<3f", d) if d else None

arisen = u32(MB + 0x14D09E0)
ap = pos(arisen)
print(f"arisen char {arisen:08X} vt {u32(arisen):08X} pos {tuple(round(x,1) for x in ap)}")

refs = []
for base, size in regions(h):
    d = read(h, base, size)
    if d:
        a = np.frombuffer(d[: len(d) // 4 * 4], dtype="<u4")
        refs += [base + int(i) * 4 for i in np.nonzero(a == arisen)[0]]
for r in refs:
    static = f" (DDDA.exe+{r - MB:X})" if MB <= r < ME else ""
    print(f"ref at {r:08X}{static}")
    for k in range(-8, 9):
        if k == 0:
            continue
        c = u32(r + 4 * k)
        if not (0x10000 <= c < 0x80000000):
            continue
        vt = u32(c)
        if not (MB <= vt < ME):
            continue
        p = pos(c)
        if p and all(abs(x) < 1e6 for x in p) and sum((p[i] - ap[i]) ** 2 for i in range(3)) < 3000 ** 2:
            print(f"    [{k:+d}] {c:08X} vt {vt:08X} pos@EC4=({p[0]:.1f}, {p[1]:.1f}, {p[2]:.1f})")
