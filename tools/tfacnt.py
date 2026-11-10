#!/usr/bin/env python3
"""Dump an NXP TFA98xx container (tfa98xx.cnt): devices, profiles and their items.

Layout from NXP's tfa_container.h (tfa98xx Linux driver): header "PM", version, subversion, u32 size, u32 CRC,
u16 rev, customer[8], application[8], type[8], u16 ndev, u16 nprof, then descriptor pointers
(u32: offset in the low 24 bits, type in the high 8). A device list: u8 length, bus, dev, func, u32 devid,
name pointer, items. A profile list: u8 length, u24 id, name pointer, items. Item types as in the v3 driver
(dscBitfield = 0x10: u16 value, u16 field = len-1:4 | pos:4 | address:8).

  tfacnt.py FILE            dump
"""
import struct
import sys

TYPES = {0: "device", 1: "profile", 2: "register", 3: "string", 4: "file", 5: "patch", 6: "marker", 7: "mode",
         8: "set-input-select", 9: "set-output-select", 10: "set-program-config", 11: "set-lagw", 12: "set-gains",
         13: "set-vbat-factors", 14: "set-senses-cal", 15: "set-senses-delay", 16: "bitfield", 17: "default",
         18: "live-data", 19: "live-data-string", 20: "group", 21: "cmd", 22: "set-mb-drc", 23: "filter",
         24: "no-init", 25: "features", 26: "cf-mem", 27: "set-fwk-use-case", 28: "set-vddp-config",
         29: "tfa-hal", 30: "info-text", 31: "info-file"}


def ptr(data, at):
    v = struct.unpack_from("<I", data, at)[0]
    return v & 0xffffff, v >> 24


def cstr(data, off):
    end = data.index(b"\0", off)
    return data[off:end].decode(errors="replace")


def item(data, off, typ):
    t = TYPES.get(typ, "type %#x" % typ)
    if typ == 3:
        return "string %r" % cstr(data, off)
    if typ == 2:
        addr, value, mask = struct.unpack_from("<BHH", data, off)
        return "register %#04x = %#06x (mask %#06x)" % (addr, value, mask)
    if typ == 16:
        value, field = struct.unpack_from("<HH", data, off)
        return "bitfield reg %#04x bits %d..%d = %#x" % (field >> 8, field >> 4 & 0xf,
                                                       (field >> 4 & 0xf) + (field & 0xf), value)
    if typ == 7:
        return "mode %d" % struct.unpack_from("<I", data, off)[0]
    if typ in (4, 5, 31):
        name_off, _ = ptr(data, off)
        size = struct.unpack_from("<I", data, off + 4)[0]
        hdr = data[off + 8:off + 8 + 4]
        return "%s %r, %d bytes, starts %r" % (t, cstr(data, name_off), size, hdr)
    return "%s at %#x" % (t, off)


def dump(path):
    data = open(path, "rb").read()
    hid, ver, sub, size, crc, rev = struct.unpack_from("<2s2s2sIIH", data, 0)
    customer, app, typ = (data[16 + 8 * i:24 + 8 * i].rstrip(b"\0").decode() for i in range(3))
    ndev, nprof = struct.unpack_from("<HH", data, 40)
    print("%s v%s%s: %d bytes, crc %#x, rev %d, customer %r, application %r, type %r, %d devices, %d profiles" % (
        hid.decode(), ver.decode(), sub.decode(), size, crc, rev, customer, app, typ, ndev, nprof))
    seen = set()

    def profile(off):
        length = data[off]
        pid = int.from_bytes(data[off + 1:off + 4], "little")
        name_off, _ = ptr(data, off + 4)
        print("    profile %r (id %#x), %d items" % (cstr(data, name_off), pid, length))
        for k in range(length):
            o, tt = ptr(data, off + 8 + 4 * k)
            print("      %2d: %s" % (k, item(data, o, tt)))

    at = 46                                     # v3: index after a u16 (0 here), up to the 0xa5a5a5 marker
    while at + 4 <= len(data):
        off, t = ptr(data, at)
        at += 4
        if t == 6 or off == 0xa5a5a5:
            break
        if t != 0:
            print("\nindex: type %d at %#x" % (t, off))
            continue
        length, bus, dev, func, devid = struct.unpack_from("<BBBBI", data, off)
        name_off, _ = ptr(data, off + 8)
        print("\ndevice %r: i2c %#x bus %d func %d devid %#x, %d items" % (cstr(data, name_off), dev, bus, func,
                                                                        devid, length))
        for k in range(length):
            o, tt = ptr(data, off + 12 + 4 * k)
            if tt == 1:
                profile(o)
            else:
                print("  %2d: %s" % (k, item(data, o, tt)))

if __name__ == "__main__":
    dump(sys.argv[1] if len(sys.argv) > 1 else "backup/vendor-audio/tfa98xx.cnt")
