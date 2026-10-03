"""Watch what happens when DDDA goes fullscreen: display mode of every monitor,
the game window's rect/style/minimized/foreground state, and whether the game
loop runs (bridge actor freshness). Prints a line whenever anything changes.

Usage: py fsmonitor.py SECONDS
"""
import ctypes as C, ctypes.wintypes as W, mmap, struct, sys, time
from memscan import find_pid

u32 = C.WinDLL("user32")
pid = find_pid()


class DEVMODEW(C.Structure):
    _fields_ = [("dmDeviceName", W.WCHAR * 32), ("dmSpecVersion", W.WORD), ("dmDriverVersion", W.WORD),
                ("dmSize", W.WORD), ("dmDriverExtra", W.WORD), ("dmFields", W.DWORD),
                ("dmPositionX", W.LONG), ("dmPositionY", W.LONG), ("dmDisplayOrientation", W.DWORD),
                ("dmDisplayFixedOutput", W.DWORD), ("dmColor", W.SHORT), ("dmDuplex", W.SHORT),
                ("dmYResolution", W.SHORT), ("dmTTOption", W.SHORT), ("dmCollate", W.SHORT),
                ("dmFormName", W.WCHAR * 32), ("dmLogPixels", W.WORD), ("dmBitsPerPel", W.DWORD),
                ("dmPelsWidth", W.DWORD), ("dmPelsHeight", W.DWORD), ("dmDisplayFlags", W.DWORD),
                ("dmDisplayFrequency", W.DWORD), ("rest", C.c_byte * 64)]


class DISPLAY_DEVICEW(C.Structure):
    _fields_ = [("cb", W.DWORD), ("DeviceName", W.WCHAR * 32), ("DeviceString", W.WCHAR * 128),
                ("StateFlags", W.DWORD), ("DeviceID", W.WCHAR * 128), ("DeviceKey", W.WCHAR * 128)]


def display_modes():
    out = []
    i = 0
    while True:
        dd = DISPLAY_DEVICEW(); dd.cb = C.sizeof(dd)
        if not u32.EnumDisplayDevicesW(None, i, C.byref(dd), 0):
            break
        i += 1
        if not dd.StateFlags & 1:  # attached to desktop
            continue
        dm = DEVMODEW(); dm.dmSize = C.sizeof(DEVMODEW) - 64
        if u32.EnumDisplaySettingsW(dd.DeviceName, -1, C.byref(dm)):
            out.append(f"{dd.DeviceName.split(chr(92))[-1]}({dd.DeviceString[:18]}) {dm.dmPelsWidth}x{dm.dmPelsHeight}@{dm.dmDisplayFrequency}")
    return " | ".join(out)


P = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
def game_window():
    found = []
    def cb(h, _):
        p = W.DWORD(); u32.GetWindowThreadProcessId(h, C.byref(p))
        if p.value == pid and u32.IsWindowVisible(h) and not u32.GetWindow(h, 4):
            found.append(h); return False
        return True
    u32.EnumWindows(P(cb), 0)
    return found[0] if found else None


def window_state(h):
    if not h:
        return "no visible window"
    r = W.RECT(); u32.GetWindowRect(h, C.byref(r))
    style = u32.GetWindowLongW(h, -16) & 0xFFFFFFFF
    fg = u32.GetForegroundWindow() == h
    return (f"rect=({r.left},{r.top})-({r.right},{r.bottom}) style={style:08X} "
            f"iconic={bool(u32.IsIconic(h))} fg={fg}")


def bridge_fresh():
    try:
        m = mmap.mmap(-1, 184, "Local\\DDDA_SkyrimBridge_v2", access=mmap.ACCESS_READ)
    except OSError:
        return "bridge?"
    ages = [struct.unpack_from("<I", m, 56 + r * 32 + 4)[0] for r in range(4)]
    m.close()
    return "game running" if min(ages) < 500 else "game NOT updating"


t0 = time.time()
end = t0 + float(sys.argv[1])
last = None
while time.time() < end:
    h = game_window()
    state = (display_modes(), window_state(h), bridge_fresh())
    if state != last:
        print(f"t={time.time() - t0:6.2f}  {state[0]}\n          {state[1]}  [{state[2]}]", flush=True)
        last = state
    time.sleep(0.05)
