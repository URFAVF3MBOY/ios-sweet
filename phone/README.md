# Metal on the Adreno 640 (metal2vulkan + Turnip)

Copied from ios-flame by Yofuness (https://github.com/Yofuness/ios-flame, commit 28fb6363), MIT: `LICENSE.ios-flame`.
`metal2/bridge/` keeps its own MIT license file (`metal2/bridge/LICENSE`).

- `metal2/`: SweetMetal2.bundle (Metal frontend over the metal2vulkan bridge).
- `turnip/`: the kernel ABI Turnip's darwin backend speaks (`NativeAdrenoABI.h`), Mesa patches, vktest/v2test.
- Build: `.github/workflows/metal2.yml` with `tools/build-turnip-ios.sh` and `tools/build-m2v-bridge.sh`.

The kernel side (an IOAcceleratorES user client of open type 0x4147 on RedwoodGPU) lives in `xnu/patches`.
