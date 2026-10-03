"""Find character objects by walking back from status objects.

Pattern learned from the Arisen: char+8FC -> X, X+8 -> status (vtable 159F198),
char+EC4 = position (float xyz). For every status object, find X with X+8 == status,
then char with char+8FC == X, and print the char's position.

Usage: py findchars.py
"""
import struct
import numpy as np
from memscan import open_proc, regions, read

MB = 0x400000
VT = 0x159F198
h = open_proc()

mem = []
for base, size in regions(h):
    d = read(h, base, size)
    if d:
        mem.append((base, np.frombuffer(d[: len(d) // 4 * 4], dtype="<u4")))

def holders(value):
    out = []
    for base, a in mem:
        for i in np.nonzero(a == value)[0]:
            out.append(base + int(i) * 4)
    return out

def u32(a):
    d = read(h, a, 4)
    return struct.unpack("<I", d)[0] if d else 0

arisen = u32(MB + 0x14D09E0)
statuses = holders(VT)
print(f"arisen char = {arisen:08X}; {len(statuses)} status objects")
for s in statuses:
    d = read(h, s + 0xEC8, 8)
    cur, mx = struct.unpack("<2f", d)
    print(f"status {s:08X} hp={cur:.1f}/{mx:.0f}")
    for p in holders(s):
        x = p - 0x8
        for c_ptr in holders(x):
            c = c_ptr - 0x8FC
            pd = read(h, c + 0xEC4, 12)
            pos = struct.unpack("<3f", pd) if pd else None
            vt = u32(c)
            tag = "  <-- ARISEN" if c == arisen else ""
            print(f"    char {c:08X} vt={vt:08X} pos={tuple(round(v, 1) for v in pos) if pos else '-'}{tag}")
