#!/var/jb/usr/bin/python3
"""Audio bring-up on the OnePlus 7T from a root shell, over the 0181 pipes (no kernel build per try).

  sweetaudio.py status        clocks, pins and SE4 of the amplifiers' I2C, read only
  sweetaudio.py i2c-init      SE4 (QUP0, 0x890000) as an I2C master at 400 kHz: clocks voted, pins 51/52 muxed
  sweetaudio.py tfa-id        each TFA98xx (0x34 right, 0x35 left): register 0x03 (revision)
  sweetaudio.py rd ADDR REG   one 16-bit TFA register (hex)
  sweetaudio.py apr-state     AVCS "are the audio services up" through kern.sweetapr, and the answer
  sweetaudio.py apr-read      everything the DSP sent since the last read
  sweetaudio.py afe-start [LINES]  quaternary MI2S: bit clock 0x106 on (1.536 MHz), port 0x1006 I2S 48 kHz 16-bit
                              stereo, word select ours, data on LINES (default 2 = SD1, the 7T DT's rx-lines 0x2), start
  sweetaudio.py afe-stop      port 0x1006 stopped, bit clock off
  sweetaudio.py fetch PATH OUT    a file from the vendor partition's ext4 (0184 kern.sweetpart, read only),
                              e.g. fetch /firmware/tfa98xx.cnt /var/root/tfa98xx.cnt

Numbers: live DT (i2c@890000, tfa98xx_right@34 reset gpio37, tfa98xx_left@35 reset gpio100,
qupv3_se4_i2c_active gpio51/52 function qup4); Linux pinctrl-sm8150.c (51, 52, 100 on SOUTH, 37 on NORTH,
qup4 = function 1); Linux gcc-sm8150.c (wrap0 votes in 0x5200c: m_ahb bit 6, s_ahb bit 7, s4 bit 14; s4 RCG
0x17608, branch 0x17604); the GENI I2C sequence is bsd/dev/sweet_touch.c's (0060/0063/0082).
The amplifiers' reset pins are only read here: TFA98xx reset is active high, nothing drives them yet.
"""
import ctypes
import os
import struct
import sys
import time

# 0181's kern.sweetmmio maps whole ranges and panics on TLMM (14 MB): pins only with a 0183 kernel, SWEET_TLMM=1
TLMM_OK = os.environ.get("SWEET_TLMM") == "1"

libc = ctypes.CDLL(None, use_errno=True)


def sysctl(name, new=None, oldlen=0):
    old = ctypes.create_string_buffer(oldlen) if oldlen else None
    olen = ctypes.c_size_t(oldlen)
    nbuf = ctypes.create_string_buffer(new, len(new)) if new is not None else None
    r = libc.sysctlbyname(name.encode(), old, ctypes.byref(olen) if old is not None else None,
                          nbuf, len(new) if new is not None else 0)
    if r != 0:
        e = ctypes.get_errno()
        raise OSError(e, "%s: %s" % (name, __import__("os").strerror(e)))
    return old.raw[:olen.value] if old is not None else b""


# ---- kern.sweetmmio ----

def mmio(op, pa, val=0, mask=0):
    req = struct.pack("<IIQII", op, 4, pa, val, mask)
    out = sysctl("kern.sweetmmio", req, len(req))
    return struct.unpack("<IIQII", out)[3]


def rd(pa):
    return mmio(0, pa)


def wr(pa, val):
    return mmio(1, pa, val)


def setbits(pa, val, mask):
    return mmio(2, pa, val, mask)


# ---- clocks (GCC) ----

GCC = 0x00100000
VOTE0 = GCC + 0x5200c           # QUP wrap0 votes
VOTE_MAHB, VOTE_SAHB, VOTE_S4 = 1 << 6, 1 << 7, 1 << 14
S4_RCG = GCC + 0x17608          # CMD, +4 CFG, +0x14 DFSR, +0x1c perf level 0
S4_BR = GCC + 0x17604
MAHB_BR, SAHB_BR = GCC + 0x17004, GCC + 0x17008


