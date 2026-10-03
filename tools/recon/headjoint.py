"""Look for the party's skeleton: long runs of joint positions in model space (relative
to the feet: |x|, |z| < 80 cm, y -20..230 cm), in memory reached from the character
object in one or two hops. Prints the best runs per character with the highest joint.
Read-only.

Usage: py headjoint.py [NAME ...]
"""
import struct
import sys
import warnings

import numpy as np
from memscan import open_proc, regions, read

warnings.filterwarnings("ignore")
PLAYER_VT, PAWN_VT = 0x15E90D0, 0x15E8468
OBJ = 0x5930
RECORD, NAME = 0x3DEC, 0x70C

h = open_proc()


def u32(addr):
    d = read(h, addr, 4)
    return struct.unpack("<I", d)[0] if d and len(d) == 4 else 0


def party():
    out = []
    for b, s in regions(h):
        d = read(h, b, s)
        if not d:
            continue
        a = np.frombuffer(d[:len(d) // 4 * 4], "<u4")
        for vt in (PLAYER_VT, PAWN_VT):
            for i in np.nonzero(a == vt)[0]:
                c = b + int(i) * 4
                rec = u32(c + RECORD)
                raw = read(h, rec + NAME, 32) if 0x10000 <= rec < 0x80000000 else None
                name = raw.split(b"\0")[0].decode("utf-8", "replace") if raw else ""
                if name and name.isprintable():
                    out.append((name, c))
    return out


def longest_run(ok):
    """(length, start) of the longest run of True."""
    if not ok.any():
        return 0, 0
    d = np.diff(np.concatenate(([0], ok.astype(np.int8), [0])))
    starts, ends = np.nonzero(d == 1)[0], np.nonzero(d == -1)[0]
    k = int(np.argmax(ends - starts))
    return int(ends[k] - starts[k]), int(starts[k])


def pointers(addr, size, lo=0x01000000):
    d = read(h, addr, size) or b""
    w = np.frombuffer(d[:len(d) // 4 * 4], "<u4")
    for i in np.nonzero((w >= lo) & (w < 0x80000000))[0]:
        yield int(i) * 4, int(w[i])


def scan(c):
    best = []
    seen = set()
    todo = [(f"+{i:X}", p) for i, p in pointers(c, OBJ)]
    todo += [(f"{lab}+{j:X}", q) for lab, p in list(todo) for j, q in pointers(p, 0x100)]
    for lab, p in todo:
        if p in seen:
            continue
        seen.add(p)
        raw = read(h, p, 0x4000)
        if not raw or len(raw) < 0x400:
            continue
        f = np.frombuffer(raw[:len(raw) // 4 * 4], "<f4")
        for s in range(0x30, 0x110, 0x10):
            S = s // 4
            for o in range(0, S, 4):
                idx = np.arange(o, len(f) - 2, S)
                x, y, z = f[idx], f[idx + 1], f[idx + 2]
                ok = (np.abs(x) < 80) & (np.abs(z) < 80) & (y > -20) & (y < 230)
                n, st = longest_run(ok)
                if n >= 20:
                    ys = y[st:st + n]
                    if ys.max() > 90 and np.ptp(ys) > 60:
                        best.append((n, lab, p, s, o * 4, st, float(ys.max()), int(np.argmax(ys))))
    return sorted(best, reverse=True)[:8]


want = set(sys.argv[1:])
for name, c in party():
    if want and name not in want:
        continue
    sy = struct.unpack("<f", read(h, c + 0x64, 4))[0]
    print(f"== {name} char {c:08X} scaleY {sy:.4f}", flush=True)
    for n, lab, p, s, o, st, top, at in scan(c):
        print(f"   run {n} [{lab}] -> {p:08X} stride {s:#x} off +{o:X} from #{st}: top {top:.1f} cm at #{at}",
              flush=True)
