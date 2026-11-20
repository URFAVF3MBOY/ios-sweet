#!/usr/bin/env python3
"""Inclusive view of a kernel-profiler run (0114): every user frame in the PROF lines (leaf pcs and
callers, tag 1 in the low bits) counted per function, so a function's count is the time spent in it
or below it. Functions are named from symbol tables (nm -n output) for the dylibs PROFREG names.

  prof-callers.py LOG STARTLINE NAME=NMFILE ... [N]
  e.g. libredwood-gl.dylib=/tmp/rw.nm libOSMesa.8.dylib=/tmp/osmesa.nm
"""
import bisect, collections, re, sys

log, start = sys.argv[1], int(sys.argv[2])
syms, top = {}, 40
for a in sys.argv[3:]:
    if '=' in a:
        name, path = a.split('=', 1)
        tab = []
        for l in open(path, errors='replace'):
            p = l.split()
            if len(p) >= 3 and p[1] in 'tT':
                tab.append((int(p[0], 16), p[2]))
        tab.sort()
        syms[name] = tab
    else:
        top = int(a)

regions = {}
incl = collections.Counter()
for l in open(log, errors='replace').read().split('\n')[start:]:
    m = re.match(r'.*PROFREG ([0-9a-f]+)-([0-9a-f]+) ([0-9a-f]+) (.*)$', l)
    if m:
        regions[(int(m.group(1), 16), int(m.group(2), 16))] = (int(m.group(3), 16), m.group(4).strip().split('/')[-1])
        continue
    if 'PROF ' in l and 'round' not in l:
        for pc, n in re.findall(r'([0-9a-f]{6,16}):(\d+)', l):
            v = int(pc, 16)
            if v >> 40 == 0xfffffe:
                continue
            incl[v & ~3] += int(n)

by = collections.Counter()
for pc, n in incl.items():
    for (lo, hi), (off, name) in regions.items():
        if lo <= pc < hi and name in syms:
            tab = syms[name]
            i = bisect.bisect_right(tab, (pc - lo + off, '~')) - 1
            by[f'{name}: {tab[i][1] if i >= 0 else "?"}'] += n
            break
for k, v in by.most_common(top):
    print(f'{v:7d}  {k}')
