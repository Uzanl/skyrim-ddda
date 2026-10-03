"""Disassemble DDDA.exe code from the running process.

Usage: py disasm.py ADDR_HEX [LEN_HEX]      (ADDR may be 'DDDA+RVA', e.g. +1DD00)
       py disasm.py func ADDR_HEX            (whole function containing ADDR)
"""
import sys
import capstone
from memscan import open_proc, read

MB = 0x400000
h = open_proc()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

def parse(s):
    return MB + int(s[1:], 16) if s.startswith("+") else int(s, 16)

def func_bounds(a):
    back = read(h, a - 0x8000, 0x8000)
    start = a
    for i in range(len(back) - 1, 1, -1):
        if back[i - 1] == 0xCC and back[i] != 0xCC:
            start = a - 0x8000 + i
            break
    fwd = read(h, a, 0x8000)
    end = a + fwd.find(b"\xcc\xcc\xcc")
    return start, end

if sys.argv[1] == "func":
    start, end = func_bounds(parse(sys.argv[2]))
else:
    start = parse(sys.argv[1])
    end = start + (int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x100)
code = read(h, start, end - start)
print(f"; {start:08X}-{end:08X} (DDDA.exe+{start - MB:X})")
for ins in md.disasm(code, start):
    print(f"+{ins.address - MB:06X}  {ins.mnemonic:6s} {ins.op_str}")
