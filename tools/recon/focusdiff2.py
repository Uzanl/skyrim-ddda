"""Full-memory version of focusdiff: wait for focus states F, U, F, U (each stable
for 3 s), snapshot all writable memory in each, then report dwords/bytes equal
within same-state snapshots and different across states.

Usage: py focusdiff2.py [TIMEOUT_SECONDS]
"""
import ctypes as C, ctypes.wintypes as W, sys, time
import numpy as np
from memscan import find_pid, open_proc, read, regions

u32 = C.WinDLL("user32")
h = open_proc()
pid = find_pid()

def foreground_is_game():
    p = W.DWORD()
    u32.GetWindowThreadProcessId(u32.GetForegroundWindow(), C.byref(p))
    return p.value == pid

def snapshot():
    out = {}
    for base, size in regions(h):
        d = read(h, base, size)
        if d:
            out[base] = np.frombuffer(d, dtype=np.uint8).copy()
    return out

want = [True, False, True, False]
snaps = []
deadline = time.time() + (float(sys.argv[1]) if len(sys.argv) > 1 else 300)
stable_since, last = time.time(), foreground_is_game()
while want and time.time() < deadline:
    cur = foreground_is_game()
    if cur != last:
        stable_since, last = time.time(), cur
    if cur == want[0] and time.time() - stable_since >= 3:
        t = time.time()
        s = snapshot()
        still = foreground_is_game() == cur
        print(f"snap {'FOCUSED' if cur else 'unfocused'}: {sum(len(v) for v in s.values())/1e6:.0f} MB in {time.time()-t:.1f}s"
              f"{'' if still else ' (focus changed during snap, retrying)'}", flush=True)
        if still:
            snaps.append(s)
            want.pop(0)
        stable_since = time.time()
    time.sleep(0.2)
if want:
    sys.exit(f"timed out; got {len(snaps)} of 4 snapshots")

F1, U1, F2, U2 = snaps
hits = []
for base in F1:
    if not all(base in s and len(s[base]) == len(F1[base]) for s in (U1, F2, U2)):
        continue
    a, b, c, d = F1[base], U1[base], F2[base], U2[base]
    m = (a == c) & (b == d) & (a != b)
    for i in np.nonzero(m)[0]:
        hits.append((base + int(i), int(a[i]), int(b[i])))
print(f"{len(hits)} bytes track focus")
prev = None
for addr, fv, uv in hits[:400]:
    print(f"  {addr:08X} focused={fv:02X} unfocused={uv:02X}")
