"""Print the party as the bridge would classify it, from the unit lists in
[DDDA.exe+122221C]: +870 (active humans) and +440 (hired pawns).

Layout per character: +0 vtable, +40 position xyz, [+4BC]+1D8 HP cur, +1DC HP max.

Usage: py party.py [seconds]
"""
import struct, sys, time
from memscan import open_proc, read

MB = 0x400000
UNIT = 0x122221C
LIST_ALL, LIST_HIRED, CAP = 0x870, 0x440, 0x40
VT_PLAYER, VT_PAWN = 0x15E90D0, 0x15E8468
h = open_proc()

def u32(a):
    d = read(h, a, 4) if a else None
    return struct.unpack("<I", d)[0] if d else 0

def lst(unit, off):
    d = read(h, unit + off, CAP * 4) or b""
    out = []
    for (p,) in struct.iter_unpack("<I", d):
        if not p:
            break
        out.append(p)
    return out

def info(c):
    vt = u32(c)
    pd = read(h, c + 0x40, 12)
    pos = struct.unpack("<3f", pd) if pd else (0, 0, 0)
    hd = read(h, u32(c + 0x4BC) + 0x1D8, 8) if u32(c + 0x4BC) else None
    hp = struct.unpack("<2f", hd) if hd else (0, 0)
    return vt, pos, hp

end = time.time() + (float(sys.argv[1]) if len(sys.argv) > 1 else 0)
while True:
    unit = u32(MB + UNIT)
    allc, hired = lst(unit, LIST_ALL), lst(unit, LIST_HIRED)
    print(f"--- unit {unit:08X}: {len(allc)} in +870, {len(hired)} in +440")
    for c in dict.fromkeys(allc + hired):
        vt, pos, hp = info(c)
        role = ("ARISEN" if vt == VT_PLAYER else
                "hired" if c in hired else
                "MAIN PAWN?" if vt == VT_PAWN else f"other vt {vt:08X}")
        where = ("A" if c in allc else "-") + ("H" if c in hired else "-")
        print(f"  {c:08X} [{where}] {role:12s} pos=({pos[0]:.0f}, {pos[1]:.0f}, {pos[2]:.0f}) hp={hp[0]:.0f}/{hp[1]:.0f}")
    if time.time() >= end:
        break
    time.sleep(2)
