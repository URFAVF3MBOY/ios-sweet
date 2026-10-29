#!/bin/bash
# Build libsweetpb.dylib with the same toolchain as u2/build-u2-libs.sh (clang-18, lld, theos iOS 16.5 SDK).
#   bash build-sweetpb.sh            -> ~/u2-out/libsweetpb.dylib
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/theos-sdks/iPhoneOS16.5.sdk}
CLANG=${CLANG:-clang-18}
LLD=${LLD:-$(command -v ld64.lld-18 || command -v ld64.lld || true)}
OUT=${OUT:-$HOME/u2-out}
[ -d "$SDK" ] || { echo "no iOS SDK at $SDK"; exit 1; }
[ -n "$LLD" ] || { echo "no ld64.lld"; exit 1; }
mkdir -p "$OUT"
$CLANG -target arm64-apple-ios16.0 -isysroot "$SDK" -fuse-ld=lld --ld-path="$LLD" -O1 -Wall \
	-dynamiclib -install_name /redwood/lib/libsweetpb.dylib -lobjc -Wl,-x -Wl,-adhoc_codesign \
	-o "$OUT/libsweetpb.dylib" sweetpb.c
ls -l "$OUT/libsweetpb.dylib"; md5sum "$OUT/libsweetpb.dylib"
grep -c "first-time setup of PosterBoard" "$OUT/libsweetpb.dylib"
