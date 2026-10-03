"""Tiny snapshot-diff memory scanner for DDDA.exe (32-bit), read-only.

Usage:
  py memscan.py snap NAME                 full snapshot of writable memory
  py memscan.py start A B changed|unchanged|inc|dec   create candidates (float32) from two snaps
  py memscan.py next  A B changed|unchanged|inc|dec   filter candidates
  py memscan.py find  SNAP VALUE [TOL]    candidates = float32 ~= VALUE
  py memscan.py show  SNAP [N]            print candidates with values
  py memscan.py live  [N]                 print candidates read live
"""
import ctypes as C, ctypes.wintypes as W, sys, os, json
import numpy as np

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_data")  # snapshots are ~900 MB each
SNAPS = os.path.join(HERE, "snaps")
CAND = os.path.join(HERE, "cand.npy")
os.makedirs(SNAPS, exist_ok=True)

k32 = C.WinDLL("kernel32", use_last_error=True)

class MBI(C.Structure):
    _fields_ = [("BaseAddress", C.c_void_p), ("AllocationBase", C.c_void_p),
                ("AllocationProtect", W.DWORD), ("PartitionId", W.WORD),
                ("RegionSize", C.c_size_t), ("State", W.DWORD),
                ("Protect", W.DWORD), ("Type", W.DWORD)]

def find_pid(name="DDDA.exe"):
    import subprocess
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"], text=True)
    for line in out.splitlines():
        p = [x.strip('"') for x in line.split('","')]
        if p and p[0].lower() == name.lower():
            return int(p[1])
    sys.exit("DDDA.exe not running")

def open_proc():
    h = k32.OpenProcess(0x0400 | 0x0010, False, find_pid())
    if not h:
        sys.exit(f"OpenProcess failed {C.get_last_error()}")
    return h

def regions(h):
    mbi, addr = MBI(), 0
    while addr < 0xFFFF0000:
        if not k32.VirtualQueryEx(h, C.c_void_p(addr), C.byref(mbi), C.sizeof(mbi)):
            break
        base, size = mbi.BaseAddress or 0, mbi.RegionSize
        if mbi.State == 0x1000 and mbi.Protect in (0x04, 0x08, 0x40) and not (mbi.Protect & 0x100):
            yield base, size
        addr = base + size

def read(h, addr, size):
    buf = (C.c_char * size)()
    n = C.c_size_t()
    if not k32.ReadProcessMemory(h, C.c_void_p(addr), buf, size, C.byref(n)):
        return None
    return buf.raw[:n.value]

def snap(name):
    h = open_proc()
    idx, path = [], os.path.join(SNAPS, name + ".bin")
    off = 0
    with open(path, "wb") as f:
        for base, size in regions(h):
            data = read(h, base, size)
            if not data:
                continue
            f.write(data)
            idx.append((base, len(data), off))
            off += len(data)
    np.save(os.path.join(SNAPS, name + ".idx.npy"), np.array(idx, dtype=np.int64))
    print(f"snap {name}: {len(idx)} regions, {off/1e6:.0f} MB")

def load(name):
    idx = np.load(os.path.join(SNAPS, name + ".idx.npy"))
    mm = np.memmap(os.path.join(SNAPS, name + ".bin"), dtype=np.uint8, mode="r")
    return idx, mm

def region_floats(mm, size, off):
    n = size // 4
    return np.frombuffer(mm[off:off + n * 4], dtype=np.float32)

def sane(v):
    return np.isfinite(v) & (np.abs(v) < 1e6) & ((np.abs(v) > 1e-3) | (v == 0))

def pred(a, b, mode):
    if mode == "changed":   return a != b
    if mode == "unchanged": return a == b
    if mode == "inc":       return b > a
    if mode == "dec":       return b < a
    sys.exit("bad mode")

def start(A, B, mode):
    ia, ma = load(A); ib, mb = load(B)
    rb = {int(r[0]): r for r in ib}
    out = []
    for base, size, off in ia:
        r = rb.get(int(base))
        if r is None or r[1] != size:
            continue
        a = region_floats(ma, size, off); b = region_floats(mb, size, r[2])
        m = pred(a, b, mode) & sane(a) & sane(b)
        out.append(np.nonzero(m)[0].astype(np.uint32) * 4 + np.uint32(base))
    c = np.concatenate(out) if out else np.zeros(0, np.uint32)
    np.save(CAND, c); print(f"{len(c)} candidates")

def values_at(name, addrs):
    idx, mm = load(name)
    bases, sizes, offs = idx[:, 0], idx[:, 1], idx[:, 2]
    a = addrs.astype(np.int64)
    k = np.searchsorted(bases, a, side="right") - 1
    ok = (k >= 0) & (a + 4 <= bases[np.clip(k, 0, None)] + sizes[np.clip(k, 0, None)])
    vals = np.full(len(a), np.nan, np.float32)
    pos = offs[k[ok]] + (a[ok] - bases[k[ok]])
    raw = np.stack([mm[pos + i] for i in range(4)], axis=1).copy()
    vals[ok] = raw.view(np.float32).ravel()
    return vals, ok

def nxt(A, B, mode):
    c = np.load(CAND)
    a, oka = values_at(A, c); b, okb = values_at(B, c)
    m = oka & okb & pred(a, b, mode) & sane(b)
    c = c[m]; np.save(CAND, c); print(f"{len(c)} candidates")

def find(S, value, tol):
    idx, mm = load(S)
    out = []
    for base, size, off in idx:
        v = region_floats(mm, size, off)
        out.append(np.nonzero(np.abs(v - value) <= tol)[0].astype(np.uint32) * 4 + np.uint32(base))
    c = np.concatenate(out); np.save(CAND, c); print(f"{len(c)} candidates")

def show(S, n):
    c = np.load(CAND)
    v, _ = values_at(S, c[:n])
    for a, x in zip(c[:n], v):
        print(f"{int(a):08X}  {x:.4f}")
    print(f"({len(c)} total)")

def live(n):
    h = open_proc(); c = np.load(CAND)
    for a in c[:n]:
        d = read(h, int(a) - 8, 24)
        if d:
            f = np.frombuffer(d, np.float32)
            print(f"{int(a):08X}  " + " ".join(f"{x:10.3f}" for x in f))
    print(f"({len(c)} total)")

if __name__ == "__main__":
    cmd, *a = sys.argv[1:]
    if cmd == "snap": snap(a[0])
    elif cmd == "start": start(*a)
    elif cmd == "next": nxt(*a)
    elif cmd == "find": find(a[0], float(a[1]), float(a[2]) if len(a) > 2 else 0.01)
    elif cmd == "show": show(a[0], int(a[1]) if len(a) > 1 else 40)
    elif cmd == "live": live(int(a[0]) if len(a) > 0 else 40)
