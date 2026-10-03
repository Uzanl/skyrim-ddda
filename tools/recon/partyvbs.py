"""Find the vertex buffers the party pawns are drawn with: trace a frame with the
pawns visible, hide them (parts masks of the pawns and the models they carry),
trace again, restore the masks, and diff the vertex buffers used per pass.

Needs the bridge with frame_trace (ddda_trace_request) and DDDA in the world.
Usage: py partyvbs.py
"""
import ctypes as C, os, re, shutil, struct, time
from collections import Counter
from memscan import k32, find_pid, read
import subprocess, sys

GAME = r"E:\SteamLibrary\steamapps\common\DDDA"
TRACE = os.path.join(GAME, "ddda_frame_trace.txt")
PAWN_VT = 0x15E8468
MASK_OFF, MASK_LEN = 0x110, 0x40

h = k32.OpenProcess(0x0010 | 0x0020 | 0x0008 | 0x0400, False, find_pid())


def u32(a):
    d = read(h, a, 4)
    return struct.unpack("<I", d)[0] if d else None


def write(a, data):
    n = C.c_size_t()
    if not k32.WriteProcessMemory(h, C.c_void_p(a), data, len(data), C.byref(n)):
        raise SystemExit(f"write failed at {a:08X}")


def find_objects():
    out = subprocess.check_output([sys.executable, "findclass.py", "^(uCmc|uWeaponPl|uOmObj10410)$"], text=True)
    objs = {}
    for line in out.splitlines():
        cls, rest = line.split(":", 1)
        objs[cls] = [int(a, 16) for a in rest.split()[1:]]
    pawns = objs.get("uCmc", [])
    carried = [o for k in ("uWeaponPl", "uOmObj10410") for o in objs.get(k, []) if u32(o + 0x30) in pawns]
    return pawns, carried


def trace(tag):
    before = os.path.getmtime(TRACE) if os.path.exists(TRACE) else 0
    open(os.path.join(GAME, "ddda_trace_request"), "w").close()
    for _ in range(100):
        time.sleep(0.05)
        if os.path.exists(TRACE) and os.path.getmtime(TRACE) > before:
            time.sleep(0.2)
            dst = os.path.join("_data", f"trace_{tag}.txt")
            shutil.copy(TRACE, dst)
            return dst
    raise SystemExit("trace did not arrive (is the Skyrim window focused / DDDA rendering?)")


def vbs(path):
    used = Counter()
    for line in open(path, encoding="latin1"):
        m = re.search(r"draw \d+ .* rt=(\S+),\S+ ds=(\S+) .* vb=(B\d+)", line)
        if m:
            used[m.group(3)] += 1
    return used


pawns, carried = find_objects()
print("pawns", [f"{p:08X}" for p in pawns], "carried", [f"{c:08X}" for c in carried])
targets = pawns + carried
saved = {o: read(h, o + MASK_OFF, MASK_LEN) for o in targets}
a = trace("visible")
try:
    for o in targets:
        write(o + MASK_OFF, b"\0" * MASK_LEN)
    time.sleep(0.15)
    b = trace("hidden")
finally:
    for o, m in saved.items():
        if read(h, o + MASK_OFF, MASK_LEN) == b"\0" * MASK_LEN:
            write(o + MASK_OFF, m)
print("masks restored")
va, vb = vbs(a), vbs(b)
party = sorted(set(va) - set(vb), key=lambda k: int(k[1:]))
print(f"draws: visible {sum(va.values())}, hidden {sum(vb.values())}")
print("vertex buffers only drawn while the party is visible:")
res = {}
for line in open(a, encoding="latin1"):
    m = re.match(r"(B\d+) = \S+ (.*)", line)
    if m:
        res[m.group(1)] = m.group(2)
for k in party:
    print(f"  {k} x{va[k]}  {res.get(k, '')}")
