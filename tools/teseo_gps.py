#!/usr/bin/env python3
"""ST Teseo GPS module ("tracker") stand-in for the GPSMAP 7x08 machine.

Phase 1 - boot (gps_st_io.c, 0x802bb068):

    host -> gps : 28-byte header + 0x134-byte UART boot loader   (921600 baud)
    gps  -> host: 0x55                       (boot loader running)
    host -> gps : 0xAA, then the tracker image (0x12344 bytes)   (3.6864 Mbaud)
    gps  -> host: 0xAA                       (image received)
    host -> gps : 0x55                       (start)

The host tries this five times; without the two acknowledgements the firmware
asserts "Tracker boot failed" and reboots.

Phase 2 - ST "RCIF over UART" remote-call protocol (host = master, tracker =
slave; the ST GNSS library is ARM code at 0x8112xxxx-0x8116xxxx, transport at
0x81132a60-0x81132e64, 115200 baud).  Every host->tracker frame is a remote
procedure call and the tracker must answer with exactly one frame:

    u32 id | u16 len | u16 spare | payload[len] | xor      (xor over all
                                                            preceding bytes)

    request payload : [u32 result slot = 0][args...]; pointer args are byte
                      offsets into the payload area (see 0x8081c198/0x8081c218)
    reply   payload : [u32 return code][out data at the same offsets]

Handshake (chunked mode, 0x81132a60 / 0x81132b58): the master writes the
frame in chunks of S bytes and after EVERY chunk waits (9.75 s) for one ACK
byte from the slave: 0xBB = ok, 0xED = slave rx timeout, 0xEF = slave checksum
error.  It then reads the reply: 9 bytes (header + first payload byte) and the
rest in chunks of M-9 bytes, sending 0xBB after every chunk except the last.
Missing a per-chunk ACK makes 0x81132e64 retry the same call forever every
~10 s (seen as repeated 0x1016 requests whose zero-filled config chunks and
0x46 checksum byte look like a "zero stream with 'F's").  M and S default to
0x40 and are announced by the first call, id 0x2303 ("update slave uart HW
buffer size", payload [0, M, S]):

    03 23 00 00 0c 00 00 00 00 00 00 00 40 00 00 00 40 00 00 00 2c

If the tracker never answers, 0x81132e64 retries the same call forever (the
GPS task stays inside "[RCIF][time_synch] init"); nothing else reboots.  The
slave never sends unsolicited frames, all navigation data is polled by RPC.

Nav loop (0x8112e9c8): the library's nav task posts itself a type-0 event and
on it calls id 0x1026 (stub 0x8116a364, "new epoch available?").  Only a
return value of 1 (0x8112eaf4) runs the epoch path (time sync 0x102d/0x1025,
fix/satellite reads such as 0x1018, callbacks into Garmin's glue) and
reschedules at the next fix epoch; anything else re-posts the event at once
(0x8112edf8), i.e. a ~7 ms poll spin, and after ~240 s without an epoch the
gps_st_config.c:338 "Watchdog expired" callback (0x80131a4c, OS-table slot
+0x60 installed at 0x800c59c8) asserts and reboots.  The stand-in therefore
answers 0x1026 with 1 once per second.

    teseo_gps.py <endpoint> [log file]

The endpoint is the UART1 chardev QEMU serves: a unix socket path on
Linux/WSL, tcp:host:port on Windows (tools/run_gpsmap.py picks it).
"""
import os
import select
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qenv                                                    # noqa: E402

BOOTLOADER_MIN = 28 + 0x134
IDLE_S = 0.15
ACK_TIMES = (0.40, 0.75, 1.10, 1.50, 2.00, 2.60)

