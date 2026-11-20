#!/usr/bin/env python3
# raw10png.py IN.bin OUT.png WIDTH HEIGHT [STRIDE] [--gain N]: MIPI RAW10 (4 pixels in 5 bytes: 4 high bytes, then
# one byte of the 4 low bit pairs) Bayer data as an 8-bit grey PNG (no demosaic: every pixel its own value, so the
# Bayer pattern shows as fine texture; enough to see whether a picture is there). Pure Python + zlib.
# STRIDE defaults to WIDTH * 10 / 8 bytes. --gain multiplies (dark rooms). Also prints min/mean/max.
import struct, sys, zlib


def main():
    a = [x for x in sys.argv[1:] if not x.startswith("--")]
    gain = 1.0
    if "--gain" in sys.argv:
        gain = float(sys.argv[sys.argv.index("--gain") + 1])
        a = [x for x in a if x != sys.argv[sys.argv.index("--gain") + 1]]
    src, dst, w, h = a[0], a[1], int(a[2]), int(a[3])
    stride = int(a[4]) if len(a) > 4 else w * 10 // 8
    d = open(src, "rb").read()
    rows, lo, hi, total, n = [], 1023, 0, 0, 0
    for y in range(h):
        line = d[y * stride:y * stride + w * 10 // 8]
        if len(line) < w * 10 // 8:
            break
        px = bytearray(w)
        for g in range(w // 4):
            b = line[g * 5:g * 5 + 5]
            for k in range(4):
                v = (b[k] << 2) | ((b[4] >> (2 * k)) & 3)
                lo, hi = min(lo, v), max(hi, v)
                total += v
                n += 1
                px[g * 4 + k] = min(255, int((v >> 2) * gain))
        rows.append(b"\0" + bytes(px))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    hh = len(rows)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, hh, 8, 0, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)
    print("%s: %dx%d, 10-bit min %d mean %.1f max %d" % (dst, w, hh, lo, total / max(n, 1), hi))


if __name__ == "__main__":
    main()
