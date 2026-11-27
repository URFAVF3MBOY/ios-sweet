#!/usr/bin/env bash
# Finish an image whose tools/mkiosroot.sh run stopped at "install: invalid user: '272'"
# (everything up to the cryptex links was already done). Runs only the steps that come after:
# logd's log stores, the system keybag, unmount, fsck. Owners set with chown (numeric 272 = _logd).
#
#   tools/mkiosroot-finish.sh ios165-root.img
set -euo pipefail
[[ $# -eq 1 ]] || { sed -n '2,7p' "$0"; exit 2; }
out=$1
[[ -f $out ]] || { echo "no image $out" >&2; exit 1; }

mnt=$(mktemp -d)
cleanup() {
	if mountpoint -q "$mnt"; then sudo umount "$mnt" 2>/dev/null || true; fi
	mountpoint -q "$mnt" || rmdir "$mnt" 2>/dev/null || true
}
trap cleanup EXIT

sudo fsck.hfsplus -f -y "$out" | grep -v 'Illegal name' || true
sudo mount -t hfsplus -o loop,force "$out" "$mnt"
[[ -d $mnt/System/Library/xpc && -d $mnt/private/preboot/Cryptexes/OS ]] ||
	{ echo "$out does not look like a finished mkiosroot.sh copy (no System or cryptexes)" >&2; exit 1; }

# logd's log stores (as mkiosroot.sh): owner _logd (272), modes, .migrated markers
db="$mnt/private/var/db"
sudo install -d -m 0751 "$db/diagnostics"
sudo install -d -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
	sudo install -m 0644 /dev/null "$db/$d/.migrated"
done
sudo chown 272:272 "$db/diagnostics" "$db/uuidtext" "$db/diagnostics/timesync" \
	"$db/diagnostics/.migrated" "$db/uuidtext/.migrated" "$db/diagnostics/timesync/.migrated"
echo "logd stores: done"

# System keybag for the built-in AppleKeyStore (no passcode), as mkiosroot.sh
sudo install -d -m 0700 -o root -g root "$mnt/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' |
	sudo install -m 0600 -o root -g root /dev/stdin "$mnt/private/var/keybags/systembag.kb"
echo "keybag: done"

df -h "$mnt"
sudo umount "$mnt"
# as mkiosroot.sh: let fsck rewrite the non-ASCII names Linux hfsplus decomposed, then verify
sudo fsck.hfsplus -f -y "$out" | grep -v 'Illegal name' || true
sudo fsck.hfsplus -f -n "$out"
echo "finished: $out"
