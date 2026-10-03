"""Guarded single write into a DDDA object: only if the object's vtable matches.

Usage: py poke.py OBJ_HEX VTABLE_HEX OFF_HEX f|u VALUE
"""
import ctypes as C, struct, sys
from memscan import k32, find_pid, read

obj, vt, off = (int(x, 16) for x in sys.argv[1:4])
kind, val = sys.argv[4], sys.argv[5]
h = k32.OpenProcess(0x0010 | 0x0020 | 0x0008 | 0x0400, False, find_pid())
cur = struct.unpack("<I", read(h, obj, 4))[0]
if cur != vt:
    sys.exit(f"vtable mismatch: {cur:08X}")
data = struct.pack("<f", float(val)) if kind == "f" else struct.pack("<I", int(val, 16))
old = read(h, obj + off, 4)
n = C.c_size_t()
ok = k32.WriteProcessMemory(h, C.c_void_p(obj + off), data, 4, C.byref(n))
print(f"+{off:X}: {old.hex()} -> {data.hex()} ok={ok}")
