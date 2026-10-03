"""Hardware write-watchpoints on up to 4 addresses in DDDA.exe (32-bit, WOW64).

Attaches as a debugger, sets DR0-DR3 on every thread to break on 4-byte writes,
logs the writing instruction (EIP) and registers, then clears the debug registers
and detaches. The game keeps running after detach (kill-on-exit is disabled).

Usage: py hwbp.py SECONDS ADDR_HEX[=label] [ADDR_HEX[=label] ...]   (max 4)
"""
import ctypes as C, ctypes.wintypes as W, sys, time
from collections import defaultdict
from memscan import find_pid

k32 = C.WinDLL("kernel32", use_last_error=True)

DEBUG_EVENT_SIZE = 0xB0
EXCEPTION_DEBUG_EVENT, CREATE_THREAD_DEBUG_EVENT, CREATE_PROCESS_DEBUG_EVENT = 1, 2, 3
EXIT_THREAD_DEBUG_EVENT, EXIT_PROCESS_DEBUG_EVENT, LOAD_DLL_DEBUG_EVENT = 4, 5, 6
DBG_CONTINUE, DBG_EXCEPTION_NOT_HANDLED = 0x00010002, 0x80010001
STATUS_SINGLE_STEP, STATUS_BREAKPOINT = 0x80000004, 0x80000003
STATUS_WX86_SINGLE_STEP, STATUS_WX86_BREAKPOINT = 0x4000001E, 0x4000001F
THREAD_ALL = 0x001F03FF
WOW64_CONTEXT_DEBUG_REGISTERS = 0x00010010
WOW64_CONTEXT_FULL = 0x00010007


class WOW64_FLOATING_SAVE_AREA(C.Structure):
    _fields_ = [("ControlWord", W.DWORD), ("StatusWord", W.DWORD), ("TagWord", W.DWORD),
                ("ErrorOffset", W.DWORD), ("ErrorSelector", W.DWORD), ("DataOffset", W.DWORD),
                ("DataSelector", W.DWORD), ("RegisterArea", C.c_ubyte * 80), ("Cr0NpxState", W.DWORD)]


class WOW64_CONTEXT(C.Structure):
    _fields_ = [("ContextFlags", W.DWORD),
                ("Dr0", W.DWORD), ("Dr1", W.DWORD), ("Dr2", W.DWORD), ("Dr3", W.DWORD),
                ("Dr6", W.DWORD), ("Dr7", W.DWORD),
                ("FloatSave", WOW64_FLOATING_SAVE_AREA),
                ("SegGs", W.DWORD), ("SegFs", W.DWORD), ("SegEs", W.DWORD), ("SegDs", W.DWORD),
                ("Edi", W.DWORD), ("Esi", W.DWORD), ("Ebx", W.DWORD), ("Edx", W.DWORD),
                ("Ecx", W.DWORD), ("Eax", W.DWORD), ("Ebp", W.DWORD), ("Eip", W.DWORD),
                ("SegCs", W.DWORD), ("EFlags", W.DWORD), ("Esp", W.DWORD), ("SegSs", W.DWORD),
                ("ExtendedRegisters", C.c_ubyte * 512)]


k32.OpenThread.restype = W.HANDLE
k32.Wow64GetThreadContext.argtypes = [W.HANDLE, C.POINTER(WOW64_CONTEXT)]
k32.Wow64SetThreadContext.argtypes = [W.HANDLE, C.POINTER(WOW64_CONTEXT)]
k32.WaitForDebugEvent.argtypes = [C.c_void_p, W.DWORD]
k32.ContinueDebugEvent.argtypes = [W.DWORD, W.DWORD, W.DWORD]


def list_threads(pid):
    class TE32(C.Structure):
        _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ThreadID", W.DWORD),
                    ("th32OwnerProcessID", W.DWORD), ("tpBasePri", W.LONG), ("tpDeltaPri", W.LONG),
                    ("dwFlags", W.DWORD)]
    snap = k32.CreateToolhelp32Snapshot(0x4, 0)
    te = TE32(); te.dwSize = C.sizeof(te)
    out = []
    ok = k32.Thread32First(snap, C.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            out.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, C.byref(te))
    k32.CloseHandle(snap)
    return out


def set_drs(tid, addrs):
    th = k32.OpenThread(THREAD_ALL, False, tid)
    if not th:
        return False
    ctx = WOW64_CONTEXT(); ctx.ContextFlags = WOW64_CONTEXT_DEBUG_REGISTERS
    ok = k32.Wow64GetThreadContext(th, C.byref(ctx))
    if ok:
        dr7 = 0
        for i in range(4):
            a = addrs[i] if i < len(addrs) else 0
            setattr(ctx, f"Dr{i}", a)
            if a:
                # local enable, RW=01 (write), LEN=11 (4 bytes)
                dr7 |= 1 << (i * 2)
                dr7 |= 0b1101 << (16 + i * 4)
        ctx.Dr7 = dr7
        ctx.Dr6 = 0
        ok = k32.Wow64SetThreadContext(th, C.byref(ctx))
    k32.CloseHandle(th)
    return bool(ok)


