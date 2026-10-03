"""Print every known position/HP chain side by side, to spot which ones drift.

Usage: py chains.py [seconds]
"""
import struct, sys, time
from memscan import open_proc, read

MB = 0x400000
POS = [(0x14D1578, 0xDF0), (0x14D09E0, 0xEC4), (0x14D08F4, 0x2B8)]
HP = [0x14D0380, 0x14D0B28, 0x14D1264]

def u32(h, a):
    d = read(h, a, 4) if a else None
    return struct.unpack("<I", d)[0] if d else 0

def floats(h, a, n):
    d = read(h, a, 4 * n) if a else None
    return struct.unpack(f"<{n}f", d) if d else None

def fmt(v):
    return "-" if v is None else "(" + ", ".join(f"{x:.1f}" for x in v) + ")"

if __name__ == "__main__":
    h = open_proc()
    end = time.time() + (float(sys.argv[1]) if len(sys.argv) > 1 else 0)
    while True:
        cols = []
        for root, off in POS:
            p = u32(h, MB + root)
            cols.append(f"{p:08X}:{fmt(floats(h, p + off, 3) if p else None)}")
        for root in HP:
            a = u32(h, MB + root)
            o = u32(h, a + 0x820) if a else 0
            cols.append(f"{a:08X}>{o:08X}:{fmt(floats(h, o + 0xEC8, 2) if o else None)}")
        print(" | ".join(cols), flush=True)
        if time.time() >= end:
            break
        time.sleep(0.5)
