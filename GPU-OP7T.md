# Adreno 640 bring-up on OnePlus 7T (hotdogb, SM8150)

Working notes for GPU acceleration on the OP7T port. Keep this file current;
`NOTES.md` links here from the active handoff.

## Goal and honest scope

1. **Milestone G1 - GPU alive:** GMU firmware booted, GPU powered, CP (command
   processor) running our ringbuffer and writing a fence value to memory.
2. **Milestone G2 - useful work:** GPU blits/fills (e.g. the IOMFB present copy
   that NOTES.md identifies as a big per-frame cost).
3. **Milestone G3 - iOS rendering on the GPU:** needs a Metal-compatible driver
   layer for Adreno (QuartzCore's GPU renderer expects Metal). Research-scale;
   the CPU renderer (CA::OGL::SWContext) stays the fallback.

## Decision 2026-10-20: no read-only probe stage

User wants to skip the "stage 0" read-only hardware probe proposed earlier
(`shim/out/what u were doing.txt`). All bring-up values below come from the
live DT + kernel sources instead.

## Decision 2026-10-20: bring-up in the shim, not XNU

Byte-patching a whole driver into the XNU Mach-O is not practical (new code,
IOKit classes, symbols). Instead do G1 in `shim/` before XNU starts:
- the shim already identity-maps all PA as Device memory (`shim/mmu.c`), so
  MMIO and uncached DMA buffers work without new mapping code;
- it builds locally in seconds (`shim/build.sh`), no CI kernel run per attempt;
- results print on the shim framebuffer console.
GPU buffers must live in RAM that XNU will not hand out: outside
`dram-base/dram-size` in the ADT (e.g. low bank when booting `--highmem`),
and not in any reserved-memory carve-out. XNU later only needs a small driver
to submit work to the already-running GPU. Byte patching stays useful for tiny
kernel tweaks (constants, NOPing a check), not for adding drivers.

## Hardware facts (from `dtb/hotdogb-live.dts`, verified 2026-10-20)

| Block | Address | Notes |
|---|---|---|
| GPU (kgsl-3d0) | 0x02c00000, size 0x40000 | chipid 0x06040001 (A640), IRQ SPI 300 |
| GMU | 0x02c6a000, size 0x30000 | IRQs SPI 304 (hfi), 305 (gmu); window also covers GPUCC |
| GMU PDC cfg / seq | 0x0b290000 / 0x0b490000 | size 0x10000 each |
| GPUCC | 0x02c90000 | CX GDSC @ +0x106c, GX GDSC @ +0x100c |
| GPU SMMU (qsmmu-v500) | 0x02ca0000, size 0x10000 | `qcom,skip-init`; GPU SIDs 0 mask 0x401, GMU user SID 4 / kernel SID 5 mask 0x400 |
| GCC GPU branches | GCC 0x00100000 + 0x71004 / 0x7100c / 0x71018 | cfg_ahb / memnoc_gfx / snoc_dvm |
| cmd-db | locator 0x0c3f000c | ARC levels `gfx.lvl`, `mx.lvl`, `cx.lvl` |
| Zap region (`pil_gpu_region`) | 0x98b15000, size 0x2000 | **differs from upstream (0x98715000)**; PAS id 13 |
| Speed-bin fuse | qfprom 0x784130, bits 31:29 | bin 0 table is a subset of bin 1 -> safe default |

GPU frequencies (speed bin 0): 585, 499.2, 427, 345, 257 MHz (initial level 4 = 257).
GMU: 200 MHz. Highest bank bit 15, UBWC mode 3, GMEM 1 MB.

## Firmware (pulled via TWRP into `gpu_accel/`, keep out of git)

- `a630_sqe.fw` (32304 B) - CP microcode, same file upstream uses for A640.
- `a640_gmu.bin` (37680 B) - block format (`{addr,size,type,value,data[]}`);
  blocks land in ITCM (0x0, 16K), DTCM (0x40000, 16K) or GMU-VA icache
  (0x4000) / dcache (0x44000) buffers.
- `a640_zap.mdt` (+ `.b00/.b01/.b02`, `.elf`) - relocatable, one PT_LOAD of
  0x7b0 bytes; fits the 0x2000 region.
- Also pulled: `abl_a.img`, `aop_a.img`, `hyp_a.img`, `tz_a.img` (reference only).
Plan: `tools/mkadt.py` embeds the blobs as properties of a new ADT node from
`gpu_accel/`, so they never enter the published patches.

## Windows DSDT reference (user's working WoA DSDT_5.aml, 2026-10-20)

Disassembled with iasl (built from acpica git; DSDT file kept out of git).
- `\_SB.MMU1` (`QCOM0509`) = the GPU SMMU at 0x02CA0000 size 0x10000, IRQs
  0x2C8-0x2CF, `_DEP` on `\_SB.PEP0`. **No PEP power component of its own**
  (no clocks/footswitch listed for MMU1); Windows' SMMU driver only touches
  it while the GPU component is powered.
- `\_SB.GPU0` PEP component 1, F0 ENTER order: FOOTSWITCH `cx_gdsc` on;
  CLOCK `gpu_cc_cxo_clk`, `gcc_ddrss_gpu_axi_clk`, `gpu_cc_cx_gmu_clk`,
  `gcc_gpu_memnoc_gfx_clk`; `gpu_cc_cx_gmu_clk` set to 0x0BEBC200 = 200 MHz;
  **BUSARB `ICBID_MASTER_GFX3D` -> `ICBID_SLAVE_EBI1`** (GPU->DDR bandwidth
  vote). F1 (off) reverses it. Neither `gcc_gpu_snoc_dvm_gfx_clk` nor
  `gcc_gpu_cfg_ahb_clk` appears anywhere in the DSDT.
- => Our steps 1 + 3a match Windows' GPU power-on except the **BUSARB
  bandwidth vote**, which we don't do yet. Best current lead for the step 3b
  SMMU read hang (unconfirmed).
- GPU0 MMUINFO: Miniport `ArmSmmuV2`, PAGETABLEUSAGE has a separate
  `GraphicsGmuPT` (type 3, 0x03030008) besides global/per-process/secure
  tables; `SMMUCount` 1; GMEM 0x100000.
- GPU0 PEP component 2 is the display (MDSS clocks), not GPU.

## Reference sources

- Upstream Linux v6.6 `drivers/gpu/drm/msm/adreno/` (`a6xx_gmu.c`,
  `a6xx_hfi.c`, `a6xx_gpu.c`): primary template, clean bare-metal sequence for
  A640 (non-legacy GMU, HFI v2, `a640_build_bw_table`, PDC offset 0x30090).
- OnePlus `android_kernel_oneplus_sm8150` branch `oneplus/SM8150_Q_10.0`
  `drivers/gpu/msm/` (KGSL) + `drivers/clk/qcom/gpucc-sm8150.c`: cross-check
  of values used by the shipping Android build.

## Bring-up order for G1 (from upstream, A640 path)

1. GCC GPU branch clocks on; GPUCC CX GDSC on; GMU clocks (GMU 200 MHz, cxo, ahb).
2. GPU SMMU: stage-1 context banks for GMU (SID 5) and GPU (SID 0/0x400).
3. GMU memory at fixed GMU VAs: dummy 0x60000000, icache 0x4000, dcache
   0x44000, then log + HFI queues in 0x60001000-0x80000000.
4. RSCC/PDC sequencer setup (`a6xx_gmu_rpmh_init`), RSC wake, GMU fw load,
   GMU start, HFI start.
5. HFI: perf table (ARC votes from cmd-db), bw table, core fw start, start.
6. GPU init (`hw_init`): GBIF, addressing, hwcg, UCHE, CP protect, SQE fw,
   ringbuffer, CP_ME_INIT, zap, fence write.

## Shim stage notes (gathered 2026-10-20)

- The shim boots the payload with the MMU **off** (`shim/shim.c` -> `loader_boot`):
  all accesses are Device memory, so use aligned 32/64-bit accesses only.
- GMU register window base 0x02c6a000; upstream `REG_A6XX_GMU_*` values are
  **dword** offsets from it (check: GX_GDSCR 0x9c03 -> 0x02c9100c, matches DT).
  CX_GDSCR = 0x02c9106c (dword 0x9c1b). RSCC = GMU base + 0x23000 (dword offsets).
- GMU firmware blocks: 0x0/0x200/0x400 (ITCM), 0x4000 (icache, GMU VA),
  0x40000/0x43fe0 (DTCM), 0x57000 (dcache, GMU VA) -> the GMU needs SMMU
  mappings before its firmware can boot.
- GPUCC GMU clock: RCG `gpu_cc_gmu_clk_src` CMD_RCGR 0x02c91120, 200 MHz =
  src 6 (GPLL0_OUT_MAIN_DIV) / 1.5; branches ahb 0x1078, cx_gmu 0x1098, cxo 0x109c.
- GCC: GPLL0 GPU votes 0x152004 bits 15/16; branches cfg_ahb 0x171004,
  memnoc_gfx 0x17100c, snoc_dvm 0x171018, ddrss_gpu_axi 0x171154, iref 0x18c010.
- OP7T RAM (live DT): 0x80000000+0x3bb00000, 0xc0000000+3G, 0x180000000+4G.
  Payload ADT in `shim/out/sweet.adt` uses dram 0x100000000+0xfcc00000.
  Candidate GPU scratch RAM: 0xb0000000 (16 MB) - after the last low carve-out
  (`mtp_mem` ends 0xace00000), below 0xbbb00000; must also be checked against
  the ABL initrd range the loader prints (`INITRD lo/hi`).
- Note: `shim/console.h` FB 0x9c000000 1080x2400 matches OP7T `cont_splash_region`.

## Shim step 1 (written 2026-10-20, NOT phone-tested yet)

Code: `shim/gpu.c` (+ `gpu.h`), called from `shim_main()` before
`loader_boot()`; `loader_cmdline()` in `shim/loader.c` hands it the payload
cmdline; `shim/build.sh` compiles/links `gpu.c`.

- Off unless the payload cmdline has `sweetgpu=1` (normal boots unchanged).
- Turns on: GCC GPLL0 votes for the GPU (0x152004 bits 15/16), `gpu_cfg_ahb`
  (0x171004), `gpu_memnoc_gfx` (0x17100c, voted), `gpu_snoc_dvm_gfx`
  (0x171018), `ddrss_gpu_axi` (0x171154, voted); GPU CX GDSC (0x2c9106c);
  GPUCC `ahb` (+0x1078), `cxo` (+0x109c), `cxo_aon` (+0x1004), `cx_gmu`
  (+0x1098, left at its 19.2 MHz XO default).
- Prints each register `was=`/`now=` and `ok`/`TIMEOUT`, then a summary line
  `GPU done fails=N`, holds the screen 10 s, then boots xnu as usual.
- Stops early (prints why) if `gpu_cfg_ahb` won't turn on, before any GPUCC access.
- Build check here (clang 18 + lld, same flags): compiles, links, `gpu.o` has
  0 ABS64 relocations and no libc calls. (Clang alone also reports 3 ABS64 in
  the unmodified `shim.c` and a `strcmp` call in `ufs.c`; GCC builds as before.)

Test: add ` sweetgpu=1` to the `--cmdline` of `tools/mkpayload.py`, rebuild
the payload and `./shim/build.sh shim/out/payload.bin`, `fastboot boot` it,
photograph the GPU lines. Expected good result: every line `ok`, CX GDSC
`now=` with bit 31 set (0x8...), `fails=00000000`.

### Boot results

- 2026-10-20 boot 1 (`sweetgpu=1`): user read the summary line as
  `done fails=00000001` (the `0000000A` was the 10 s hold value), so 1 step
  timed out; which one was not read off the screen. The shim then continued.
  The summary now prints `GPU FAIL <step>` lines / `GPU OK` instead.
- 2026-10-20 boot 2: `GPU FAIL SNOCDVM` (only failure). Observation: the
  `gcc_gpu_snoc_dvm_gfx` branch (0x171018) never cleared CLK_OFF within the
  wait. Definitions match in OnePlus and upstream gcc-sm8150.c (BRANCH_HALT).
  Guess: it was enabled before the GPU CX GDSC, and its consumer (GPU SMMU)
  is in the CX domain (upstream DT: SMMU power-domains = GPU_CX_GDSC).
  Change: memnoc/snoc_dvm/axi now enabled after CX GDSC; wait 500 -> 2000 us.
- 2026-10-20 boot 3: **`GPU OK` - step 1 passes** (all clocks + CX GDSC ok
  with the reorder). Confirms bus clocks must follow the CX GDSC.
  Photo values (sweetgpu=1): GPLL0 vote 0x01605D7F -> 0x0161DD7F; CFG AHB
  0x1 (already on from ABL); CX GDSC 0x00222001 -> 0xF8222000 (PWR_ON);
  MEMNOC 0x221 (already on); SNOCDVM 0x1 -> 0x1 (EN already set, CLK_OFF clear
  once CX is up); GPU AXI 0x80000000 -> 0x1; CC AHB 0x1; CXO 0x1;
  CXO AON 0x0 -> 0x0 (EN bit does not stick, CLK_OFF clear: likely
  hardware-controlled always-on, harmless); CX GMU 0x80004220 -> 0x4221
  (running at the 19.2 MHz XO default).

## Shim step 2: built-in firmware check (written 2026-10-20, NOT phone-tested)

- `shim/build.sh` generates `out/fw.S` that `.incbin`s
  `../gpu_accel/a640_gmu.bin` and `../gpu_accel/a630_sqe.fw` (symbols
  `sweet_gmu_fw[_end]`, `sweet_sqe_fw[_end]`); missing files only warn and give
  empty symbols. Firmware stays out of git (only in the local image).
- `sweetgpu=2` runs step 1 then step 2: walks the GMU blocks and checks the
  SQE version. No hardware access in step 2.
- Expected screen with the files we have (computed here from the same files):
  `GMU FW ok bytes=00009330 blocks=00000007`, `GMU TCM itcm=00000003
  dtcm=00000002`, `GMU VA icache=00000001 dcache=00000001`,
  `SQE FW ok bytes=00007E30 ver=00000190`, then `GPU OK`.
- GMU blocks: 0x0/0x30, 0x200/0x140, 0x400/0x38bc (ITCM); 0x4000/0x3a58
  (icache); 0x40000/0x1b20, 0x43fe0/0x14 (DTCM); 0x57000/0x308 (dcache).
- 2026-10-20 boot 4 (`sweetgpu=2`): **step 2 passes**, every value exactly as
  expected above (GMU 0x9330 bytes, 7 blocks, 3 ITCM / 2 DTCM / 1 icache /
  1 dcache; SQE 0x7E30 bytes, ver 0x190; `GPU OK`). Step 1 values identical
  to boot 3.

## Shim step 3a: GMU clock 200 MHz (written 2026-10-20, NOT phone-tested)

- `sweetgpu=3`: after steps 1-2 (skipped if they failed), programs
  `gpu_cc_gmu_clk_src` (GPUCC +0x1120 CMD, +0x1124 CFG): src_sel 6
  (GPLL0_OUT_MAIN_DIV, gpu_cc_parent_map_0) and src_div 2 (= /1.5), sets
  CMD UPDATE and waits for it to clear. 300 MHz / 1.5 = 200 MHz, the rate
  upstream `a6xx_gmu_resume()` uses.
- Prints `GMU CMD` (ok when UPDATE cleared, ROOT_OFF clear, CFG src/div
  match), `GMU CFG was/now`, and `CX GMU` branch state.
- Expected: `GMU CMD ok`, CFG `now=` low bits `...602`, `CX GMU on`.
- 2026-10-20 boot 5 (`sweetgpu=3`): **step 3a passes**: `GMU CMD ok`
  0x0 -> 0x0 (UPDATE cleared, root on), `GMU CFG` 0x00000001 (XO, div 1)
  -> 0x00000602, `CX GMU on cbcr=0x4221`, `GPU OK`. Steps 1-2 unchanged.

## Shim step 3b: GPU SMMU state, read-only (written 2026-10-20, NOT phone-tested)

- Why: DT marks the GPU SMMU `qcom,skip-init` (firmware pre-configured it)
  and doesn't say which stream-match (SMR/S2CR) entries or context banks are
  already in use. Read them before claiming any. Agreed with user 2026-10-20.
- `sweetgpu=4` (runs only if steps 1-3a had no failure): prints sCR0/IDR0/
  IDR1, page size, number of SMRs/CBs; for valid SMRs matching GPU SIDs
  0/0x400 or GMU SIDs 4/5 (max 6): `SMR nn smr= s2cr=` and, if it translates,
  `CB nn cbar=`; then totals: valid SMRs, first free SMR, first free CB,
  free CB count, EXIDS mode. Stops (`SMMU ID` fail) on odd geometry or
  non-4K pages. No writes.
- Next (3c): program a free SMR/S2CR + context bank for GMU SID 5 with a
  stage-1 table for the GMU buffers.
- 2026-10-20 boot 6 (`sweetgpu=4`): steps 1-3a ok again (same values). Screen
  stops right after `step 4: GPU SMMU state (read only)`: no SMMU lines, no
  blue exception screen; phone reset later on its own (user: "crashed after
  the wait"). Observation: the first read of the SMMU global space (sCR0 @
  0x02ca0000) never completed. Guess: SMMU global registers are not
  accessible from the shim's EL1 in this state (firmware-owned, matching
  `qcom,skip-init`), or need a clock/vote not yet on. **Use sweetgpu<=3 for
  normal boots.** Step 4 code stays (only runs at sweetgpu>=4).

