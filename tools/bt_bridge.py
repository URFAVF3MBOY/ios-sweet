#!/usr/bin/env python3
"""bt_bridge.py: give bluetoothd a /dev/uart.bluetooth that is the WCN3990 (OnePlus 7T), without a kernel tty.

bluetoothd's transport is H4 over a UART tty ("DeviceTree transport = 3" -> "HCI Transport is set to H4BC"); it
opens /dev/uart.bluetooth and aborts with "HCI initialization failed" when it cannot. Here a pseudo-terminal
stands in: its slave gets a second device node, /dev/uart.bluetooth (mknod on devfs, same major/minor), and
this process moves bytes between the pty master and the chip through kern.sweetbtio (patch 0175), logging
every H4 packet both ways. The chip must already have its firmware: `sysctl kern.sweetbt=1` (stage 6).

  bt_bridge.py [--log FILE]      run as root; stays in the foreground
"""
import ctypes, os, select, stat, sys, time, termios, tty

libc = ctypes.CDLL(None, use_errno=True)
libc.sysctlbyname.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.c_void_p, ctypes.c_size_t]
NODE = '/dev/uart.bluetooth'
LOG = open(sys.argv[sys.argv.index('--log') + 1], 'a') if '--log' in sys.argv else sys.stderr


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, file=LOG, flush=True)


def io(send=b''):
    buf = ctypes.create_string_buffer(4096)
    n = ctypes.c_size_t(4096)
    src = ctypes.create_string_buffer(send, len(send)) if send else None
    if libc.sysctlbyname(b'kern.sweetbtio', buf, ctypes.byref(n), src, len(send)) != 0:
        raise OSError(ctypes.get_errno(), 'kern.sweetbtio')
    return buf.raw[:n.value]


class Framer:
    """cuts an H4 byte stream into packets for the log (does not change the stream)"""
    def __init__(self, who):
        self.who, self.buf = who, b''

    def feed(self, data):
        self.buf += data
        while self.buf:
            t = self.buf[0]
            need = {1: 4, 2: 5, 3: 4, 4: 3}.get(t)
            if need is None:
                log(self.who, 'stray %02x' % t)
                self.buf = self.buf[1:]
                continue
            if len(self.buf) < need:
                return
            body = {1: self.buf[3] if need == 4 and t == 1 else 0}.get(t, 0)
            if t == 1:
                body = self.buf[3]
            elif t == 2:
                body = self.buf[3] | self.buf[4] << 8
            elif t == 3:
                body = self.buf[3]
            elif t == 4:
                body = self.buf[2]
            if len(self.buf) < need + body:
                return
            p, self.buf = self.buf[:need + body], self.buf[need + body:]
            what = ''
            if t == 1:
                what = ' cmd %04x' % (p[1] | p[2] << 8)
            elif t == 4:
                what = ' evt %02x' % p[1] + (' for %04x' % (p[4] | p[5] << 8) if p[1] == 0x0e and len(p) >= 6 else '')
            log(self.who, ('%d bytes%s: ' % (len(p), what)) + p[:64].hex(' ') + (' ...' if len(p) > 64 else ''))


def main():
    master, slave = os.openpty()
    tty.setraw(slave)
    name = os.ttyname(slave)
    rdev = os.stat(name).st_rdev
    try:
        os.unlink(NODE)
    except FileNotFoundError:
        pass
    os.mknod(NODE, 0o666 | stat.S_IFCHR, rdev)
    os.chmod(NODE, 0o666)
    log('pty %s (major %d minor %d) is now also %s' % (name, os.major(rdev), os.minor(rdev), NODE))
    host, chip = Framer('bluetoothd ->'), Framer('<- chip     ')
    pending = io()                      # whatever the chip said before
    if pending:
        chip.feed(pending)
        os.write(master, pending)
    while True:
        r, _, _ = select.select([master], [], [], 0.005)
        if master in r:
            try:
                data = os.read(master, 4096)
            except OSError:
                data = b''              # (no one has the slave open yet)
                time.sleep(0.05)
            if data:
                host.feed(data)
                back = io(data)
                if back:
                    chip.feed(back)
                    os.write(master, back)
        back = io()
        if back:
            chip.feed(back)
            os.write(master, back)


if __name__ == '__main__':
    main()
