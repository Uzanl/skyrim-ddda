"""Find every player/pawn-class object and show which unit-list slots in
[DDDA.exe+122221C] hold them. Also dumps all list headers in the unit object.

Usage: py unitlines.py
"""
import struct
import numpy as np
from memscan import open_proc, regions, read

MB = 0x400000
VT_PLAYER, VT_PAWN = 0x15E90D0, 0x15E8468
h = open_proc()

def u32(a):
    d = read(h, a, 4) if a else None
    return struct.unpack("<I", d)[0] if d else 0

unit = u32(MB + 0x122221C)
chars = []
for base, size in regions(h):
    d = read(h, base, size)
    if d:
        a = np.frombuffer(d[: len(d) // 4 * 4], dtype="<u4")
        for vt in (VT_PLAYER, VT_PAWN):
            chars += [(base + int(i) * 4, vt) for i in np.nonzero(a == vt)[0]]

ud = read(h, unit, 0x4000)
uw = struct.unpack(f"<{len(ud)//4}I", ud)
print(f"unit {unit:08X}")
for i in range(len(uw) - 4):
    if (uw[i] >> 24) == 0x10 and (uw[i + 1] >> 24) == 0x06 and uw[i + 3] == 0x30:
        n = 0
        while i + 4 + n < len(uw) and uw[i + 4 + n] and n < uw[i + 2]:
            n += 1
        print(f"  list header +{i*4:X}: {uw[i]:08X} {uw[i+1]:08X} cap={uw[i+2]} -> entries at +{(i+4)*4:X}, {n} used")

for c, vt in chars:
    hd = read(h, u32(c + 0x4BC) + 0x1D8, 8) if u32(c + 0x4BC) else None
    hp = struct.unpack("<2f", hd) if hd else (0, 0)
    pos = struct.unpack("<3f", read(h, c + 0x40, 12))
    slots = [f"+{j*4:X}" for j, v in enumerate(uw) if v == c]
    kind = "PLAYER" if vt == VT_PLAYER else "pawn"
    print(f"{kind:6s} {c:08X} hp={hp[0]:.0f}/{hp[1]:.0f} pos=({pos[0]:.0f}, {pos[1]:.0f}, {pos[2]:.0f}) unit slots {slots}")