- Remaining before the GMU can boot (step 3b+): GPU SMMU context bank for
  the GMU (SID 5) mapping icache 0x4000, dcache 0x44000, dummy 0x60000000,
  log/HFI buffers; then RSCC/PDC setup and the GMU start sequence.

## Shim step 7: RSCC/PDC setup + RSC sleep/wake (phone-tested 2026-10-20)

- `sweetgpu=7`, upstream `a6xx_gmu_rpmh_init()` A640 path (RSCC at GMU +
  0x23000 = 0x02c8d000, PDC seq 0x0b490000, PDC cfg 0x0b290000, PDC address
  offset 0x30090, TCS3 CMD0 DATA+4 = 0x3), then `a6xx_rpmh_stop()`,
  GENERAL_7 = 1, `a6xx_rpmh_start()`.
- First attempt skipped `a6xx_rpmh_stop()`: wake ACK timed out (val 0).
  With the sleep step: SLEEP ok (STATUS0 0x01010006, bit 16 set), wake ACK
  ok (0x3), SEQ_BUSY 0, STATUS0 after 0x01000000; AO host IRQ status read 0
  both times (RSCC_COMP not latched, probably masked); GPU OK, xnu boots.
- All three blocks (RSCC, PDC seq, PDC cfg) are reachable from the shim and
  read back what was written.

