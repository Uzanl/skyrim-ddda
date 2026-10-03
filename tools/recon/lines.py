"""Print the flags of every sUnit move line ([DDDA.exe+14D09E0] + 0x28 + i*0x18).
Bit 1 set = line is paused (its units' move() is skipped).

Usage: py lines.py [label]
"""
import ctypes as C, ctypes.wintypes as W, struct, sys
from memscan import find_pid, open_proc, read

h = open_proc()
u32 = C.WinDLL("user32")
p = W.DWORD()
u32.GetWindowThreadProcessId(u32.GetForegroundWindow(), C.byref(p))
unit = struct.unpack("<I", read(h, 0x18D09E0, 4))[0]
n = struct.unpack("<I", read(h, unit + 0x620, 4))[0]
d = read(h, unit + 0x28, n * 0x18)
flags = [struct.unpack_from("<I", d, i * 0x18)[0] for i in range(n)]
print(f"{sys.argv[1] if len(sys.argv) > 1 else ''} focused={p.value == find_pid()} sUnit={unit:08X} lines={n}")
print("  paused:", [i for i, f in enumerate(flags) if f & 2])
print("  flags :", " ".join(f"{f:X}" for f in flags))
