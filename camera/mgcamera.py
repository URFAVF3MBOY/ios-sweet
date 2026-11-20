# mgcamera.py: tell MobileGestalt this phone has cameras (userland test before the kernel/device-tree way).
# MobileGestalt answers from its cache file first; its CacheExtra dictionary holds per-answer overrides keyed by
# the obfuscated name base64(md5("MGCopyAnswer" + key))[:22]. The ADT has no camera nodes, so the computed answers
# were still-camera / rear-facing-camera / front-facing-camera = false (seen 2026-11-20) and iOS hid the camera.
# Run on the phone as root:  python3 mgcamera.py [--undo]   (backup: /var/root/MobileGestalt.plist.camera-bak)
import base64, hashlib, os, plistlib, shutil, sys

CACHE = ('/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/'
         'Library/Caches/com.apple.MobileGestalt.plist')
BACKUP = '/var/root/MobileGestalt.plist.camera-bak'
ANSWERS = {
    'still-camera': True,
    'rear-facing-camera': True,
    'front-facing-camera': True,
    'auto-focus-camera': True,
    # not 'camera-flash': with it, SpringBoard's lock screen asks mediaserverd's com.apple.coremedia.flashlight
    # at boot and waits forever (seen 2026-11-20, logs/sb-camera-hang.spin): the flashlight needs the camera driver.
    'video-camera': True,
}


def obfuscated(key):
    return base64.b64encode(hashlib.md5(b'MGCopyAnswer' + key.encode()).digest()).decode()[:22]


if len(sys.argv) > 1 and sys.argv[1] == '--undo':
    shutil.copy2(BACKUP, CACHE)
    print('restored', CACHE)
    sys.exit(0)
if not os.path.exists(BACKUP):
    shutil.copy2(CACHE, BACKUP)
with open(CACHE, 'rb') as f:
    cache = plistlib.load(f)
extra = cache.setdefault('CacheExtra', {})
for key, value in ANSWERS.items():
    name = obfuscated(key)
    print('%-22s %-20s was %r -> %r' % (name, key, extra.get(name), value))
    extra[name] = value
with open(CACHE + '.new', 'wb') as f:
    plistlib.dump(cache, f, fmt=plistlib.FMT_BINARY)
st = os.stat(CACHE)
os.chown(CACHE + '.new', st.st_uid, st.st_gid)
os.chmod(CACHE + '.new', st.st_mode & 0o7777)
os.replace(CACHE + '.new', CACHE)
print('written', CACHE)