def safe_detach(pid):
    """Clear debug registers on suspended threads, drain queued debug events, then
    detach. Detaching with a queued single-step event re-raises it in the game,
    which has no handler for it and crashes (this happened once)."""
    handles = []
    for tid in list_threads(pid):
        th = k32.OpenThread(THREAD_ALL, False, tid)
        if th:
            k32.SuspendThread(th)
            handles.append((tid, th))
    for tid, th in handles:
        ctx = WOW64_CONTEXT(); ctx.ContextFlags = WOW64_CONTEXT_DEBUG_REGISTERS
        if k32.Wow64GetThreadContext(th, C.byref(ctx)):
            ctx.Dr0 = ctx.Dr1 = ctx.Dr2 = ctx.Dr3 = ctx.Dr6 = ctx.Dr7 = 0
            k32.Wow64SetThreadContext(th, C.byref(ctx))
    ev = (C.c_ubyte * DEBUG_EVENT_SIZE)()
    # Drain events already queued (threads may be stopped on a breakpoint hit).
    while k32.WaitForDebugEvent(ev, 50):
        code, epid, etid = (C.c_uint32.from_buffer(ev, o).value for o in (0, 4, 8))
        if code == LOAD_DLL_DEBUG_EVENT:
            fh = C.c_void_p.from_buffer(ev, 16).value
            if fh:
                k32.CloseHandle(C.c_void_p(fh))
        k32.ContinueDebugEvent(epid, etid, DBG_CONTINUE)
    k32.DebugActiveProcessStop(pid)
    for _, th in handles:
        k32.ResumeThread(th)
        k32.CloseHandle(th)


def main():
    seconds = float(sys.argv[1])
    targets = []
    for arg in sys.argv[2:6]:
        a, _, label = arg.partition("=")
        targets.append((int(a, 16), label or a))
    addrs = [a for a, _ in targets]
    pid = find_pid()

    if not k32.DebugActiveProcess(pid):
        sys.exit(f"DebugActiveProcess failed {C.get_last_error()}")
    k32.DebugSetProcessKillOnExit(False)
    print(f"attached to {pid}; watching {', '.join(f'{l}@{a:08X}' for a, l in targets)} for {seconds:.0f}s", flush=True)

    hits = defaultdict(lambda: defaultdict(int))
    samples = {}
    ev = (C.c_ubyte * DEBUG_EVENT_SIZE)()
    armed = set()
    end = time.time() + seconds
    try:
        while time.time() < end:
            if not k32.WaitForDebugEvent(ev, 100):
                continue
            code, epid, etid = C.c_uint32.from_buffer(ev, 0).value, C.c_uint32.from_buffer(ev, 4).value, C.c_uint32.from_buffer(ev, 8).value
            status = DBG_CONTINUE
            if code == CREATE_PROCESS_DEBUG_EVENT and not armed:
                for tid in list_threads(pid):
                    if set_drs(tid, addrs):
                        armed.add(tid)
                print(f"armed {len(armed)} threads", flush=True)
            elif code == CREATE_THREAD_DEBUG_EVENT:
                if set_drs(etid, addrs):
                    armed.add(etid)
            elif code == EXCEPTION_DEBUG_EVENT:
                exc = C.c_uint32.from_buffer(ev, 16).value  # union is 8-aligned on x64
                if exc in (STATUS_SINGLE_STEP, STATUS_WX86_SINGLE_STEP):
                    th = k32.OpenThread(THREAD_ALL, False, etid)
                    ctx = WOW64_CONTEXT(); ctx.ContextFlags = WOW64_CONTEXT_FULL | WOW64_CONTEXT_DEBUG_REGISTERS
                    k32.Wow64GetThreadContext(th, C.byref(ctx))
                    for i in range(4):
                        if ctx.Dr6 & (1 << i) and i < len(targets):
                            label = targets[i][1]
                            hits[label][ctx.Eip] += 1
                            if (label, ctx.Eip) not in samples:
                                samples[(label, ctx.Eip)] = {r: getattr(ctx, r) for r in
                                                             ("Eax", "Ebx", "Ecx", "Edx", "Esi", "Edi", "Ebp", "Esp")}
                    ctx.Dr6 = 0
                    k32.Wow64SetThreadContext(th, C.byref(ctx))
                    k32.CloseHandle(th)
                elif exc in (STATUS_BREAKPOINT, STATUS_WX86_BREAKPOINT):
                    pass  # the attach breakpoint
                else:
                    status = DBG_EXCEPTION_NOT_HANDLED  # let the game handle its own exceptions
            elif code == LOAD_DLL_DEBUG_EVENT:
                fh = C.c_void_p.from_buffer(ev, 16).value
                if fh:
                    k32.CloseHandle(C.c_void_p(fh))
            elif code == EXIT_PROCESS_DEBUG_EVENT:
                print("game exited", flush=True)
                break
            k32.ContinueDebugEvent(epid, etid, status)
    finally:
        safe_detach(pid)
        print("detached", flush=True)

    for label, eips in hits.items():
        print(f"== {label}")
        for eip, n in sorted(eips.items(), key=lambda x: -x[1]):
            regs = " ".join(f"{k}={v:08X}" for k, v in samples[(label, eip)].items())
            print(f"   EIP {eip:08X} (DDDA.exe+{eip - 0x400000:X}) x{n}   {regs}")


if __name__ == "__main__":
    main()
