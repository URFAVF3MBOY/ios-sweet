# ios-hotdogb

iOS 16.5 on the OnePlus 7T (`hotdogb`). Boots tethered: every boot is `fastboot boot` from a PC.

## Works

- Boot to the lock screen and home screen
- Touch, power and volume buttons, home gesture, app switcher
- GPU acceleration (Adreno 640), about 60 fps when swiping
- Wallpapers, widgets, app extensions
- Brightness (Control Center slider)
- Battery percentage (from the PMIC fuel gauge)
- Wi-Fi on open networks, internet
- Root shell over SSH (Wi-Fi or USB), apt
- USB networking to the PC
- Clock from the phone's real-time clock
- Extra Apple apps (Weather, Notes, Reminders, ...)

## Partly works

- Wi-Fi with a password (WPA2): joins usually fail
- Wi-Fi drops now and then
- USB networking can stop after 10-20 minutes
- Small screen glitches (black triangles, a few pixels off while scrolling)

## Doesn't work

- Audio (work started)
- Bluetooth
- Camera
- Sensors
- Cellular / calls / SMS
- Face ID, Apple Pay, App Store
- Settings > iPhone Storage (crashes)

## Credits

Built on Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), Mesa, the published Linux kernel sources for the hardware details, and [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet), which this started from. iOS itself is Apple's and is not included.
