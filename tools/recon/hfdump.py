"""Read-only dump of sCollision's height fields (see docs/terrain-proxy.md).

Usage: py hfdump.py [WORDS]    WORDS = dwords of each resource to dump (default 64)
"""
import struct
import sys
from memscan import open_proc, read

SCOL = 0x18D0DD0
h = open_proc()


def u32(a):
    d = read(h, a, 4)
    return struct.unpack("<I", d)[0] if d and len(d) == 4 else None


def cstr(a, n=128):
    d = read(h, a, n) or b""
    return d.split(b"\0")[0].decode("latin1", "replace")


def dump(a, words):
    d = read(h, a, words * 4) or b""
    for i in range(0, len(d) // 4, 4):
        ws = struct.unpack_from("<4I", d, i * 4)
        fs = struct.unpack_from("<4f", d, i * 4)
        fstr = " ".join(f"{f:12.4g}" if 1e-4 < abs(f) < 1e8 else f"{'':12s}" for f in fs)
        print(f"  +{i*4:04X}  {' '.join(f'{w:08X}' for w in ws)}  {fstr}")


words = int(sys.argv[1]) if len(sys.argv) > 1 else 64
sc = u32(SCOL)
print(f"sCollision {sc:08X} vt {u32(sc):08X} (expect 0142BEE4)")
hf = sc + 0x3DE8
print(f"cSbcHeightField @ {hf:08X} vt {u32(hf):08X}")
dump(hf, 8)
cnt, arr = u32(hf + 8), u32(hf + 0x14)
print(f"count {cnt} array {arr:08X}" if arr else f"count {cnt} array null")
for i in range(min(cnt or 0, 64)):
    e = u32(arr + 4 * i)
    print(f"\n[{i}] cHeightField {e:08X}")
    if not e:
        continue
    dump(e, 8)
    res = u32(e + 0x18)
    if res:
        vt = u32(res)
        print(f"  resource {res:08X} vt {vt:08X} path '{cstr(res + 0x0C)}'")
        dump(res, words)
