#!/usr/bin/env python3
"""Make a payload drop: files the kernel (patch 0102) writes into the iOS root at boot.

  python3 tools/mkdrop.py OUT.drop DEVICE_PATH=LOCAL_FILE[:MODE[:UID[:GID]]] ...

Example (new libredwood-gl, root:wheel 0755 is the default):
  python3 tools/mkdrop.py shim/out/u2.drop \\
      /private/preboot/jb/procursus/usr/lib/libredwood-gl.dylib=$HOME/u2-out/libredwood-gl.dylib
  python3 tools/mkpayload.py KERNEL ADT OUT --cmdline '...' --ramdisk shim/out/u2.drop

DEVICE_PATH must be absolute and real (not through the /var/jb link): the folder must already exist on
the phone. Up to 8 files. The kernel logs "SWEET DROP: PATH: N bytes, fnv XXXXXXXX ... -> written";
the fnv printed here must match.
"""
import struct, sys

MAGIC = b'SWEETDRP'


def fnv1a(data):
    h = 2166136261
    for byte in data:
        h = ((h ^ byte) * 16777619) & 0xffffffff
    return h


def pad8(b):
    return b + b'\0' * (-len(b) % 8)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, specs = sys.argv[1], sys.argv[2:]
    if len(specs) > 8:
        sys.exit('at most 8 files')
    body = b''
    for spec in specs:
        dev, sep, rest = spec.partition('=')
        if not sep or not dev.startswith('/'):
            sys.exit(f'bad spec {spec!r}: DEVICE_PATH=LOCAL_FILE[:MODE[:UID[:GID]]]')
        parts = rest.split(':')
        local = parts[0]
        mode = int(parts[1], 8) if len(parts) > 1 and parts[1] else 0o755
        uid = int(parts[2]) if len(parts) > 2 and parts[2] else 0
        gid = int(parts[3]) if len(parts) > 3 and parts[3] else 0
        data = open(local, 'rb').read()
        path = dev.encode() + b'\0'
        if len(path) > 256:
            sys.exit(f'path too long: {dev}')
        h = fnv1a(data)
        body += struct.pack('<IIIIQII', len(path), mode, uid, gid, len(data), h, 0) + pad8(path) + pad8(data)
        print(f'{dev}: {len(data)} bytes, fnv {h:08x}, mode {mode:o} uid {uid} gid {gid}')
    blob = MAGIC + struct.pack('<II', 1, len(specs)) + body
    open(out, 'wb').write(blob)
    print(f'{out}: {len(blob)} bytes')


if __name__ == '__main__':
    main()