## Shim step 8: start the GMU (phone-tested 2026-10-20, NOT passing)

- `sweetgpu=8`: HFI queue table at GMU 0x60005000 (PA 0xB0095000, table size
  120 as upstream), log at 0x60001000 (PA 0xB0091000), boot registers per
  upstream `a6xx_gmu_fw_start()` (BOOT_CONFIG 2, CM3_CFG 0x4052, chipid
  0x06040100, PWR_COL_CP_MSG = log | 3), `a6xx_gmu_power_config()`, then
  CM3_SYSRESET 1 -> 0. Firmware version word (DTCM 0x43fe0) = 0x20000105,
  so success would be FW_INIT_RESULT = 0xBABEFACE.
- Result: the whole phone freezes right after the GMU is released (screen
  stops after the `SYSRESET=0 (release)` marker, sometimes mid-character;
  no reset, no crashdump). Pre-release: FW_INIT_RESULT 0, SYSRESET 1.
- Brief-run test (`sweetgmu=3`: release, `sweetgmuus` µs, halt again, print):
  survives at 1, 20 and 40 µs; freezes at 41 µs and above (repeatable).
  At 20 and 40 µs: FW_INIT 0, AO IRQ 0, CB0 FSR 0x400 (no fault; FAR/FSYNR
  are stale), CX GDSC F8222000, GX GDSC 00222001 (off), GMU clock unchanged,
  RSCC STATUS0 0x01000000, CX_BUSY 0x30000000, log/dummy/HFI memory untouched.
