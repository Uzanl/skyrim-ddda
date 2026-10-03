"""Per pass of a trace: how many draws have a NORMAL in their vertex declaration,
and how many use a party vertex buffer (list from partyvbs.py output piped in).

Usage: py drawclass.py TRACE B7,B17,...
"""
import re, sys
from collections import Counter

path, party = sys.argv[1], set(sys.argv[2].split(","))
decl = {}
for line in open(path, encoding="latin1"):
    m = re.match(r"(V\d+) = \S+ decl(.*)", line)
    if m: decl[m.group(1)] = m.group(2)
cur, stats = None, []
for line in open(path, encoding="latin1"):
    m = re.search(r"draw (\d+) \S+ prims=(\d+) verts=\d+ rt=(\S+),\S+ ds=(\S+) (\S+) .* vb=(B\d+|-)", line)
    if not m: continue
    n, prims, rt, ds, v, vb = m.groups()
    key = (rt, ds)
    if not cur or cur[0] != key:
        cur = (key, int(n), Counter())
        stats.append(cur)
    nrm = "NRM" in decl.get(v, "")
    cur[2][("N" if nrm else "-") + ("P" if vb in party else "o")] += 1
for key, first, c in stats:
    if sum(c.values()) > 2:
        print(f"from draw {first:4d} rt={key[0]:5s} ds={key[1]:5s} {dict(c)}")
