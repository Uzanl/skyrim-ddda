"""The AI's target list: sAISensorTarget ([0x18D9274], read-only).

Layout from the constructor (+0x15120) and the query (+0x15460), 2026-10-10:
  +0x20 pending array (count +0x24, items +0x30), moved into the live list each update
  +0x34 live array    (count +0x38, items +0x44), sorted by group
  +0x48 13 dwords: first index of each group (-1 = empty)
  entry: +0x04 flags (bit 0 skipped, bit 1 active), +0x0C mask tested by the query,
         +0x44 group (0..12)
Prints every live entry: class, flags, mask, group and the MT objects it points to in its
first SCAN bytes (one level deeper for non-unit objects), to find its owner character.

Usage: py sensortargets.py [SCAN_HEX]
"""
import struct
import sys
from memscan import open_proc, read

h = open_proc()
SCAN = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x80
U = lambda a: struct.unpack("<I", read(h, a, 4) or b"\0" * 4)[0]
F = lambda a: struct.unpack("<f", read(h, a, 4) or b"\0" * 4)[0]
_names = {}


def cls(obj):
    """MT class name of obj (vtable slot 4 is `mov eax, DTI; ret`), or None."""
    if obj < 0x10000:
        return None
    vt = U(obj)
    if vt in _names:
        return _names[vt]
    n = None
    fn = U(vt + 16) if vt > 0x400000 else 0
    code = (read(h, fn, 6) or b"") if fn else b""
    if len(code) == 6 and code[0] == 0xB8 and code[5] == 0xC3:
        dti = struct.unpack_from("<I", code, 1)[0]
        s = (read(h, U(dti + 4), 64) or b"").split(b"\0")[0]
        if s and all(32 <= c < 127 for c in s):
            n = s.decode()
    _names[vt] = n
    return n


def refs(obj, size):
    out = []
    for off in range(0, size, 4):
        v = U(obj + off)
        if 0x01000000 <= v < 0x7FFF0000 and not (0x400000 <= v < 0x1C00000):
            c = cls(v)
            if c:
                out.append((off, v, c))
    return out


mgr = U(0x18D9274)
print(f"sAISensorTarget {mgr:08X} class {cls(mgr)}")
if not mgr:
    sys.exit()
pend, live, items = U(mgr + 0x24), U(mgr + 0x38), U(mgr + 0x44)
starts = [struct.unpack("<i", read(h, mgr + 0x48 + 4 * g, 4))[0] for g in range(13)]
print(f"pending {pend}, live {live}, group starts {starts}")
for i in range(min(live, 400)):
    e = U(items + 4 * i)
    flags, mask, group = U(e + 4), U(e + 0xC), U(e + 0x44)
    line = f"[{i:3}] {e:08X} {cls(e) or '?':28} flags {flags:08X} mask {mask:08X} group {group:2}"
    parts = []
    for off, v, c in refs(e, SCAN):
        if c.startswith("u"):
            pos = struct.unpack("<3f", read(h, v + 0x40, 12) or b"\0" * 12)
            parts.append(f"+{off:X}={c}:{v:08X}@({pos[0]:.0f},{pos[1]:.0f},{pos[2]:.0f})")
        else:
            deeper = [f"{c2}" for o2, v2, c2 in refs(v, 0x40) if c2.startswith("u")]
            parts.append(f"+{off:X}={c}" + (f"->{'/'.join(deeper)}" if deeper else ""))
    print(line, " ".join(parts))
