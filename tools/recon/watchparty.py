"""Locate the party's status objects right now (by vtable + max HP) and put write
watchpoints on each one's current-HP float.

Usage: py watchparty.py SECONDS MAXHP=label [MAXHP=label ...]   (max 4)
  e.g. py watchparty.py 120 1427=arisen 2401=mariana 1468=diana 1225=jack
"""
import struct, sys
import numpy as np
from memscan import open_proc, regions, read
import hwbp

VT = 0x159F198
seconds = sys.argv[1]
want = {}
for arg in sys.argv[2:6]:
    mx, _, label = arg.partition("=")
    want[float(mx)] = label

h = open_proc()
found = {}
for base, size in regions(h):
    d = read(h, base, size)
    if not d:
        continue
    a = np.frombuffer(d[: len(d) // 4 * 4], dtype="<u4")
    for i in np.nonzero(a == VT)[0]:
        s = base + int(i) * 4
        cur, mx = struct.unpack("<2f", read(h, s + 0xEC8, 8))
        if mx in want:
            found.setdefault(want[mx], []).append((s, cur, mx))

args = []
for label in want.values():
    hits = found.get(label, [])
    if len(hits) != 1:
        print(f"{label}: {len(hits)} matches, skipping {[hex(s) for s, *_ in hits]}")
        continue
    s, cur, mx = hits[0]
    print(f"{label}: status {s:08X} hp {cur:.1f}/{mx:.0f} -> watch {s + 0xEC8:08X}")
    args.append(f"{s + 0xEC8:08X}={label}")

if not args:
    sys.exit("nothing to watch")
sys.argv = ["hwbp.py", seconds] + args
hwbp.main()
