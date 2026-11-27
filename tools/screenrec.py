#!/var/jb/usr/bin/python3
# screenrec.py OUT SECONDS [SWIPE_SECONDS [SID,SID,...]]: a quarter-size recording of every full-screen surface of the window
# server (run as root on the phone; 2026-11-13).
#
# Like screencap.py, the 1125x2436 BGRA IOSurfaces are found by looking up ids 1..20000; which one the panel shows
# is not known from here (GPU writes leave the seed alone), so every one is recorded. Each tick (as fast as the
# copies allow, aimed at 20/s) each surface is read whole, cut to the 1080x2400 panel and kept at every 4th row and
# column: 270x600 BGR. OUT gets a header line, then per frame "F <surface id> <seconds since start>\n" and 486000
# bytes of pixels. With SWIPE_SECONDS the kernel's synthetic home-screen swipes (kern.sweetswipe, 0140) run during
# the recording. tools/screenrec-video.py turns OUT into a video on the PC.
import ctypes, sys, time

out, secs = sys.argv[1], float(sys.argv[2])
swipe = int(sys.argv[3]) if len(sys.argv) > 3 else 0
wanted = {int(x) for x in sys.argv[4].split(",")} if len(sys.argv) > 4 else None   # only these surfaces
UI_W, UI_H, PANEL_W, PANEL_H, TOP, LEFT, STEP = 1125, 2436, 1080, 2400, 18, 22, 4

libc = ctypes.CDLL(None)
CF = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
IOS = ctypes.CDLL("/System/Library/Frameworks/IOSurface.framework/IOSurface")
vp, sz = ctypes.c_void_p, ctypes.c_size_t
CF.CFRelease.argtypes = [vp]
IOS.IOSurfaceLookup.restype = vp
IOS.IOSurfaceLookup.argtypes = [ctypes.c_uint32]
for f, r in (("IOSurfaceGetWidth", sz), ("IOSurfaceGetHeight", sz), ("IOSurfaceGetBytesPerRow", sz),
             ("IOSurfaceGetPixelFormat", ctypes.c_uint32), ("IOSurfaceGetBaseAddress", vp)):
    getattr(IOS, f).restype = r
    getattr(IOS, f).argtypes = [vp]
IOS.IOSurfaceLock.argtypes = [vp, ctypes.c_uint32, vp]
IOS.IOSurfaceUnlock.argtypes = [vp, ctypes.c_uint32, vp]


def sysctl_int(name, value):
    v = ctypes.c_int(value)
    return libc.sysctlbyname(name.encode(), None, None, ctypes.byref(v), 4)


surfaces = []
for sid in (sorted(wanted) if wanted else range(1, 20001)):
    s = IOS.IOSurfaceLookup(sid)
    if not s:
        continue
    if (IOS.IOSurfaceGetWidth(s), IOS.IOSurfaceGetHeight(s)) == (UI_W, UI_H) and \
            IOS.IOSurfaceGetPixelFormat(s) == 0x42475241:
        surfaces.append((sid, s, IOS.IOSurfaceGetBytesPerRow(s)))
    else:
        CF.CFRelease(s)
if not surfaces:
    sys.exit("no full-screen surface")
print("surfaces:", " ".join(str(sid) for sid, _, _ in surfaces))

f = open(out, "wb")
f.write(b"screenrec 270x600 bgr surfaces %s\n" % " ".join(str(sid) for sid, _, _ in surfaces).encode())
if swipe:
    if sysctl_int("kern.sweetswipe", swipe) != 0:
        print("no kern.sweetswipe")
t0 = time.time()
frames = 0
while time.time() - t0 < secs:
    tick = time.time()
    for sid, s, stride in surfaces:
        IOS.IOSurfaceLock(s, 1, None)
        try:
            raw = ctypes.string_at(IOS.IOSurfaceGetBaseAddress(s), stride * UI_H)
        finally:
            IOS.IOSurfaceUnlock(s, 1, None)
        rows = []
        for y in range(TOP, TOP + PANEL_H, STEP):
            row = raw[y * stride + LEFT * 4:y * stride + (LEFT + PANEL_W) * 4]
            px = bytearray(PANEL_W // STEP * 3)
            px[0::3], px[1::3], px[2::3] = row[0::4 * STEP], row[1::4 * STEP], row[2::4 * STEP]
            rows.append(bytes(px))
        f.write(b"F %d %.3f\n" % (sid, time.time() - t0))
        f.write(b"".join(rows))
        frames += 1
    rest = 0.05 - (time.time() - tick)
    if rest > 0:
        time.sleep(rest)
if swipe:
    sysctl_int("kern.sweetswipe", 0)
f.close()
print("%d frames in %.1f s" % (frames, time.time() - t0))
for _, s, _ in surfaces:
    CF.CFRelease(s)
