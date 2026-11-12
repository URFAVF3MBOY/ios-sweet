#!/var/jb/usr/bin/python3
# camio.py: the OnePlus 7T's rear camera bus from a root shell (kernel 0203 kern.sweetcami2c; run
# `sysctl -w kern.sweetcam=2` first: power, clocks, CCI). CCI0 master 1: IMX586 at 0x1a, EEPROM at 0x51,
# actuator/OIS(?) at 0x59.
#   camio.py read  ADDR REG [N] [REGLEN]     hex: camio.py read 1a 0016 2
#   camio.py write ADDR REG BYTES [REGLEN]   camio.py write 1a 0100 01
#   camio.py eeprom [BYTES] [OUT]            dump the module EEPROM (16-bit addresses), default 0x2000 bytes
#   camio.py sensor                          IMX586 id/revision/mode/frame count
import ctypes, struct, sys

libc = ctypes.CDLL(None)
libc.sysctlbyname.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t),
                              ctypes.c_void_p, ctypes.c_size_t]


def xfer(op, addr7, reg, reg_len, n=0, data=b""):
    req = struct.pack("<BBBBI8s", op, addr7, reg_len, n if op == 0 else len(data), reg, data.ljust(8, b"\0"))
    rep = ctypes.create_string_buffer(16)
    size = ctypes.c_size_t(16)
    rq = ctypes.create_string_buffer(req, 16)
    if libc.sysctlbyname(b"kern.sweetcami2c", rep, ctypes.byref(size), rq, 16) != 0:
        raise OSError(ctypes.get_errno(), "sysctl kern.sweetcami2c")
    result, status, out = struct.unpack("<iI8s", rep.raw)
    return result, status, out[:n]


def read(addr7, reg, n=1, reg_len=2):
    r, st, d = xfer(0, addr7, reg, reg_len, n)
    if r != 0:
        raise IOError("read %#x reg %#x: result %d status %#x" % (addr7, reg, r, st))
    return d


def write(addr7, reg, data, reg_len=2):
    r, st, _ = xfer(1, addr7, reg, reg_len, data=data)
    if r != 0:
        raise IOError("write %#x reg %#x: result %d status %#x" % (addr7, reg, r, st))


def main(a):
    if not a:
        print(__doc__ if __doc__ else open(sys.argv[0]).read().split("import")[0])
        return
    cmd = a[0]
    if cmd == "read":
        addr, reg = int(a[1], 16), int(a[2], 16)
        n = int(a[3]) if len(a) > 3 else 1
        rl = int(a[4]) if len(a) > 4 else 2
        print(read(addr, reg, n, rl).hex())
    elif cmd == "write":
        addr, reg = int(a[1], 16), int(a[2], 16)
        rl = int(a[4]) if len(a) > 4 else 2
        write(addr, reg, bytes.fromhex(a[3]), rl)
        print("ok")
    elif cmd == "eeprom":
        total = int(a[1], 0) if len(a) > 1 else 0x2000
        out = a[2] if len(a) > 2 else "/var/root/cam-eeprom.bin"
        buf = bytearray()
        for off in range(0, total, 8):
            try:
                buf += read(0x51, off, 8, 2)
            except IOError as e:
                print("stopped at %#x: %s" % (off, e))
                break
        open(out, "wb").write(buf)
        print("%d bytes -> %s" % (len(buf), out))
        for off in range(0, min(len(buf), 0x100), 16):
            row = buf[off:off + 16]
            print("%04x  %-48s %s" % (off, row.hex(" "), "".join(chr(c) if 32 <= c < 127 else "." for c in row)))
    elif cmd == "sensor":
        mid = read(0x1a, 0x0016, 2)
        rev = read(0x1a, 0x0018, 1)
        mode = read(0x1a, 0x0100, 1)
        fc = read(0x1a, 0x0005, 1)
        print("model %s revision %s mode_select %s frame_count %d" % (mid.hex(), rev.hex(), mode.hex(), fc[0]))
    else:
        print("unknown command")


if __name__ == "__main__":
    main(sys.argv[1:])
