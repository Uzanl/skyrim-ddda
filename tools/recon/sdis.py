"""Static disassembly of DDDA.exe from disk (the game does not need to run).

Usage: py sdis.py ADDR [LEN]        ADDR absolute hex, or +RVA
       py sdis.py func ADDR         whole function containing ADDR
       py sdis.py calls ADDR        direct calls made by the function containing ADDR
       py sdis.py xref TARGET       every direct call/jmp to TARGET in .text
       py sdis.py vtref VALUE       every place in the image holding the dword VALUE
Import as a module for `img`, `rd`, `dis`, `func_bounds`, `xrefs`.
"""
import struct
import sys
import capstone

EXE = r"E:\SteamLibrary\steamapps\common\DDDA\DDDA.exe"
IB = 0x400000

raw = open(EXE, "rb").read()
pe = struct.unpack_from("<I", raw, 0x3C)[0]
nsec = struct.unpack_from("<H", raw, pe + 6)[0]
optsz = struct.unpack_from("<H", raw, pe + 20)[0]
imgsz = struct.unpack_from("<I", raw, pe + 24 + 56)[0]
img = bytearray(imgsz)
img[:0x1000] = raw[:0x1000]
SECTIONS = []
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    name = raw[o:o + 8].rstrip(b"\0").decode(errors="replace")
    vsz, va, rsz, rptr = struct.unpack_from("<IIII", raw, o + 8)
    img[va:va + min(vsz, rsz)] = raw[rptr:rptr + min(vsz, rsz)]
    SECTIONS.append((name, IB + va, IB + va + vsz))
TEXT = next(s for s in SECTIONS if s[0] == ".text")

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def rd(a, n):
    return bytes(img[a - IB:a - IB + n])


def u32(a):
    return struct.unpack("<I", rd(a, 4))[0]


def f32(a):
    return struct.unpack("<f", rd(a, 4))[0]


def func_bounds(a):
    start = a
    for p in range(a, a - 0x10000, -1):
        if img[p - 1 - IB] == 0xCC and img[p - IB] != 0xCC:
            start = p
            break
    end = a + rd(a, 0x20000).find(b"\xcc\xcc\xcc")
    return start, end


def dis(start, end):
    return list(md.disasm(rd(start, end - start), start))


def parse(s):
    return IB + int(s[1:], 16) if s.startswith("+") else int(s, 16)


def xrefs(target):
    """Direct E8/E9 rel32 references to target in .text."""
    t = bytes(img[TEXT[1] - IB:TEXT[2] - IB])
    out = []
    i = t.find(b"\xe8")
    for op in (0xE8, 0xE9):
        i = t.find(bytes([op]))
        while i >= 0:
            if i + 5 <= len(t):
                rel = struct.unpack_from("<i", t, i + 1)[0]
                if TEXT[1] + i + 5 + rel == target:
                    out.append((TEXT[1] + i, "call" if op == 0xE8 else "jmp"))
            i = t.find(bytes([op]), i + 1)
    return sorted(out)


def dwordrefs(value):
    pat = struct.pack("<I", value)
    out, i = [], img.find(pat)
    while i >= 0:
        out.append(IB + i)
        i = img.find(pat, i + 1)
    return out


def show(insns):
    for ins in insns:
        print(f"{ins.address:08X} +{ins.address - IB:06X}  {ins.mnemonic:6s} {ins.op_str}")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "func":
        s, e = func_bounds(parse(sys.argv[2]))
        print(f"; func {s:08X}-{e:08X}")
        show(dis(s, e))
    elif cmd == "calls":
        s, e = func_bounds(parse(sys.argv[2]))
        print(f"; func {s:08X}-{e:08X}")
        show([i for i in dis(s, e) if i.mnemonic == "call"])
    elif cmd == "xref":
        for a, k in xrefs(parse(sys.argv[2])):
            s, _ = func_bounds(a)
            print(f"{a:08X} {k} (in func {s:08X})")
    elif cmd == "vtref":
        for a in dwordrefs(parse(sys.argv[2])):
            print(f"{a:08X}")
    else:
        s = parse(cmd)
        show(dis(s, s + (int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x100)))
