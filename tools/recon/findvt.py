"""Find live objects by vtable and show HP-like floats at +EC8/+ECC.

Usage: py findvt.py [VTABLE_HEX]   (default 159F198, the HP object's vtable)
"""
import struct, sys
import numpy as np
from memscan import open_proc, regions, read

vt = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x159F198
h = open_proc()
hits = []
for base, size in regions(h):
    data = read(h, base, size)
    if not data:
        continue
    a = np.frombuffer(data[: len(data) // 4 * 4], dtype="<u4")
    for i in np.nonzero(a == vt)[0]:
        hits.append(base + int(i) * 4)

print(f"{len(hits)} objects with vtable {vt:08X}")
for o in hits:
    d = read(h, o + 0xEC8, 8)
    cur, mx = struct.unpack("<2f", d) if d else (float("nan"),) * 2
    print(f"  {o:08X}  hp={cur:.2f}/{mx:.2f}")
