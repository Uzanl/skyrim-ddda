"""Annotated dword dump: for each dword that points at an object with an image vtable,
show that vtable; mark known status objects and plausible floats.

Usage: py dumpobj.py ADDR_HEX START_OFF_HEX END_OFF_HEX
"""
import struct, sys
from memscan import open_proc, read

MB, ME = 0x400000, 0x400000 + 0x160C000
h = open_proc()
base, s, e = (int(x, 16) for x in sys.argv[1:4])

def u32(a):
    d = read(h, a, 4)
    return struct.unpack("<I", d)[0] if d else None

d = read(h, base + s, e - s)
for i in range(0, len(d) // 4 * 4, 4):
    w = struct.unpack_from("<I", d, i)[0]
    f = struct.unpack_from("<f", d, i)[0]
    note = ""
    if 0x10000 <= w < 0x80000000:
        vt = u32(w)
        if vt is not None and MB <= vt < ME:
            note = f"-> obj vt {vt:08X}"
            if vt == 0x159F198:
                hp = struct.unpack("<2f", read(h, w + 0xEC8, 8))
                note += f"  STATUS hp={hp[0]:.0f}/{hp[1]:.0f}"
    if not note and 1e-3 < abs(f) < 1e6:
        note = f"float {f:.3f}"
    print(f"+{s + i:04X}  {w:08X}  {note}")
