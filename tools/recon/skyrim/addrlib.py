"""Address Library reader for the AE "format 5" versionlib-*.bin: ID -> RVA.

Layout (found 2026-10-01): int32 format (5), int32 version[4], char name[64],
int32 pointer size, int32 0, int32 count, then uint32 rva[count] indexed by ID
starting at 0x60 (0 = no entry).

Usage: py addrlib.py ID...
"""
import struct, sys

BIN = r"E:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data\SKSE\Plugins\versionlib-1-7-104-0.bin"

def load(path=BIN):
    d = open(path, "rb").read()
    fmt = struct.unpack_from("<i", d, 0)[0]
    assert fmt == 5, f"unsupported format {fmt}"
    ver = struct.unpack_from("<4i", d, 4)
    count = struct.unpack_from("<I", d, 0x5C)[0]
    rvas = struct.unpack_from(f"<{count}I", d, 0x60)
    return ver, rvas

def rva(i, _cache=[]):
    if not _cache:
        _cache.append(load()[1])
    return _cache[0][i]

if __name__ == "__main__":
    ver, rvas = load()
    print("version", ver, len(rvas), "ids")
    for a in sys.argv[1:]:
        print(a, hex(rvas[int(a)]))
