"""Static MtDTI lookup in DDDA.exe: class name -> DTI object -> GetDTI functions -> vtables.

Usage: py sdti.py REGEX
DTI objects are constructed by static initialisers that push the name string and
put the DTI address in ECX before calling the MtDTI constructor. A class's vtable
slot 4 is a tiny `mov eax, DTI; ret`, so vtables are found through that function.
"""
import re
import struct
import sys
from sdis import img, IB, TEXT, rd, u32, md, dwordrefs


def strings(rx):
    out = []
    for m in re.finditer(rb"[\x20-\x7e]{3,}\x00", bytes(img)):
        s = m.group()[:-1].decode()
        if re.fullmatch(rx, s):
            out.append((IB + m.start(), s))
    return out


def dti_for_name(name_addr):
    """DTI addresses whose constructor call pushes name_addr."""
    res = set()
    for ref in dwordrefs(name_addr):
        if not (TEXT[1] <= ref < TEXT[2]) or img[ref - 1 - IB] != 0x68:  # push imm32
            continue
        code = list(md.disasm(rd(ref - 1, 0x30), ref - 1))
        for ins in code[1:8]:
            m = re.fullmatch(r"ecx, (0x[0-9a-f]+)", ins.op_str)
            if ins.mnemonic == "mov" and m:
                res.add(int(m.group(1), 16))
                break
    return sorted(res)


def getdti_funcs(dti):
    pat = b"\xb8" + struct.pack("<I", dti) + b"\xc3"
    out, i = [], img.find(pat)
    while i >= 0:
        out.append(IB + i)
        i = img.find(pat, i + 1)
    return out


def vtables(dti):
    out = []
    for f in getdti_funcs(dti):
        for r in dwordrefs(f):
            if not (TEXT[1] <= r < TEXT[2]):
                out.append(r - 16)  # slot 4
    return out


if __name__ == "__main__":
    for a, s in strings(sys.argv[1]):
        for d in dti_for_name(a):
            vts = vtables(d)
            print(f"{s:45s} name {a:08X} DTI {d:08X} vt {' '.join(f'{v:08X}' for v in vts)}")
