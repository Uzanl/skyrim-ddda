"""Find MtDTI property registrations for property-name strings: for every code
reference to the string, print the `lea reg, [reg + OFF]` that precedes it
(the field offset) and the function's class name string if nearby.

Usage: py propoff.py NAME...
"""
import re, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from memscan import open_proc, read

MB = 0x400000
h = open_proc()
img = read(h, MB, 0x160C000)
TEXT_LO, TEXT_HI = 0x1000, 0x139D000 - MB
md = Cs(CS_ARCH_X86, CS_MODE_32)
for name in sys.argv[1:]:
    for m in re.finditer(re.escape(name.encode()) + b"\x00", img):
        if m.start() > 0 and img[m.start() - 1] != 0:
            continue
        sa = MB + m.start()
        print(f"== {name} @ {sa:08X}")
        pat = struct.pack("<I", sa)
        for r in re.finditer(re.escape(pat), img[TEXT_LO:TEXT_HI]):
            at = TEXT_LO + r.start()
            code = img[at - 0x30: at + 8]
            lea = None
            for ins in md.disasm(code, MB + at - 0x30):
                if ins.mnemonic == "lea" and "+" in ins.op_str:
                    lea = ins.op_str
            print(f"  ref {MB + at:08X}  {lea}")
