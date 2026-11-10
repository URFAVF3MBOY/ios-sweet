#!/bin/sh
# Start the audio DSP (kern.sweetadsp=1, once per boot) and keep every kernel log line about it.
# The phone's msgbuf only holds ~740 lines (exec traces push the rest out in seconds), so dmesg is
# polled twice a second and new matching lines are appended to OUT. Run on the phone as root.
OUT=${1:-/var/root/adsp.log}
SECS=${2:-90}
DMESG=/var/jb/sbin/dmesg
SYSCTL=/var/jb/usr/sbin/sysctl
PAT='adsp|RPMH|smem|SMP2P|glink|APR'

: > "$OUT"
( n=0; while [ $n -lt $((SECS * 2)) ]; do
	$DMESG | grep -E "$PAT"
	sleep 0.5; n=$((n + 1))
done ) | awk '!seen[$0]++' >> "$OUT" &
POLL=$!
sleep 1
echo "stage before: $($SYSCTL -n kern.sweetadsp)" >> "$OUT"
$SYSCTL -w kern.sweetadsp=1 >> "$OUT" 2>&1
wait $POLL
echo "stage after: $($SYSCTL -n kern.sweetadsp)" >> "$OUT"
wc -l "$OUT"
