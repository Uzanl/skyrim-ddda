"""Class name of MT Framework objects by vtable: finds the getDTI slot
(`mov eax, imm32; ret`) and reads the MtDTI name.

Usage: py dtiname.py VT_HEX...
"""
import struct, sys
from memscan import open_proc, read

h = open_proc()

def name(vt):
    for k in range(12):
        fn = struct.unpack("<I", read(h, vt + 4 * k, 4))[0]
        code = read(h, fn, 6) or b""
        if len(code) == 6 and code[0] == 0xB8 and code[5] == 0xC3:
            dti = struct.unpack_from("<I", code, 1)[0]
            np_ = struct.unpack("<I", read(h, dti + 4, 4) or b"\0\0\0\0")[0]
            s = (read(h, np_, 64) or b"").split(b"\0")[0]
            if s and all(32 <= c < 127 for c in s):
                return k, s.decode()
    return None

if __name__ == "__main__":
    for a in sys.argv[1:]:
        print(a, name(int(a, 16)))
