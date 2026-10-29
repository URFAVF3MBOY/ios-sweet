#!/usr/bin/env python3
"""img2simg in plain Python (Windows, macOS, Linux; no Android tools needed).

  python img2simg.py RAW.img OUT.simg          convert
  python img2simg.py --check RAW.img OUT.simg  decode OUT.simg and compare it with RAW.img

Android sparse format v1.0 (libsparse: sparse_format.h), 4096-byte blocks. Like libsparse's
reader for a normal file, a block whose 4-byte words are all the same becomes a FILL chunk
(free space of a filesystem image is zeros, so it costs 16 bytes) and other blocks become RAW
chunks. No DONT_CARE chunks: those would leave the old flash contents in blocks the filesystem
uses (a file that happens to contain a zero page), so every block is written.
Streams the input; memory use stays under about 150 MiB (measured: 1 GiB in ~1 s on Linux). Flash with: fastboot -S 512M flash userdata OUT.simg
"""
import struct, sys, zlib

MAGIC = 0xED26FF3A
BLK = 4096
RAW, FILL = 0xCAC1, 0xCAC2
MAX_RAW_BLOCKS = 16384              # 64 MiB per RAW chunk
READ = 1 << 20                      # read 1 MiB at a time


def convert(src, dst):
    with open(src, 'rb') as f, open(dst, 'wb') as out:
        out.write(b'\0' * 28)       # header, filled in at the end
        nchunks = 0
        nblocks = 0
        pend_kind = None            # current chunk being built
        pend_fill = None
        pend_raw = []
        pend_n = 0

        def flush():
            nonlocal pend_kind, pend_n, pend_raw, nchunks
            if pend_kind == FILL:
                out.write(struct.pack('<HHII', FILL, 0, pend_n, 12 + 4))
                out.write(pend_fill)
                nchunks += 1
            elif pend_kind == RAW:
                out.write(struct.pack('<HHII', RAW, 0, pend_n, 12 + pend_n * BLK))
                out.write(b''.join(pend_raw))
                nchunks += 1
            pend_kind, pend_n, pend_raw = None, 0, []

        tail = b''
        while True:
            buf = f.read(READ)
            if not buf:
                break
            buf = tail + buf
            usable = len(buf) - len(buf) % BLK
            tail = buf[usable:]
            for o in range(0, usable, BLK):
                b = buf[o:o + BLK]
                word = b[:4]
                if b == word * (BLK // 4):
                    if pend_kind != FILL or pend_fill != word:
                        flush()
                        pend_kind, pend_fill = FILL, word
                    pend_n += 1
                else:
                    if pend_kind != RAW or pend_n >= MAX_RAW_BLOCKS:
                        flush()
                        pend_kind = RAW
                    pend_raw.append(b)
                    pend_n += 1
                nblocks += 1
        if tail:                    # last partial block, padded with zeros
            b = tail + b'\0' * (BLK - len(tail))
            if pend_kind != RAW:
                flush()
                pend_kind = RAW
            pend_raw.append(b)
            pend_n += 1
            nblocks += 1
        flush()
        out.seek(0)
        out.write(struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, BLK, nblocks, nchunks, 0))
    print(f'{dst}: {nblocks} blocks of {BLK}, {nchunks} chunks')


def check(src, simg):
    """Decode simg block by block and compare with src (CRC32 of both, block count)."""
    with open(simg, 'rb') as s, open(src, 'rb') as f:
        magic, major, minor, fh, ch, blk, total, chunks, _ = struct.unpack('<IHHHHIIII', s.read(28))
        if magic != MAGIC or major != 1 or fh != 28 or ch != 12:
            sys.exit('not a sparse v1 image')
        crc_s = crc_f = 0
        seen = 0
        for _ in range(chunks):
            kind, _, n, size = struct.unpack('<HHII', s.read(12))
            if kind == RAW:
                for _ in range(n):
                    crc_s = zlib.crc32(s.read(blk), crc_s)
            elif kind == FILL:
                pattern = s.read(4) * (blk // 4)
                for _ in range(n):
                    crc_s = zlib.crc32(pattern, crc_s)
            else:
                sys.exit(f'unexpected chunk type {kind:#x}')
            seen += n
        while True:
            b = f.read(READ)
            if not b:
                break
            crc_f = zlib.crc32(b, crc_f)
        pad = total * blk - f.tell()
        if pad:
            crc_f = zlib.crc32(b'\0' * pad, crc_f)
    ok = seen == total and crc_s == crc_f
    print(f'{simg}: {seen}/{total} blocks, crc {crc_s:08x} vs {crc_f:08x}: {"OK" if ok else "MISMATCH"}')
    return ok


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == '--check':
        sys.exit(0 if check(a[1], a[2]) else 1)
    if len(a) != 2:
        sys.exit(__doc__)
    convert(a[0], a[1])
