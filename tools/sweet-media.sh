#!/var/jb/bin/bash
# sweet-media.sh: Bluetooth and the speakers, once per boot, by themselves (OnePlus 7T, iOS 16.5).
# Started in the background by prep.sh (/private/preboot/jb/procursus/prep.sh), as SSH is:
#   [ -f /var/jb/sweet-media.sh ] && /var/jb/bin/bash /var/jb/sweet-media.sh >/dev/null 2>&1 &
# Installed as /var/jb/sweet-media.sh; its helpers in /var/jb/usr/libexec (bt_up.sh, bluetool_stub.py and
# bluetool-stub-user.plist) and /var/jb/usr/libexec/sweet-audio (sweetaudio.py, tfacnt.py, tfa98xx.cnt).
#
#   1. wait until SpringBoard, bluetoothd and mediaserverd run (2 minutes at most), then 10 s more
#   2. Bluetooth: bt_up.sh (the chip, the BlueTool stand-in, bluetoothd restarted; its hook comes with the boot line)
#   3. the speakers: the audio DSP started (kern.sweetadsp=1), the amplifiers' I2C, both TFA9874s set up and muted,
#      the MI2S pins, the AFE port (32-bit, 48 kHz), /dev/redwood-audio made and mediaserverd restarted so that
#      the RedwoodAudio HAL plug-in opens it
#   4. the feeder (sweetaudio.py feed, -12 dB, amplifiers on only while there is sound), started again whenever it ends
#
# Logging: one line per step in /var/root/sweet-media.log, rewritten at every boot; nothing else is kept.
export PATH=/var/jb/usr/bin:/var/jb/usr/sbin:/var/jb/bin:/var/jb/sbin:/usr/bin:/usr/sbin:/bin:/sbin:$PATH
export SWEET_TLMM=1
A=/var/jb/usr/libexec/sweet-audio
export SWEET_TFA_CNT=$A/tfa98xx.cnt
LOG=/var/root/sweet-media.log
: > "$LOG"
say() { echo "$(date +%H:%M:%S) $*" >> "$LOG"; }
running() { ps -axo command | grep -q "^$1"; }
audio() { python3 "$A/sweetaudio.py" "$@" >/dev/null 2>&1; }

say "waiting for SpringBoard, bluetoothd, mediaserverd"
n=0
while [ $n -lt 60 ]; do
	running /System/Library/CoreServices/SpringBoard.app/SpringBoard && running /usr/sbin/bluetoothd &&
		running /usr/sbin/mediaserverd && break
	sleep 2; n=$((n + 1))
done
sleep 10

# ---- Bluetooth ----
if /var/jb/bin/bash /var/jb/usr/libexec/bt_up.sh >/dev/null 2>&1; then
	say "bluetooth: chip stage $(sysctl -n kern.sweetbt), BlueTool stand-in in place, bluetoothd restarted"
else
	say "bluetooth: FAILED (chip stage $(sysctl -n kern.sweetbt))"
fi

# ---- the speakers ----
sysctl -w kern.sweetadsp=1 >/dev/null 2>&1
n=0
while [ $n -lt 60 ]; do
	s=$(sysctl -n kern.sweetadsp 2>/dev/null)
	[ "${s:-0}" -ge 6 ] || [ "${s:-0}" -lt 0 ] && break
	sleep 1; n=$((n + 1))
done
s=$(sysctl -n kern.sweetadsp 2>/dev/null)
if [ "${s:-0}" -lt 6 ]; then
	say "audio: DSP did not come up (stage $s): no sound this boot"
	exit 1
fi
n=0
until audio apr-state; do
	n=$((n + 1)); [ $n -ge 15 ] && break; sleep 2
done
audio i2c-init && audio tfa-init && audio mi2s-pins && audio afe-start 2 32 && audio tfa-up && audio tfa-mute
r=$?
say "audio: DSP stage $s, amplifiers and port set up: $([ $r -eq 0 ] && echo ok || echo "FAILED ($r)")"
[ $r -eq 0 ] || exit 1
sysctl -w kern.sweetaudiodev=1 >/dev/null 2>&1
killall mediaserverd 2>/dev/null
say "audio: /dev/redwood-audio made, mediaserverd restarted; feeder starting"

starts=0
while true; do
	audio dsp-reset
	starts=$((starts + 1))
	[ $starts -le 3 ] || [ $((starts % 20)) -eq 0 ] && say "audio: feeder start $starts"
	python3 -u "$A/sweetaudio.py" feed 0 -12 >/dev/null 2>&1
	sleep 3
done
