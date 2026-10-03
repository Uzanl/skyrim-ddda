"""Look for each party member's body height: floats near 1 (a scale) inside the
character objects and small fields in the save-data record that differ between them.
Read-only.

Usage: py pawnheight.py
"""
import struct

import numpy as np
from memscan import open_proc, regions, read

PLAYER_VT, PAWN_VT = 0x15E90D0, 0x15E8468
OBJ = 0x5930
RECORD, NAME = 0x3DEC, 0x70C
REC_SIZE = 0x1000

h = open_proc()
chars = []
for b, s in regions(h):
    d = read(h, b, s)
    if not d:
        continue
    a = np.frombuffer(d[:len(d) // 4 * 4], "<u4")
    for vt in (PLAYER_VT, PAWN_VT):
        for i in np.nonzero(a == vt)[0]:
            chars.append(b + int(i) * 4)


def u32(addr):
    d = read(h, addr, 4)
    return struct.unpack("<I", d)[0] if d and len(d) == 4 else 0


party = []
for c in chars:
    rec = u32(c + RECORD)
    if not (0x10000 <= rec < 0x80000000):
        continue
    raw = read(h, rec + NAME, 32) or b""
    name = raw.split(b"\0")[0].decode("utf-8", "replace")
    pos = struct.unpack("<3f", read(h, c + 0x40, 12))
    if name and name.isprintable():
        party.append((name, c, rec, pos))
for name, c, rec, pos in party:
    print(f"{name:16} char {c:08X} record {rec:08X} pos {pos[0]:.0f} {pos[1]:.0f} {pos[2]:.0f}")

objs = [np.frombuffer(read(h, c, OBJ), "<f4") for _, c, _, _ in party]
print("\n-- floats in [0.5, 1.6] that differ between characters (character object)")
F = np.stack(objs)
for i in range(F.shape[1]):
    col = F[:, i]
    if np.all((col > 0.5) & (col < 1.6)) and np.ptp(col) > 1e-4:
        print(f"  +{i * 4:04X}", " ".join(f"{v:.4f}" for v in col))

recs = [read(h, r, REC_SIZE) for _, _, r, _ in party]
print("\n-- record bytes before the name that differ (u8 / u16 / f32 views)")
for off in range(0, NAME, 4):
    w = [r[off:off + 4] for r in recs]
    if len(set(w)) > 1:
        f = [struct.unpack("<f", x)[0] for x in w]
        u = [x.hex() for x in w]
        fs = " ".join(f"{v:.3f}" if abs(v) < 1e5 and (v == 0 or abs(v) > 1e-4) else "-" for v in f)
        print(f"  +{off:04X} {' '.join(u)} | {fs}")
