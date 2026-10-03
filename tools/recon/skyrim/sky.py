"""Read-only helpers for a running SkyrimSE.exe (64-bit)."""
import ctypes as C, ctypes.wintypes as W, struct, subprocess, sys

k32 = C.WinDLL("kernel32", use_last_error=True)
psapi = C.WinDLL("psapi")

def find_pid(name="SkyrimSE.exe"):
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"], text=True)
    for line in out.splitlines():
        p = [x.strip('"') for x in line.split('","')]
        if p and p[0].lower() == name.lower():
            return int(p[1])
    sys.exit(f"{name} not running")

def open_proc():
    h = k32.OpenProcess(0x0400 | 0x0010, False, find_pid())
    if not h:
        sys.exit(f"OpenProcess failed {C.get_last_error()}")
    return h

def base(h):
    mods = (C.c_void_p * 1024)()
    need = W.DWORD()
    psapi.EnumProcessModulesEx(h, mods, C.sizeof(mods), C.byref(need), 3)
    return mods[0]

def read(h, addr, size):
    buf = (C.c_char * size)()
    n = C.c_size_t()
    if not k32.ReadProcessMemory(h, C.c_void_p(addr), buf, size, C.byref(n)):
        return None
    return buf.raw[:n.value]

def u64(h, a):
    d = read(h, a, 8)
    return struct.unpack("<Q", d)[0] if d else None

def floats(h, a, n):
    d = read(h, a, 4 * n)
    return struct.unpack(f"<{n}f", d) if d else None
