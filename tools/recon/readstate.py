"""Print the Arisen's live position and HP from a running DDDA.exe (read-only).

Usage: py readstate.py [seconds]
"""
import struct, sys, time
from memscan import open_proc, read

MB = 0x400000  # DDDA.exe image base (32-bit, no ASLR)

def u32(h, a):
    d = read(h, a, 4)
    return struct.unpack("<I", d)[0] if d else 0

def state(h):
    p = u32(h, MB + 0x14D1578)
    pos = struct.unpack("<3f", read(h, p + 0xDF0, 12)) if p else None
    o = u32(h, u32(h, MB + 0x14D0380) + 0x820)
    hp = struct.unpack("<2f", read(h, o + 0xEC8, 8)) if o else None
    return pos, hp

if __name__ == "__main__":
    h = open_proc()
    end = time.time() + (float(sys.argv[1]) if len(sys.argv) > 1 else 0)
    while True:
        pos, hp = state(h)
        print("pos", pos and tuple(round(x, 1) for x in pos), "hp", hp and f"{hp[0]:.1f}/{hp[1]:.0f}")
        if time.time() >= end:
            break
        time.sleep(0.5)
