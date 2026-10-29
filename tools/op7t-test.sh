#!/bin/bash
# OnePlus 7T test runs from WSL, without touching the phone (beyond plugging it in once).
#
#   tools/op7t-test.sh up               USB link: find the phone (1209:0001) in usbipd, attach it to WSL,
#                                       192.168.7.1 on our side, wait for ssh root@192.168.7.2
#   tools/op7t-test.sh fps [SECS]       the phone swipes its home screen itself (kern.sweetswipe, 0140)
#                                       while redwood-gl's frame lines are recorded; prints the fps
#   tools/op7t-test.sh ab [SECS]        the same twice: all cores (255), then the A76s (241)
#   tools/op7t-test.sh headroom [SECS]  fps with the UI's vsync at 120 Hz (kern.sweetvsync_hz, 0141): how many
#                                       frames the pipeline can make; back to 60 afterwards
#   tools/op7t-test.sh boot IMAGE       reboot the running phone into fastboot (kern.sweetreboot=2, 0140),
#                                       fastboot boot IMAGE, then `up`
#
# Needs: the image booted with sweetusb=2 (and redwood-gl lines in the kernel log, i.e. not
# sweetnull=-), a kernel with 0140 for fps/ab/boot, passwordless `sudo ip` in WSL
# (/etc/sudoers.d/sweet-usbnet), the device bound once in usbipd (admin: usbipd bind --force --busid X).
# Results: logs/fps-*.txt (raw), parsed with tools/fps-parse.py.
set -euo pipefail
cd "$(dirname "$0")/.."
PHONE=root@192.168.7.2
SSH="ssh -o BatchMode=yes -o ConnectTimeout=6 -o StrictHostKeyChecking=accept-new -o ServerAliveInterval=3 -o ServerAliveCountMax=2"
MAC=02:00:5e:77:ee:01
mkdir -p logs

ps_win() { powershell.exe -NoProfile -Command "$1" 2>/dev/null | tr -d '\r'; }

up() {
	local busid tries=0
	until busid=$(ps_win "usbipd list" | awk '/1209:0001/ {print $1; exit}'); [ -n "$busid" ]; do
		tries=$((tries + 1))
		[ $tries -gt 90 ] && { echo "the phone's USB device did not show up (booted with sweetusb=2?)" >&2; exit 1; }
		sleep 2
	done
	if ! ip -o link | grep -i "$MAC" >/dev/null; then
		echo "attaching $busid to WSL" >&2
		ps_win "usbipd attach --wsl --busid $busid" >&2 || true
	fi
	tries=0
	until ip -o link | grep -i "$MAC" >/dev/null; do
		tries=$((tries + 1))
		[ $tries -gt 30 ] && { echo "attach failed: run in an admin PowerShell: usbipd attach --wsl --busid $busid" >&2; exit 1; }
		sleep 1
	done
	local ifname
	ifname=$(ip -o link | grep -i "$MAC" | awk -F': ' '{print $2}' | cut -d@ -f1)
	sudo -n ip addr replace 192.168.7.1/24 dev "$ifname"
	sudo -n ip link set "$ifname" up
	tries=0
	until $SSH "$PHONE" true 2>/dev/null; do
		tries=$((tries + 1))
		[ $tries -gt 60 ] && { echo "no ssh on 192.168.7.2" >&2; exit 1; }
		sleep 2
	done
	echo "up: $ifname 192.168.7.1 <-> $PHONE" >&2
}

# record redwood-gl frame lines once a second for $1 s while the phone swipes; raw to $2
record() {
	local secs=$1 out=$2
	$SSH "$PHONE" sh -s "$secs" > "$out" <<'EOF'
PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin
sysctl -w kern.sweetswipe="$1" >/dev/null || { echo "no kern.sweetswipe (kernel without 0140?)" >&2; exit 1; }
sleep 1
i=0
while [ $i -lt "$1" ]; do
	echo "T $(date +%s)"
	dmesg | grep -a "redwood-gl: \(after\|frame\)\|SWEET SCAN: flip\|HOTDOGB SCAN: flip" | tail -6
	ps -axo pcpu,comm -r | head -6 | tail -5
	i=$((i + 1))
	sleep 1
done
sysctl -w kern.sweetswipe=0 >/dev/null
EOF
}

fps_run() {
	local secs=$1 tag=$2 out
	out=logs/fps-$tag-$(date +%Y%m%d-%H%M%S).txt
	echo "== $tag: $secs s of synthetic swipes" >&2
	record "$secs" "$out"
	python3 tools/fps-parse.py "$out" | tail -4
	echo "(raw: $out)"
}

cmd=${1:-}
case "$cmd" in
up) up ;;
fps)
	up
	fps_run "${2:-45}" fps
	;;
ab)
	up
	$SSH "$PHONE" "/var/jb/usr/sbin/sysctl -w kern.sweetcores=255" >/dev/null
	fps_run "${2:-45}" all-cores
	$SSH "$PHONE" "/var/jb/usr/sbin/sysctl -w kern.sweetcores=240" >/dev/null
	sleep 2
	fps_run "${2:-45}" big-cores
	;;
headroom)
	up
	$SSH "$PHONE" "/var/jb/usr/sbin/sysctl -w kern.sweetvsync_hz=120" >/dev/null ||
		{ echo "no kern.sweetvsync_hz (kernel without 0141?)" >&2; exit 1; }
	# the finger's moves every 8 ms (0142): at 16 the drag paces the UI at ~60 updates/s
	$SSH "$PHONE" "/var/jb/usr/sbin/sysctl -w kern.sweetswipe_ms=8" >/dev/null 2>&1 ||
		echo "(no kern.sweetswipe_ms, kernel without 0142: the drag stays at 16 ms steps)" >&2
	trap '$SSH "$PHONE" "/var/jb/usr/sbin/sysctl -w kern.sweetvsync_hz=60 kern.sweetswipe_ms=16" >/dev/null 2>&1 || true' EXIT
	fps_run "${2:-45}" vsync120
	;;
boot)
	img=${2:?usage: op7t-test.sh boot IMAGE}
	[ -f "$img" ] || { echo "no $img" >&2; exit 1; }
	if $SSH "$PHONE" true 2>/dev/null; then
		echo "rebooting the phone into fastboot" >&2
		# (the connection dies with the phone: never wait on it)
		timeout 10 $SSH "$PHONE" "sync; /var/jb/usr/sbin/sysctl -w kern.sweetreboot=2" >/dev/null 2>&1 || true
	else
		echo "phone not reachable over USB: put it in fastboot by hand (Power + Vol Down)" >&2
	fi
	# (grep without -q: with pipefail, -q's early exit makes the pipe fail and this would never end)
	until ps_win "fastboot devices" | grep fastboot >/dev/null; do sleep 2; done
	ps_win "fastboot boot '$(wslpath -w "$img")'" >&2
	sleep 20
	up
	;;
*)
	sed -n '2,15p' "$0"
	exit 2
	;;
esac
