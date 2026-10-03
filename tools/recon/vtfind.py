"""Find the start of the function containing a code address (MSVC pads functions
with 0xCC), then look for it in the given vtables.

Usage: py vtfind.py CODE_HEX VTABLE_HEX [VTABLE_HEX ...]
"""
import struct, sys
from memscan import open_proc, read

h = open_proc()
code = int(sys.argv[1], 16)
vts = [int(x, 16) for x in sys.argv[2:]]

back = read(h, code - 0x4000, 0x4000)
start = None
for i in range(len(back) - 1, 1, -1):
    if back[i - 1] == 0xCC and back[i] != 0xCC:
        start = code - 0x4000 + i
        break
print(f"code {code:08X}: function likely starts at {start:08X} (DDDA.exe+{start - 0x400000:X})")
print("   first bytes:", read(h, start, 16).hex(" "))

for vt in vts:
    d = read(h, vt, 0x400)
    entries = struct.unpack(f"<{len(d)//4}I", d)
    for i, e in enumerate(entries):
        if not (0x401000 <= e < 0x1400000):
            print(f"vtable {vt:08X}: {i} entries")
            break
        if e == start:
            print(f"vtable {vt:08X}: slot {i} (+{i*4:X}) == {start:08X}  <-- match")
