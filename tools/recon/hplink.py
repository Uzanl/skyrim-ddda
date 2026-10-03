"""Look for a link from the Arisen's position objects to the HP (status) object.

Usage: py hplink.py STATUS_OBJ_HEX
"""
import struct, sys
from memscan import open_proc, read

MB = 0x400000
POS = [(0x14D1578, 0xDF0), (0x14D09E0, 0xEC4), (0x14D08F4, 0x2B8)]
target = int(sys.argv[1], 16)
h = open_proc()

def u32(a):
    d = read(h, a, 4) if a else None
    return struct.unpack("<I", d)[0] if d else 0

for root, _ in POS:
    obj = u32(MB + root)
    data = read(h, obj, 0x4000) or b""
    words = struct.unpack(f"<{len(data)//4}I", data[: len(data) // 4 * 4])
    direct = [i * 4 for i, w in enumerate(words) if w == target]
    print(f"root +{root:X} obj {obj:08X}: direct ptr at offsets {[hex(o) for o in direct]}")
    # one hop: obj+X -> p, p+Y == target
    for i, w in enumerate(words):
        if w < 0x10000:
            continue
        sub = read(h, w, 0x2000)
        if not sub:
            continue
        sw = struct.unpack(f"<{len(sub)//4}I", sub[: len(sub) // 4 * 4])
        for j, v in enumerate(sw):
            if v == target:
                print(f"    [obj+{i*4:X}]+{j*4:X} -> target")

print("static 12579DC chain:", hex(u32(u32(MB + 0x12579DC) + 0x380)))
