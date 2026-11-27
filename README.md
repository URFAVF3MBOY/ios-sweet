# ios-hotdogb

iOS 16.5 on the OnePlus 7T (`hotdogb`, Snapdragon 855, Adreno 640). Boots tethered: every boot is `fastboot boot`
from a PC, nothing is flashed. The kernel is a patched XNU (`xnu/patches/`).

## Works

- Boots to the lock screen in seconds (kernel up ~5 s after fastboot)
- Touch, power and volume buttons, home gesture, app switcher
- GPU acceleration (Adreno 640): the UI draws through **Metal** (my Metal driver over metal2vulkan and Mesa's
  Turnip), ~56 fps when swiping; OpenGL ES also works
- **Vulkan 1.3** (Mesa Turnip on my own kernel GPU interface)
- **Metal** for apps that get it: buffers, compute and render pipelines, textures, IOSurfaces
- Wallpapers, widgets, App Library, Control Center
- Safari, video playback (YouTube in Safari is 360p only: that's YouTube's choice for iOS 16)
- Speakers and iOS audio, volume keys
- Bluetooth (pairing, devices)
- Wi-Fi with WPA2, internet (joined by hand)
- Battery percentage and charging
- USB: iPhone mode (shows up as an iPhone on the PC) or USB networking
- Root shell over SSH, apt

## Partly works

- Wi-Fi doesn't rejoin on its own after boot (iOS thinks the signal is too weak)
- Camera: the sensor streams real frames into memory, but iOS's camera app doesn't see it yet (work going on)
- Metal compute is slower than Vulkan for the same work

## Doesn't work

- Sensors
- Cellular / calls / SMS
- Face ID, Apple Pay, App Store

## Layout

- `xnu/patches/`: the kernel patches, built by GitHub Actions (`xnu.yml`, `xnu-intel.yml`)
- `vulkan/`: `sweetdrm` (libdrm over my kernel GPU driver), Mesa patches, Vulkan tests and benchmarks
- `metal/`: the Metal driver (`SweetMTL.bundle`: Objective-C front over a Rust bridge to metal2vulkan)
- `camera/`: the iOS camera plugin work
- `shim/`: the small first-stage loader fastboot starts
- `tools/`: image building, phone tools, profiling
- `NOTES.md`: the working log

## Credits

Built on Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org),
[blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw),
[Mesa](https://gitlab.freedesktop.org/mesa/mesa) (freedreno, Turnip),
[steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan), the published Linux kernel sources for the
hardware details, and [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet), which this started from.
iOS itself is Apple's and is not included.
