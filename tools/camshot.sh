#!/bin/sh
# camshot.sh OUT [AGAIN_HI AGAIN_LO DGAIN [COARSE_HI COARSE_LO]]: one IMX586 frame on the phone (0212+ kernel,
# camio.py + camreg.py in /var/root). Powers + loads table 15/4 if the camera is off, arms (kern.sweetcam=4),
# camnoc_axi 150 MHz (the WM hangs at XO), sets analog gain 0x0204/5, digital 0x020e, optional coarse
# integration 0x0202/3, streams 1 s, reports (=5), dumps the 2 MiB buffer to OUT (1296x736 RAW10, stride 1632).
S=/var/jb/usr/sbin/sysctl; P=/var/jb/usr/bin/python3; C=/var/root/camio.py; R=/var/root/camreg.py
OUT=$1; AH=${2:-00}; AL=${3:-70}; DG=${4:-01}
if [ "$($S -n kern.sweetcam)" = "0" ]; then
	$S -w kern.sweetcam=2 >/dev/null 2>&1
	$P $C table /var/root/imx586_regs.txt 15 4 >/dev/null
	$S -w kern.sweetcamstop=7 >/dev/null
fi
$S -w kern.sweetcam=4 >/dev/null
$P $R wr cc+c174 207 >/dev/null
$P $R wr cc+c170 1 >/dev/null
$P $C write 1a 0204 $AH >/dev/null; $P $C write 1a 0205 $AL >/dev/null; $P $C write 1a 020e $DG >/dev/null
if [ -n "$5" ]; then $P $C write 1a 0202 $5 >/dev/null; $P $C write 1a 0203 $6 >/dev/null; fi
$P $C write 1a 0100 01 >/dev/null
sleep 1
$S -w kern.sweetcam=5 >/dev/null
$P $C write 1a 0100 00 >/dev/null
$S -n kern.sweetcamlog | grep "buffer:" | cut -c1-50
$P $C dumpbuf "$OUT" >/dev/null
