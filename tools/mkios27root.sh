#!/bin/bash
# Build the iOS 27.2 (24B119, iPhone15,2) root image for the 7T's userdata partition, on Linux/WSL (needs a kernel
# with hfsplus, run as root). Same layout as tools/mkiosroot.sh for iOS 16: one case-sensitive journaled HFS+
# volume, OS at /, SystemOS cryptex at /private/preboot/Cryptexes/OS, AppOS at .../App, the shared cache and
# cryptex-only files hard-linked to their root paths.
#
#   mkios27root.sh ORIG LOW EXTRAS OUT.img [SIZE]      (SIZE default 44774195200 = the 7T's userdata - 8 MiB,
#                                                      partition-size:userdata 0xA6D400000; the kernel log (0047)
#                                                      writes the last 4 MiB + 4 KiB of the partition)
#
#   ORIG    files copied out of the IPSW: ORIG/{os,systemos,appos}/root (apfs-fuse copies, uncompressed)
#   LOW     lowered arm64 images (tools/lower): LOW/{os,systemos}/<same paths>, LOW/dsc/<shared cache files>
#   EXTRAS  bootstrap-ssh-iphoneos-arm64.tar.zst (Procursus rootless), authorized_keys, ssh_host_keys/ (optional,
#           made and saved there on the first build)
#
# iOS 27 changes on top of the iOS 16 layout (facts from the ios-guacamole port, rewritten here):
#   - lowered files are written INTO the copied files (cat >), so modes and hard links stay Apple's
#   - staged system apps go to /Applications (27.2's installd refuses to stage on a non-APFS /private/var)
#   - launchd.plist: no FairPlay (fairplayd.H2 restarts forever), no PerfPowerServices (spins on missing SMC keys);
#     sshd + pwd_mkdb boot jobs for the jailbreak
#   - Setup skipped (purplebuddy prefs), CoreAnimation without deep/compressed framebuffers, en0 network service
#   - Procursus rootless bootstrap in /var/jb (root and mobile password "alpine"), /bin/{sh,bash,zsh} links
set -euo pipefail

[[ $# -ge 4 ]] || { sed -n '2,15p' "$0"; exit 2; }
orig=$(realpath "$1") low=$(realpath "$2") extras=$(realpath "$3") out=$4 size=${5:-44774195200}
here=$(dirname "$(realpath "$0")")
[[ $(id -u) == 0 ]] || { echo "run as root" >&2; exit 1; }
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }
for d in "$orig/os/root" "$orig/systemos/root" "$orig/appos/root" "$low/os" "$low/systemos" "$low/dsc"; do
	[[ -d $d ]] || { echo "missing $d" >&2; exit 1; }
done

R=$(mktemp -d)
cleanup() { mountpoint -q "$R" && umount "$R" || true; mountpoint -q "$R" || rmdir "$R"; }
trap cleanup EXIT

truncate -s "$size" "$out"
mkfs.hfsplus -s -J -v iOSRoot27 "$out" >/dev/null
mount -t hfsplus -o loop,force "$out" "$R"

copy() { echo "copy $1 -> /${2#"$R"/}"; mkdir -p "$2"; rsync -aH --numeric-ids "$1/" "$2/"; }
copy "$orig/os/root" "$R"
copy "$orig/systemos/root" "$R/private/preboot/Cryptexes/OS"
copy "$orig/appos/root" "$R/private/preboot/Cryptexes/App"

# Lowered images over their originals: write the content in place (owner, mode, hard links unchanged).
overlay() {
	local n=0
	while IFS= read -r -d '' f; do
		local rel=${f#"$1"/}
		[[ -f "$2/$rel" ]] || { echo "overlay: no original for $rel" >&2; exit 1; }
		cat "$f" > "$2/$rel"; n=$((n + 1))
	done < <(find "$1" -type f -print0)
	echo "lowered over $3: $n files"
}
overlay "$low/os" "$R" "OS"
overlay "$low/systemos" "$R/private/preboot/Cryptexes/OS" "SystemOS"
dsc=private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
overlay "$low/dsc" "$R/$dsc" "shared cache"

# Owners: ORIG was copied out of the IPSW without root, so every file carries the build user's uid (boot 27m: the
# kernel refused the shared cache "owned by uid=1000 instead of 0"; 27p: tzinit failed). Apple's system volume is
# root:wheel throughout and mobile (501) owns its home; the steps below set their own owners on top of this.
chown -R 0:0 "$R"
[[ -d $R/private/var/mobile ]] && chown -R 501:501 "$R/private/var/mobile"
echo "owners: root:wheel, /private/var/mobile mobile:mobile"

# Shared cache where dyld looks without cryptex mounts: hard links, no extra space.
mkdir -p "$R/System/Library/Caches/com.apple.dyld"
for f in "$R/$dsc"/*; do ln -f "$f" "$R/System/Library/Caches/com.apple.dyld/${f##*/}"; done

# Cryptex files with no counterpart in the OS root: hard-linked (symlinks copied: Linux hfsplus cannot hard-link
# them) to their root paths.
n=0
for c in OS App; do
	while IFS= read -r -d '' f; do
		f=${f#./}
		if [[ ! -e $R/$f && ! -L $R/$f ]]; then
			mkdir -p "$R/$(dirname "$f")"
			src="$R/private/preboot/Cryptexes/$c/$f"
			if [[ -L $src ]]; then cp -P "$src" "$R/$f"; else ln "$src" "$R/$f"; fi
			n=$((n + 1))
		fi
	done < <(cd "$R/private/preboot/Cryptexes/$c" && find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*' -print0)
done
echo "cryptex-only files linked to root paths: $n"

# Staged system apps -> /Applications.
st="$R/private/var/staged_system_apps" n=0
if [[ -d $st ]]; then
	for a in "$st"/*.app; do
		[[ -e $a ]] || continue
		if [[ -e $R/Applications/${a##*/} ]]; then echo "staged ${a##*/}: already in /Applications"; continue; fi
		mv "$a" "$R/Applications/"; n=$((n + 1))
	done
