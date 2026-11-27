#!/usr/bin/env python3
# screenrec-video.py IN OUT.gif [--only SID] [--every N]: tools/screenrec.py's recording as an animated GIF (PC side).
#
# Every surface the recording holds is shown side by side at each tick (the panel shows one of them; which one is
# not recorded), unless --only picks one. Prints, per surface, how many ticks changed its picture.
import sys
from PIL import Image, ImageDraw

W, H, FRAME = 270, 600, 270 * 600 * 3
args = sys.argv[1:]
only = int(args[args.index("--only") + 1]) if "--only" in args else None
every = int(args[args.index("--every") + 1]) if "--every" in args else 1
src, out = args[0], args[1]

data = open(src, "rb").read()
head_end = data.index(b"\n")
sids = [int(x) for x in data[:head_end].split(b"surfaces ")[1].split()]
if only is not None:
    sids = [only]
pos = head_end + 1
ticks, cur = [], {}
while pos < len(data):
    nl = data.index(b"\n", pos)
    _, sid, t = data[pos:nl].split()
    sid, t = int(sid), float(t)
    pix = data[nl + 1:nl + 1 + FRAME]
    pos = nl + 1 + FRAME
    if sid in cur:                       # a new tick starts when a surface comes round again
        ticks.append(cur)
        cur = {}
    cur[sid] = (t, pix)
if cur:
    ticks.append(cur)

changed = {sid: 0 for sid in sids}
last = {}
images = []
for i, tick in enumerate(ticks):
    canvas = Image.new("RGB", (W * len(sids), H + 16))
    d = ImageDraw.Draw(canvas)
    t0 = None
    for j, sid in enumerate(sids):
        if sid not in tick:
            continue
        t, pix = tick[sid]
        t0 = t if t0 is None else t0
        if last.get(sid) is not None and last[sid] != pix:
            changed[sid] += 1
        last[sid] = pix
        canvas.paste(Image.frombytes("RGB", (W, H), pix, "raw", "BGR"), (W * j, 16))
        d.text((W * j + 4, 2), "surface %d" % sid, fill=(255, 255, 0))
    if t0 is not None:
        d.text((W * len(sids) - 60, 2), "%.2f s" % t0, fill=(255, 255, 0))
    if i % every == 0:
        images.append(canvas)
span = ticks[-1][next(iter(ticks[-1]))][0] - ticks[0][next(iter(ticks[0]))][0] if len(ticks) > 1 else 1
ms = max(20, int(span * 1000 / max(1, len(ticks))) * every)
images[0].save(out, save_all=True, append_images=images[1:], duration=ms, loop=0)
print("%d ticks over %.1f s (%.0f ms each) -> %s" % (len(ticks), span, span * 1000 / max(1, len(ticks)), out))
for sid in sids:
    print("surface %d: picture changed in %d of %d ticks" % (sid, changed[sid], len(ticks) - 1))
