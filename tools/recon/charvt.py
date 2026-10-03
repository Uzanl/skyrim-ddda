"""List objects whose vtable matches a character class and show their pos/status.

Usage: py charvt.py [VTABLE_HEX ...]   (default: the Arisen's char vtable)
"""
import struct, sys
import numpy as np
from memscan import open_proc, regions, read

MB = 0x400000
h = open_proc()

def u32(a):
    d = read(h, a, 4) if a else None
    return struct.unpack("<I", d)[0] if d else 0

arisen = u32(MB + 0x14D09E0)
vts = [int(x, 16) for x in sys.argv[1:]] or [u32(arisen)]
print("arisen", hex(arisen), "vtables", [hex(v) for v in vts])

for base, size in regions(h):
    d = read(h, base, size)
    if not d:
        continue
    a = np.frombuffer(d[: len(d) // 4 * 4], dtype="<u4")
    for vt in vts:
        for i in np.nonzero(a == vt)[0]:
            c = base + int(i) * 4
            pd = read(h, c + 0xEC4, 12)
            pos = struct.unpack("<3f", pd) if pd else (0, 0, 0)
            s = u32(u32(c + 0x8FC) + 0x8)
            hd = read(h, s + 0xEC8, 8) if s else None
            hp = "%.1f/%.0f" % struct.unpack("<2f", hd) if hd else "-"
            tag = "  <-- ARISEN" if c == arisen else ""
            print(f"  vt {vt:08X} obj {c:08X} pos=({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f}) status={s:08X} hp={hp}{tag}")
