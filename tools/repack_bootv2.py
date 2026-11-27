#!/usr/bin/env python3
# repack_bootv2.py OLD.img NEW_RAMDISK OUT.img: boot.img v2 with OLD's header, kernel (shim) and dtb
# and a new ramdisk (a tools/mkpayload.py payload): new kernels without WSL / the aarch64 cross compiler.
# Checked 2026-10-29: OLD's own ramdisk gives back OLD byte for byte.
import hashlib, struct, sys

old, rd_path, out = sys.argv[1:4]
img = open(old, 'rb').read()
assert img[:8] == b'ANDROID!'
ks, _, rs, _, ss, _, _, ps, hv = struct.unpack_from('<9I', img, 8)
assert hv == 2 and ss == 0
dtbo_size, = struct.unpack_from('<I', img, 1632)
dtb_size, = struct.unpack_from('<I', img, 1648)
assert dtbo_size == 0

def pages(n):
    return (n + ps - 1) // ps * ps

off = ps
kernel = img[off:off + ks]; off += pages(ks)
off += pages(rs)
dtb = img[off:off + dtb_size]
ramdisk = open(rd_path, 'rb').read()

sha = hashlib.sha1()
for blob in (kernel, ramdisk, b'', b'', dtb):
    sha.update(blob)
    sha.update(struct.pack('<I', len(blob)))
hdr = bytearray(img[:ps])
struct.pack_into('<I', hdr, 16, len(ramdisk))
hdr[576:608] = sha.digest().ljust(32, b'\0')

def pad(b):
    return b + b'\0' * (pages(len(b)) - len(b))

with open(out, 'wb') as f:
    f.write(bytes(hdr) + pad(kernel) + pad(ramdisk) + pad(dtb))
