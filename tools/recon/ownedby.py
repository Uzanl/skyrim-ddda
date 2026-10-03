"""List objects of the given classes whose +0x30 points at OWNER.

Usage: py ownedby.py OWNER_HEX REGEX
"""
import re, struct, sys, subprocess
from memscan import open_proc, read

owner = int(sys.argv[1], 16)
out = subprocess.check_output([sys.executable, "findclass.py", sys.argv[2]], text=True)
h = open_proc()
for line in out.splitlines():
    cls, rest = line.split(":", 1)
    for a in rest.split()[1:]:
        a = int(a, 16)
        d = read(h, a + 0x30, 4)
        if d and struct.unpack("<I", d)[0] == owner:
            f = struct.unpack("<f", read(h, a + 0x158, 4))[0]
            m = read(h, a + 0x110, 0x40).hex()
            print(f"{cls} {a:08X} tr={f:.2f} mask={m[:16]}...")