def wait_clear(pa, mask, tries=5000):
    for _ in range(tries):
        if not rd(pa) & mask:
            return True
    return False


def clocks_on():
    v = setbits(VOTE0, VOTE_MAHB | VOTE_SAHB | VOTE_S4, VOTE_MAHB | VOTE_SAHB | VOTE_S4)
    print("gcc: wrap0 vote %#x (bits 8/9, core clocks, as found: %d/%d)" % (v, v >> 8 & 1, v >> 9 & 1))
    dfs = rd(S4_RCG + 0x14)
    cfg = rd(S4_RCG + 4)
    if dfs & 1:
        lvl0 = rd(S4_RCG + 0x1c)
        print("gcc: s4 RCG in DFS mode, level 0 cfg %#x%s" % (lvl0, "" if lvl0 & 0x371f == 1 else " (NOT XO/1)"))
        if lvl0 & 0x371f != 1:
            return False
    else:
        wr(S4_RCG + 4, (cfg & ~(0x371f | 1 << 20)) | 1)      # XO, divider 1: 19.2 MHz
        setbits(S4_RCG, 1, 1)
        if not wait_clear(S4_RCG, 1):
            print("gcc: s4 RCG update did not finish")
            return False
    for name, pa, gated in (("m_ahb", MAHB_BR, False), ("s_ahb", SAHB_BR, True), ("s4", S4_BR, False)):
        on = wait_clear(pa, 1 << 31)
        print("gcc: branch %s %#x%s" % (name, rd(pa), "" if on else (" (hw-gated idle, ok)" if gated else " HALTED")))
        if not on and not gated:
            return False
    return True


# ---- pins (TLMM) ----

NORTH, SOUTH = 0x03900000, 0x03d00000
TILE = {37: NORTH, 51: SOUTH, 52: SOUTH, 100: SOUTH}


def pin_cfg(n):
    if not TLMM_OK:
        raise OSError("TLMM needs a 0183 kernel (SWEET_TLMM=1); 0181 panics mapping it")
    return TILE[n] + n * 0x1000


def pin_show(n, what):
    cfg, io = rd(pin_cfg(n)), rd(pin_cfg(n) + 4)
    print("pin %3d %-22s cfg %#05x (func %d, pull %d, drive %d mA, %s) level %d" % (
        n, what, cfg, cfg >> 2 & 0xf, cfg & 3, ((cfg >> 6 & 7) + 1) * 2, "out" if cfg & 0x200 else "in", io & 1))


def pins_i2c():
    if not TLMM_OK:
        print("pins: 51/52 not muxed (SWEET_TLMM=1 with a 0183 kernel): the transfer will likely fail")
        return
    for n in (51, 52):
        setbits(pin_cfg(n), 1 << 2, 0x3ff)     # function 1 (qup4), no pull, 2 mA, input


# ---- SE4 as I2C master (GENI) ----

SE = 0x00890000
SE_STATUS, SE_MCLK, SE_FW, SE_CLK_SEL, SE_DMA = 0x040, 0x048, 0x068, 0x07c, 0x258
SE_TX_LEN, SE_RX_LEN, SE_SCL = 0x26c, 0x270, 0x278
SE_CMD, SE_CTRL, SE_IRQ, SE_IRQ_EN, SE_IRQ_CLR = 0x600, 0x604, 0x610, 0x614, 0x618
SE_TX, SE_RX, SE_TX_COUNT, SE_RX_COUNT, SE_TX_WM = 0x700, 0x780, 0x800, 0x804, 0x80c
SE_HW_TX, SE_HW_RX = 0xe24, 0xe28
DONE, NACK, RX_WM, RX_LAST, TX_WM = 1, 1 << 10, 1 << 26, 1 << 27, 1 << 30
ERRORS = 0x3e | 1 << 9 | 1 << 10 | 1 << 12
PACK0, PACK1 = 0x0007f8fe, 0x000ffefe


def se(off):
    return rd(SE + off)


def se_w(off, v):
    wr(SE + off, v)


