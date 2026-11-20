#!/bin/sh
# camstream.sh start|stop [AGAIN_HI AGAIN_LO DGAIN COARSE_HI COARSE_LO]: the IMX586 streaming for iOS's camera (0236+
# kernel; camio.py + camreg.py + imx586_regs.txt in /var/root). start = power + table 15/4 if the camera is off, arm
# (kern.sweetcam=4, which starts the kernel's frame loop), camnoc_axi 150 MHz, exposure/gains, stream on -- and it
# keeps streaming: msdhook in mediaserverd reads frames from kern.sweetcamframe. stop = stream off, kern.sweetcam=5.
# Defaults as camshot.sh's indoor settings: frame length 0x0c00, coarse 0x0bf0 (~7.5 fps), analog gain 0x0300 (4x).
S=/var/jb/usr/sbin/sysctl; P=/var/jb/usr/bin/python3; C=/var/root/camio.py; R=/var/root/camreg.py
case "$1" in
start)
	AH=${2:-03}; AL=${3:-00}; DG=${4:-01}; CH=${5:-0b}; CL=${6:-f0}
	if [ "$($S -n kern.sweetcam)" = "0" ]; then
		$S -w kern.sweetcam=2 >/dev/null 2>&1
		$P $C table /var/root/imx586_regs.txt 15 4 >/dev/null
		$S -w kern.sweetcamstop=7 >/dev/null
	fi
	$S -w kern.sweetcam=4 >/dev/null
	$P $R wr cc+c174 207 >/dev/null
	$P $R wr cc+c170 1 >/dev/null
	$P $C write 1a 0340 0c >/dev/null; $P $C write 1a 0341 00 >/dev/null
	$P $C write 1a 0202 $CH >/dev/null; $P $C write 1a 0203 $CL >/dev/null
	$P $C write 1a 0204 $AH >/dev/null; $P $C write 1a 0205 $AL >/dev/null; $P $C write 1a 020e $DG >/dev/null
	$P $C write 1a 0100 01 >/dev/null
	sleep 1
	$S -n kern.sweetcamlog | grep -a "frame loop"
	;;
stop)
	$P $C write 1a 0100 00 >/dev/null
	$S -w kern.sweetcam=5 >/dev/null
	$S -n kern.sweetcamlog | grep -a "buffer:" | cut -c1-60
	;;
*)
	echo "usage: camstream.sh start|stop [AGAIN_HI AGAIN_LO DGAIN COARSE_HI COARSE_LO]"
	exit 2
	;;
esac