RCIF_HDR = 8
RCIF_MAX_FRAME = 0x534           # 0x81132a14: frames this long or longer are rejected
ACK_OK, ACK_RX_TIMEOUT, ACK_BAD_CKSUM = 0xBB, 0xED, 0xEF
RPC_SET_UART_BUF = 0x2303        # [RCIF][time_synch] init, 0x81133498
RPC_GET_SLAVE_TIME = 0x102D      # second call of the same init, result printed as "slv %u"
RPC_EPOCH_READY = 0x1026         # nav loop poll, 0x8112eaf0: proceed only when the reply is 1
EPOCH_PERIOD_S = 1.0             # emulated fix rate: how often 0x1026 answers 1
CHUNKED_MODE = True              # per-chunk ACKs of 0x81132a60/0x81132b58 (confirmed on the emulator)
MASTER_ACK_WAIT_S = 1.0          # wait for the master's 0xBB between reply chunks, then continue anyway
RPC_SET_PAIR = 0x1031            # stub 0x8116a578: set (u32 a, u32 b) ...
RPC_GET_PAIR = 0x1032            # stub 0x8116a5c0: ... 0x80818c70 polls until the reply echoes them
RPC_SUBSYSTEM_VER = 0x103C       # stub 0x8116a8d8: 0x40-byte "GPSSUBSYSTEMVER %s" string (gps_init 0x808180e0)
SUBSYSTEM_VER = b"GPSMAP7x08 Teseo stand-in 1.0"
# RPCs whose reply must carry out-data (id -> payload length); everything else gets a 4-byte status.
OUT_DATA_LEN = {
    0x1018: 4 + 0x44,            # stub 0x81169e08: 0x44-byte block copied back to the caller
    0x1019: 4 + 0x44,            # stub 0x81169e80: same layout
    0x101D: 4 + 0x254,           # stub 0x81169ffc: 0x254-byte block
    0x1022: 4 + 8,               # stub 0x8116a250: two status words
    RPC_GET_PAIR: 4 + 8,         # u16 at +4, byte at +8
    RPC_SUBSYSTEM_VER: 4 + 0x40,
}


def log(f, msg):
    f.write(f"{time.strftime('%H:%M:%S')}.{int(time.time() * 1000) % 1000:03d} {msg}\n")
    f.flush()


def xor_sum(data):
    c = 0
    for b in data:
        c ^= b
    return c


def send_ack(s, logf, byte, what):
    """Boot phase: send `byte` at each ACK_TIMES offset unless the host talks first."""
    t0 = time.time()
    for t in ACK_TIMES:
        remain = t0 + t - time.time()
        if remain > 0:
            r, _, _ = select.select([s], [], [], remain)
            if r:
                log(logf, f"{what}: host sent data before ack schedule finished")
                return
        s.sendall(bytes([byte]))
    log(logf, f"{what}: sent 0x{byte:02x} x{len(ACK_TIMES)}")


