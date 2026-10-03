"""Every property name a MtDTI registration function registers, with the field
offset (the last `lea reg, [reg + OFF]` before the name), plus the class whose
vtable holds that function.

Usage: py regprops.py CODE_HEX   (any address inside the registration function)
"""
import re, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from memscan import open_proc, read
from dtiname import name

MB = 0x400000
h = open_proc()
code = int(sys.argv[1], 16)
img = read(h, MB, 0x160C000)
start = code
while start > code - 0x4000 and not (img[start - MB - 1] == 0xCC and img[start - MB] != 0xCC):
    start -= 1
end = code
while end < code + 0x8000 and not (img[end - MB] == 0xCC and img[end - MB + 1] == 0xCC):
    end += 1
cls = None
for m in re.finditer(re.escape(struct.pack("<I", start)), img):
    for k in range(0, 40):
        vt = MB + m.start() - 4 * k
        n = name(vt)
        if n and n[0] == 4:
            cls = f"{n[1]} (vtable {vt:08X}, slot {k})"
            break
print(f"function {start:08X}-{end:08X}  class {cls}")

def cstr(a):
    d = read(h, a, 48) or b""
    s = d.split(b"\0")[0]
    return s.decode() if s and all(32 <= c < 127 for c in s) and len(s) > 2 else None

md = Cs(CS_ARCH_X86, CS_MODE_32)
lea = None
for ins in md.disasm(read(h, start, end - start), start):
    if ins.mnemonic == "lea" and "+" in ins.op_str:
        r = ins.op_str.split(",")[1].strip()
        if "esp" not in r and "ebp" not in r:
            lea = r
    if ins.mnemonic in ("mov", "push"):
        m = re.search(r"0x([0-9a-f]{6,8})$", ins.op_str)
        if m:
            s = cstr(int(m.group(1), 16))
            if s and (s[0] == "m" or s[0].isupper()) and " " not in s:
                print(f"  {s:32s} {lea}")
                lea = None
