"""Find pawn name strings and how each pawn object reaches its name (0..2 hops).

Usage: py names.py PAWN_HEX=Name [PAWN_HEX=Name ...]
"""
import struct, sys
from memscan import open_proc, regions, read

MB, ME = 0x400000, 0x400000 + 0x160C000
h = open_proc()
targets = [(int(a, 16), n) for a, _, n in (x.partition("=") for x in sys.argv[1:])]

hits = {}
for base, size in regions(h):
    d = read(h, base, size)
    if not d:
        continue
    for _, name in targets:
        for enc in ("utf-8", "utf-16-le"):
            pat = name.encode(enc) + (b"\0\0" if enc == "utf-16-le" else b"\0")
            i = d.find(pat)
            while i != -1:
                hits.setdefault(name, []).append((base + i, enc))
                i = d.find(pat, i + 1)
for name, lst in hits.items():
    print(name, [f"{a:08X}({e[:5]})" for a, e in lst][:20])

OBJ = 0x5930
def words(a, n):
    d = read(h, a, n) or b""
    return struct.unpack(f"<{len(d)//4}I", d[: len(d) // 4 * 4])

for obj, name in targets:
    spots = [a for a, _ in hits.get(name, [])]
    print(f"== {name} pawn {obj:08X}")
    w = words(obj, OBJ)
    for i, p in enumerate(w):
        if not (0x10000 <= p < 0x80000000):
            continue
        for s in spots:
            if p <= s < p + 0x2000:
                static = " (static)" if MB <= p < ME else ""
                print(f"   [+{i*4:X}] = {p:08X}{static}, name at +{s - p:X}")
        w2 = words(p, 0x400)
        for j, q in enumerate(w2):
            if not (0x10000 <= q < 0x80000000):
                continue
            for s in spots:
                if q <= s < q + 0x400:
                    static = " (static)" if MB <= q < ME else ""
                    print(f"   [[+{i*4:X}]+{j*4:X}] = {q:08X}{static}, name at +{s - q:X}")