class Rcif:
    """Slave side of the RCIF-over-UART transport."""

    def __init__(self, sock, logf):
        self.s = sock
        self.logf = logf
        self.buf = bytearray()
        self.master_buf = 0x40      # M: master HW buffer, reply chunking
        self.slave_buf = 0x40       # S: slave HW buffer, request chunking
        self.acked = 0              # bytes of the current request already ACKed (chunked mode)
        self.t0 = time.time()
        self.calls = 0
        self.started = False
        self.pair = (0, 0)          # last values set with 0x1031, echoed by 0x1032
        self.last_epoch = 0.0       # when 0x1026 last answered "epoch ready"
        self.epochs = 0
        self.polls_since_epoch = 0

    # -- receive -----------------------------------------------------------
    def feed(self, data):
        self.buf += data
        while self.buf:
            if not self.started and self.buf[0] == 0x55:
                # boot "start" byte sent right after the image ack (0x802bb1c8)
                del self.buf[0]
                self.started = True
                log(self.logf, "start byte received; tracker running, RCIF slave ready")
                continue
            if len(self.buf) < RCIF_HDR:
                return
            hdr = self._header()
            if hdr is None:
                log(self.logf, f"rcif: resync, dropping byte 0x{self.buf[0]:02x}")
                del self.buf[0]
                self.acked = 0
                continue
            rpc_id, plen, spare = hdr
            total = RCIF_HDR + plen + 1
            if len(self.buf) < total:
                self._ack_chunks(total)
                return
            frame = bytes(self.buf[:total])
            del self.buf[:total]
            self.acked = 0
            self._handle(rpc_id, plen, spare, frame)

    def _header(self):
        rpc_id, plen, spare = struct.unpack_from("<IHH", self.buf, 0)
        if rpc_id == 0 or rpc_id > 0xFFFF or RCIF_HDR + plen + 1 >= RCIF_MAX_FRAME:
            return None
        return rpc_id, plen, spare

    def _ack_chunks(self, total):
        """Chunked-mode master TX (0x81132a60) waits for 0xBB after every S-byte chunk.
        In the observed single-write mode (0x81132c58) any extra byte would be read as
        the start of the reply, so this is off by default."""
        if not CHUNKED_MODE:
            return
        have = len(self.buf)
        while self.acked + self.slave_buf <= have and self.acked + self.slave_buf < total:
            self.s.sendall(bytes([ACK_OK]))
            self.acked += self.slave_buf

    # -- handle ------------------------------------------------------------
    def _handle(self, rpc_id, plen, spare, frame):
        payload = frame[RCIF_HDR:RCIF_HDR + plen]
        if os.environ.get("TESEO_NO_RCIF"):
            log(self.logf, f"rcif <- id 0x{rpc_id:04x} len {plen} (not answered: TESEO_NO_RCIF)")
            return
        if xor_sum(frame[:-1]) != frame[-1]:
            log(self.logf, f"rcif: checksum error on id 0x{rpc_id:04x} len {plen}: {frame[:32].hex()}")
            self.s.sendall(bytes([ACK_BAD_CKSUM]))
            return
        self.calls += 1
        if rpc_id != RPC_EPOCH_READY:            # the poll is logged once per epoch instead
            log(self.logf, f"rcif <- id 0x{rpc_id:04x} len {plen} spare {spare} args {payload[:48].hex()}"
                           f"{'...' if plen > 48 else ''}")
        self.s.sendall(bytes([ACK_OK]))          # frame received ok
        reply = self._reply_payload(rpc_id, payload)
        self._send_frame(rpc_id, reply)

    def _reply_payload(self, rpc_id, payload):
        """Return code 0 ("no error") plus zeroed out-data.  Out-data lives at the
        offsets the caller allocated, so replies are kept short unless a stub is
        known to copy a block back (OUT_DATA_LEN); zero data reads as no satellites,
        no fix, no time."""
        out = bytearray(OUT_DATA_LEN.get(rpc_id, 4))
        if rpc_id == RPC_SET_UART_BUF and len(payload) >= 12:
            _, m, s = struct.unpack_from("<III", payload, 0)
            if 9 < m < RCIF_MAX_FRAME and 0 < s < RCIF_MAX_FRAME:
                self.master_buf, self.slave_buf = m, s
            log(self.logf, f"rcif: uart HW buffer sizes M={self.master_buf} S={self.slave_buf}")
        elif rpc_id == RPC_GET_SLAVE_TIME:
            struct.pack_into("<I", out, 0, self._ms())
        elif rpc_id == RPC_SET_PAIR and len(payload) >= 12:
            self.pair = struct.unpack_from("<II", payload, 4)
        elif rpc_id == RPC_GET_PAIR:
            struct.pack_into("<HxxB", out, 4, self.pair[0] & 0xFFFF, self.pair[1] & 0xFF)
        elif rpc_id == RPC_SUBSYSTEM_VER:
            out[4:4 + len(SUBSYSTEM_VER)] = SUBSYSTEM_VER
        elif rpc_id == RPC_EPOCH_READY:
            self.polls_since_epoch += 1
            now = time.time()
            if now - self.last_epoch >= EPOCH_PERIOD_S:
                self.last_epoch = now
                self.epochs += 1
                log(self.logf, f"rcif: epoch {self.epochs} ready after {self.polls_since_epoch} polls of 0x1026")
                self.polls_since_epoch = 0
                struct.pack_into("<I", out, 0, 1)   # 0x8112eaf4: == 1 -> process the epoch
        return bytes(out)

    def _ms(self):
        return int((time.time() - self.t0) * 1000) & 0xFFFFFFFF

    # -- send --------------------------------------------------------------
    def _send_frame(self, rpc_id, payload):
        frame = bytearray(struct.pack("<IHH", rpc_id, len(payload), 0)) + payload
        frame.append(xor_sum(frame))
        if not CHUNKED_MODE:
            # Master RX 0x81132d54: 9 bytes then the remainder, no ACKs.
            self.s.sendall(frame)
        else:
            # Master RX 0x81132b58: 9 bytes, then chunks of M-9 with an ACK between chunks.
            first = min(len(frame), self.master_buf)
            self.s.sendall(frame[:first])
            pos = first
            while pos < len(frame):
                if not self._wait_master_ack():
                    log(self.logf, "rcif: no master ack between reply chunks, continuing")
                n = min(len(frame) - pos, self.master_buf - 9)
                self.s.sendall(frame[pos:pos + n])
                pos += n
        if rpc_id != RPC_EPOCH_READY:
            log(self.logf, f"rcif -> id 0x{rpc_id:04x} len {len(payload)} ({len(frame)} bytes)")

    def _wait_master_ack(self):
        deadline = time.time() + MASTER_ACK_WAIT_S
        while True:
            remain = deadline - time.time()
            if remain <= 0:
                return False
            r, _, _ = select.select([self.s], [], [], remain)
            if not r:
                return False
            data = self.s.recv(65536)
            if not data:
                return False
            if data[0] == ACK_OK:
                self.buf += data[1:]
                return True
            self.buf += data


