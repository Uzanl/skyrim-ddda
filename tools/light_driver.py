"""Stand-in for the SKSE plugin's lighting sender: publishes a LightCmd (bridge_shared.h)
so the DDDA bridge's relighting can be tested with DDDA alone (plus terrain_sim.py for
the camera link).

Usage: py light_driver.py [day|sunset|night|fog|interior|cycle] [SECONDS]
  cycle: walks through the presets, 10 s each (watch the pawns change).
Do not run it while Skyrim is running (both would write the same mapping).
"""
import ctypes as C
import math
import struct
import sys
import time

NAME = r"Local\DDDA_SkyrimBridge_light_v1"
MAGIC, VERSION = 0x4C424444, 1
SIZE = 112
VALID, INTERIOR = 1, 2


def sun(elev_deg, azim_deg=135.0):
    e, a = math.radians(elev_deg), math.radians(azim_deg)
    # direction the light travels: from the sun towards the ground
    return (-math.cos(e) * math.sin(a), -math.cos(e) * math.cos(a), -math.sin(e))


PRESETS = {
    #           hour  flags     sun dir      sun colour          ambient up          ambient down        fog colour         near    far
    "day":     (12.0, VALID, sun(60), (1.00, 0.95, 0.85), (0.40, 0.45, 0.55), (0.25, 0.22, 0.18), (0.70, 0.75, 0.80), 2000.0, 150000.0),
    "sunset":  (19.0, VALID, sun(8, 250), (1.00, 0.55, 0.30), (0.30, 0.25, 0.30), (0.15, 0.10, 0.08), (0.80, 0.50, 0.35), 1000.0, 80000.0),
    "night":   (1.0, VALID, sun(40, 60), (0.15, 0.20, 0.35), (0.06, 0.08, 0.14), (0.03, 0.03, 0.05), (0.05, 0.07, 0.12), 500.0, 40000.0),
    "fog":     (10.0, VALID, sun(50), (0.50, 0.50, 0.50), (0.45, 0.45, 0.45), (0.30, 0.30, 0.30), (0.60, 0.62, 0.65), 0.0, 4000.0),
    "interior": (12.0, VALID | INTERIOR, (0.2, 0.1, -0.97), (0.45, 0.35, 0.25), (0.20, 0.16, 0.12), (0.20, 0.16, 0.12), (0.10, 0.08, 0.06), 300.0, 6000.0),
}


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "cycle"
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 600.0
    k = C.WinDLL("kernel32", use_last_error=True)
    k.CreateFileMappingW.restype = C.c_void_p
    k.CreateFileMappingW.argtypes = [C.c_void_p, C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32, C.c_wchar_p]
    k.MapViewOfFile.restype = C.c_void_p
    k.MapViewOfFile.argtypes = [C.c_void_p, C.c_uint32, C.c_uint32, C.c_uint32, C.c_size_t]
    h = k.CreateFileMappingW(C.c_void_p(-1), None, 0x04, 0, SIZE, NAME)
    view = k.MapViewOfFile(h, 0xF001F, 0, 0, SIZE)
    if not view:
        sys.exit(f"mapping failed: {C.get_last_error()}")
    buf = (C.c_char * SIZE).from_address(view)
    struct.pack_into("<II", buf, 0, MAGIC, VERSION)
    names = list(PRESETS) if mode == "cycle" else [mode]
    t0, updates, last = time.time(), 0, None
    while time.time() - t0 < seconds:
        name = names[int((time.time() - t0) // 10) % len(names)]
        if name != last:
            print(f"{time.strftime('%H:%M:%S')} {name}", flush=True)
            last = name
        hour, flags, d, sc, au, ad, fc, fn, ff = PRESETS[name]
        seq, = struct.unpack_from("<I", buf, 8)
        struct.pack_into("<I", buf, 8, seq + 1)
        updates += 1
        struct.pack_into("<IQf3f3f3f3f3fff", buf, 12, flags, updates, hour, *d, *sc, *au, *ad, *fc, fn, ff)
        struct.pack_into("<I", buf, 8, seq + 2)
        time.sleep(0.1)


if __name__ == "__main__":
    main()
