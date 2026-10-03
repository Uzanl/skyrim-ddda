"""Read-only census of live stage collision and navigation objects.

Usage: py sbcscan.py
Lists rCollision / rCollisionHeightField / rNavigationMesh / rAIWayPoint resources
(with their file path) and uScrollCollision* units, plus the Arisen's position.
"""
import struct
import numpy as np
from memscan import open_proc, regions, read

CLASSES = {
    0x143F780: "rCollision",
    0x1439A3C: "rCollisionHeightField",
    0x143F5D8: "rNavigationMesh",
    0x14481C4: "rAIWayPoint",
    0x14498E8: "uScrollCollisionSbc",
    0x1449160: "uScrollCollision",
    0x15E90D0: "uPlayer",
}
h = open_proc()
vts = np.array(list(CLASSES), dtype="<u4")
hits = []
for base, size in regions(h):
    data = read(h, base, size)
    if not data:
        continue
    a = np.frombuffer(data[: len(data) // 4 * 4], dtype="<u4")
    for i in np.nonzero(np.isin(a, vts))[0]:
        hits.append((CLASSES[int(a[i])], base + int(i) * 4))


def cstr(a, n=96):
    d = read(h, a, n) or b""
    s = d.split(b"\0")[0]
    return s.decode("latin1") if s and all(32 <= c < 127 for c in s) else ""


for name, o in sorted(hits):
    extra = ""
    if name.startswith("r"):
        extra = cstr(o + 0x0C)
        sz = struct.unpack("<I", read(h, o + 0x54, 4))[0]
        extra = f"size {sz:8d}  {extra}"
    elif name == "uPlayer":
        extra = "pos %.1f %.1f %.1f" % struct.unpack("<3f", read(h, o + 0x40, 12))
    elif name == "uScrollCollisionSbc":
        d = read(h, o + 0x38, 4)
        extra = "handle %08X pos %.0f %.0f %.0f" % ((struct.unpack("<I", d)[0],) + struct.unpack("<3f", read(h, o + 0x50, 12)))
    print(f"{name:22s} {o:08X}  {extra}")
