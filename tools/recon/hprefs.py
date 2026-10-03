"""Who points at status+0xEC0 (the HP struct) for each character? Any owner.

Usage: py hprefs.py
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

def u32(a):
    d = read(h, a, 4) if a else None
    return struct.unpack("<I", d)[0] if d else 0

def owner(addr, back=0x3000):
    start = max(addr - back, 0)
    d = read(h, start, addr - start + 4)
    if not d:
        return None
    w = struct.unpack(f"<{len(d)//4}I", d[: len(d) // 4 * 4])
    for i in range(len(w) - 1, -1, -1):
        if 0x1390000 <= w[i] < ME and w[i] % 4 == 0:
            return start + i * 4, w[i]
    return None

names = {1427: "ARISEN", 2401: "Mariana(main)", 1468: "Diana", 1225: "Jack"}
for base, a in mem:
    pass
statuses = []
for base, a in mem:
    statuses += [base + int(i) * 4 for i in np.nonzero(a == VT)[0]]
for s in statuses:
    mx = struct.unpack("<f", read(h, s + 0xECC, 4))[0]
    if mx < 100:
        continue
    print(f"== {names.get(int(mx), '?')} status {s:08X}")
    for tgt_off in (0xEC0, 0xEC4, 0xEC8):
        t = s + tgt_off
        for base, a in mem:
            for i in np.nonzero(a == t)[0]:
                p = base + int(i) * 4
                o = owner(p)
                static = f" STATIC DDDA.exe+{p - MB:X}" if MB <= p < ME else ""
                od = f"owner {o[0]:08X} vt {o[1]:08X} +{p - o[0]:X}" if o else "?"
                print(f"   ->status+{tgt_off:X} held at {p:08X} {od}{static}")
