#!/usr/bin/env python3
"""Put a rootless jailbreak prefix (Procursus bootstrap + Sileo + ElleKit, made by tools/mk-jb.sh
from HackiOS) into an iOS root made by tools/mkiosroot.sh.

  sudo sweet-jb-install.py MOUNTED_ROOT JB_PREFIX [--force]

MOUNTED_ROOT is the HFS+ image mounted read-write (after sweet-root-tweaks.py), JB_PREFIX is
mk-jb.sh's output jb/root/var/jb. Run as root. --force replaces an installed prefix (what was
installed on the phone with Sileo/apt is lost).

What it does, and why (HackiOS userland: tools/mk-jb.sh, userland/build.sh, userland/src/init.c):
  prefix       copied to /private/preboot/jb/procursus, and /private/var/jb is a symbolic link to it:
               the layout of a rootless jailbreak. LaunchServices refuses to register an application
               under /private/var ("Unable to register"); from /private/preboot it registers it.
               On this port /private/preboot is a plain directory of the one writable root volume,
               so the prefix stays across boots (HackiOS rebuilds it in memory every boot).
  owners       everything root:wheel, except mobile's corner (var/mobile, Sileo's sileolists),
               as HackiOS init.c does. Linux clears set-user-ID bits on chown, so every mode is put
               back afterwards (giveMeRoot, login, su, sudo need them).
  apps         every prefix app (Sileo.app) is also copied into /Applications: LaunchServices
               rebuilds its database from /Applications at every boot (HackiOS build.sh).
  /bin         chmod cp mkdir mv rm -> /var/jb/usr/bin/...: Sileo spawns them by /bin path and
               iOS's /bin has df and ps only (HackiOS build.sh, SILEO_BIN).
  launchd      a job com.sweet.jbprep in the launchd job cache (System/Library/xpc/launchd.plist,
               the file sweet-root-tweaks.py edits) runs /var/jb/prep.sh once per boot: dpkg's
               "firmware" package, uicache for apps not in /Applications, pwd_mkdb, postinsts.
               Log: /var/jb/var/log/jbprep.log.
Code signing: the port's AMFI stand-in (patches 0025/0035/0036) takes ad-hoc signed binaries on the
root mount as platform binaries; the prefix is on the root mount.
"""
import glob, os, plistlib, shutil, stat, sys

PREFIX_REL = 'private/preboot/jb/procursus'
PREFIX_ABS = '/private/preboot/jb/procursus'
MOBILE_DIRS = ['var/mobile', 'var/mobile/Library', 'var/mobile/Library/Caches',
               'var/mobile/Library/Preferences', 'var/mobile/Documents',
               'var/lib/apt/sileolists', 'var/lib/apt/sileolists/partial']
SILEO_BIN = ['chmod', 'cp', 'mkdir', 'mv', 'rm']
JOB_KEY = '/System/Library/LaunchDaemons/com.sweet.jbprep.plist'
JOB = {
    'Label': 'com.sweet.jbprep',
    'ProgramArguments': ['/var/jb/bin/sh', '/var/jb/prep.sh'],
    'RunAtLoad': True,
    'LaunchOnlyOnce': True,
    'UserName': 'root',
    'StandardOutPath': '/var/jb/var/log/jbprep.log',
    'StandardErrorPath': '/var/jb/var/log/jbprep.log',
    'EnvironmentVariables': {
        'PATH': '/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin'},
}


def own_tree(top, uid, gid):
    """lchown everything under top (top included), then put each mode back (set-ID bits)."""
    paths = [top]
    for dirpath, dirnames, filenames in os.walk(top):
        paths += [os.path.join(dirpath, n) for n in dirnames + filenames]
    for p in paths:
        st = os.lstat(p)
        os.lchown(p, uid, gid)
        if not stat.S_ISLNK(st.st_mode):
            os.chmod(p, stat.S_IMODE(st.st_mode))


def copy_plain(src, dst):
    """Copy a tree with file data, permission bits and symlinks only: no extended attributes,
    no copystat. Linux's hfsplus driver took WSL down (twice, same place) while Python's
    copytree copied a bundle out of the mounted image (reading HFS+ xattrs); every copy that
    read from the Linux disk and only wrote to HFS+ went through. So: read only from src
    (ext4), never from the image, and do not touch xattrs."""
    os.makedirs(dst, exist_ok=True)
    dir_modes = [(dst, stat.S_IMODE(os.lstat(src).st_mode))]
    for dirpath, dirnames, filenames in os.walk(src):
        rel = os.path.relpath(dirpath, src)
        out = dst if rel == '.' else os.path.join(dst, rel)
        for n in list(dirnames):
            s_ = os.path.join(dirpath, n)
            d_ = os.path.join(out, n)
            if os.path.islink(s_):
                os.symlink(os.readlink(s_), d_)
                dirnames.remove(n)          # do not descend into a linked directory
            else:
                os.mkdir(d_)
                dir_modes.append((d_, stat.S_IMODE(os.lstat(s_).st_mode)))
        for n in filenames:
            s_ = os.path.join(dirpath, n)
            d_ = os.path.join(out, n)
            if os.path.islink(s_):
                os.symlink(os.readlink(s_), d_)
            else:
                shutil.copyfile(s_, d_)
                os.chmod(d_, stat.S_IMODE(os.lstat(s_).st_mode))
    for d_, mode in reversed(dir_modes):
        os.chmod(d_, mode)


