"""Static MtDTI property list of a class: walks vtable slot 3 (the property registration) and
prints each property-name string with the field offset loaded just before it.

Usage: py sprops.py VTABLE_HEX
"""
import re
import sys
from sdis import IB, rd, u32, func_bounds, dis, SECTIONS

RDATA = next(s for s in SECTIONS if s[0] == ".rdata")


def cstr(a):
    if not (RDATA[1] <= a < RDATA[2]):
        return None
    s = rd(a, 64).split(b"\0")[0]
    return s.decode("latin1") if len(s) >= 2 and all(32 <= c < 127 for c in s) else None


def props(func):
    s, e = func_bounds(func)
    lea = None
    for ins in dis(s, e):
        m = re.search(r"lea \w+, \[(\w+) \+ (0x[0-9a-f]+)\]", f"{ins.mnemonic} {ins.op_str}")
        if m and m.group(1) not in ("esp", "ebp"):
            lea = int(m.group(2), 16)
        for imm in re.findall(r"0x[0-9a-f]{6,8}", ins.op_str):
            n = cstr(int(imm, 16))
            if n:
                yield ins.address, n, lea
                lea = None


if __name__ == "__main__":
    vt = int(sys.argv[1], 16)
    for a, n, off in props(u32(vt + 12)):
        print(f"{a:08X}  {n:36s} {'' if off is None else hex(off)}")
