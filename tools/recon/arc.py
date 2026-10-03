"""MT Framework .arc reader (DDDA, version 7).

Usage: py arc.py FILE.arc               list entries
       py arc.py FILE.arc OUTDIR [SUB]  extract entries (name contains SUB)
Entry: name[64], type hash u32, compressed size u32, size|flags u32, offset u32.
Data is zlib-compressed. Files are written as NAME.<typehash hex>.
"""
import os
import struct
import sys
import zlib


def entries(path):
    d = open(path, "rb").read()
    magic, ver, n = struct.unpack_from("<4sHH", d, 0)
    assert magic == b"ARC\0", magic
    out = []
    for i in range(n):
        o = 8 + i * 80
        name = d[o:o + 64].split(b"\0")[0].decode("latin1")
        th, csz, sz, off = struct.unpack_from("<4I", d, o + 64)
        out.append((name, th, csz, sz & 0x00FFFFFF, off, d))
    return out


def data(e):
    name, th, csz, sz, off, d = e
    raw = d[off:off + csz]
    try:
        return zlib.decompress(raw)
    except zlib.error:
        return raw


if __name__ == "__main__":
    es = entries(sys.argv[1])
    if len(sys.argv) == 2:
        for name, th, csz, sz, off, _ in es:
            print(f"{th:08X} {sz:9d} {name}")
    else:
        sub = sys.argv[3] if len(sys.argv) > 3 else ""
        for e in es:
            if sub in e[0]:
                p = os.path.join(sys.argv[2], e[0].replace("\\", "/") + f".{e[1]:08X}")
                os.makedirs(os.path.dirname(p), exist_ok=True)
                open(p, "wb").write(data(e))
                print(p)


def rebuild(path, replace):
    """Return a new .arc with entries replaced: replace = {name: raw bytes}.
    Keeps order, type hashes and size flags; data starts at 0x8000 and is contiguous,
    like the game's own archives."""
    d = open(path, "rb").read()
    magic, ver, n = struct.unpack_from("<4sHH", d, 0)
    hdr = bytearray(d[:8 + n * 80])
    blobs = []
    for i in range(n):
        o = 8 + i * 80
        name = d[o:o + 64].split(b"\0")[0].decode("latin1")
        th, csz, sz, off = struct.unpack_from("<4I", d, o + 64)
        if name in replace:
            raw = replace[name]
            blob = zlib.compress(raw, 9)
            sz = (sz & 0xFF000000) | len(raw)
        else:
            blob = d[off:off + csz]
        blobs.append((o, th, blob, sz))
    out_off = 0x8000
    for o, th, blob, sz in blobs:
        struct.pack_into("<4I", hdr, o + 64, th, len(blob), sz, out_off)
        out_off += len(blob)
    out = bytearray(hdr) + bytes(0x8000 - len(hdr))
    for _, _, blob, _ in blobs:
        out += blob
    return bytes(out)