def se_info():
    fw, tx, rx = se(SE_FW), se(SE_HW_TX), se(SE_HW_RX)
    print("se4: fw %#x (protocol %d: %s), status %#x, tx fifo %#x, rx fifo %#x" % (
        fw, fw >> 8 & 0xff, {1: "SPI", 2: "UART", 3: "I2C"}.get(fw >> 8 & 0xff, "?"), se(SE_STATUS), tx, rx))
    return fw, tx, rx


def i2c_init():
    if not clocks_on():
        return False
    pins_i2c()
    fw, tx, rx = se_info()
    if (fw >> 8 & 0xff) != 3 or ((tx >> 24) & 0x3f) != 32 or ((rx >> 24) & 0x3f) != 32:
        print("se4: no I2C firmware / 32-bit FIFOs: stopped")
        return False
    if se(SE_STATUS) & 0x1001:
        print("se4: busy: stopped")
        return False
    for off, v in ((0xe18, 0), (0xe1c, 0), (0x644, 0), (SE_IRQ_CLR, 0xffffffff), (0x648, 0xffffffff), (SE_DMA, 0)):
        se_w(off, v)
    se_w(0x28, se(0x28) | 0x7f)
    se_w(0xe30, se(0xe30) | 0x0f)
    se_w(0x24, 0x7f)
    se_w(0x20, 1)
    se_w(SE_CLK_SEL, 0)
    se_w(SE_MCLK, (2 << 4) | 1)                 # 400 kHz from 19.2 MHz: div 2, t_high 5, t_low 12, t_cycle 24
    se_w(SE_SCL, (5 << 20) | (12 << 10) | 24)
    se_w(0x254, 0)
    for off in (0x260, 0x284):
        se_w(off, PACK0)
    for off in (0x264, 0x288):
        se_w(off, PACK1)
    se_w(SE_TX_WM, 0)
    se_w(0x810, 0)
    se_w(0x814, ((rx >> 16) & 0x3f) - 1)
    se_w(SE_IRQ_EN, DONE | ERRORS | RX_WM | RX_LAST | TX_WM)
    ok = se(0x260) == PACK0 and se(0x264) == PACK1 and se(0x284) == PACK0 and se(0x288) == PACK1
    print("se4: I2C master set up at 400 kHz%s" % ("" if ok else ", PACKING READBACK WRONG"))
    return ok


def abort():
    se_w(SE_CTRL, 2)
    for _ in range(2000):
        if se(SE_IRQ) & (1 << 5):
            break
    for _ in range(64):
        if not se(SE_RX_COUNT) & 0x1ffffff:
            break
        se(SE_RX)
    se_w(SE_IRQ_CLR, 0xffffffff)


def msg(addr, tx=b"", nr=0, stretch=False):
    """One FIFO message: a write of up to 3 bytes or a read of nr bytes (as the touch driver)."""
    assert (len(tx) == 0) != (nr == 0) and len(tx) <= 3 and nr <= 90
    if not wait_clear(SE + SE_STATUS, 1):
        raise OSError("se4 busy")
    se_w(SE_IRQ_CLR, 0xffffffff)
    se_w(SE_TX_WM, 1 if tx else 0)
    se_w(SE_RX_LEN if nr else SE_TX_LEN, nr if nr else len(tx))
    se_w(SE_CMD, ((2 if nr else 1) << 27) | (addr << 9) | (4 if stretch else 0))
    sent, got = 0, b""
    for _ in range(4000):
        irq = se(SE_IRQ)
        if irq & ERRORS:
            for _ in range(2000):
                if irq & DONE:
                    break
                irq = se(SE_IRQ)
            se_w(SE_IRQ_CLR, irq)
            if not irq & DONE:
                abort()
            raise OSError("i2c %#x: %s (irq %#x)" % (addr, "NACK" if irq & NACK else "error", irq))
        if tx and not sent and irq & TX_WM:
            se_w(SE_TX, int.from_bytes(tx, "little"))
            sent = len(tx)
            se_w(SE_TX_WM, 0)
        count = se(SE_RX_COUNT) & 0x1ffffff
        for _ in range(min(count, 64)):
            got += se(SE_RX).to_bytes(4, "little")
        if irq & DONE:
            se_w(SE_IRQ_CLR, irq)
            return got[:nr]
        se_w(SE_IRQ_CLR, irq & (RX_WM | RX_LAST | TX_WM))
    abort()
    raise OSError("i2c %#x: timeout" % addr)


