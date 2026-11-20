#!/usr/bin/env python3
"""Kernel profiler (0114, sweetprof=NAME) output: where the samples land, by library and pc.  prof-summary.py LOG STARTLINE [N]"""
import re, sys, collections

# PROF lines from sleh.c / sweet_sample.c (0114): "PROF pc:count pc:count ..." (top 640 of a 10 s round),
# leaf user pcs are 4-aligned, callers carry tag 1 in the low bits, kernel frames are fffffe... (depth in low bits).
log, start = sys.argv[1], int(sys.argv[2])
lines = open(log, errors='replace').read().split('\n')[start:]
regions = {}
leaf = collections.Counter()
callers = collections.Counter()
rounds = 0
for l in lines:
    m = re.match(r'.*PROFREG ([0-9a-f]+)-([0-9a-f]+) ([0-9a-f]+) (.*)$', l)
    if m:
        regions[(int(m.group(1), 16), int(m.group(2), 16))] = (int(m.group(3), 16), m.group(4).strip())
        continue
    if re.match(r'.*PROF \S+\[\d+\] round', l):
        rounds += 1
        continue
    if 'PROF ' in l:
        for pc, n in re.findall(r'([0-9a-f]{6,16}):(\d+)', l):
            v = int(pc, 16)
            if v >> 40 == 0xfffffe:
                continue
            (callers if v & 3 else leaf)[v & ~3] += int(n)


def where(pc):
    for (lo, hi), (off, path) in regions.items():
        if lo <= pc < hi:
            name = path.split('/')[-1] if path != '(no file)' else 'shared cache'
            return name, pc - lo + off
    return '?', pc


by = collections.Counter()
for pc, n in leaf.items():
    by[where(pc)[0]] += n
tot = sum(by.values())
print(f'{rounds} rounds, {tot} leaf samples in the top lists')
for k, n in by.most_common():
    print(f'  {n * 100 / tot:5.1f}%  {k}')
print('\ntop leaf pcs:')
for pc, n in leaf.most_common(int(sys.argv[3]) if len(sys.argv) > 3 else 40):
    name, off = where(pc)
    print(f'  {n:6d}  {pc:#x}  {name}+{off:#x}')
