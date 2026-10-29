#!/var/jb/bin/sh
# Wi-Fi recorder (2026-10-25), started in the background by prep.sh at every boot. Keeps what the kernel and
# wifid say about scans, joins and drops, so a join that failed while SSH was unreachable can be read later:
#   /var/root/wifi-kern.txt    kernel lines (join/link/kickout/firmware), each once (by its timestamp)
#   /var/root/wifi-os.txt      wifid/configd/IPConfiguration lines about joins, auth, links, DHCP
#   /var/root/wifi-state.txt   every 30 s: time, en0's address, wifid's pid
# Each file is moved to .old past 4 MB. Read after a bad join:  tail -60 /var/root/wifi-kern.txt
PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin
K=/var/root/wifi-kern.txt
O=/var/root/wifi-os.txt
S=/var/root/wifi-state.txt

rotate() {
	for f in "$K" "$O" "$S"; do
		[ -f "$f" ] && [ "$(wc -c < "$f")" -gt 4000000 ] && mv "$f" "$f.old"
	done
}
# kernel time of a dmesg line ("[  692.760328]: ..."), 0 if none
ktime='{ if (match($0, /\[ *[0-9]+\.[0-9]+\]/)) printf "%.6f\n", substr($0, RSTART + 1, RLENGTH - 2); else print 0 }'

rotate
for f in "$K" "$O" "$S"; do echo "== boot $(date)" >> "$f"; done

# (2026-10-25: no oslog here any more: it never wrote wifi-os.txt, and oslog + diagnosticd cost
# ~12 % CPU for the whole session. For wifid's own log, run oslog by hand while reproducing.)

last=0
n=0
while :; do
	dmesg | grep -a -E 'join:|kickout|torn down|PFR|wlan_process|link \(|station says|WLFW back|VDEV_START|wifid sets|SWEET NET|HOTDOGB NET|deauth|disassoc|apple80211' |
		awk -v last="$last" '{ if (match($0, /\[ *[0-9]+\.[0-9]+\]/) && substr($0, RSTART + 1, RLENGTH - 2) + 0 > last + 0) print }' >> "$K"
	t=$(dmesg | tail -1 | awk "$ktime")
	[ -n "$t" ] && [ "$t" != 0 ] && last=$t
	n=$((n + 1))
	if [ $((n % 3)) = 0 ]; then
		echo "$(date +%T) en0=$(ifconfig en0 2>/dev/null | awk '/inet /{print $2}') wifid=$(ps -axo pid,comm | awk '/wifid$/{print $1}')" >> "$S"
		rotate
	fi
	sleep 10
done