def tfa_rd(addr, reg):
    msg(addr, bytes([reg]), stretch=True)       # register pointer, no stop
    b = msg(addr, nr=2)
    return b[0] << 8 | b[1]                     # TFA98xx registers: 16 bits, big endian


# ---- kern.sweetapr ----

APR_DOMAIN_ADSP, APR_DOMAIN_APPS = 4, 5


APR_HDR = 20                                    # field, size, src svc/domain/port, dst svc/domain/port, token, opcode


def apr_packet(svc, opcode, payload=b"", token=1, port=0):
    size = APR_HDR + len(payload)
    field = 2 << 8 | (APR_HDR // 4) << 4        # sequenced command, 5 header words
    return struct.pack("<HHBBHBBHII", field, size, svc, APR_DOMAIN_APPS, port, svc, APR_DOMAIN_ADSP, port,
                       token, opcode) + payload


def apr_read():
    data = sysctl("kern.sweetapr", oldlen=4096)
    out, at = [], 0
    while at + 4 <= len(data):
        n = struct.unpack_from("<I", data, at)[0]
        out.append(data[at + 4:at + 4 + n])
        at += 4 + n
    return out


def apr_show(p):
    if len(p) < APR_HDR:
        print("apr: %d bytes: %s" % (len(p), p.hex()))
        return
    field, size, fs, fd, fp, ts, td, tp, token, opcode = struct.unpack_from("<HHBBHBBHII", p)
    hl = max(APR_HDR, (field >> 4 & 0xf) * 4)
    words = [struct.unpack_from("<I", p, o)[0] for o in range(hl, len(p) - 3, 4)]
    print("apr: %d.%d -> %d.%d opcode %08x token %u: %s" % (fd, fs, td, ts, opcode, token,
                                                          " ".join("%08x" % w for w in words[:12])))


# ---- AFE (APR service 4): the quaternary MI2S port, as HackiOS vayu_audio.c (same SM8150) ----

APR_SVC_AFE = 4
APR_BASIC_RSP = 0x000110e8
AFE_PORT_CMD_DEVICE_START, AFE_PORT_CMD_DEVICE_STOP = 0x000100e5, 0x000100e6
AFE_PORT_CMD_SET_PARAM_V2, AFE_SVC_CMD_SET_PARAM = 0x000100ef, 0x000100f3
AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_I2S_CONFIG = 0x0001020c, 0x0001020d
AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET = 0x0001028f, 0x00010290
QUAT_MI2S_RX = 0x1006
CLOCK_QUAD_MI2S_IBIT = 0x106
RATE = 48000
_token = [0x100]


def apr_cmd(what, svc, opcode, payload):
    """Send one command, wait up to 1 s for its basic response; True if status 0."""
    _token[0] += 1
    tok = _token[0]
    apr_read()                                  # drop anything older
    sysctl("kern.sweetapr", apr_packet(svc, opcode, payload, token=tok))
    for _ in range(100):
        for p in apr_read():
            field, size, fs, fd, fp, ts, td, tp, token, op = struct.unpack_from("<HHBBHBBHII", p)
            hl = max(APR_HDR, (field >> 4 & 0xf) * 4)
            w = [struct.unpack_from("<I", p, o)[0] for o in range(hl, len(p) - 3, 4)]
            if op == APR_BASIC_RSP and w and w[0] == opcode:
                st = w[1] if len(w) > 1 else -1
                print("afe: %s: %s" % (what, "done" if st == 0 else "REFUSED, status %#x" % st))
                return st == 0
            print("afe: (other answer: opcode %08x token %u: %s)" % (op, token, " ".join("%08x" % x for x in w[:6])))
        time.sleep(0.01)
    print("afe: %s: NO ANSWER" % what)
    return False


def afe_param_hdr(module, param, size):
    return struct.pack("<IIHH", module, param, size, 0)


def afe_service_param(what, module, param, data):
    body = afe_param_hdr(module, param, len(data)) + data
    return apr_cmd(what, APR_SVC_AFE, AFE_SVC_CMD_SET_PARAM, struct.pack("<IIII", len(body), 0, 0, 0) + body)


def afe_port_param(what, port, module, param, data):
    body = afe_param_hdr(module, param, len(data)) + data
    return apr_cmd(what, APR_SVC_AFE, AFE_PORT_CMD_SET_PARAM_V2, struct.pack("<HHIII", port, len(body), 0, 0, 0) + body)


def afe_clock(on):
    clock = struct.pack("<IIIHHI", 1, CLOCK_QUAD_MI2S_IBIT, RATE * 16 * 2, 1, 0, 1 if on else 0)
    return afe_service_param("quaternary MI2S bit clock 0x106 %s" % ("on (1.536 MHz)" if on else "off"),
                             AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, clock)


def afe_start(lines=2):
    if not afe_clock(True):
        return False
    i2s = struct.pack("<IHHHHIHH", 1, 16, lines, 1, 1, RATE, 0, 0)    # version, bits, lines, stereo, ws ours, rate
    if not afe_port_param("port 0x1006: I2S, lines %d, stereo, 16 bits, 48 kHz, word select ours" % lines,
                          QUAT_MI2S_RX, AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_I2S_CONFIG, i2s):
        return False
    return apr_cmd("port 0x1006 started", APR_SVC_AFE, AFE_PORT_CMD_DEVICE_START, struct.pack("<HH", QUAT_MI2S_RX, 0))


def afe_stop():
    apr_cmd("port 0x1006 stopped", APR_SVC_AFE, AFE_PORT_CMD_DEVICE_STOP, struct.pack("<HH", QUAT_MI2S_RX, 0))
    afe_clock(False)


# ---- kern.sweetpart (0184) and a read-only ext4 reader, for /vendor/firmware/tfa98xx.cnt ----

def part_read(name, off, n):
    out = b""
    while n > 0:
        k = min(n, 65536)
        req = struct.pack("<32sQII", name.encode(), off, k, 0)
        out += sysctl("kern.sweetpart", req, k)
        off += k
        n -= k
    return out


class Ext4:
    def __init__(self, part):
        self.part = part
        sb = part_read(part, 1024, 1024)
        if struct.unpack_from("<H", sb, 56)[0] != 0xEF53:
            raise OSError("%s: no ext4 superblock" % part)
        self.bs = 1024 << struct.unpack_from("<I", sb, 24)[0]
        self.ipg = struct.unpack_from("<I", sb, 40)[0]
        self.isz = struct.unpack_from("<H", sb, 88)[0] if struct.unpack_from("<I", sb, 76)[0] >= 1 else 128
        incompat = struct.unpack_from("<I", sb, 96)[0]
        self.is64 = bool(incompat & 0x80)
        self.dsz = struct.unpack_from("<H", sb, 254)[0] if self.is64 else 32
        self.gdt = (2 if self.bs == 1024 else 1) * self.bs

    def block(self, n, count=1):
        return part_read(self.part, n * self.bs, count * self.bs)

    def inode(self, ino):
        g, i = (ino - 1) // self.ipg, (ino - 1) % self.ipg
        d = part_read(self.part, self.gdt + g * self.dsz, self.dsz)
        table = struct.unpack_from("<I", d, 8)[0] | ((struct.unpack_from("<I", d, 0x28)[0] << 32) if self.is64 and self.dsz >= 64 else 0)
        return part_read(self.part, table * self.bs + i * self.isz, self.isz)

    def extents(self, node):
        magic, entries, _, depth = struct.unpack_from("<HHHH", node, 0)
        if magic != 0xF30A:
            raise OSError("bad extent header")
        out = []
        for e in range(entries):
            o = 12 + 12 * e
            if depth == 0:
                lblk, ln, hi, lo = struct.unpack_from("<IHHI", node, o)
                if ln > 32768:
                    ln -= 32768             # uninitialized extent: reads as zeros, length still counts
                out.append((lblk, ln, hi << 32 | lo))
            else:
                lblk, lo, hi = struct.unpack_from("<IIH", node, o)
                out += self.extents(self.block(hi << 32 | lo))
        return out

    def data(self, ino):
        inode = self.inode(ino)
        size = struct.unpack_from("<I", inode, 4)[0] | struct.unpack_from("<I", inode, 108)[0] << 32
        flags = struct.unpack_from("<I", inode, 32)[0]
        iblock = inode[40:100]
        buf = bytearray()
        if flags & 0x80000:                 # extents
            for lblk, ln, phys in sorted(self.extents(iblock)):
                if len(buf) < lblk * self.bs:
                    buf += bytes(lblk * self.bs - len(buf))
                buf += self.block(phys, ln)
        elif flags & 0x10000000:            # inline data
            buf += iblock
        else:                               # direct blocks only (enough for small directories/files)
            for b in struct.unpack_from("<12I", iblock, 0):
                if b:
                    buf += self.block(b)
        return bytes(buf[:size])

    def lookup(self, path):
        ino = 2
        for name in [x for x in path.split("/") if x]:
            d, at, found = self.data(ino), 0, None
            while at + 8 <= len(d):
                child, rec, nlen = struct.unpack_from("<IHB", d, at)
                if rec < 8:
                    break
                if child and d[at + 8:at + 8 + nlen].decode(errors="replace") == name:
                    found = child
                    break
                at += rec
            if found is None:
                raise OSError("%s: %s not found" % (path, name))
            ino = found
        return ino


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "status"
    if cmd == "status":
        v = rd(VOTE0)
        print("gcc: wrap0 vote %#x: m_ahb %d s_ahb %d s4 %d core %d/%d" % (
            v, v >> 6 & 1, v >> 7 & 1, v >> 14 & 1, v >> 8 & 1, v >> 9 & 1))
        print("gcc: s4 rcg %#x cfg %#x dfsr %#x, branch %#x" % (rd(S4_RCG), rd(S4_RCG + 4), rd(S4_RCG + 0x14), rd(S4_BR)))
        if TLMM_OK:
            for n, what in ((51, "SE4 SDA"), (52, "SE4 SCL"), (37, "TFA right reset"), (100, "TFA left reset")):
                pin_show(n, what)
        else:
            print("pins: not read (SWEET_TLMM=1 with a 0183 kernel)")
        if v & (VOTE_MAHB | VOTE_SAHB) == VOTE_MAHB | VOTE_SAHB and not rd(MAHB_BR) >> 31:
            se_info()
        else:
            print("se4: AHB clocks not on: its registers are not read (i2c-init votes them)")
    elif cmd == "i2c-init":
        i2c_init()
    elif cmd == "tfa-id":
        for addr, side in ((0x34, "right"), (0x35, "left")):
            try:
                print("tfa %#x (%s): register 0x03 = %#06x" % (addr, side, tfa_rd(addr, 0x03)))
            except OSError as e:
                print("tfa %#x (%s): %s" % (addr, side, e))
    elif cmd == "rd":
        a, r = int(argv[2], 16), int(argv[3], 16)
        print("%#x reg %#04x = %#06x" % (a, r, tfa_rd(a, r)))
    elif cmd == "apr-state":
        sysctl("kern.sweetapr", apr_packet(3, 0x0001290c, token=0x77))
        time.sleep(0.2)
        for p in apr_read():
            apr_show(p)
    elif cmd == "afe-start":
        afe_start(int(argv[2]) if len(argv) > 2 else 2)
    elif cmd == "afe-stop":
        afe_stop()
    elif cmd == "fetch":
        fs = Ext4(argv[4] if len(argv) > 4 else "vendor_a")
        data = fs.data(fs.lookup(argv[2]))
        open(argv[3], "wb").write(data)
        print("%s: %d bytes -> %s (block size %d)" % (argv[2], len(data), argv[3], fs.bs))
    elif cmd == "apr-read":
        for p in apr_read():
            apr_show(p)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
