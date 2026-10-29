#!/var/jb/bin/sh
# 0129: SSH on the OnePlus 7T port. Started by prep.sh once per boot; log in /var/jb/var/log/sweet-ssh.log.
# The phone answers at the address the kernel gives en0 (default 10.0.0.237, 0129): ssh root@10.0.0.237
exec >>/var/jb/var/log/sweet-ssh.log 2>&1
PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin
echo "== sweet-ssh $(date)"

# sshd drops privileges to the user "sshd"; iOS and the prefix only have "_sshd" (uid 75). Add the
# name once, with the same ids, to every user database a lookup might read.
for f in /etc/passwd /var/jb/etc/passwd; do
	[ -f "$f" ] && ! grep -q '^sshd:' "$f" && echo 'sshd:*:75:75:sshd Privilege separation:/var/jb/var/empty:/usr/bin/false' >>"$f" && echo "added sshd to $f"
done
for f in /etc/master.passwd /var/jb/etc/master.passwd; do
	[ -f "$f" ] && ! grep -q '^sshd:' "$f" && echo 'sshd:*:75:75::0:0:sshd Privilege separation:/var/jb/var/empty:/usr/bin/false' >>"$f" && echo "added sshd to $f"
done
/var/jb/usr/sbin/pwd_mkdb -p /var/jb/etc/master.passwd 2>/dev/null
id sshd || echo "WARNING: no user sshd for lookups: sshd will refuse to start"

# The PC's public key (dropped by the kernel to /var/jb/etc/ssh/sweet_authorized_keys) for root,
# in every place root's home might be. Root has no password: key login only.
key=/var/jb/etc/ssh/sweet_authorized_keys
if [ -s "$key" ]; then
	for home in /var/root /var/jb/var/root $(eval echo ~root); do
		[ -d "$home" ] || continue
		mkdir -p "$home/.ssh" && cp "$key" "$home/.ssh/authorized_keys" &&
			chown -R 0:0 "$home/.ssh" && chmod 700 "$home/.ssh" && chmod 600 "$home/.ssh/authorized_keys" &&
			chmod go-w "$home" && echo "key installed in $home/.ssh"
	done
else
	echo "WARNING: no $key: nobody can log in"
fi
chmod 755 /var/jb/var/empty 2>/dev/null; chown 0:0 /var/jb/var/empty 2>/dev/null

# Host keys on first use (RSA takes a few seconds), then sshd as a daemon on port 22.
/var/jb/bin/sh /var/jb/usr/libexec/sshd-keygen-wrapper -E /var/jb/var/log/sshd.log
echo "sshd started: $? (sshd's own log: /var/jb/var/log/sshd.log)"
