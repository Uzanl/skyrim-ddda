"""Watch candidate HP chains (and position) and print a line whenever any changes.

Usage: py hpwatch.py [seconds]
"""
import struct, sys, time
from memscan import open_proc, read

MB = 0x400000
h = open_proc()

def u32(a):
    d = read(h, a, 4) if a else None
    return struct.unpack("<I", d)[0] if d else 0

def hp(obj):
    d = read(h, obj + 0xEC8, 8) if obj else None
    return f"{obj:08X}=" + ("%.1f/%.0f" % struct.unpack("<2f", d) if d else "-")

CANDS = {
    "old  [[14D0380]+820]": lambda: u32(u32(MB + 0x14D0380) + 0x820),
    "A    [[12579DC]+380]": lambda: u32(u32(MB + 0x12579DC) + 0x380),
    "B1 [[[14D09E0]+8FC]+8]": lambda: u32(u32(u32(MB + 0x14D09E0) + 0x8FC) + 0x8),
    "B2 [[[14D09E0]+C44]+1E0]": lambda: u32(u32(u32(MB + 0x14D09E0) + 0xC44) + 0x1E0),
    "B3 [[[14D09E0]+C98]+D0]": lambda: u32(u32(u32(MB + 0x14D09E0) + 0xC98) + 0xD0),
    "B4 [[[14D09E0]+CD0]+304]": lambda: u32(u32(u32(MB + 0x14D09E0) + 0xCD0) + 0x304),
}

end = time.time() + (float(sys.argv[1]) if len(sys.argv) > 1 else 0)
t0, prev = time.time(), None
while True:
    p = u32(MB + 0x14D1578)
    d = read(h, p + 0xDF0, 12) if p else None
    pos = "(%.0f, %.0f, %.0f)" % struct.unpack("<3f", d) if d else "-"
    vals = [f"{k}: {hp(f())}" for k, f in CANDS.items()]
    key = tuple(vals)
    if key != prev:
        print(f"t={time.time()-t0:6.1f}s pos={pos}\n    " + "\n    ".join(vals), flush=True)
        prev = key
    if time.time() >= end:
        break
    time.sleep(0.1)
