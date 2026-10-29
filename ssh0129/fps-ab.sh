#!/var/jb/bin/sh
# fps A/B for the core choice (0134): swipe the home screen for the whole run (~95 s).
#   1) all cores (kern.sweetcores=255), 45 s -> /var/root/fps-all.txt
#   2) A76 cores + cpu0 (=241), 45 s      -> /var/root/fps-big.txt
# then all cores again. Each part: redwood-gl's frame lines + top CPU once a second (as fpsrec.sh).
PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin

record() {   # $1 = seconds, $2 = file
	: > "$2"
	i=0
	while [ $i -lt "$1" ]; do
		echo "T $(date +%s)" >> "$2"
		dmesg | grep -a "redwood-gl: \(after\|frame\)\|SWEET SCAN: flip\|HOTDOGB SCAN: flip" | tail -6 >> "$2"
		ps -axo pcpu,comm -r | head -6 | tail -5 >> "$2"
		i=$((i + 1))
		sleep 1
	done
}

sysctl -w kern.sweetcores=255 >/dev/null
echo "all cores: swipe now (45 s)"
record 45 /var/root/fps-all.txt
sysctl -w kern.sweetcores=241 >/dev/null
echo "big cores only: keep swiping (45 s)"
sleep 2
record 45 /var/root/fps-big.txt
sysctl -w kern.sweetcores=255 >/dev/null
echo "done: /var/root/fps-all.txt, /var/root/fps-big.txt (cores back to all)"
