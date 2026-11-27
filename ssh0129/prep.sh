#!/var/jb/bin/sh
# Run once per boot, after the prefix has been grafted onto /var/jb (rc.jobs.sileo does it).
/var/jb/usr/libexec/firmware
# 2026-10-25: ordinary threads on the A76 cores (kernel 0134; home-screen swipe median 38.4 -> 49.8 fps).
# 2026-10-26: 0xf0, without the boot CPU (an A55) as well: same boot, kernel 0145, synthetic swipes,
# 0xf1 median 56.8 fps (SpringBoard 32 % CPU) -> 0xf0 60.9 (SpringBoard 20 %), the display's 60.
# Here and not on the boot line, which is full (607 bytes). Older kernels have no such sysctl: no harm.
/usr/sbin/sysctl -w kern.sweetcores=240 >/dev/null 2>&1 || /var/jb/usr/sbin/sysctl -w kern.sweetcores=240 >/dev/null 2>&1
# The prefix's applications: in the system image they are in /Applications already (userland/build.sh),
# where LaunchServices finds them by itself; with the iOS volume on the flash as the root they are not,
# and are registered from the prefix as a rootless jailbreak does it (uikittools' uicache) -- by where the
# bundle really is, as there: in /var/jb, grafted, its files are links into the boot volume
# (/private/preboot/jb), and LaunchServices refuses a bundle made of links ("Unable to register").
# Asked again every five seconds, two minutes at most, aside: right after SpringBoard it may be busy.
for app in /var/jb/Applications/*.app; do
	[ -d "/Applications/${app##*/}" ] && continue
	# by its own path in the prefix first: that is the one a later uicache -- the trigger of any package
	# that brings an application -- finds again when it rebuilds the database, and an application
	# registered under some other path is "uninstalled" by that rebuild, which makes SpringBoard kill it
	# (a running Sileo died the moment a tweak was installed). The real path, with the grafted prefix a
	# directory of the boot volume, is only the fallback.
	real=$(/var/jb/usr/bin/readlink "$app/Info.plist" 2>/dev/null)
	real=${real%/Info.plist}
	(
		for try in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24; do
			for path in "$app" ${real:+"$real"}; do
				case "$(/var/jb/usr/bin/uicache -p "$path" 2>&1)" in
				*Error*) ;;
				*) echo "prep.sh: ${app##*/} registered from $path (try $try)"; exit 0 ;;
				esac
			done
			sleep 5
		done
		echo "prep.sh: ${app##*/} could not be registered"
	) &
done
/var/jb/usr/sbin/pwd_mkdb -p /var/jb/etc/master.passwd 2>/dev/null
for package in debianutils apt dash bash zsh vi; do
	[ -x "/var/jb/Library/dpkg/info/$package.postinst" ] &&
		"/var/jb/Library/dpkg/info/$package.postinst" configure 99999 >/dev/null 2>&1
done
# 0129: SSH (sweet-ssh.sh: privilege-separation user, host keys, the PC's key, sshd), in the background
[ -f /var/jb/sweet-ssh.sh ] && /var/jb/bin/sh /var/jb/sweet-ssh.sh &
# 2026-10-25: Wi-Fi recorder (sweet-wifilog.sh -> /var/root/wifi-*.txt), in the background
[ -f /var/jb/sweet-wifilog.sh ] && /var/jb/bin/sh /var/jb/sweet-wifilog.sh >/dev/null 2>&1 &
exit 0
