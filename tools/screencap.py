#!/var/jb/usr/bin/python3
# screencap.py [OUT_PREFIX] [MAX_ID]: what the phone shows, as PNG(s) (run as root on the phone; 2026-11-12).
#
# Since 0108 (direct scanout) the panel shows one of backboardd's full-screen 1125x2436 BGRA IOSurfaces from the
# scanout pool, not the old panel memory (AppleCLCD's GetLayerDefaultSurface still gives that: the boot console text).
# QuartzCore's CARenderServerRenderDisplay into our own surface came back all black on 0197. So: every surface id
# 1..MAX_ID is looked up (our kernel's IOSurface lookup does not check ownership), the 1125x2436 ones are written as
# OUT_PREFIX-<id>.png, cropped to the 1080x2400 panel the way sweetscan=2 shows it (18 rows down, 22 columns in).
# backboardd flips between 2-3 of them; on a still screen they hold the same picture. The newest by IOSurfaceGetSeed
# is also written as OUT_PREFIX.png.
import ctypes, struct, sys, zlib

prefix = sys.argv[1] if len(sys.argv) > 1 else "/var/tmp/screen"
max_id = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
UI_W, UI_H, PANEL_W, PANEL_H, TOP, LEFT = 1125, 2436, 1080, 2400, 18, 22

CF = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
IOS = ctypes.CDLL("/System/Library/Frameworks/IOSurface.framework/IOSurface")
vp, sz = ctypes.c_void_p, ctypes.c_size_t
CF.CFRelease.argtypes = [vp]
IOS.IOSurfaceLookup.restype = vp
IOS.IOSurfaceLookup.argtypes = [ctypes.c_uint32]
for f, r in (("IOSurfaceGetWidth", sz), ("IOSurfaceGetHeight", sz), ("IOSurfaceGetBytesPerRow", sz),
             ("IOSurfaceGetPixelFormat", ctypes.c_uint32), ("IOSurfaceGetSeed", ctypes.c_uint32),
             ("IOSurfaceGetBaseAddress", vp)):
    getattr(IOS, f).restype = r
    getattr(IOS, f).argtypes = [vp]
IOS.IOSurfaceLock.argtypes = [vp, ctypes.c_uint32, vp]
IOS.IOSurfaceUnlock.argtypes = [vp, ctypes.c_uint32, vp]


def png(path, w, h, rows_rgb):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    raw = b"".join(b"\0" + r for r in rows_rgb)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def grab(s):
    stride = IOS.IOSurfaceGetBytesPerRow(s)
    IOS.IOSurfaceLock(s, 1, None)  # read-only
    try:
        raw = ctypes.string_at(IOS.IOSurfaceGetBaseAddress(s), stride * UI_H)
    finally:
        IOS.IOSurfaceUnlock(s, 1, None)
    rows, lit = [], 0
    for y in range(TOP, TOP + PANEL_H):
        row = raw[y * stride + LEFT * 4:y * stride + (LEFT + PANEL_W) * 4]
        rgb = bytearray(PANEL_W * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
        lit += any(rgb)
        rows.append(bytes(rgb))
    return rows, lit


found = []
for sid in range(1, max_id + 1):
    s = IOS.IOSurfaceLookup(sid)
    if not s:
        continue
    if (IOS.IOSurfaceGetWidth(s), IOS.IOSurfaceGetHeight(s)) == (UI_W, UI_H) and \
            IOS.IOSurfaceGetPixelFormat(s) == 0x42475241:  # 'BGRA'
        found.append((IOS.IOSurfaceGetSeed(s), sid, s))
    else:
        CF.CFRelease(s)
if not found:
    sys.exit("no %dx%d BGRA surface among ids 1..%d" % (UI_W, UI_H, max_id))
newest = max(found)[1]
for seed, sid, s in found:
    rows, lit = grab(s)
    png("%s-%d.png" % (prefix, sid), PANEL_W, PANEL_H, rows)
    if sid == newest:
        png(prefix + ".png", PANEL_W, PANEL_H, rows)
    print("surface %d: seed %d, %d of %d rows not black%s" % (sid, seed, lit, PANEL_H, "  <- newest" if sid == newest else ""))
    CF.CFRelease(s)
