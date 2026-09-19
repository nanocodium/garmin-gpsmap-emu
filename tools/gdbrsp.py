#!/usr/bin/env python3
"""Minimal GDB remote-protocol client for QEMU's gdbstub (ARM32).

    gdbrsp.py [--port 1234] --break 0x801c098c [--mem 0xA4AADF58:4 ...] [--timeout 90]

Connects (QEMU pauses the VM on attach), inserts a software breakpoint,
continues, and on the first stop dumps r0-r15/cpsr plus requested memory.
"""
import argparse
import functools
import socket
import struct
import sys
import time

print = functools.partial(print, flush=True)  # noqa: A001


class Rsp:
    def __init__(self, host, port, timeout):
        self.s = socket.create_connection((host, port), timeout=10)
        self.s.settimeout(timeout)
        self.buf = b""

    def send(self, payload: bytes):
        csum = sum(payload) & 0xff
        self.s.sendall(b"$" + payload + b"#" + f"{csum:02x}".encode())
        # wait for ack
        while True:
            c = self._recvbyte()
            if c == b"+":
                return
            if c == b"-":
                self.s.sendall(b"$" + payload + b"#" + f"{csum:02x}".encode())

    def _recvbyte(self):
        while not self.buf:
            self.buf = self.s.recv(65536)
        c, self.buf = self.buf[:1], self.buf[1:]
        return c

    def recv(self) -> bytes:
        while True:
            c = self._recvbyte()
            if c == b"$":
                break
        data = b""
        while True:
            c = self._recvbyte()
            if c == b"#":
                break
            data += c
        self._recvbyte(); self._recvbyte()          # checksum
        self.s.sendall(b"+")
        return data

    def cmd(self, payload: str) -> bytes:
        self.send(payload.encode())
        return self.recv()


def read_regs(r):
    g = r.cmd("g")
    regs = [struct.unpack("<I", bytes.fromhex(g[i * 8:(i + 1) * 8].decode()))[0] for i in range(16)]
    # QEMU arm core layout: 16 regs, 8 FPA (12 bytes), fps, cpsr
    cps_off = 16 * 8 + 8 * 24 + 8
    cpsr = struct.unpack("<I", bytes.fromhex(g[cps_off:cps_off + 8].decode()))[0] if len(g) >= cps_off + 8 else None
    return regs, cpsr


def read_mem(r, addr, n):
    d = r.cmd(f"m{addr:x},{n:x}")
    if d.startswith(b"E"):
        return None
    return bytes.fromhex(d.decode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1234)
    ap.add_argument("--break", dest="bp", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--mem", action="append", default=[], help="addr:len (hex ok)")
    ap.add_argument("--stack", type=int, default=32, help="words of stack to dump")
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--trace", type=int, default=0,
                    help="trace mode: on each hit print r0-r3 and the string at [r0+off], continue N times")
    ap.add_argument("--name-off", type=lambda x: int(x, 0), default=0x7c)
    ap.add_argument("--stop-at", type=lambda x: int(x, 0), default=None,
                    help="second breakpoint that ends the trace and dumps state")
    ap.add_argument("--r0-dump", type=int, default=0, help="also hex-dump N bytes at r0")
    ap.add_argument("--no-kill", action="store_true", help="leave QEMU stopped at the breakpoint")
    ap.add_argument("--watch", default=None, help="write watchpoint addr:len instead of a breakpoint")
    ap.add_argument("--cond", default=None,
                    help="in trace mode stop (and dump) at the first hit where rN=VAL, e.g. r0=0x4")
    ap.add_argument("--cur-name", action="store_true",
                    help="print name of current task ([0xA4AACAB8]+0x7c)")
    a = ap.parse_args()

    r = Rsp(a.host, a.port, a.timeout)
    r.cmd("qSupported:multiprocess-")
    r.cmd("?")
    if a.watch:
        wa, wl = a.watch.split(":")
        print("watchpoint:", r.cmd(f"Z2,{int(wa,0):x},{int(wl,0):x}").decode())
    else:
        print("breakpoint:", r.cmd(f"Z0,{a.bp:x},2").decode())
    if a.stop_at is not None:
        r.cmd(f"Z0,{a.stop_at:x},2")
    if a.trace:
        for n in range(a.trace):
            if n > 0:
                r.cmd("s")          # step off the breakpoint first
            r.send(b"c")
            try:
                stop = r.recv()
            except socket.timeout:
                print("timeout (trace ended)"); break
            regs, cpsr = read_regs(r)
            if a.stop_at is not None and regs[15] == a.stop_at:
                print(f"--- stop-at {a.stop_at:#x} reached after {n} hits")
                break
            if a.cond is not None:
                creg, cval = a.cond.split("=")
                if regs[int(creg.lstrip("r"))] == int(cval, 0):
                    print(f"--- condition {a.cond} met after {n} hits")
                    break
            name = read_mem(r, regs[0] + a.name_off, 32) or b""
            name = name.split(b"\x00")[0].decode(errors="replace")
            extra = ""
            if a.r0_dump:
                d = read_mem(r, regs[0], a.r0_dump)
                extra += f" r0[]={d.hex(' ') if d else '?'}"
            if a.cur_name:
                cp = read_mem(r, 0xA4AACAB8, 4)
                if cp:
                    cur = struct.unpack("<I", cp)[0]
                    nm = read_mem(r, cur + 0x7c, 16) or b""
                    extra += f" cur={cur:08x}'{nm.split(b'\x00')[0].decode(errors='replace')}'"
            print(f"hit {n}: pc={regs[15]:08x} r0={regs[0]:08x} r1={regs[1]:08x} r2={regs[2]:08x} "
                  f"r3={regs[3]:08x} lr={regs[14]:08x} [r0+{a.name_off:#x}]='{name}'{extra}")
        stop = b""
    else:
        r.send(b"c")
        try:
            stop = r.recv()
        except socket.timeout:
            print("timeout waiting for breakpoint"); sys.exit(1)
        print("stop:", stop.decode()[:40])
    regs, cpsr = read_regs(r)
    for i in range(0, 16, 4):
        print("  " + "  ".join(f"r{i+j:<2}={regs[i+j]:08x}" for j in range(4)))
    print(f"  cpsr={cpsr:08x}" if cpsr is not None else "  cpsr=?")
    sp = regs[13]
    st = read_mem(r, sp, a.stack * 4)
    if st:
        words = struct.unpack(f"<{a.stack}I", st)
        for i in range(0, a.stack, 4):
            print(f"  [sp+{i*4:03x}] " + " ".join(f"{w:08x}" for w in words[i:i + 4]))
    for spec in a.mem:
        addr, n = spec.split(":")
        addr = int(addr, 0); n = int(n, 0)
        d = read_mem(r, addr, n)
        print(f"  mem {addr:08x}: {d.hex(' ') if d else 'ERR'}")
    if a.no_kill:
        return
    try:
        r.send(b"k")
    except Exception:
        pass


if __name__ == "__main__":
    main()
