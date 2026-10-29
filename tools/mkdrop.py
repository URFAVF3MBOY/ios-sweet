#!/usr/bin/env python3
"""Make a payload drop: what the kernel (patch 0107 v6, SweetDrop.cpp) does to the iOS root at boot.

  python3 tools/mkdrop.py OUT.drop ENTRY ...

ENTRY is one of (done in the order given, up to 16):
  DEVICE_PATH=LOCAL_FILE[:MODE[:UID[:GID]]]   write the file (root:wheel 0755 by default)
  -DEVICE_PATH                                 delete it (a file, or an EMPTY folder)
  ~DEVICE_PATH                                 move it aside: a file or a whole folder is renamed to
                                               DEVICE_PATH.sweetdrop-old (-old2 ... if taken)

Examples:
  # new libredwood-gl
  python3 tools/mkdrop.py shim/out/u2.drop \\
      /private/preboot/jb/procursus/usr/lib/libredwood-gl.dylib=$HOME/u2-out/libredwood-gl.dylib
  # PosterBoard's state aside, so it starts fresh
  python3 tools/mkdrop.py shim/out/wall.drop '~/private/var/mobile/Library/PosterBoard'
  python3 tools/mkpayload.py KERNEL ADT OUT --cmdline '...' --ramdisk shim/out/wall.drop

DEVICE_PATH must be absolute and real (not through the /var/jb or /var links: /private/var/...).
For a write, the folder must already exist on the phone. A path that is not there is left alone.
The kernel logs one "HOTDOGB DROP: PATH: ..." line an entry; for writes the fnv printed here must match.
"""
import struct, sys

MAGIC = b'SWEETDRP'
OP_WRITE, OP_DELETE, OP_ASIDE = 0, 1, 2


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
    if len(specs) > 16:
        sys.exit('at most 16 entries')
    body = b''
    version = 1
    for spec in specs:
        op = OP_WRITE
        if spec[:1] in ('-', '~'):
            op = OP_DELETE if spec[0] == '-' else OP_ASIDE
            dev, data, mode, uid, gid = spec[1:], b'', 0, 0, 0
            version = 2
            if not dev.startswith('/') or '=' in dev:
                sys.exit(f'bad entry {spec!r}: -DEVICE_PATH or ~DEVICE_PATH')
        else:
            dev, sep, rest = spec.partition('=')
            if not sep or not dev.startswith('/'):
                sys.exit(f'bad entry {spec!r}: DEVICE_PATH=LOCAL_FILE[:MODE[:UID[:GID]]]')
            parts = rest.split(':')
            mode = int(parts[1], 8) if len(parts) > 1 and parts[1] else 0o755
            uid = int(parts[2]) if len(parts) > 2 and parts[2] else 0
            gid = int(parts[3]) if len(parts) > 3 and parts[3] else 0
            data = open(parts[0], 'rb').read()
        if dev.rstrip('/') in ('', '/private', '/private/var', '/private/var/mobile', '/private/preboot',
                               '/System', '/usr', '/bin', '/sbin', '/Applications', '/Library'):
            sys.exit(f'refusing a top-level path: {dev}')
        path = dev.encode() + b'\0'
        if len(path) > 256:
            sys.exit(f'path too long: {dev}')
        h = fnv1a(data)
        body += struct.pack('<IIIIQII', len(path), (op << 24) | mode, uid, gid, len(data), h, 0) + pad8(path) + pad8(data)
        what = {OP_WRITE: f'write {len(data)} bytes, fnv {h:08x}, mode {mode:o} uid {uid} gid {gid}',
                OP_DELETE: 'delete', OP_ASIDE: 'move aside'}[op]
        print(f'{dev}: {what}')
    blob = MAGIC + struct.pack('<II', version, len(specs)) + body
    open(out, 'wb').write(blob)
    print(f'{out}: {len(blob)} bytes (format {version}{": needs 0107 v6" if version == 2 else ""})')


if __name__ == '__main__':
    main()
