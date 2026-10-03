"""Find memory that tracks DDDA's focus state.

Every second: record whether DDDA's window is the foreground window, and snapshot
DDDA.exe's writable image sections (.data/.bss). Afterwards, list bytes/dwords
that hold one value in every focused sample and a different one in every
unfocused sample.

Usage: py focusdiff.py SECONDS
"""
import ctypes as C, ctypes.wintypes as W, struct, sys, time
import numpy as np
from memscan import find_pid, open_proc, read

u32 = C.WinDLL("user32")
MB = 0x400000
h = open_proc()
pid = find_pid()

# Writable sections from the PE header.
hdr = read(h, MB, 0x1000)
pe = struct.unpack_from("<I", hdr, 0x3C)[0]
nsec = struct.unpack_from("<H", hdr, pe + 6)[0]
optsz = struct.unpack_from("<H", hdr, pe + 20)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    name = hdr[o:o + 8].rstrip(b"\0").decode()
    vsize, va, chars = struct.unpack_from("<I", hdr, o + 8)[0], struct.unpack_from("<I", hdr, o + 12)[0], struct.unpack_from("<I", hdr, o + 36)[0]
    if chars & 0x80000000:  # IMAGE_SCN_MEM_WRITE
        secs.append((name, MB + va, vsize))
print("writable sections:", [(n, hex(a), hex(s)) for n, a, s in secs])

def foreground_is_game():
    fg = u32.GetForegroundWindow()
    p = W.DWORD()
    u32.GetWindowThreadProcessId(fg, C.byref(p))
    return p.value == pid

samples = []
end = time.time() + float(sys.argv[1])
while time.time() < end:
    focused = foreground_is_game()
    blob = b"".join((read(h, a, s) or b"\0" * s) for _, a, s in secs)
    samples.append((focused, np.frombuffer(blob, dtype=np.uint8).copy()))
    print(f"t={len(samples):3d} focused={focused}", flush=True)
    time.sleep(1.0)

F = [s for f, s in samples if f]
U = [s for f, s in samples if not f]
print(f"{len(F)} focused, {len(U)} unfocused samples")
if not F or not U:
    sys.exit("need both focused and unfocused samples")
Fa, Ua = np.stack(F), np.stack(U)
const_f = (Fa == Fa[0]).all(axis=0)
const_u = (Ua == Ua[0]).all(axis=0)
diff = const_f & const_u & (Fa[0] != Ua[0])
idx = np.nonzero(diff)[0]

def addr_of(i):
    off = 0
    for n, a, s in secs:
        if i < off + s:
            return a + (i - off), n
        off += s

print(f"{len(idx)} bytes track focus")
for i in idx[:200]:
    a, n = addr_of(int(i))
    print(f"  {a:08X} (DDDA.exe+{a - MB:X}, {n})  focused={Fa[0][i]:02X} unfocused={Ua[0][i]:02X}")
