#!/usr/bin/env python3
"""btio.py: talk to the WCN3990 through kern.sweetbtio (patch 0175), after `sysctl kern.sweetbt=1` (stage 6).

  btio.py raw HEX...          send bytes as given, print what comes back for 0.5 s
  btio.py read [SECONDS]      print what comes in
  btio.py cmd OPCODE [HEX]    one HCI command (H4 01 + opcode + len + params), IBS handled, print events
  btio.py init                reset (no wake first), read local version, BD address, local name

In-band sleep (Qualcomm IBS): 0xFD WAKE_IND, 0xFC WAKE_ACK, 0xFE SLEEP_IND between H4 packets.
"""
import ctypes, sys, time

libc = ctypes.CDLL(None, use_errno=True)
libc.sysctlbyname.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.c_void_p, ctypes.c_size_t]
NAME = b'kern.sweetbtio'
WAKE_IND, WAKE_ACK, SLEEP_IND = 0xFD, 0xFC, 0xFE


def io(send=b'', want=4096):
    want = 4096                     # (0175's read drops the rest of a 4-byte FIFO word beyond 'want')
    buf = ctypes.create_string_buffer(want)
    n = ctypes.c_size_t(want)
    src = ctypes.create_string_buffer(send, len(send)) if send else None
    r = libc.sysctlbyname(NAME, buf, ctypes.byref(n), src, len(send))
    if r != 0:
        raise OSError(ctypes.get_errno(), 'sysctl kern.sweetbtio')
    return buf.raw[:n.value]


class H4:
    """splits the byte stream into packets; answers WAKE_IND; tracks the chip's sleep state"""
    def __init__(self):
        self.buf = b''
        self.awake = None
        self.packets = []
        self.log = []

    def feed(self, data):
        self.buf += data
        while self.buf:
            t = self.buf[0]
            if t == 0x04:
                if len(self.buf) < 3 or len(self.buf) < 3 + self.buf[2]:
                    return
                n = 3 + self.buf[2]
            elif t == 0x02:
                if len(self.buf) < 5:
                    return
                n = 5 + (self.buf[3] | self.buf[4] << 8)
                if len(self.buf) < n:
                    return
            elif t == 0x03:
                if len(self.buf) < 4 or len(self.buf) < 4 + self.buf[3]:
                    return
                n = 4 + self.buf[3]
            else:
                if t == WAKE_IND:
                    self.log.append('<- WAKE_IND, -> WAKE_ACK')
                    self.buf = self.buf[1:]
                    self.send(bytes([WAKE_ACK]))
                    continue
                    self.awake = True
                elif t == WAKE_ACK:
                    self.log.append('<- WAKE_ACK')
                    self.awake = True
                elif t == SLEEP_IND:
                    self.log.append('<- SLEEP_IND')
                    self.awake = False
                else:
                    self.log.append('<- stray %02x' % t)
                self.buf = self.buf[1:]
                continue
            self.packets.append(self.buf[:n])
            self.buf = self.buf[n:]

    def send(self, data):
        self.feed(io(data))

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            d = io(b'', 4096)
            if d:
                self.feed(d)
            else:
                time.sleep(0.005)

    def flush_log(self):
        for line in self.log:
            print('  ' + line)
        self.log = []


def hexs(b):
    return ' '.join('%02x' % x for x in b)


def wake(h):
    if h.awake:
        return True
    for _ in range(10):
        h.send(bytes([WAKE_IND]))
        h.pump(0.02)
        if h.awake:
            return True
    print('  no WAKE_ACK to WAKE_IND')
    return False


def cmd(h, opcode, params=b'', wake_first=True, wait=1.0):
    if wake_first:
        wake(h)
    pkt = bytes([0x01, opcode & 0xff, opcode >> 8, len(params)]) + params
    print('-> %s' % hexs(pkt))
    h.send(pkt)
    end = time.time() + wait
    reply = None
    while time.time() < end and reply is None:
        h.pump(0.02)
        h.flush_log()
        while h.packets:
            p = h.packets.pop(0)
            print('<- %s' % hexs(p))
            if p[0] == 0x04 and ((p[1] == 0x0e and (p[4] | p[5] << 8) == opcode) or (p[1] == 0x0f and (p[5] | p[6] << 8) == opcode)):
                reply = p
    if reply is None:
        print('  no reply to %04x' % opcode)
    return reply


def main():
    a = sys.argv[1:]
    h = H4()
    if not a or a[0] == 'read':
        secs = float(a[1]) if len(a) > 1 else 1.0
        end = time.time() + secs
        while time.time() < end:
            d = io(b'', 4096)
            if d:
                print('<- %s' % hexs(d))
            time.sleep(0.01)
    elif a[0] == 'raw':
        got = io(bytes.fromhex(''.join(a[1:])))
        time.sleep(0.5)
        print('<- %s' % hexs(got + io(b'')))
    elif a[0] == 'cmd':
        cmd(h, int(a[1], 16), bytes.fromhex(''.join(a[2:])))
    elif a[0] == 'init':
        h.pump(0.05)
        h.flush_log()
        r = cmd(h, 0x0c03, wake_first=False)
        if r is not None:
            print('reset status %#x' % r[6])
        h.awake = True                  # (0176: in-band sleep off in the NVM)
        r = cmd(h, 0x1001)
        if r is not None and len(r) >= 15:
            print('local version: hci %u rev %#x lmp %u manufacturer %#x subversion %#x' %
                  (r[7], r[8] | r[9] << 8, r[10], r[11] | r[12] << 8, r[13] | r[14] << 8))
        r = cmd(h, 0x1009)
        if r is not None and len(r) >= 13:
            print('bd addr %s' % ':'.join('%02x' % x for x in reversed(r[7:13])))
        r = cmd(h, 0x0c14)
        if r is not None and len(r) > 7:
            print('local name %r' % r[7:].split(b'\0')[0])


if __name__ == '__main__':
    main()
