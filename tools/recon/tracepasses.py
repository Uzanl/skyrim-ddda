"""Summarise ddda_frame_trace.txt into passes: consecutive draws with the same
render target / depth, with draw counts per vertex-declaration kind.

Usage: py tracepasses.py [TRACE]
"""
import re, sys
from collections import Counter, OrderedDict

path = sys.argv[1] if len(sys.argv) > 1 else r"E:\SteamLibrary\steamapps\common\DDDA\ddda_frame_trace.txt"
res, calls = {}, []
section = None
for line in open(path, encoding="latin1"):
    line = line.rstrip("\n")
    if line.startswith("# resources"): section = "res"; continue
    if line.startswith("# calls"): section = "calls"; continue
    if section == "res" and " = " in line:
        k, rest = line.split(" = ", 1)
        res[k] = rest.split(" ", 1)[1] if " " in rest else ""
    elif section == "calls":
        calls.append(line)

def kind(decl):
    d = res.get(decl, "")
    if "BW" in d or "BI" in d: return "skinned"
    if decl == "-": return "fvf"
    return "static"

passes, cur = [], None
for c in calls:
    m = re.match(r"\s+draw (\d+) (\S+) prims=(\d+) verts=(\d+) rt=(\S+),(\S+) ds=(\S+) (\S+)", c)
    if m:
        n, typ, prims, verts, rt0, rt1, ds, decl = m.groups()
        key = (rt0, rt1, ds)
        if not cur or cur["key"] != key:
            cur = {"key": key, "first": int(n), "kinds": Counter(), "prims": 0, "decls": Counter(), "ps": Counter()}
            passes.append(cur)
        cur["kinds"][kind(decl)] += 1
        cur["decls"][decl] += 1
        cur["prims"] += int(prims)
        cur["last"] = int(n)
        ps = re.search(r" ps=(\S+)", c).group(1)
        cur["ps"][ps] += 1
    elif c.startswith("Clear") or c.startswith("StretchRect"):
        passes.append({"note": c})
for p in passes:
    if "note" in p:
        print("   ", p["note"]); continue
    rt0, rt1, ds = p["key"]
    print(f"draws {p['first']:4d}-{p['last']:4d} rt0={rt0}[{res.get(rt0,'')[:40]}] rt1={rt1} ds={ds}[{res.get(ds,'')[:30]}]"
          f" {dict(p['kinds'])} prims={p['prims']} ps={len(p['ps'])}")