- Firmware disassembly (Cortex-M3, ITCM): reset -> VTOR 0x200 -> startup
  table at ITCM 0x3c7c: copy 0xF80 B 0x40BA0->0x43000, zero 0x1548 B at
  0x40E40, zero 4 B at 0x43F80 (all DTCM), then zero 0x4068 B at GMU VA
  0x57308 (dcache .bss) = first access to DDR through the GPU SMMU.
  Sentinels A5A5A5A5 at PA 0xB0063308/0xB0065000/0xB006736C are still
  intact at 40 µs -> the freeze coincides with that first SMMU->DDR access.
- Ruled out / tried, still freezes: GMU RPMh votes (RPMH_CTRL untouched),
  SMMU S2CR "fault" for SID 5, KGSL pre-release settings (CX_GMU_WFI_CONFIG
  0, GMU interrupts masked, AO AHB fence allow, KGSL fence range
  0x80A008A0), and an active RPMh vote for BCM SH3 (GPU SMMU walker port
  acm_gpu_tcu) from the apps RSC (cmd-db at 0x85F20000 size 0x1228, SH3 =
  0x50030, SH0 = 0x5003C; apps RSC DRV2 id 0x00010700 = v1.7, upstream
  ver_2_7 offsets; TCS cmd completed, status 0x00010101).
- PDC addresses checked against OnePlus KGSL: A640 v2 uses 0x0b290000 /
  0x0b490000 (sm8150-v2.dtsi), same as the shim.
- Reference for what Linux does around GMU start on this phone: OnePlus
  `drivers/gpu/msm/kgsl_gmu.c` `gmu_start()` (DDR bus vote "for GMU to
  init" via msm_bus before start) and `adreno_a6xx_gmu.c`
  (`a6xx_gmu_fw_start`, `a6xx_gmu_start`).
- Claude stopped working on the GMU start sequence on 2026-10-20 after an
  automated safety filter interrupted it; further step 8 work is done
  outside Claude.

## Status

- 2026-10-20: shim steps 1-7 pass on the phone (GPU CX power + clocks, GMU
  200 MHz, GPU SMMU CB0 for GMU SID 5, GMU firmware loaded, RSC/PDC set up
  and woken). Step 8 (GMU start) freezes the phone ~40 µs after release.
- Normal boots: use `sweetgpu=7` or lower (or omit it).
- Remaining after step 8: 9 HFI, 10 GX power + chipid, 11 SQE + ringbuffer
  + fence write (= G1; zap shader via TZ PAS 13 needed there).
