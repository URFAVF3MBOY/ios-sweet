#!/usr/bin/env python3
# On the phone: dlopen H264SW.videocodec (shared cache only) and dump 0xc0 bytes around OFFSET to /var/tmp/peek.bin.
import ctypes, sys
libc = ctypes.CDLL(None)
P = b'/System/Library/VideoCodecs/H264SW.videocodec'
libc.dlopen.restype = ctypes.c_void_p
h = libc.dlopen(P, 1)
print('dlopen', bool(h))
libc._dyld_image_count.restype = ctypes.c_uint32
libc._dyld_get_image_name.restype = ctypes.c_char_p
libc._dyld_get_image_header.restype = ctypes.c_void_p
base = None
for i in range(libc._dyld_image_count()):
    n = libc._dyld_get_image_name(i)
    if n and b'H264SW' in n:
        base = libc._dyld_get_image_header(i); print(n, hex(base))
off = int(sys.argv[1], 16)
open('/var/tmp/peek.bin', 'wb').write(ctypes.string_at(base + off - 0x80, 0xc0)); print('ok')
