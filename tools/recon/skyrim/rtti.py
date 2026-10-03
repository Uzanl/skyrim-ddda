"""MSVC RTTI helpers for live Skyrim (x64), plus a finder for static pointers to
objects of a class.

Usage:
  py rtti.py name ADDR_HEX           class name of the object at ADDR
  py rtti.py statics CLASSNAME       static pointers in SkyrimSE.exe to objects of that class
"""
import struct, sys
from sky import open_proc, base, read, u64

h = open_proc()
B = base(h)


def class_name(obj, _cache={}):
    vt = u64(h, obj)
    if not vt or not (B <= vt < B + 0x8000000):
        return None
    if vt in _cache:
        return _cache[vt]
    name = None
    col = u64(h, vt - 8)
    if col:
        d = read(h, col, 0x18)
        if d and struct.unpack_from("<I", d, 0)[0] == 1:  # x64 COL signature
            td = B + struct.unpack_from("<I", d, 0x0C)[0]
            raw = read(h, td + 0x10, 128) or b""
            name = raw.split(b"\0")[0].decode("latin1") or None
    _cache[vt] = name
    return name


def statics(want):
    # Scan the image for 8-byte aligned values that point to heap objects of `want`.
    import ctypes as C, ctypes.wintypes as W
    psapi = C.WinDLL("psapi")

    class MI(C.Structure):
        _fields_ = [("base", C.c_void_p), ("size", W.DWORD), ("ep", C.c_void_p)]

    mi = MI()
    psapi.GetModuleInformation(h, C.c_void_p(B), C.byref(mi), C.sizeof(mi))
    for off in range(0, mi.size, 0x10000):
        d = read(h, B + off, 0x10000)
        if not d:
            continue
        for i in range(0, len(d) - 7, 8):
            p = struct.unpack_from("<Q", d, i)[0]
            if p < 0x10000 or (B <= p < B + mi.size) or p > 0x7FFFFFFFFFFF:
                continue
            n = class_name(p)
            if n and want in n:
                print(f"RVA {off + i:08X} -> {p:016X} {n}")


if __name__ == "__main__":
    if sys.argv[1] == "name":
        print(class_name(int(sys.argv[2], 16)))
    else:
        statics(sys.argv[2])