def main():
    path = sys.argv[1]
    logf = (open(sys.argv[2], "a") if len(sys.argv) > 2 else sys.stderr)
    # QEMU is the server on this endpoint and may still be starting up.
    for _ in range(300):
        try:
            s = qenv.connect(path, timeout=IDLE_S)
            break
        except (OSError, SystemExit):
            time.sleep(0.1)
    else:
        log(logf, f"gave up connecting to {path}")
        return
    s.settimeout(IDLE_S)
    log(logf, f"connected to {path}")

    phase = 0            # 0: expect boot loader, 1: expect image, 2: running (RCIF slave)
    burst = bytearray()
    rcif = None
    while True:
        try:
            data = s.recv(65536)
            if not data:
                log(logf, "socket closed")
                return
            if phase == 2 and not (burst or data[:2] == b"\xf4\x01"):
                rcif.feed(data)          # answer remote calls immediately
                continue
            burst += data
            continue
        except socket.timeout:
            pass
        if not burst:
            continue
        if len(burst) >= BOOTLOADER_MIN and burst[:2] == b"\xf4\x01" and len(burst) < 0x1000:
            log(logf, f"boot loader received ({len(burst)} bytes){' (tracker restart)' if phase == 2 else ''}")
            burst.clear()
            phase = 1
            send_ack(s, logf, 0x55, "boot loader ack")
        elif phase == 1 and len(burst) >= 0x1000:
            log(logf, f"tracker image burst ({len(burst)} bytes)")
            burst.clear()
            phase = 2
            rcif = Rcif(s, logf)
            send_ack(s, logf, 0xAA, "image ack")
        elif phase == 2:
            rcif.feed(bytes(burst))      # a burst that only looked like a boot loader
            burst.clear()
        elif phase == 0 and burst[:2] != b"\xf4\x01":
            # Not a boot: the tracker was already running (e.g. the VM was
            # restored from a snapshot).  Serve remote calls straight away.
            log(logf, f"phase0 data without boot ({len(burst)} bytes): assuming tracker already running")
            phase = 2
            rcif = Rcif(s, logf)
            rcif.started = True
            rcif.feed(bytes(burst))
            burst.clear()
        else:
            log(logf, f"phase{phase} rx {len(burst)} bytes: {bytes(burst[:64]).hex()}")
            burst.clear()


if __name__ == "__main__":
    main()
