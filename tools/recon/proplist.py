"""List the properties a MtDTI registration function registers: (name, offset).

Usage: py proplist.py START_HEX END_HEX
"""
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from memscan import open_proc, read

h = open_proc()
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
md = Cs(CS_ARCH_X86, CS_MODE_32)
code = read(h, lo, hi - lo)

def cstr(a):
    d = read(h, a, 64) or b""
    s = d.split(b"\0")[0]
    return s.decode("latin1") if s and all(32 <= c < 127 for c in s) else None

lea = None
for ins in md.disasm(code, lo):
    if ins.mnemonic == "lea" and "+" in ins.op_str and "esp" not in ins.op_str.split(",")[1] and "ebp" not in ins.op_str.split(",")[1]:
        lea = ins.op_str.split(",")[1].strip()
    if ins.mnemonic == "mov" and ins.op_str.startswith("dword ptr [esp + 0x10], 0x"):
        s = cstr(int(ins.op_str.split(",")[1], 16))
        if s:
            print(f"{ins.address:08X}  {s:32s} {lea}")
            lea = None
