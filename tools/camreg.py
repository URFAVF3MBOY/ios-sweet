#!/usr/bin/env python3
# camreg.py: camera register reads/writes from userland through kern.sweetmmio (0212 allows the Titan camera block,
# camcc and the apps SMMU). Arm the camera first (kern.sweetcam=4): a block whose clocks/GDSC are off faults.
#   camreg.py rd ADDR [COUNT]        32-bit reads (ADDR: hex PA, or ife+OFF / csid+OFF / phy+OFF / cc+OFF / smmu+OFF)
#   camreg.py wr ADDR VAL [MASK]     write (masked when MASK given), prints the value read back
#   camreg.py ife | csid | wm        named dumps (IFE top/bus status, CSID RX/RDI0, write master 0 block)
import ctypes, struct, sys

BASES = {"ife": 0x0acaf000, "csid": 0x0acb3000, "phy": 0x0ac66000, "cc": 0x0ad00000, "camnoc": 0x0ac42000,
         "smmu": 0x15000000}
libc = ctypes.CDLL(None, use_errno=True)


def mmio(op, pa, val=0, mask=0):
    req = struct.pack("<IIQII", op, 4, pa, val, mask)
    old = ctypes.create_string_buffer(len(req))
    olen = ctypes.c_size_t(len(req))
    if libc.sysctlbyname(b"kern.sweetmmio", old, ctypes.byref(olen), req, len(req)) != 0:
        e = ctypes.get_errno()
        raise OSError(e, "kern.sweetmmio %#x: %s" % (pa, __import__("os").strerror(e)))
    return struct.unpack("<IIQII", old.raw)[3]


def addr(s):
    if "+" in s:
        b, o = s.split("+", 1)
        return BASES[b] + int(o, 16)
    return int(s, 16)


def dump(base, ranges):
    for lo, hi in ranges:
        for o in range(lo, hi, 16):
            print("%#010x +%04x: %s" % (base + o, o, " ".join("%08x" % mmio(0, base + o + 4 * k) for k in range(4))))


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__ if __doc__ else open(__file__).read().split("\n")[1:7])
        return
    if a[0] == "rd":
        pa, n = addr(a[1]), int(a[2], 0) if len(a) > 2 else 1
        for i in range(n):
            print("%#010x: %08x" % (pa + 4 * i, mmio(0, pa + 4 * i)))
    elif a[0] == "wr":
        pa, v = addr(a[1]), int(a[2], 16)
        print("%#010x <- %08x: %08x" % (pa, v, mmio(2, pa, v, int(a[3], 16)) if len(a) > 3 else mmio(1, pa, v)))
    elif a[0] == "ife":
        dump(BASES["ife"], [(0x0, 0x80), (0x4a0, 0x4b0), (0x2000, 0x2130)])
    elif a[0] == "wm":
        dump(BASES["ife"], [(0x2200, 0x2290)])
    elif a[0] == "rewm":
        # rewm BUFPA [FRAME_INC]: WM0 as downstream's RDI path (cam_vfe_bus_ver2.c): framedrop / irq subsample
        # pattern 1 period 0 (keep every frame), frame increment, fresh image address, enable, RDI0 reg update.
        ife, buf = BASES["ife"], int(a[1], 16)
        inc = int(a[2], 16) if len(a) > 2 else 0
        for off, v in ((0x2248, 0), (0x224c, 1), (0x2250, 0), (0x2254, 1), (0x2258, inc), (0x2214, buf),
                       (0x2208, 3)):
            mmio(1, ife + off, v)
        mmio(1, ife + 0x4ac, 1 << 1)
        dump(ife, [(0x2200, 0x2260)])
    elif a[0] == "csid":
        dump(BASES["csid"], [(0x0, 0x110), (0x300, 0x3f0)])
    else:
        raise SystemExit("unknown command " + a[0])


if __name__ == "__main__":
    main()