fi
echo "staged system apps moved to /Applications: $n"

# logd's stores (owner _logd 272, .migrated markers) and the plain system keybag, as for iOS 16.
db="$R/private/var/db"
install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do install -o 272 -g 272 -m 0644 /dev/null "$db/$d/.migrated"; done
install -d -m 0700 -o 0 -g 0 "$R/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' |
	install -m 0600 -o 0 -g 0 /dev/stdin "$R/private/var/keybags/systembag.kb"

# Preferences: Setup skipped, CoreAnimation framebuffers, en0 network service.
F="$here/ios27root"
mp="$R/private/var/mobile/Library/Preferences"
install -d -o 501 -g 501 -m 0755 "$mp"
for f in com.apple.purplebuddy.plist com.apple.purplebuddy.notbackedup.plist com.apple.backboardd.plist .GlobalPreferences.plist; do
	install -o 501 -g 501 -m 0600 "$F/$f" "$mp/$f"
done
install -d -o 0 -g 0 -m 0755 "$R/private/var/preferences/SystemConfiguration"
install -o 0 -g 0 -m 0644 "$F/preferences.plist" "$R/private/var/preferences/SystemConfiguration/preferences.plist"
echo "prefs: Setup skipped, CoreAnimation framebuffers, en0 service"

# Jailbreak: Procursus rootless bootstrap in /var/jb.
bs="$extras/bootstrap-ssh-iphoneos-arm64.tar.zst"
[[ -f $bs ]] || { echo "missing $bs" >&2; exit 1; }
# tar drops the trailing slash of the "./var/" entry before --transform, so the pattern also has to match a bare
# "./var": otherwise tar puts an empty directory in place of the /var link (boot 28g: every /var/... path ENOENT,
# tzinit exit(2)).
zstd -dc "$bs" | tar -xpf - -C "$R" --numeric-owner \
	--transform 's#^\./var\(/\|$\)#./private/var\1#;s#^var\(/\|$\)#private/var\1#'
[[ -x $R/private/var/jb/usr/sbin/sshd ]] || { echo "bootstrap: no /var/jb/usr/sbin/sshd" >&2; exit 1; }
for l in var tmp etc; do
	[[ -L $R/$l ]] || { echo "bootstrap: /$l is no longer a symlink" >&2; exit 1; }
done
hk="$extras/ssh_host_keys"
mkdir -p "$R/private/var/jb/etc/ssh"
if ls "$hk"/ssh_host_*_key >/dev/null 2>&1; then
	cp "$hk"/ssh_host_* "$R/private/var/jb/etc/ssh/"; echo "ssh host keys reused"
