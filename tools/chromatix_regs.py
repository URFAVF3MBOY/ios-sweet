#!/usr/bin/env python3
# chromatix_regs.py MODULE.bin [--all]: register tables from a Qualcomm CamX sensor module file
# (com.qti.sensormodule.*.bin, "QTI Chromatix Header / Parameter Parser V1.3.2"), as found on the OnePlus 7T.
#
# Format (reversed 2026-11-12 from com.qti.sensormodule.semco_imx586.bin; no public description found):
#   0x0000  "QTI Chromatix Header", u32 file size, version string at 0x28
#   0x0310  schema: 0x48-byte records  ff ff ff ff 00 00 00 00 | u64 offset | u64 size | u64 id | char name[40]
#           (one (slaveAddr, registerData, delayUs) triple per register write; ~10000 records)
#   ~0xb6b50 value stream, 64-bit words in schema order. A register write appears as
#           [id] ADDR COUNT [id] ADDRTYPE DATATYPE OP X [id] DELAY
#           (ADDRTYPE 2 = 16-bit address, DATATYPE 1 = 8-bit data, OP 0 = write); the register VALUES are not
#           there: they follow all register entries as a pool of (value, 0) word pairs, COUNT values per write,
#           in the same order (checked: the init table starts 0x0136, 0x0137 and the pool 0x13, 0x33 = EXCK_FREQ
#           for a 19.2 MHz MCLK).
# Output: one table per run of consecutive register entries (a gap in the stream starts a new table), as
#   "addr value" lines; tables are numbered in file order. With --all every table is printed, else a summary.
import struct, sys


def words(d, start):
    return [struct.unpack_from('<Q', d, o)[0] for o in range(start, len(d) - 7, 8)]


def schema_end(d):
    i, last = 0x310, 0x310
    while i < len(d) - 0x48:
        if d[i:i + 8] == b'\xff\xff\xff\xff\x00\x00\x00\x00':
            name = d[i + 0x20:i + 0x48].split(b'\0')[0]
            if name and all(32 <= c < 127 for c in name):
                last = i + 0x48
                i += 0x48
                continue
        if i - last > 0x400:
            break
        i += 8
    return last


def parse(path):
    d = open(path, 'rb').read()
    if not d.startswith(b'QTI Chromatix Header'):
        raise SystemExit('not a Chromatix file')
    base = schema_end(d)
    w = words(d, base)
    entries = []                    # (word index, addr, count)
    i = 0
    # [id] ADDR COUNT [id+1] ADDRTYPE DATATYPE OP X [id+2] DELAY  -> 10 words
    while i + 10 <= len(w):
        idv = w[i]
        if (w[i + 3] == idv + 1 and w[i + 8] == idv + 2 and w[i + 4] in (1, 2, 3, 4) and
                w[i + 5] in (1, 2, 3, 4) and w[i + 2] in range(1, 65) and w[i + 1] < 0x10000):
            entries.append((i, w[i + 1], w[i + 2], w[i + 4], w[i + 5], w[i + 6], w[i + 9]))
            i += 10
            continue
        i += 1
    if not entries:
        raise SystemExit('no register entries found')
    return_pools = []
    # each table (a run of consecutive entries) is followed by its own value pool: (value, 0) word pairs, starting
    # right after the last entry's third id (its 10th word is already the first value)
    runs, cur = [], [entries[0]]
    for e in entries[1:]:
        if e[0] == cur[-1][0] + 10:
            cur.append(e)
        else:
            runs.append(cur)
            cur = [e]
    runs.append(cur)
    pool = []
    for run in runs:
        q = run[-1][0] + 9
        need = sum(e[2] for e in run)
        for k in range(need):
            pool.append(w[q + 2 * k] if q + 2 * k < len(w) else None)
    p = runs[0][-1][0] + 9
    return d, base, entries, pool, p


def tables(entries, pool):
    out, cur, k, prev = [], [], 0, None
    for e in entries:
        idx, addr, count, at, dt, op, delay = e
        if prev is not None and idx != prev + 10:
            out.append(cur)
            cur = []
        vals = pool[k:k + count]
        k += count
        cur.append((addr, vals, at, dt, op, delay))
        prev = idx
    if cur:
        out.append(cur)
    return out


def main():
    if len(sys.argv) < 2:
        print(open(sys.argv[0]).read().split('import')[0])
        return
    d, base, entries, pool, p = parse(sys.argv[1])
    t = tables(entries, pool)
    print('# %s: schema ends %#x, %d register writes in %d tables, value pool at stream word %d (%d values)'
          % (sys.argv[1], base, len(entries), len(t), p, len(pool)))
    for n, tab in enumerate(t):
        first = ' '.join('%04x' % a for a, *_ in tab[:6])
        print('# table %d: %d writes, first %s' % (n, len(tab), first))
        if '--all' in sys.argv:
            for addr, vals, at, dt, op, delay in tab:
                print('%04x %s%s' % (addr, ' '.join('%02x' % v for v in vals),
                                     ('  (delay %u us)' % delay) if delay else ''))


if __name__ == '__main__':
    main()
