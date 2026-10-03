"""Find every copy of the current camera position (from [DDDA.exe+14D1578]+DF0) and
show the vtable of the object it likely lives in plus nearby floats.

Usage: py camscan.py
"""
import struct
import numpy as np
from memscan import open_proc, read, regions

MB, ME = 0x400000, 0x400000 + 0x160C000
h = open_proc()
u = lambda a: struct.unpack("<I", read(h, a, 4))[0]
cam = np.frombuffer(read(h, u(MB + 0x14D1578) + 0xDF0, 12), dtype="<f4")
print("cam", cam)
for base, size in regions(h):
    d = read(h, base, size)
    if not d:
        continue
    f = np.frombuffer(d[: len(d) // 4 * 4], dtype="<f4")
    hit = np.nonzero((np.abs(f[:-2] - cam[0]) < 0.5) & (np.abs(f[1:-1] - cam[1]) < 0.5) & (np.abs(f[2:] - cam[2]) < 0.5))[0]
    for i in hit:
        a = base + i * 4
        # nearest preceding dword that looks like an image vtable (object start), within 0x2000
        owner = ""
        for back in range(0, 0x2000, 4):
            j = i - back // 4
            if j < 0:
                break
            w = struct.unpack_from("<I", d, j * 4)[0]
            if 0x1400000 <= w < ME:
                owner = f"vt {w:08X} @ {a - back:08X} (+{back:X})"
                break
        print(f"{a:08X}  {owner}")
