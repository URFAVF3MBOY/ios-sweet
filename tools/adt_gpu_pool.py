#!/usr/bin/env python3
# adt_gpu_pool.py IN.adt OUT.adt [--mb 1024]: takes MB off the top of /chosen dram-size, so XNU never
# manages that RAM (no physmap view of it), and names it /chosen sweet-gpu-pool = <base, size> (two u64).
# Kernel 0225 makes GPU buffers there, write-combined, with no cache upkeep at submission.
# On the 7T's 7 GiB ADT (dram 0xc0000000 + 0x1c0000000): --mb 1024 gives the pool 0x240000000..0x280000000,
# inside the live FDT's 0x180000000 + 0x100000000 bank, clear of every reserved-memory carve-out
# (all below 0xc0000000) and of the shim's scratch (0xb0000000..).
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt

POOL = 'sweet-gpu-pool'
PAGE = 16 << 10


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--mb', type=int, default=1024)
    a = ap.parse_args()
    buf = open(a.src, 'rb').read()
    root, end = adt.parse(buf)
    assert adt.serialize(root) == buf[:end], 'ADT does not round-trip'
    chosen = next(c for c in root.children if c.name == 'chosen')
    assert chosen.prop(POOL) is None, 'ADT already has ' + POOL
    base = struct.unpack('<Q', chosen.prop('dram-base'))[0]
    size = struct.unpack('<Q', chosen.prop('dram-size'))[0]
    pool = a.mb << 20
    assert pool % PAGE == 0 and 0 < pool <= size // 2, 'pool size'
    left = size - pool
    chosen.props = [(k, f, struct.pack('<Q', left) if k == 'dram-size' else v) for k, f, v in chosen.props]
    chosen.props.append((POOL, 0, struct.pack('<2Q', base + left, pool)))
    out = adt.serialize(root)
    open(a.dst, 'wb').write(out)
    print('%s: dram %#x + %#x (was + %#x), %s %#x + %#x (%d MB)' % (a.dst, base, left, size, POOL, base + left, pool, a.mb))


if __name__ == '__main__':
    main()
