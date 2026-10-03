"""Show who points at each status object (vtable 159F198), and for each holder the
nearest preceding image vtable (likely the owning object's start).

Usage: py statusrefs.py
"""
import struct
import numpy as np
from memscan import open_proc, regions, read

MB, ME = 0x400000, 0x400000 + 0x160C000
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

def owner(addr, back=0x3000):
    """Walk back to the nearest dword that looks like a vtable in DDDA.exe's .rdata."""
    start = max(addr - back, 0)
    d = read(h, start, addr - start + 4)
    if not d:
        return None
    w = struct.unpack(f"<{len(d)//4}I", d[: len(d) // 4 * 4])
    for i in range(len(w) - 1, -1, -1):
        if 0x1390000 <= w[i] < ME and w[i] % 4 == 0:
            return start + i * 4, w[i]
    return None

def u32(a):
    d = read(h, a, 4)
    return struct.unpack("<I", d)[0] if d else 0

arisen = u32(MB + 0x14D09E0)
for s in holders(VT):
    cur, mx = struct.unpack("<2f", read(h, s + 0xEC8, 8))
    if mx < 100:
        continue
    print(f"status {s:08X} hp={cur:.1f}/{mx:.0f}")
    for p in holders(s):
        o = owner(p)
        desc = f"owner {o[0]:08X} vt {o[1]:08X} +{p - o[0]:X}" if o else "?"
        static = "  STATIC" if MB <= p < ME else ""
        print(f"    held at {p:08X}  {desc}{static}")
print(f"arisen char {arisen:08X} vt {u32(arisen):08X}; [char+8FC]={u32(arisen + 0x8FC):08X}")
