"""Execute breakpoint on a code address: capture EAX/ECX and a stack walk of
return addresses (values on the stack that point right after a CALL in .text).

Usage: py hwexec.py SECONDS CODE_HEX [MAX_SAMPLES]
"""
import ctypes as C, struct, sys, time
from collections import Counter
from memscan import find_pid, open_proc, read
import hwbp
from hwbp import k32, WOW64_CONTEXT, THREAD_ALL

TEXT_LO, TEXT_HI = 0x401000, 0x139D000
seconds, target = float(sys.argv[1]), int(sys.argv[2], 16)
max_samples = int(sys.argv[3]) if len(sys.argv) > 3 else 40
pid = find_pid()
ph = open_proc()


def is_ret_site(a):
    if not (TEXT_LO <= a < TEXT_HI):
        return False
    b = read(ph, a - 6, 6)
    if not b:
        return False
    return b[1] == 0xE8 or (b[4] == 0xFF and (b[5] & 0x38) == 0x10) or (b[0] == 0xFF and (b[1] & 0x38) == 0x10)


def set_exec(tid, addr):
    th = k32.OpenThread(THREAD_ALL, False, tid)
    if not th:
        return
    ctx = WOW64_CONTEXT(); ctx.ContextFlags = hwbp.WOW64_CONTEXT_DEBUG_REGISTERS
    k32.Wow64GetThreadContext(th, C.byref(ctx))
    ctx.Dr0 = addr
    ctx.Dr7 = 1 if addr else 0  # L0, RW=00 (execute), LEN=00
    ctx.Dr6 = 0
    k32.Wow64SetThreadContext(th, C.byref(ctx))
    k32.CloseHandle(th)


if not k32.DebugActiveProcess(pid):
    sys.exit(f"DebugActiveProcess failed {C.get_last_error()}")
k32.DebugSetProcessKillOnExit(False)
ev = (C.c_ubyte * hwbp.DEBUG_EVENT_SIZE)()
stacks = Counter()
regs = {}
armed = False
end = time.time() + seconds
n = 0
try:
    while time.time() < end and n < max_samples:
        if not k32.WaitForDebugEvent(ev, 100):
            continue
        code = C.c_uint32.from_buffer(ev, 0).value
        epid, etid = C.c_uint32.from_buffer(ev, 4).value, C.c_uint32.from_buffer(ev, 8).value
        status = hwbp.DBG_CONTINUE
        if code == hwbp.CREATE_PROCESS_DEBUG_EVENT and not armed:
            for tid in hwbp.list_threads(pid):
                set_exec(tid, target)
            armed = True
        elif code == hwbp.CREATE_THREAD_DEBUG_EVENT:
            set_exec(etid, target)
        elif code == hwbp.LOAD_DLL_DEBUG_EVENT:
            fh = C.c_void_p.from_buffer(ev, 16).value
            if fh:
                k32.CloseHandle(C.c_void_p(fh))
        elif code == hwbp.EXCEPTION_DEBUG_EVENT:
            exc = C.c_uint32.from_buffer(ev, 16).value
            if exc in (hwbp.STATUS_SINGLE_STEP, hwbp.STATUS_WX86_SINGLE_STEP):
                th = k32.OpenThread(THREAD_ALL, False, etid)
                ctx = WOW64_CONTEXT(); ctx.ContextFlags = hwbp.WOW64_CONTEXT_FULL | hwbp.WOW64_CONTEXT_DEBUG_REGISTERS
                k32.Wow64GetThreadContext(th, C.byref(ctx))
                if ctx.Eip == target:
                    sd = read(ph, ctx.Esp, 0x400) or b""
                    rets = tuple(v for (v,) in struct.iter_unpack("<I", sd[: len(sd) // 4 * 4]) if is_ret_site(v))[:8]
                    stacks[rets] += 1
                    regs.setdefault(rets, []).append((ctx.Eax, ctx.Ecx, ctx.Esi, ctx.Edi))
                    n += 1
                    ctx.EFlags |= 0x10000  # RF: resume past the breakpoint
                ctx.Dr6 = 0
                k32.Wow64SetThreadContext(th, C.byref(ctx))
                k32.CloseHandle(th)
            elif exc not in (hwbp.STATUS_BREAKPOINT, hwbp.STATUS_WX86_BREAKPOINT):
                status = hwbp.DBG_EXCEPTION_NOT_HANDLED
        elif code == hwbp.EXIT_PROCESS_DEBUG_EVENT:
            break
        k32.ContinueDebugEvent(epid, etid, status)
finally:
    hwbp.safe_detach(pid)
    print("detached")

for rets, cnt in stacks.most_common():
    print(f"x{cnt}: " + " <- ".join(f"+{r - 0x400000:X}" for r in rets))
    for eax, ecx, esi, edi in sorted(set(regs[rets]))[:4]:
        print(f"      eax={eax:08X} ecx={ecx:08X} esi={esi:08X} edi={edi:08X}")
