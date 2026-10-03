"""Find live objects whose MtDTI class name matches a regex.

Usage: py findclass.py REGEX
"""
import re, struct, sys
import numpy as np
from memscan import open_proc, read, regions
from dtiname import name

MB, ME = 0x400000, 0x400000 + 0x160C000
h = open_proc()
rx = re.compile(sys.argv[1], re.I)
cache, hits = {}, {}
for base, size in regions(h):
    d = read(h, base, size)
    if not d:
        continue
    w = np.frombuffer(d[: len(d) // 4 * 4], dtype="<u4")
    idx = np.nonzero((w >= 0x1400000) & (w < ME) & (np.arange(len(w)) % 4 == 0))[0]
    for vt in np.unique(w[idx]):
        vt = int(vt)
        if vt not in cache:
            try:
                cache[vt] = name(vt)
            except Exception:
                cache[vt] = None
        n = cache[vt]
        if n and rx.search(n[1]):
            for i in idx[w[idx] == vt]:
                hits.setdefault(n[1], []).append(base + int(i) * 4)
for n, objs in sorted(hits.items()):
    print(f"{n}: {len(objs)}  " + " ".join(f"{a:08X}" for a in objs[:200]))
