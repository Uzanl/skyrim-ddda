import numpy as np, struct, sys, warnings; warnings.filterwarnings("ignore")
from memscan import open_proc, read, regions
h = open_proc(); MB, ME = 0x400000, 0x400000+0x160C000
targets = [int(x,16) for x in sys.argv[1:]]
mem = [(b, np.frombuffer(d[:len(d)//4*4], np.uint32).astype(np.int64)) for b,s in regions(h) if (d:=read(h,b,s))]
def holders(t, rng):
    out=[]
    for b,u in mem:
        for i in np.nonzero((u <= t) & (u > t-rng))[0]: out.append((b+int(i)*4, int(u[i])))
    return out
for t in targets:
    print(f"== {t:08X}")
    l1 = holders(t, 0x2000)
    found = 0
    for p1, o1 in l1:
        if MB <= p1 < ME:
            print(f"  [DDDA.exe+{p1-MB:X}]+{t-o1:X}"); found += 1
    # level 2
    l1h = [x for x in l1 if not (MB <= x[0] < ME)]
    P = np.array(sorted(set(p for p,_ in l1h)), np.int64)
    off1 = {p: t-o for p,o in l1h}
    for b,u in mem:
        if not (MB <= b < ME): continue
        k = np.searchsorted(P, u, side="left")
        ok = k < len(P)
        d = np.where(ok, P[np.clip(k,0,len(P)-1)] - u, 1<<40)
        for i in np.nonzero(ok & (d >= 0) & (d < 0x2000))[0]:
            p1 = int(P[k[i]])
            print(f"  [[DDDA.exe+{b+int(i)*4-MB:X}]+{p1-int(u[i]):X}]+{off1[p1]:X}"); found += 1
            if found > 25: break
    print("  total", found)
