"""Statistics of the G-buffer (mask plane) pixels that belong to the pawns, from
the latest frame in the frame mapping: per-channel ranges and a few samples.

Usage: py gbufstat.py [OUT.png]
"""
import ctypes as C, struct, sys
import numpy as np

k = C.WinDLL("kernel32", use_last_error=True)
k.OpenFileMappingW.restype = C.c_void_p
k.MapViewOfFile.restype = C.c_void_p
k.MapViewOfFile.argtypes = [C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32, C.c_size_t]
h = k.OpenFileMappingW(4, False, r"Local\DDDA_SkyrimBridge_frame_v2")
if not h:
    raise SystemExit("frame mapping not found")
HDR, PLANE = 4096, 1920 * 1080 * 4
v = k.MapViewOfFile(h, 4, 0, 0, HDR + 3 * 2 * PLANE)
latest = struct.unpack("<I", C.string_at(v + 16, 4))[0]
seq, w, hh, pitch, fid, qpc, flags = struct.unpack("<4I2QI", C.string_at(v + 32 + 48 * latest, 36))
base = v + HDR + latest * 2 * PLANE
color = np.frombuffer(C.string_at(base, pitch * hh), np.uint8).reshape(hh, pitch // 4, 4)[:, :w]
mask = np.frombuffer(C.string_at(base + PLANE, pitch * hh), np.uint8).reshape(hh, pitch // 4, 4)[:, :w]
pawn = ~((mask[..., 0] == 255) & (mask[..., 1] == 255) & (mask[..., 2] == 255))
print(f"frame {fid} {w}x{hh} flags={flags} pawn pixels={pawn.sum()}")
if pawn.sum():
    px = mask[pawn]
    for c, n in enumerate("BGRA"):
        print(f"  {n}: min {px[:, c].min()} max {px[:, c].max()} mean {px[:, c].mean():.1f}")
    ys, xs = np.nonzero(pawn)
    for i in np.linspace(0, len(ys) - 1, 8).astype(int):
        print(f"  ({xs[i]},{ys[i]}) BGRA={tuple(mask[ys[i], xs[i]])}")
if len(sys.argv) > 1:
    import zlib
    def png(path, img):
        hgt, wid = img.shape[:2]
        raw = b"".join(b"\0" + img[y, :, [2, 1, 0]].T.tobytes() for y in range(hgt))
        def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
        open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", wid, hgt, 8, 2, 0, 0, 0))
                                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    png(sys.argv[1], np.ascontiguousarray(mask))
    a = np.zeros_like(mask); a[..., 0] = a[..., 1] = a[..., 2] = mask[..., 3]
    png(sys.argv[1].replace(".png", "_alpha.png"), a)
    print("wrote", sys.argv[1])
