#!/usr/bin/env python3
# rawview.py IN OUT.png [--grey] [--bayer RGGB|GRBG|GBRG|BGGR] [--w 1296 --h 736 --stride 1632]: IMX586 RAW10 frame
# (camshot.sh) as a half-size picture: each 2x2 Bayer cell one pixel, black level 64 subtracted, grey-world white
# balance, stretched so the 99.5th percentile is white, gamma 2.2. --grey: luminance only. Pure Python + zlib.
import struct, sys, zlib


def arg(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def main():
    src, dst = sys.argv[1], sys.argv[2]
    w, h, st = int(arg("--w", 1296)), int(arg("--h", 736)), int(arg("--stride", 1632))
    pat = arg("--bayer", "RGGB").upper()
    d = open(src, "rb").read()
    px = []
    for y in range(h):
        line = d[y * st:y * st + w * 5 // 4]
        row = [0] * w
        for g in range(w // 4):
            b = line[g * 5:g * 5 + 5]
            for k in range(4):
                row[g * 4 + k] = max(0, ((b[k] << 2) | ((b[4] >> (2 * k)) & 3)) - 64)
        px.append(row)
    idx = {c: [i for i, ch in enumerate(pat) if ch == c] for c in "RGB"}
    cells = []
    sums = [0.0, 0.0, 0.0]
    for y in range(0, h - 1, 2):
        out = []
        for x in range(0, w - 1, 2):
            q = (px[y][x], px[y][x + 1], px[y + 1][x], px[y + 1][x + 1])
            r = q[idx["R"][0]]
            g = (q[idx["G"][0]] + q[idx["G"][1]]) / 2
            b = q[idx["B"][0]]
            out.append((r, g, b))
            sums[0] += r; sums[1] += g; sums[2] += b
        cells.append(out)
    gains = [sums[1] / s if s else 1.0 for s in sums]
    grey = "--grey" in sys.argv
    vals = sorted(max(c[0] * gains[0], c[1], c[2] * gains[2]) for row in cells for c in row)
    hi = max(vals[int(len(vals) * 0.995)], 1.0)
    print("%s: %dx%d, wb gains R %.2f B %.2f, white at %.1f/959 above black" % (dst, len(cells[0]), len(cells),
          gains[0], gains[2], hi))

    def tone(v):
        return int(255 * min(1.0, max(0.0, v / hi)) ** (1 / 2.2))
    rows = []
    for row in cells:
        if grey:
            rows.append(b"\0" + bytes(tone(0.3 * c[0] * gains[0] + 0.59 * c[1] + 0.11 * c[2] * gains[2]) for c in row))
        else:
            rows.append(b"\0" + b"".join(bytes((tone(c[0] * gains[0]), tone(c[1]), tone(c[2] * gains[2]))) for c in row))

    def chunk(t, x):
        return struct.pack(">I", len(x)) + t + x + struct.pack(">I", zlib.crc32(t + x) & 0xffffffff)
    hdr = struct.pack(">IIBBBBB", len(cells[0]), len(cells), 8, 0 if grey else 2, 0, 0, 0)
    open(dst, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr) + chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) +
                          chunk(b"IEND", b""))


if __name__ == "__main__":
    main()
