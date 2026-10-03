"""Sample the game window thread's call stack (suspend / get context / resume, no
debugger) and group the game-code return chains by focus state.

Usage: py stacksample.py SECONDS
"""
import ctypes as C, ctypes.wintypes as W, struct, sys, time
from collections import Counter
from memscan import find_pid, open_proc, read
from hwbp import k32, WOW64_CONTEXT, THREAD_ALL, WOW64_CONTEXT_FULL

u32 = C.WinDLL("user32")
TEXT_LO, TEXT_HI = 0x401000, 0x139D000
pid = find_pid()
ph = open_proc()

# The thread that owns the visible game window runs the message loop / frame loop.
P = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
found = {}
def cb(hw, _):
    p = W.DWORD()
    tid = u32.GetWindowThreadProcessId(hw, C.byref(p))
    if p.value == pid and u32.IsWindowVisible(hw):
        found["tid"], found["hwnd"] = tid, hw
        return False
    return True
u32.EnumWindows(P(cb), 0)
tid = found["tid"]
print(f"window thread {tid}")
th = k32.OpenThread(THREAD_ALL, False, tid)

def is_ret_site(a):
    if not (TEXT_LO <= a < TEXT_HI):
        return False
    b = read(ph, a - 6, 6)
    return bool(b) and (b[1] == 0xE8 or (b[4] == 0xFF and (b[5] & 0x38) == 0x10) or (b[0] == 0xFF and (b[1] & 0x38) == 0x10))

def focused():
    p = W.DWORD()
    u32.GetWindowThreadProcessId(u32.GetForegroundWindow(), C.byref(p))
    return p.value == pid

stats = {True: Counter(), False: Counter()}
eips = {True: Counter(), False: Counter()}
end = time.time() + float(sys.argv[1])
while time.time() < end:
    f = focused()
    if k32.SuspendThread(th) == 0xFFFFFFFF:
        break
    ctx = WOW64_CONTEXT(); ctx.ContextFlags = WOW64_CONTEXT_FULL
    ok = k32.Wow64GetThreadContext(th, C.byref(ctx))
    stack = read(ph, ctx.Esp, 0x800) if ok else None
    k32.ResumeThread(th)
    if not stack:
        continue
    vals = [v for (v,) in struct.iter_unpack("<I", stack[: len(stack) // 4 * 4])]
    chain = tuple(v for v in vals if is_ret_site(v))[:6]
    stats[f][chain] += 1
    eips[f][ctx.Eip >> 12] += 1
    time.sleep(0.01)

for f in (True, False):
    total = sum(stats[f].values())
    print(f"=== {'FOCUSED' if f else 'UNFOCUSED'} ({total} samples)")
    for chain, n in stats[f].most_common(8):
        print(f"  {n:4d}  " + " <- ".join(f"+{r - 0x400000:X}" for r in chain))
