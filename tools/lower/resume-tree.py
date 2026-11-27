#!/usr/bin/env python3
"""Lower the thin arm64e Mach-Os of ROOT into OUT that are not there yet (resume after a stop).

An output whose size differs from its input (a write cut short) is done again. Each image goes through
rewrite-zeta-userland.py in its own process; one JSON line per image goes to the log.

  resume-tree.py --root ROOT --output OUT --apple-trust-cache TC [...] --jobs N --log LOG
"""
import argparse
import json
import multiprocessing
import os
from pathlib import Path
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ARM64E = (0x0100000c, 2)


def thin_arm64e(path):
    try:
        with open(path, 'rb') as f:
            head = f.read(16)
    except OSError:
        return False
    if len(head) < 16:
        return False
    magic, cputype, subtype, filetype = struct.unpack('<IIII', head)
    return magic == 0xfeedfacf and cputype == ARM64E[0] and subtype & 0xff == ARM64E[1] and filetype in (2, 6, 7, 8)


def work(job):
    src, dst, report, trust, tool = job
    for p in (dst, report):
        if p.exists():
            p.unlink()
    dst.parent.mkdir(parents=True, exist_ok=True)
    report.parent.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, str(HERE / tool), '--input', str(src), '--output', str(dst),
           '--report', str(report)] + [x for t in trust for x in ('--apple-trust-cache', t)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return dict(path=str(src), rc=r.returncode, error=r.stderr.strip()[-300:] if r.returncode else '')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--apple-trust-cache', action='append', required=True)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--log', type=Path, required=True)
    ap.add_argument('--fast', action='store_true', help='fast-userland.py (no code proof) instead of the proven tool')
    a = ap.parse_args()
    seen, jobs, done = set(), [], 0
    for d, _, names in os.walk(a.root):
        for n in names:
            src = Path(d) / n
            if src.is_symlink() or not src.is_file():
                continue
            st = src.stat()
            if (st.st_dev, st.st_ino) in seen or not thin_arm64e(src):
                continue
            seen.add((st.st_dev, st.st_ino))
            rel = src.relative_to(a.root)
            dst = a.output / rel
            if dst.exists() and dst.stat().st_size == st.st_size:
                done += 1
                continue
            jobs.append((src, dst, a.output.parent / 'rep' / 'images' / (str(rel).replace('/', '__') + '.json'),
                         a.apple_trust_cache, 'fast-userland.py' if a.fast else 'rewrite-zeta-userland.py'))
    print(f'{done} already done, {len(jobs)} to do', flush=True)
    fails = 0
    with multiprocessing.Pool(a.jobs) as pool, a.log.open('a') as log:
        for i, r in enumerate(pool.imap_unordered(work, jobs), 1):
            log.write(json.dumps(r) + '\n')
            fails += r['rc'] != 0
            if i % 200 == 0:
                print(f'{i}/{len(jobs)} done, {fails} failed', flush=True)
    print(f'finished: {len(jobs)} done now, {fails} failed', flush=True)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
