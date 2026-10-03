"""Print the frame transport header (frame_shared.h): reader heartbeat, frames.

Usage: py framestat.py
"""
import ctypes as C, struct

k = C.WinDLL("kernel32", use_last_error=True)
k.OpenFileMappingW.restype = C.c_void_p
k.MapViewOfFile.restype = C.c_void_p
k.MapViewOfFile.argtypes = [C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32, C.c_size_t]
h = k.OpenFileMappingW(4, False, r"Local\DDDA_SkyrimBridge_frame_v2")
if not h:
    raise SystemExit("frame mapping not found")
v = k.MapViewOfFile(h, 4, 0, 0, 4096)
magic, ver, pid, fmt, latest, tick, frames = struct.unpack("<6IQ", C.string_at(v, 32))
now = k.GetTickCount()
print(f"magic={magic:08X} v{ver} writer={pid} latest={latest} frames={frames} "
      f"readerTick age={(now - tick) & 0xFFFFFFFF} ms")
for i in range(3):
    seq, w, hh, pitch, fid, qpc, flags = struct.unpack("<4I2QI", C.string_at(v + 32 + 48 * i, 36))
    print(f"  slot{i}: seq={seq} {w}x{hh} pitch={pitch} frame={fid} flags={flags}")