else
	mkdir -p "$hk"
	for t in ed25519 ecdsa rsa; do ssh-keygen -q -t $t -N '' -f "$hk/ssh_host_${t}_key"; done
	cp "$hk"/ssh_host_* "$R/private/var/jb/etc/ssh/"; echo "ssh host keys made (saved in $hk)"
fi
chown 0:0 "$R/private/var/jb/etc/ssh/"ssh_host_*
chmod 0600 "$R/private/var/jb/etc/ssh/"ssh_host_*_key
chmod 0644 "$R/private/var/jb/etc/ssh/"ssh_host_*.pub
pw="$R/private/var/jb/etc/master.passwd"
mh=$(awk -F: '$1=="mobile"{print $2}' "$pw")
[[ -n $mh ]] && awk -F: -v OFS=: -v h="$mh" '$1=="root" && $2=="!"{$2=h}1' "$pw" > "$pw.new" &&
	chmod 0600 "$pw.new" && mv "$pw.new" "$pw" && echo "root password = mobile's (alpine)"
for sh in sh bash zsh; do
	if [[ ! -e $R/bin/$sh && ! -L $R/bin/$sh ]]; then
		for t in "/var/jb/bin/$sh" "/var/jb/usr/bin/$sh"; do
			if [[ -e $R/private$t || -L $R/private$t ]]; then ln -s "$t" "$R/bin/$sh"; echo "/bin/$sh -> $t"; break; fi
		done
	fi
done
if [[ -f $extras/authorized_keys ]]; then
	for h in root:0:0 mobile:501:501; do
		IFS=: read -r who uid gid <<<"$h"
		for home in "$R/private/var/jb/var/$who" "$R/private/var/$who"; do
			install -d -o "$uid" -g "$gid" -m 0755 "$home"
			install -d -o "$uid" -g "$gid" -m 0700 "$home/.ssh"
			install -o "$uid" -g "$gid" -m 0600 "$extras/authorized_keys" "$home/.ssh/authorized_keys"
		done
	done
	echo "authorized_keys for root and mobile"
fi
install -d -m 0755 "$R/private/var/local"

# launchd.plist: drop jobs that loop here, add the jailbreak's boot jobs.
python3 - "$R/System/Library/xpc/launchd.plist" <<'PYEOF'
import plistlib, sys
p = sys.argv[1]
d = plistlib.load(open(p, 'rb'))
ld = d['LaunchDaemons']
gone = [k for k in list(ld) if any(s in k for s in ('com.apple.fairplayd.H2', 'com.apple.PerfPowerServices.plist'))]
for k in gone:
    del ld[k]
ld['/System/Library/LaunchDaemons/com.sweet.sshd.plist'] = {
    'Label': 'com.sweet.sshd', 'ProgramArguments': ['/var/jb/usr/sbin/sshd', '-D', '-e'], 'RunAtLoad': True,
    'KeepAlive': True, 'POSIXSpawnType': 'Interactive', 'StandardErrorPath': '/var/local/sshd.out'}
ld['/System/Library/LaunchDaemons/com.sweet.pwdmkdb.plist'] = {
    'Label': 'com.sweet.pwdmkdb', 'ProgramArguments': ['/var/jb/usr/sbin/pwd_mkdb', '-p', '/var/jb/etc/master.passwd'],
    'RunAtLoad': True, 'POSIXSpawnType': 'Interactive', 'StandardOutPath': '/var/local/pwdmkdb.out',
    'StandardErrorPath': '/var/local/pwdmkdb.out'}
data = plistlib.dumps(d, fmt=plistlib.FMT_BINARY)
with open(p, 'r+b') as f:
    f.write(data)
    f.truncate()
print('launchd.plist: removed', gone, '+ com.sweet.sshd, com.sweet.pwdmkdb')
PYEOF

df -h "$R" | tail -1
for l in var tmp etc; do
	[[ $(readlink "$R/$l") == private/* ]] || { echo "root: /$l is not the private/ link" >&2; exit 1; }
done
umount "$R"
# Linux hfsplus decomposes some non-ASCII names differently from Apple's tables; let fsck fix them, then verify.
fsck.hfsplus -f -y "$out" | grep -v 'Illegal name' || true
fsck.hfsplus -f -n "$out"
echo "done: $out"