def count_setid(top):
    n = 0
    for dirpath, _, filenames in os.walk(top):
        for f in filenames:
            st = os.lstat(os.path.join(dirpath, f))
            if not stat.S_ISLNK(st.st_mode) and st.st_mode & (stat.S_ISUID | stat.S_ISGID):
                n += 1
    return n


def install_prefix(root, src, force):
    dst = os.path.join(root, PREFIX_REL)
    if os.path.lexists(dst):
        if not force:
            sys.exit(f'{dst} exists: already installed (use --force to replace it)')
        shutil.rmtree(dst)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    os.chmod(os.path.dirname(dst), 0o755)
    copy_plain(src, dst)
    os.makedirs(os.path.join(dst, 'var/log'), exist_ok=True)
    own_tree(dst, 0, 0)
    for d in MOBILE_DIRS:
        p = os.path.join(dst, d)
        os.makedirs(p, exist_ok=True)
        os.chmod(p, 0o755)
    own_tree(os.path.join(dst, 'var/mobile'), 501, 501)
    for d in MOBILE_DIRS[5:]:
        os.chown(os.path.join(dst, d), 501, 501)
    print(f'prefix: {PREFIX_ABS} ({count_setid(dst)} set-ID files kept)')
    return dst


def link_var_jb(root, force):
    link = os.path.join(root, 'private/var/jb')
    if os.path.islink(link):
        os.unlink(link)
    elif os.path.exists(link):
        if not force:
            sys.exit(f'{link} is a real directory: move it away or use --force')
        shutil.rmtree(link)
    os.symlink(PREFIX_ABS, link)
    os.lchown(link, 0, 0)
    print(f'/private/var/jb -> {PREFIX_ABS}')


def copy_apps(root, src):
    """From the source prefix on the Linux disk (not the copy on the image: see copy_plain)."""
    for app in sorted(glob.glob(os.path.join(src, 'Applications/*.app'))):
        dst = os.path.join(root, 'Applications', os.path.basename(app))
        if os.path.lexists(dst):
            shutil.rmtree(dst)
        copy_plain(app, dst)
        own_tree(dst, 0, 0)
        print(f'/Applications/{os.path.basename(app)} ({count_setid(dst)} set-ID files kept)')


def bin_links(root):
    for c in SILEO_BIN:
        p = os.path.join(root, 'bin', c)
        if os.path.lexists(p) and not os.path.islink(p):
            print(f'/bin/{c}: a real file is there, left alone')
            continue
        if os.path.islink(p):
            os.unlink(p)
        os.symlink(f'/var/jb/usr/bin/{c}', p)
        os.lchown(p, 0, 0)
    print('/bin: ' + ' '.join(SILEO_BIN) + ' -> /var/jb/usr/bin')


def launchd_job(root):
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    plist['LaunchDaemons'][JOB_KEY] = JOB
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, stat.S_IMODE(st.st_mode))
    print(f'launchd.plist: job {JOB["Label"]} (runs /var/jb/prep.sh once per boot)')


def main():
    args = [a for a in sys.argv[1:] if a != '--force']
    force = '--force' in sys.argv[1:]
    if len(args) != 2:
        sys.exit(__doc__)
    root, src = args
    if os.geteuid() != 0:
        sys.exit('run as root (sudo): files must belong to root/mobile on the image')
    for need in ('System/Library/xpc/launchd.plist', 'Applications', 'bin', 'private/var'):
        if not os.path.exists(os.path.join(root, need)):
            sys.exit(f'{root} does not look like a mounted iOS root (no {need})')
    for need in ('prep.sh', 'Applications/Sileo.app', 'Library/dpkg/status'):
        if not os.path.exists(os.path.join(src, need)):
            sys.exit(f'{src} is not mk-jb.sh output (no {need}): run tools/mk-jb.sh first')
    if count_setid(src) == 0:
        sys.exit(f'{src} has no set-user-ID files (sudo, login, Sileo\'s giveMeRoot): it was made on a '
                 'filesystem without Unix permissions (WSL: /mnt/c). Run tools/mk-jb.sh in the Linux '
                 'home directory (~) and pass that jb/root/var/jb.')
    prefix = install_prefix(root, src, force)
    link_var_jb(root, force)
    copy_apps(root, src)
    bin_links(root)
    launchd_job(root)
    print('done: boot, then open Sileo from the home screen (installing packages needs the network)')


if __name__ == '__main__':
    main()
