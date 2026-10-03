"""For each character's status block (small status .. main status + 0x1000), list
heap objects with an image vtable that point anywhere into the block, with the
holding offset. Comparing the Arisen's list against the pawns' reveals the pawn
character objects (same owner vtable family, same field offset).

Usage: py blockrefs.py
"""
import struct
from collections import defaultdict
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

def owner(addr, back=0x2000):
    start = max(addr - back, 0)
    d = read(h, start, addr - start + 4)
    if not d:
        return None
    w = struct.unpack(f"<{len(d)//4}I", d[: len(d) // 4 * 4])
    for i in range(len(w) - 1, -1, -1):
        if 0x1390000 <= w[i] < ME and w[i] % 4 == 0:
            return start + i * 4, w[i]
    return None

statuses = []
for base, a in mem:
    statuses += [base + int(i) * 4 for i in np.nonzero(a == VT)[0]]
main = {}
for s in statuses:
    mx = struct.unpack("<f", read(h, s + 0xECC, 4))[0]
    if mx >= 100:
        main[s] = mx

names = {1427: "ARISEN", 2401: "Mariana(main)", 1468: "Diana", 1225: "Jack"}
arisen = u32(MB + 0x14D09E0)
for s, mx in main.items():
    lo, hi = s - 0x12E0, s + 0x1000
    print(f"== {names.get(int(mx), '?')} status {s:08X} block {lo:08X}-{hi:08X}")
    seen = defaultdict(list)
    for base, a in mem:
        if lo - 0x10000 <= base <= hi:  # skip refs from inside the block's own pages
            pass
        for i in np.nonzero((a >= lo) & (a < hi))[0]:
            p = base + int(i) * 4
            if lo <= p < hi:
                continue
            o = owner(p)
            if not o:
                continue
            seen[o[1]].append((o[0], p - o[0], int(a[i]) - lo))
    for vt in sorted(seen):
        if not (0x1550000 <= vt < 0x1570000):  # character-ish classes live around here
            continue
        for obj, field, tgt in seen[vt][:6]:
            tag = "  <-- arisen char" if obj == arisen else ""
            print(f"   owner vt {vt:08X} obj {obj:08X} +{field:X} -> block+{tgt:X}{tag}")
