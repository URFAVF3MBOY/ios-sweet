#!/bin/bash
# firehose-ios.sh LIB: re-label dxb's libfirehose_kernel.a (built with the macOS SDK) as iOS, in place.
# The iOS-configured kernel (0022) links it and ld refuses "object file built for 'macOS'" in an iOS link.
# The code is freestanding kernel code; only the platform in LC_BUILD_VERSION differs. Building it with the
# iPhoneOS SDK fails (its kernel headers need the macOS SDK's Availability.h), and vtool -set-build-version has
# no room to rewrite the load commands of these objects, so the platform field is changed in place
# (LC_BUILD_VERSION platform 1 macOS -> 2 iOS, minos 18.0; LC_VERSION_MIN_MACOSX -> _IPHONEOS). Keeps the arm64
# slice only (the SWEET kernel is plain arm64).
set -euo pipefail
lib=$1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if lipo -info "$lib" | grep -q 'Architectures in the fat file'; then
	lipo "$lib" -thin arm64 -output "$work/thin.a"
else
	cp "$lib" "$work/thin.a"
fi
mkdir "$work/o"
(cd "$work/o" && ar -x ../thin.a)
python3 - "$work"/o/*.o <<'PY'
import struct, sys
n = 0
for path in sys.argv[1:]:
    b = bytearray(open(path, 'rb').read())
    magic, cputype, sub, ftype, ncmds, sizeofcmds = struct.unpack_from('<IiiIII', b, 0)
    assert magic == 0xfeedfacf, (path, hex(magic))
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', b, off)
        if cmd == 0x32:                                   # LC_BUILD_VERSION: platform, minos, sdk
            struct.pack_into('<III', b, off + 8, 2, 0x120000, 0x1a0000)
            n += 1
        elif cmd == 0x24:                                 # LC_VERSION_MIN_MACOSX -> LC_VERSION_MIN_IPHONEOS
            struct.pack_into('<III', b, off, 0x25, size, 0x120000)
            n += 1
        off += size
    open(path, 'wb').write(b)
print('firehose-ios: %d of %d objects re-labelled iOS' % (n, len(sys.argv) - 1))
PY
xcrun libtool -static -o "$lib" "$work"/o/*.o
lipo -info "$lib"
