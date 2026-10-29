#!/usr/bin/env python3
"""fps from a recorder file (op7t-test.sh fps / ssh0129/fps-ab.sh): redwood-gl frame lines, fps = frames / kernel time."""
import re, sys

# redwood-gl samples: "[  701.53]: |backboardd| redwood-gl: frame 6220: 699 GL calls, 137 draws, ... 13613 us (4214 handing on, 931 waiting, 0 copying out), 4760 before it ..."
pat = re.compile(r'\[\s*([\d.]+)\]: \|backboardd\| redwood-gl: frame (\d+): (\d+) GL calls, (\d+) draws.*?; (\d+) us \((\d+) handing on, (\d+) waiting, (\d+) copying out\), (\d+) before')
flip = re.compile(r'\[\s*([\d.]+)\]: (?:SWEET|HOTDOGB) SCAN: flip (\d+)')
seen, flips = {}, {}
for line in open(sys.argv[1], errors='replace'):
    m = pat.search(line)
    if m:
        seen[int(m.group(2))] = (float(m.group(1)), *map(int, m.group(3, 4, 5, 6, 7, 9)))
    m = flip.search(line)
    if m:
        flips[int(m.group(2))] = float(m.group(1))
frames = sorted(seen)
print(f'{len(frames)} samples, frames {frames[0]}..{frames[-1]}')
print('  t(s)   frame  fps(interval)  calls draws  us/frame  handing waiting  before')
busy = []
for a, b in zip(frames, frames[1:]):
    ta, tb = seen[a][0], seen[b][0]
    fps = (b - a) / (tb - ta) if tb > ta else 0
    _, calls, draws, us, hand, wait, before = seen[b]
    print(f'{tb:7.2f} {b:6d} {fps:8.1f}      {calls:5d} {draws:5d} {us:8d} {hand:8d} {wait:7d} {before:7d}')
    if fps > 20:
        busy.append((fps, us, hand, wait, before, draws))
if busy:
    n = len(busy)
    avg = lambda i: sum(x[i] for x in busy) / n
    s = sorted(x[0] for x in busy)
    print(f'\nanimating intervals (>20 fps): {n}; fps mean {avg(0):.1f}, median {s[n // 2]:.1f}, min {s[0]:.1f}, max {s[-1]:.1f}')
    print(f'sampled frame: {avg(1):.0f} us total, {avg(2):.0f} handing on, {avg(3):.0f} waiting, {avg(4):.0f} before it, {avg(5):.0f} draws')
fl = sorted(flips)
if len(fl) > 1:
    print(f'scanout flips: {fl[0]}..{fl[-1]} over {flips[fl[-1]] - flips[fl[0]]:.1f} s')
