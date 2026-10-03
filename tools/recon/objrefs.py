"""List pointers inside an object (and one level deeper) to MT objects whose class
name matches a regex.

Usage: py objrefs.py OBJ_HEX SIZE_HEX REGEX [DEPTH]
"""
import re, struct, sys
from memscan import open_proc, read
from dtiname import name

MB, ME = 0x400000, 0x400000 + 0x160C000
h = open_proc()
obj, size = int(sys.argv[1], 16), int(sys.argv[2], 16)
rx = re.compile(sys.argv[3], re.I)
depth = int(sys.argv[4]) if len(sys.argv) > 4 else 1
cache = {}

def cls(p):
    d = read(h, p, 4)
    if not d: return None
    vt = struct.unpack("<I", d)[0]
    if not (MB <= vt < ME): return None
    if vt not in cache:
        try: cache[vt] = name(vt)
        except Exception: cache[vt] = None
    n = cache[vt]
    return n[1] if n else None

seen = set()
def walk(base, size, path, lvl):
    d = read(h, base, size)
    if not d: return
    for i in range(0, len(d) - 3, 4):
        p = struct.unpack_from("<I", d, i)[0]
        if p < 0x10000 or p >= 0x80000000 or p in seen: continue
        n = cls(p)
        if not n: continue
        seen.add(p)
        here = f"{path}+{i:X}"
        if rx.search(n):
            print(f"{here} -> {p:08X} {n}")
        if lvl < depth:
            walk(p, 0x200, here + f"->[{n}]", lvl + 1)

walk(obj, size, f"{obj:08X}", 1)
