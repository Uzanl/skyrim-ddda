"""Find model objects near a character: an image vtable at +0, position at +0x40
within RADIUS of the character, and mTransparency (+0x158) == 1.0. Prints whether
the object references the character, and its parts mask.

Usage: py nearmodels.py CHAR_HEX [RADIUS]
"""
import struct, sys
import numpy as np
from memscan import open_proc, read, regions

MB, ME = 0x400000, 0x400000 + 0x160C000
h = open_proc()
ch = int(sys.argv[1], 16)
rad = float(sys.argv[2]) if len(sys.argv) > 2 else 400
ONLY_MODELS = "--all" not in sys.argv
cp = np.array(struct.unpack("<3f", read(h, ch + 0x40, 12)))
for base, size in regions(h):
    d = read(h, base, size)
    if not d or len(d) < 0x200:
        continue
    w = np.frombuffer(d[: len(d) // 4 * 4], dtype="<u4")
    f = w.view("<f4")
    idx = np.nonzero((w[:-0x58] >= 0x1400000) & (w[:-0x58] < ME))[0]
    idx = idx[(idx % 4 == 0)]  # 16-byte aligned objects
    for i in idx:
        if ONLY_MODELS and f[i + 0x56] != 1.0:  # +0x158
            continue
        p = f[i + 0x10: i + 0x13]
        if not np.all(np.isfinite(p)) or np.linalg.norm(p - cp) > rad:
            continue
        a = base + i * 4
        objw = w[i: i + 0x200] if i + 0x200 <= len(w) else w[i:]
        refs = [hex(k * 4) for k in np.nonzero(objw == ch)[0]]
        vt = int(w[i])
        print(f"{a:08X} vt {vt:08X} pos=({p[0]:.0f},{p[1]:.0f},{p[2]:.0f}) mask110={int(w[i+0x44]):08X} refsChar={refs}")
