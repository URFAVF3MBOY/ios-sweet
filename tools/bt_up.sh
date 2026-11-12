#!/bin/bash
# bt_up.sh: Bluetooth for iOS on the OnePlus 7T, the userland half, once per boot (as root on the phone).
#   1. kernel: power/UART/firmware of the WCN3990 (sweet_bt.c): sysctl kern.sweetbt=1 (stage 6); from 0178 that
#      also registers the kernel control com.apple.uart.sk.bluetooth that bluetoothd's H4 driver connects to.
#      From 0197 the kernel does this at boot by itself (and adds the name com.sweet.bt, bluetoothd's
#      BT_HCI_TRANSPORT and the HCI fix-ups bthook did); here we only wait for stage 6.
#   2. com.apple.BlueTool answered by bluetool_stub.py instead of Apple's BlueTool (Broadcom only)
#   3. bluetoothd restarted, so that it finds both (ADT transport-encoding 1 = H4)
# Files: /var/jb/usr/libexec/bluetool_stub.py, /var/jb/usr/libexec/bluetool-stub-user.plist
# (tools/bt_bridge.py, the pty + /dev/uart.bluetooth bridge, is not needed for this: bluetoothd 16.5's H4 driver
#  does not open a tty.)
export PATH=/var/jb/usr/bin:/var/jb/usr/sbin:/var/jb/bin:/var/jb/sbin:/usr/bin:/usr/sbin:/bin:/sbin:$PATH
set -u
stage=$(sysctl -n kern.sweetbt)
if [ "$stage" -lt 6 ]; then
	# before 0197 this does the whole bring-up (and returns at stage 6); from 0197 the kernel does it at
	# boot, and this says EALREADY while that runs (or brings it up after three failed boot tries)
	sysctl -w kern.sweetbt=1 >/dev/null 2>&1
	# 0197: wait for the kernel's boot bring-up (12 s after boot, ~4 s of firmware; 90 s at most)
	for i in $(seq 1 90); do
		stage=$(sysctl -n kern.sweetbt)
		[ "$stage" -ge 6 ] && break
		sleep 1
	done
fi
echo "chip: stage $stage"
[ "$stage" -ge 6 ] || { echo "chip not up"; exit 1; }
# (Apple's job answers to system/, user/501/ and user/foreground/ alike; bootstrap goes to user/501)
# Right after boot the bootstrap can fail with "17: File exists" while Apple's job stays loaded: retry.
for try in 1 2 3; do
	for d in user/501 user/foreground system; do launchctl bootout $d/com.apple.BlueTool 2>/dev/null; done
	sleep 2
	launchctl bootstrap user/501 /var/jb/usr/libexec/bluetool-stub-user.plist 2>&1 | tail -1
	launchctl print user/501/com.apple.BlueTool 2>/dev/null | grep -q 'program = /var/jb/usr/bin/python3' && break
done
launchctl print user/501/com.apple.BlueTool 2>/dev/null | grep -m1 'program'
killall -9 bluetoothd 2>/dev/null
echo "bluetoothd restarted"
