"""Find fields that tell the main pawn apart from hired pawns.

Locates the pawn-class objects (by vtable), identifies each by max HP, and prints
small-integer / byte fields whose value for the main pawn differs from both hired
pawns while the two hired pawns agree (or all three are distinct small ints).

Usage: py pawndiff.py MAIN_MAXHP
"""
import struct, sys
import numpy as np
from memscan import open_proc, regions, read

VT_PLAYER, VT_PAWN = 0x15E90D0, 0x15E8468
SIZE = 0x8000
h = open_proc()
main_max = float(sys.argv[1])

def u32(a):
    d = read(h, a, 4) if a else None
    return struct.unpack("<I", d)[0] if d else 0

objs = {}
for base, size in regions(h):
    d = read(h, base, size)
    if d:
        a = np.frombuffer(d[: len(d) // 4 * 4], dtype="<u4")
        for vt in (VT_PLAYER, VT_PAWN):
            for i in np.nonzero(a == vt)[0]:
                c = base + int(i) * 4
                st = u32(c + 0x4BC)
                mx = struct.unpack("<f", read(h, st + 0x1DC, 4))[0] if st else 0
                objs[c] = (vt, mx)
for c, (vt, mx) in objs.items():
    print(f"{'player' if vt == VT_PLAYER else 'pawn  '} {c:08X} max {mx:.0f}")

pawns = [c for c, (vt, _) in objs.items() if vt == VT_PAWN]
main = [c for c in pawns if objs[c][1] == main_max]
hired = [c for c in pawns if objs[c][1] != main_max]
if len(main) != 1 or len(hired) != 2:
    sys.exit(f"expected 1 main + 2 hired, got {len(main)} + {len(hired)}")
m = read(h, main[0], SIZE)
h1, h2 = read(h, hired[0], SIZE), read(h, hired[1], SIZE)
print(f"main {main[0]:08X}, hired {hired[0]:08X} {hired[1]:08X}")
for off in range(0, SIZE, 4):
    vm, v1, v2 = (struct.unpack_from("<I", b, off)[0] for b in (m, h1, h2))
    if max(vm, v1, v2) <= 16 and v1 == v2 and vm != v1:
        print(f"  dword +{off:X}: main={vm} hired={v1}")
for off in range(SIZE):
    bm, b1, b2 = m[off], h1[off], h2[off]
    if b1 == b2 and bm != b1 and max(bm, b1) <= 4 and off % 4 != 0:
        print(f"  byte  +{off:X}: main={bm} hired={b1}")
