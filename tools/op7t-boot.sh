#!/bin/bash
# OnePlus 7T: make the boot image (shim + XNU + ADT + boot-args + payload drop) in one step,
# and boot it with --boot. Run from WSL/Linux (shim/build.sh needs aarch64-linux-gnu-gcc, mkbootimg).
#
#   tools/op7t-boot.sh [--boot] [KEY=VALUE ...]
#
# KEY=VALUE boot-args replace the same key in the base line (or are added), e.g.
#   tools/op7t-boot.sh sweetgpufreq=6 sweetddr=1        # fps test: 675 MHz GPU + DDR held at max
#   tools/op7t-boot.sh sweetclock=0                      # without 0131's clock (needs 0131 kernel)
# The base line is the one the phone ran on 2026-10-24 (kern.bootargs over SSH). It used to live
# only in shell history; keep it here so every boot and every user starts from the same line.
# The line has room for 607 bytes (mkpayload.py BOOT_LINE_LENGTH 608, as xnu's arm64 boot_args) and
# mkpayload cuts the rest off without a word: on 2026-10-25 that silently dropped sweetusb=2 and
# sweetprof=, so this script now refuses a longer line. !KEY drops KEY (or the flag KEY) for one build,
# e.g. !keepsyms. The fast cores (kern.sweetcores=240, 0134: home-screen swipe median 38.4 -> 49.8
# fps) are set by prep.sh in the drop at boot, not here, to keep the line short.
# Profiling: tools/op7t-boot.sh sweetusb=2 sweetprof=backboardd sweetnull=- !keepsyms
#
# Env: KERNEL (xnu/out/kernel.development.sweet), ADT (shim/out/sweet.adt), DROP (shim/out/ssh.drop),
#      FASTBOOT (fastboot; on WSL without USB passthrough use fastboot.exe).
# Note: this does not build the kernel; get it from the xnu.yml workflow first (README step 1).
set -euo pipefail
cd "$(dirname "$0")/.."

BASE='debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=60 sweetgpu=15 sweetgmu=7 sweetgpu4=1 sweetk1=1 sweetk1b=1 sweetgpu4delay=0 sweetgpufreq=5 sweetcpufreq=1 sweetscan=1 sweetinjectlib=/redwood/lib/sp.dylib sweetinject=backboardd=/redwood/lib/libredwood.dylib,PosterBoard,containermanagerd,lsd,installd sweetdyld=backboardd,PosterBoard,containermanager,lsd,installd sweetnull=backboardd sweetmodem=1 sweetwifi=1'

KERNEL=${KERNEL:-xnu/out/kernel.development.sweet}
ADT=${ADT:-shim/out/sweet.adt}
DROP=${DROP:-shim/out/ssh.drop}
FASTBOOT=${FASTBOOT:-fastboot}

boot=0
args=" $BASE "
for a in "$@"; do
	case "$a" in
	--boot) boot=1 ;;
	!*)
		key=${a#!}
		args=$(printf '%s' "$args" | sed -E "s/ ${key}(=[^ ]*)?( |\$)/ /g")
		;;
	*=*)
		key=${a%%=*}
		# drop the key's old value (" key=..." up to the next space), then append the new one
		args=$(printf '%s' "$args" | sed -E "s/ ${key}=[^ ]*//g")
		args="$args $a "
		;;
	*) echo "unknown argument: $a (want KEY=VALUE, !KEY or --boot)" >&2; exit 2 ;;
	esac
done
args=$(printf '%s' "$args" | tr -s ' ' | sed -E 's/^ //; s/ $//')
if [ "${#args}" -gt 607 ]; then
	echo "boot-args are ${#args} bytes, the limit is 607 (the rest would be cut off silently)." >&2
	echo "drop something for this build with !KEY, e.g. !keepsyms" >&2
	exit 3
fi

for f in "$KERNEL" "$ADT" "$DROP"; do
	[ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
done
echo "boot-args: $args"
python3 tools/mkpayload.py "$KERNEL" "$ADT" shim/out/payload.bin --cmdline "$args" --ramdisk "$DROP"
./shim/build.sh ../shim/out/payload.bin
# the line as it went into the image, whole
python3 -c "import sys; b=open('shim/out/shim-boot.img','rb').read(); a=sys.argv[1].encode(); sys.exit(0 if a + b'\0' in b else 1)" "$args" ||
	{ echo "the boot line in shim/out/shim-boot.img is not the one asked for" >&2; exit 4; }
echo "boot image: shim/out/shim-boot.img (${#args}-byte boot line checked)"
if [ "$boot" = 1 ]; then
	"$FASTBOOT" boot shim/out/shim-boot.img
fi
