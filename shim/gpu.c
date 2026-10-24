/*
 * OP7T (SM8150) Adreno 640 bring-up, run before the xnu loader.
 * See GPU-OP7T.md. Enabled by "sweetgpu=N" in the payload cmdline (default
 * off, so normal boots are unchanged). N is the highest step to run:
 *   1 = GPU bus clocks (GCC) + GPU CX power domain + GPUCC CX clocks
 *   2 = + check the firmware built into this image (build.sh embeds
 *       ../gpu_accel/a640_gmu.bin and a630_sqe.fw); no hardware access
 *   3 = + GMU clock source 19.2 MHz -> 200 MHz (GPLL0 main div / 1.5),
 *       the rate upstream a6xx_gmu_resume() uses to boot the GMU
 *   4 = + READ-ONLY dump of the GPU SMMU state left by firmware for the
 *       GPU/GMU stream IDs (DT says qcom,skip-init): which SMR/S2CR/context
 *       banks are already set up and which are free. Writes nothing.
 *   5 = + program one free SMR/S2CR + context bank for GMU SID 5 with a
 *       stage-1 page table in scratch RAM (0xb0000000). Does not start GMU.
 *   ... 8 = + start the GMU (sweetgmu=7: full start, wait for BABEFACE)
 *   9 = + HFI: start the message queues, send H2F_MSG_FW_VER, read the
 *       GMU's reply (needs sweetgmu=7)
 *  10 = + HFI tables: perf/DCVS table (GX + GMU ARC votes from cmd-db),
 *       bus-vote table (one "off" level), feature ctrl ECP off,
 *       H2F_MSG_CORE_FW_START (KGSL hfi_start order for A640, HFI v2).
 *       Does not power GX yet (step 11).
 *  11 = + GX power: H2F_MSG_GX_BW_PERF_VOTE (257 MHz), OOB "gpu" request
 *       (KGSL a6xx_gmu_oob_set), H2F_MSG_START, then - only if the GMU
 *       granted it and SPTPRAC status says GX is on - read three 3D-core
 *       (RBBM) registers. Releases the OOB again at sweetgpu=11.
 *  12 = + (keeps GX on) the 3D core's own SMMU context (SID 0 mask 0x401,
 *       unprivileged pages: SQE microcode, 4 KiB ringbuffer, scratch page),
 *       the a6xx_start register subset, ringbuffer registers, SQE base,
 *       CP_SQE_CNTL = 1 (CP starts, no commands yet), then
 *       RBBM_SECVID_TRUST_CNTL = 0 with readback. sweetgpucb=1 also reads
 *       the GPU context bank's fault registers afterwards.
 *       (TRUST_CNTL access only with sweettrust=1: it switched the phone off.)
 *  13 = + first commands (planned "12c"): CP_ME_INIT and a CP_MEM_WRITE of
 *       0x600DCAFE to GPU VA 0x01011100; wait up to 2 s for it = G1.
 *  14 = + G2: the GPU draws a 250x250 green square into the framebuffer
 *       (PA 0x9C000000) with CP_MEM_WRITE rows from an indirect buffer, then
 *       the CPU reads the pixels back. Mappings are added in step 12, before
 *       the CP runs, so no SMMU maintenance happens while the GPU is active.
 *  15 = + G3: the 2D engine (CP_BLIT, BLIT_OP_SCALE) scales a 128x128
 *       gradient to 256x256, first into scratch memory (G3a), then onto the
 *       screen at x 100, y 2100 in two passes (even/odd rows, pitch 8640,
 *       because the framebuffer pitch 4320 is not a multiple of 64) (G3b).
 *       Options: sweetg3fb=0 (G3a only), sweetg3ccu=1, sweetg3magic=0.
 * Every register is printed before and after; the screen is held for a
 * few seconds, then the normal xnu boot continues. Clocks stay on.
 *
 * MMU is off here: all accesses are Device memory, aligned 32-bit only.
 * Offsets: OnePlus kernel drivers/clk/qcom/gcc-sm8150.c, gpucc-sm8150.c,
 * and the hotdogb live DT (gpu_cx_gdsc @ 0x2c9106c).
 */
#include <stdint.h>
#include "console.h"
#include "gpu.h"

/*
 * Output mute for steps already proven on the phone. gpu_stage() sets
 * con_mute while running steps 5-7 at sweetgpu>=8 and prints one summary
 * line instead (failures are still collected by note_fail and listed at
 * the end; rerun with sweetgpu=7 for the full detail of steps 5-7).
 */
static int con_mute;
static void con_str_m(const char *label, const char *s)
{
	if (!con_mute)
		con_str(label, s);
}
static void con_hex2_m(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	if (!con_mute)
		con_hex2(label, a, va, b, vb);
}
#define con_str con_str_m
#define con_hex2 con_hex2_m

#define GCC_BASE		0x00100000UL
#define GCC_GPLL0_VOTE		(GCC_BASE + 0x52004)	/* bit15 gpu_gpll0, bit16 gpu_gpll0_div */
#define GCC_GPU_CFG_AHB		(GCC_BASE + 0x71004)	/* BRANCH_HALT */
#define GCC_GPU_MEMNOC_GFX	(GCC_BASE + 0x7100c)	/* BRANCH_VOTED */
#define GCC_GPU_SNOC_DVM_GFX	(GCC_BASE + 0x71018)	/* BRANCH_HALT */
#define GCC_DDRSS_GPU_AXI	(GCC_BASE + 0x71154)	/* BRANCH_VOTED */

#define GPUCC_BASE		0x02c90000UL
#define GPUCC_CXO_AON		(GPUCC_BASE + 0x1004)
#define GPUCC_CX_GDSCR		(GPUCC_BASE + 0x106c)
#define GPUCC_AHB		(GPUCC_BASE + 0x1078)	/* BRANCH_HALT_DELAY */
#define GPUCC_CX_GMU		(GPUCC_BASE + 0x1098)
#define GPUCC_CXO		(GPUCC_BASE + 0x109c)
#define GPUCC_GMU_CMD_RCGR	(GPUCC_BASE + 0x1120)	/* gpu_cc_gmu_clk_src */
#define GPUCC_GMU_CFG_RCGR	(GPUCC_BASE + 0x1124)

#define RCG_CMD_UPDATE		(1U << 0)
#define RCG_CMD_ROOT_OFF	(1U << 31)
#define RCG_CFG_DIV_MASK	0x1fU		/* 2*div - 1 */
#define RCG_CFG_SRC_MASK	(7U << 8)
#define GMU_SRC_GPLL0_DIV	6U		/* gpu_cc_parent_map_0: P_GPLL0_OUT_MAIN_DIV */
#define GMU_DIV_1_5		2U		/* 2*1.5 - 1 */

#define CBCR_EN			(1U << 0)
#define CBCR_OFF		(1U << 31)
#define GDSC_SW_COLLAPSE	(1U << 0)
#define GDSC_PWR_ON		(1U << 31)

#define HOLD_S			10

/* GPU SMMU (MMU-500, DT kgsl-smmu@0x02ca0000, size 0x10000); arm-smmu.h */
#define SMMU_BASE		0x02ca0000UL
#define SMMU_SCR0		0x000
#define SMMU_IDR0		0x020
#define SMMU_IDR1		0x024
#define SMMU_SMR(n)		(0x800 + 4 * (n))
#define SMMU_S2CR(n)		(0xc00 + 4 * (n))
#define SMMU_CBAR(n)		(4 * (n))		/* in GR1 = page 1 */
#define SMMU_CBA2R(n)		(0x800 + 4 * (n))	/* in GR1; VA64 bit 0 */
#define CBA2R_VA64		(1U << 0)
#define SCR0_EXIDENABLE		(1U << 3)
#define S2CR_EXIDVALID		(1U << 10)

/* Context-bank page (GR1 + cb * pagesize). ARM SMMUv2 + Linux arm-smmu.h */
#define SMMU_CB_SCTLR		0x000
#define SMMU_CB_TTBR0		0x020
#define SMMU_CB_TTBR1		0x028
#define SMMU_CB_TTBCR		0x030
#define SMMU_CB_MAIR0		0x038
#define SMMU_CB_MAIR1		0x03c
#define SMMU_CB_FSR		0x058
#define SMMU_CB_TLBIVA		0x600
#define SMMU_CB_TLBIASID	0x610
#define SMMU_CB_TLBSYNC		0x7f0
#define SMMU_CB_TLBSTATUS	0x7f4
#define SMMU_CB_ATS1PR		0x800
#define SMMU_CB_PAR		0x850
#define SMMU_CB_ATSR		0x8f0
#define ATSR_ACTIVE		(1U << 0)
#define PAR_FAULT		(1U << 0)
/* CBAR in GR1: type S1_TRANS_S2_BYPASS = 1 << 16 on some; Linux uses CBAR_TYPE_S2_BYPASS=1 */
#define CBAR_TYPE_S1_S2_BYPASS	(1U << 16)
#define S2CR_TYPE_TRANS		0		/* translate via CB */
#define SMR_VALID		(1U << 31)
#define SCTLR_M			(1U << 0)
#define SCTLR_TRE		(1U << 1)
#define SCTLR_AFE		(1U << 2)
#define SCTLR_CFRE		(1U << 5)
#define SCTLR_CFIE		(1U << 6)
#define SCTLR_CFCFG		(1U << 7)
#define SCTLR_HUPCF		(1U << 8)

/* Scratch RAM for GMU buffers + SMMU page tables (outside xnu highmem range). */
#define SCRATCH_PA		0xb0000000UL
#define SCRATCH_SIZE		0x01000000UL	/* 16 MiB */
#define GMU_SID_KERNEL		5U
#define GMU_SID_MASK		0x400U		/* DT gmu_kernel <smmu 5 0x400>: SIDs 5 and 0x405 */

/* GMU virtual addresses that must be mapped (a6xx_gmu.c). */
#define GMU_VA_ICACHE		0x00004000UL
#define GMU_VA_DCACHE		0x00044000UL
#define GMU_VA_DUMMY		0x60000000UL
#define GMU_VA_LOG		0x60001000UL

/* GMU register window (DT 0x02c6a000). Offsets are DWORD indices (<<2). */
#define GMU_BASE		0x02c6a000UL
#define GMU_REG(d)		(GMU_BASE + ((uint32_t)(d) << 2))
#define GMU_ITCM_START		0x0c00U		/* dword; PA 0x02c6d000 */
#define GMU_DTCM_START		0x1c00U		/* dword; PA 0x02c71000 */
#define GMU_CM3_SYSRESET	0x5000U		/* 1 = core halted */
#define GMU_CM3_BOOT_CONFIG	0x5001U
#define GMU_CX_WFI_CONFIG	0x5002U
#define GMU_CM3_FW_INIT_RESULT	0x501cU
#define GMU_CM3_CFG		0x502dU
#define GMU_PWR_COL_CP_MSG	0x5100U
#define GMU_PWR_COL_CP_RESP	0x5101U
#define GMU_SYS_BUS_CONFIG	0x4c0fU
#define GMU_ICACHE_CONFIG	0x4c00U
#define GMU_DCACHE_CONFIG	0x4c01U
#define GMU_PWR_COL_INTER_FRAME_CTRL	0x50c0U
#define GMU_RPMH_CTRL		0x50e8U
#define GMU_HFI_SFR_ADDR	0x5182U
#define GMU_HFI_QTBL_INFO	0x5184U
#define GMU_HFI_QTBL_ADDR	0x5185U
#define GMU_AHB_FENCE_RANGE_0	0x9311U
#define GMU_DTCM_START		0x1c00U
/* Chip ID 0x06040001 -> GMU format 0x06040100 */
#define GMU_CHIPID_A640		0x06040100U
#define GMU_VA_HFI_LOG		0x60001000U
#define GMU_VA_HFI_QTBL		0x60005000U
#define GMU_ITCM_VA_BASE	0x00000000U
#define GMU_DTCM_VA_BASE	0x00040000U
#define GMU_TCM_SIZE		0x4000U

/* RSCC window = GMU + 0x23000 (non-A650). Offsets are DWORD indices. */
#define RSCC_BASE		0x02c8d000UL
#define RSCC_REG(d)		(RSCC_BASE + ((uint32_t)(d) << 2))
#define PDC_CFG_BASE		0x0b290000UL
#define PDC_SEQ_BASE		0x0b490000UL
#define PDC_REG(base, d)	((base) + ((uint32_t)(d) << 2))

/* GMU dword regs used by step 7 */
#define GMU_GENERAL_7		0x51ccU
#define GMU_AO_HOST_INTERRUPT_STATUS	0x9305U
#define GMU_AO_HOST_INTERRUPT_CLR	0x9304U
#define GMU_AO_HOST_INTERRUPT_MASK	0x9306U
#define GMU_AO_AHB_FENCE_CTRL	0x9310U
#define GMU_GMU2HOST_INTR_CLR	0x5191U
#define GMU_GMU2HOST_INTR_MASK	0x5193U
/* HFI (step 9): a6xx_reg.h dword offset - 0x1a800 */
#define GMU_HFI_CTRL_STATUS	0x5180U		/* bit 0: queues up */
#define GMU_HFI_VERSION_INFO	0x5181U
#define GMU_HFI_CTRL_INIT	0x5186U
#define GMU_GMU2HOST_INTR_INFO	0x5192U		/* bit 0: message queue has data */
#define GMU_HOST2GMU_INTR_SET	0x5194U		/* bit 0: doorbell for the command queue */
#define GMU_FENCE_RANGE_KGSL	0x80A008A0U
#define GMU_RSCC_CONTROL_REQ	0x9307U
#define GMU_RSCC_CONTROL_ACK	0x9308U
#define AO_IRQ_RSCC_COMP	(1U << 1)

/* RSCC dword regs (from a6xx_gmu.xml.h) */
#define RSCC_RSC_STATUS0_DRV0	0x0004U
#define RSCC_PDC_SEQ_START_ADDR	0x0008U
#define RSCC_PDC_MATCH_VALUE_LO	0x0009U
#define RSCC_PDC_MATCH_VALUE_HI	0x000aU
#define RSCC_PDC_SLAVE_ID_DRV0	0x000bU
#define RSCC_HIDDEN_TCS_CMD0_ADDR	0x000dU
#define RSCC_HIDDEN_TCS_CMD0_DATA	0x000eU
#define RSCC_OVERRIDE_START_ADDR	0x0100U
#define RSCC_SEQ_BUSY_DRV0	0x0101U
#define RSCC_SEQ_MEM_0_DRV0	0x0180U

/*
 * PDC cfg/seq dword offsets relative to the DT-mapped windows.
 * Match the low bits of the kgsl AOSS offsets (ENABLE 0x1140, TCS1 0x1572, …).
 */
#define PDC_ENABLE_PDC		0x1140U
#define PDC_SEQ_START_ADDR	0x1148U
#define PDC_TCS1_CONTROL	0x1572U
#define PDC_TCS1_CMD_ENABLE_BANK	0x1573U
#define PDC_TCS1_CMD_WAIT	0x1574U
#define PDC_TCS1_CMD0_MSGID	0x1575U
#define PDC_TCS1_CMD0_ADDR	0x1576U
#define PDC_TCS1_CMD0_DATA	0x1577U
#define PDC_TCS3_CONTROL	0x15d6U
#define PDC_TCS3_CMD_ENABLE_BANK	0x15d7U
#define PDC_TCS3_CMD_WAIT	0x15d8U
#define PDC_TCS3_CMD0_MSGID	0x15d9U
#define PDC_TCS3_CMD0_ADDR	0x15daU
#define PDC_TCS3_CMD0_DATA	0x15dbU
#define PDC_SEQ_MEM_0		0x0000U		/* relative to seq window */
#define PDC_ADDRESS_OFFSET_A640	0x30090U

/* out/fw.S (generated by build.sh): empty when the files were missing. */
extern const uint32_t sweet_gmu_fw[], sweet_sqe_fw[];
extern const char sweet_gmu_fw_end[], sweet_sqe_fw_end[];
/* zap shader (a640_zap.mdt header + hashes, a640_zap.b02 image), step 15 */
extern const uint32_t sweet_zapmdt_fw[], sweet_zapseg_fw[];
extern const char sweet_zapmdt_fw_end[], sweet_zapseg_fw_end[];

static inline uint32_t rd(uintptr_t a)
{
	uint32_t v = *(volatile uint32_t *)a;
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline void wr(uintptr_t a, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t) :: "memory");
	return t;
}

static void udelay(unsigned us)
{
	uint64_t end = ticks() + (uint64_t)us * 192 / 10;	/* 19.2 MHz */
	while (ticks() < end)
		;
}

/* Wait until (reg & mask) == want; 1 on success. */
static int poll(uintptr_t a, uint32_t mask, uint32_t want, unsigned us)
{
	for (unsigned i = 0; i < us; i += 10) {
		if ((rd(a) & mask) == want)
			return 1;
		udelay(10);
	}
	return (rd(a) & mask) == want;
}

static int n_fail;
static const char *fail_name[4];	/* set at runtime only: no absolute pointers in data */

static void note_fail(const char *label)
{
	if (n_fail < 4)
		fail_name[n_fail] = label;
	n_fail++;
}

/*
 * quiet (sweetgpu>=5): steps confirmed on the phone (1-4, boots 3-7 and the
 * step-5 boots) print only failures, so the new step-5 lines fit on the
 * non-scrolling console. sweetgpu<=4 prints everything as before.
 */
static int quiet;
static int very_quiet;
static unsigned rb_n, rb_bad;

static void hdr(const char *s)
{
	if (!quiet)
		con_str("GPU", s);
}

/* Register readback: counted always, printed if it differs or not quiet. */
static void rb(const char *label, uint32_t w, uint32_t r)
{
	rb_n++;
	if (w != r)
		rb_bad++;
	if (w != r || !quiet)
		con_hex2(label, w != r ? "MISMATCH w=" : "w=", w, "r=", r);
}

/* Enable one branch clock. Voted branches can't report their own state. */
static void branch_on(const char *label, uintptr_t cbcr, int voted)
{
	uint32_t before = rd(cbcr);
	wr(cbcr, before | CBCR_EN);
	int ok = voted ? 1 : poll(cbcr, CBCR_OFF, 0, 2000);
	uint32_t after = rd(cbcr);
	if (!ok)
		note_fail(label);
	if (!ok || !quiet)
		con_hex2(label, ok ? "ok  was=" : "TIMEOUT was=", before, "now=", after);
}

/* cmdline helpers: no libc, keep it simple. */
static int match(const char *s, const char *key)
{
	while (*key)
		if (*s++ != *key++)
			return 0;
	return 1;
}

static unsigned cmdline_uint(const char *cmd, const char *key, unsigned dflt)
{
	if (!cmd)
		return dflt;
	for (const char *p = cmd; *p; p++) {
		if ((p == cmd || p[-1] == ' ') && match(p, key)) {
			const char *q = p;
			while (*key++)
				q++;
			unsigned v = 0;
			while (*q >= '0' && *q <= '9')
				v = v * 10 + (unsigned)(*q++ - '0');
			return v;
		}
	}
	return dflt;
}

static void step1_power(void)
{
	hdr("step 1: clocks + CX power domain");

	/* GPLL0 feeds for GPUCC (voted, shared with others: only set bits). */
	uint32_t v = rd(GCC_GPLL0_VOTE);
	wr(GCC_GPLL0_VOTE, v | (1U << 15) | (1U << 16));
	if (!quiet)
		con_hex2("GPLL0", "was=", v, "now=", rd(GCC_GPLL0_VOTE));

	/* gpu_cfg_ahb first: the GPUCC block is behind it. */
	branch_on("CFG AHB", GCC_GPU_CFG_AHB, 0);
	if (rd(GCC_GPU_CFG_AHB) & CBCR_OFF) {
		con_str("GPU", "cfg_ahb still off: GPUCC not touched");
		note_fail("CFG AHB");
		return;
	}
	/* CX GDSC: clear SW_COLLAPSE, wait for PWR_ON. */
	uint32_t g = rd(GPUCC_CX_GDSCR);
	if (g & GDSC_SW_COLLAPSE)
		wr(GPUCC_CX_GDSCR, g & ~GDSC_SW_COLLAPSE);
	int ok = poll(GPUCC_CX_GDSCR, GDSC_PWR_ON, GDSC_PWR_ON, 1000);
	if (!ok)
		note_fail("CX GDSC");
	if (!ok || !quiet)
		con_hex2("CX GDSC", ok ? "ok  was=" : "TIMEOUT was=", g, "now=", rd(GPUCC_CX_GDSCR));
	if (!ok)
		return;

	/*
	 * Bus clocks into the GPU subsystem only after CX is powered: their
	 * consumers (GPU SMMU, GMU) sit in the CX domain. Boot 2 (enabled before
	 * CX) timed out on snoc_dvm; guess: CLK_OFF can't clear while CX is
	 * collapsed. Linux likewise powers the SMMU's CX domain first.
	 */
	branch_on("MEMNOC", GCC_GPU_MEMNOC_GFX, 1);
	branch_on("SNOCDVM", GCC_GPU_SNOC_DVM_GFX, 0);
	branch_on("GPU AXI", GCC_DDRSS_GPU_AXI, 1);

	branch_on("CC AHB", GPUCC_AHB, 1);	/* HALT_DELAY: no status bit */
	branch_on("CXO", GPUCC_CXO, 0);
	branch_on("CXO AON", GPUCC_CXO_AON, 0);
	branch_on("CX GMU", GPUCC_CX_GMU, 0);	/* still at the 19.2 MHz XO default */
}

/*
 * GMU firmware = blocks {addr, size, type, value, data[size]}. Upstream
 * a6xx_gmu_fw_load (A640, non-legacy): ITCM 0x0 and DTCM 0x40000 (16K each)
 * are written through GMU registers; the rest must land in GMU-VA buffers
 * icache 0x4000 and dcache 0x44000 (256K - 16K each).
 */
static void step2_firmware(void)
{
	hdr("step 2: built-in firmware check");

	uint32_t len = (uint32_t)(sweet_gmu_fw_end - (const char *)sweet_gmu_fw);
	if (len == 0) {
		con_str("GMU FW", "not built in (gpu_accel/ missing at build)");
		note_fail("GMU FW");
	} else {
		uint32_t off = 0, n = 0, itcm = 0, dtcm = 0, ic = 0, dc = 0, other = 0;
		while (off + 16 <= len) {
			const uint32_t *b = sweet_gmu_fw + off / 4;
			uint32_t addr = b[0], size = b[1];
			if ((size & 3) || size > len - off - 16)
				break;
			n++;
			if (size == 0)
				;
			else if (addr < 0x4000 && addr + size <= 0x4000)
				itcm++;
			else if (addr >= 0x40000 && addr + size <= 0x44000)
				dtcm++;
			else if (addr >= 0x4000 && addr + size <= 0x40000)
				ic++;
			else if (addr >= 0x44000 && addr + size <= 0x80000)
				dc++;
			else
				other++;
			off += 16 + size;
		}
		int ok = off == len && other == 0 && itcm && dtcm;
		if (!ok)
			note_fail("GMU FW");
		if (!ok || !quiet) {
			con_hex2("GMU FW", ok ? "ok  bytes=" : "BAD bytes=", len, "blocks=", n);
			con_hex2("GMU TCM", "itcm=", itcm, "dtcm=", dtcm);
			con_hex2("GMU VA", "icache=", ic, "dcache=", dc);
		}
		if (other || off != len)
			con_hex2("GMU BAD", "other=", other, "stop=", off);
	}

	/* adreno_fw_create_bo skips dword 0; the version is the next dword. */
	len = (uint32_t)(sweet_sqe_fw_end - (const char *)sweet_sqe_fw);
	if (len < 12) {
		con_str("SQE FW", "not built in (gpu_accel/ missing at build)");
		note_fail("SQE FW");
	} else {
		uint32_t v = sweet_sqe_fw[1];
		int ok = ((v & 0xf) == 0xa && (sweet_sqe_fw[3] & 0xf) >= 1) || (v & 0xfff) >= 0x190;
		if (!ok)
			note_fail("SQE FW");
		if (!ok || !quiet)
			con_hex2("SQE FW", ok ? "ok  bytes=" : "OLD bytes=", len, "ver=", v & 0xfff);
	}
}

/*
 * gpu_cc_gmu_clk_src (clk-rcg2): CFG src_sel [10:8], src_div [4:0] = 2*div-1;
 * CMD bit0 UPDATE latches CFG, cleared by hardware; bit31 ROOT_OFF.
 * 200 MHz = GPLL0_OUT_MAIN_DIV (300 MHz, voted in step 1) / 1.5.
 */
static void step3_gmu_clock(void)
{
	hdr("step 3: GMU clock -> 200 MHz");

	uint32_t cmd0 = rd(GPUCC_GMU_CMD_RCGR), cfg0 = rd(GPUCC_GMU_CFG_RCGR);
	uint32_t want = (GMU_SRC_GPLL0_DIV << 8) | GMU_DIV_1_5;
	uint32_t cfg = (cfg0 & ~(RCG_CFG_SRC_MASK | RCG_CFG_DIV_MASK)) | want;
	wr(GPUCC_GMU_CFG_RCGR, cfg);
	wr(GPUCC_GMU_CMD_RCGR, rd(GPUCC_GMU_CMD_RCGR) | RCG_CMD_UPDATE);
	int ok = poll(GPUCC_GMU_CMD_RCGR, RCG_CMD_UPDATE, 0, 2000);
	uint32_t cmd1 = rd(GPUCC_GMU_CMD_RCGR), cfg1 = rd(GPUCC_GMU_CFG_RCGR);
	ok = ok && !(cmd1 & RCG_CMD_ROOT_OFF) &&
	     (cfg1 & (RCG_CFG_SRC_MASK | RCG_CFG_DIV_MASK)) == want;
	if (!ok)
		note_fail("GMU CLK");
	uint32_t br = rd(GPUCC_CX_GMU);
	if (!ok || (br & CBCR_OFF) || !quiet) {
		con_hex2("GMU CMD", ok ? "ok  was=" : "FAIL was=", cmd0, "now=", cmd1);
		con_hex2("GMU CFG", "was=", cfg0, "now=", cfg1);
		con_hex2("CX GMU", (br & CBCR_OFF) ? "OFF cbcr=" : "on  cbcr=", br, "want cfg=", cfg);
	}
	if (br & CBCR_OFF)
		note_fail("CX GMU");
}

static void label_n(char *buf, const char *pfx, unsigned n)
{
	static const char hex[] = "0123456789ABCDEF";
	int i = 0;
	while (pfx[i] && i < 5) {
		buf[i] = pfx[i];
		i++;
	}
	buf[i++] = hex[(n >> 4) & 0xf];
	buf[i++] = hex[n & 0xf];
	buf[i] = 0;
}

/* Does stream ID sid match this SMR entry (ID/MASK compare)? */
static int smr_matches(uint32_t id, uint32_t mask, uint32_t sid)
{
	return ((sid ^ id) & ~mask & 0x7fff) == 0;
}

/*
 * Read-only: what firmware left in the GPU SMMU for our stream IDs.
 * GPU: SID 0 mask 0x401 (0 and 0x400); GMU: SID 4 (user), 5 (kernel).
 */
static void step4_smmu_dump(void)
{
	con_str("GPU", "step 4: GPU SMMU state (read only)");

	/*
	 * Boot 6 hung on the first SMMU read with no exception. Linux's arm-smmu
	 * probe reads IDR0/IDR1 under the same hypervisor, so read those first
	 * and print a marker before each read: if it hangs again, the last line
	 * on screen names the register.
	 */
	con_str("SMMU rd", "IDR0 @02CA0020 ...");
	uint32_t id0 = rd(SMMU_BASE + SMMU_IDR0);
	con_str("SMMU rd", "IDR1 @02CA0024 ...");
	uint32_t id1 = rd(SMMU_BASE + SMMU_IDR1);
	con_str("SMMU rd", "sCR0 @02CA0000 ...");
	uint32_t scr0 = rd(SMMU_BASE + SMMU_SCR0);
	uint32_t nsmr = id0 & 0xff, ncb = id1 & 0xff;
	uint32_t pg = (id1 & (1U << 31)) ? 0x10000 : 0x1000;
	int exids = (scr0 & SCR0_EXIDENABLE) != 0;
	con_hex2("SMMU", "scr0=", scr0, "idr0=", id0);
	con_hex2("SMMU", "idr1=", id1, "page=", pg);
	con_hex2("SMMU", "nsmr=", nsmr, "ncb=", ncb);
	if (nsmr > 128 || ncb > 128 || nsmr == 0 || ncb == 0 || pg != 0x1000) {
		con_str("SMMU", "unexpected geometry: stop");
		note_fail("SMMU ID");
		return;
	}

	uint32_t cb_used[4] = { 0, 0, 0, 0 };	/* bitmap of CBs some S2CR translates to */
	uint32_t valid = 0, shown = 0, first_free_smr = 0xff;
	static const uint32_t sids[4] = { 0x0, 0x400, 0x4, 0x5 };
	con_str("SMMU rd", "SMR/S2CR table @02CA0800/0C00 ...");
	for (uint32_t n = 0; n < nsmr; n++) {
		uint32_t smr = rd(SMMU_BASE + SMMU_SMR(n));
		uint32_t s2cr = rd(SMMU_BASE + SMMU_S2CR(n));
		int v = exids ? (s2cr & S2CR_EXIDVALID) != 0 : (smr >> 31) != 0;
		if (!v) {
			if (first_free_smr == 0xff)
				first_free_smr = n;
			continue;
		}
		valid++;
		uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
		uint32_t type = (s2cr >> 16) & 3, cb = s2cr & 0xff;
		if (type == 0 && cb < 128)
			cb_used[cb / 32] |= 1U << (cb % 32);
		int ours = 0;
		for (int k = 0; k < 4; k++)
			ours |= smr_matches(id, mask, sids[k]);
		if (ours && shown < 6) {
			char lab[8];
			label_n(lab, "SMR ", n);
			con_hex2(lab, "smr=", smr, "s2cr=", s2cr);
			if (type == 0 && cb < ncb) {
				label_n(lab, "CB ", cb);
				con_hex2(lab, "cbar=", rd(SMMU_BASE + pg + SMMU_CBAR(cb)), "type=", type);
			}
			shown++;
		}
	}
	uint32_t first_free_cb = 0xff, nfree = 0;
	for (uint32_t c = 0; c < ncb; c++)
		if (!(cb_used[c / 32] & (1U << (c % 32)))) {
			nfree++;
			if (first_free_cb == 0xff)
				first_free_cb = c;
		}
	con_hex2("SMMU", "valid smr=", valid, "ours=", shown);
	con_hex2("SMMU", "free smr=", first_free_smr, "free cb=", first_free_cb);
	con_hex2("SMMU", "free cbs=", nfree, "exids=", exids);
}


/*
 * Step 3c: program one free SMR/S2CR and one context bank for GMU SID 5 with a
 * stage-1 long-descriptor page table mapping GMU buffer VAs into scratch RAM
 * at 0xb0000000. Does not start the GMU.
 *
 * sweetgpu>=5 only; requires steps 1-4 already succeeded (n_fail==0).
 * Boot-7 geometry: 4K pages, first free SMR 0, first free CB 0, no EXIDS.
 *
 * Page-table layout in scratch (all 4K-aligned):
 *   +0x0000  L1 table (1 page)   -- covers VA 0 and VA 0x60000000 via 1GB entries
 *   +0x1000  L2 for VA 0         -- 2MB blocks for low GMU icache/dcache
 *   +0x2000  L2 for VA 0x60000000
 *   +0x3000  L3 for low VA (icache/dcache pages)
 *   +0x4000  L3 for 0x60000xxx
 *   +0x10000 GMU buffer payload region (icache, dcache, log, ...)
 */
static void pte_write(uintptr_t pa, uint64_t val)
{
	*(volatile uint64_t *)pa = val;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void step5_smmu_gmu(void)
{
	con_str("GPU", "step 5: GMU SMMU CB + stage-1 table");

	/* boot-7 order kept (IDR0, IDR1, then sCR0); one marker for all three */
	con_str("SMMU rd", "IDR0, IDR1, sCR0 ...");
	uint32_t id0 = rd(SMMU_BASE + SMMU_IDR0);
	uint32_t id1 = rd(SMMU_BASE + SMMU_IDR1);
	uint32_t scr0 = rd(SMMU_BASE + SMMU_SCR0);
	uint32_t nsmr = id0 & 0xff, ncb = id1 & 0xff;
	uint32_t pg = (id1 & (1U << 31)) ? 0x10000 : 0x1000;
	int exids = (scr0 & SCR0_EXIDENABLE) != 0;
	int geo_bad = nsmr == 0 || ncb == 0 || nsmr > 128 || ncb > 128 || pg != 0x1000;
	if (geo_bad || !quiet) {
		con_hex2("SMMU", "scr0=", scr0, "idr0=", id0);
		con_hex2("SMMU", "idr1=", id1, "page=", pg);
		con_hex2("SMMU", "nsmr=", nsmr, "ncb=", ncb);
	}
	if (nsmr == 0 || ncb == 0 || nsmr > 128 || ncb > 128 || pg != 0x1000) {
		con_str("SMMU", "unexpected geometry: stop");
		note_fail("SMMU ID");
		return;
	}

	/* Find first free SMR and CB (same rules as step 4). */
	uint32_t cb_used[4] = { 0, 0, 0, 0 };
	uint32_t first_free_smr = 0xff;
	for (uint32_t n = 0; n < nsmr; n++) {
		uint32_t smr = rd(SMMU_BASE + SMMU_SMR(n));
		uint32_t s2cr = rd(SMMU_BASE + SMMU_S2CR(n));
		int v = exids ? (s2cr & S2CR_EXIDVALID) != 0 : (smr >> 31) != 0;
		if (!v) {
			if (first_free_smr == 0xff)
				first_free_smr = n;
			continue;
		}
		uint32_t type = (s2cr >> 16) & 3, cb = s2cr & 0xff;
		if (type == 0 && cb < 128)
			cb_used[cb / 32] |= 1U << (cb % 32);
	}
	uint32_t first_free_cb = 0xff;
	for (uint32_t c = 0; c < ncb; c++)
		if (!(cb_used[c / 32] & (1U << (c % 32)))) {
			first_free_cb = c;
			break;
		}
	con_hex2("SMMU", "free smr=", first_free_smr, "free cb=", first_free_cb);	/* shows reuse after a warm reboot */
	if (first_free_smr == 0xff || first_free_cb == 0xff) {
		con_str("SMMU", "no free SMR/CB");
		note_fail("SMMU free");
		return;
	}

	/* --- build stage-1 tables in scratch --- */
	uintptr_t l1 = SCRATCH_PA;
	uintptr_t l2_lo = SCRATCH_PA + 0x1000;	/* covers VA 0 .. 1GB-1 via L1[0] */
	uintptr_t l2_hi = SCRATCH_PA + 0x2000;	/* covers VA 0x40000000.. via L1[1] */
	uintptr_t l3_lo = SCRATCH_PA + 0x3000;	/* VA 0x000000-0x1fffff */
	uintptr_t l3_hi = SCRATCH_PA + 0x4000;	/* VA 0x60000000-0x601fffff */
	uintptr_t buf_base = SCRATCH_PA + 0x10000;	/* GMU buffers */

	/* zero table pages + buffer space (1 MiB is enough for maps + fw) */
	if (!quiet)
		con_str("SMMU", "zero PT + buf @b0000000 ...");
	for (uintptr_t a = SCRATCH_PA; a < SCRATCH_PA + 0x100000; a += 8)
		*(volatile uint64_t *)a = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	/*
	 * Long-descriptor stage-1, 4K granule, T0SZ=25 (39-bit IPA):
	 *   L1 index = VA[38:30], L2 = VA[29:21], L3 = VA[20:12]
	 * 0x00004000 / 0x00057000 -> L1[0], L2[0], L3[...]
	 * 0x60000000             -> L1[1] (0x60000000>>30 == 1), L2[0x100], L3[0]
	 * AttrIndx 0 = Device-nGnRnE (MAIR byte0 = 0x00)
	 * AttrIndx 1 = Normal NC   (MAIR byte1 = 0x44)
	 */
	const uint64_t ATTR_DEV = (0ULL << 2);
	const uint64_t ATTR_NC  = (1ULL << 2);
	/*
	 * AP[2:1]=00: read/write for privileged accesses only. NOT 01 (RW at any
	 * privilege): a page writable by unprivileged accesses is implicitly
	 * privileged-execute-never, and the GMU's Cortex-M3 runs privileged, so
	 * with 01 its data accesses worked but its instruction fetches from the
	 * ICACHE region (VA 0x4000-0x3ffff) were refused (boot 2026-10-22: the
	 * first call into ICACHE code, 0x4dc4, faulted at 0x40E4E, the fault
	 * handler's call into ICACHE faulted again -> lockup). KGSL maps GMU
	 * memory with IOMMU_PRIV (kgsl_gmu.c), HackiOS leaves PTE bit 6 clear for
	 * the GMU ("the 3D core's accesses are [unprivileged]; the GMU's are not").
	 */
	const uint64_t AP_PRIV_RW = 0ULL;
	const uint64_t SH_OS    = (2ULL << 8);
	const uint64_t AF       = (1ULL << 10);
	const uint64_t PAGE     = 3ULL;
	const uint64_t TABLE    = 3ULL;

	/* L1[0] -> L2_lo; L1[1] -> L2_hi (VA 0x40000000-0x7fffffff) */
	pte_write(l1 + 0 * 8, l2_lo | TABLE);
	pte_write(l1 + 1 * 8, l2_hi | TABLE);

	/* L2_lo[0] -> L3_lo (VA 0x00000000-0x001fffff) */
	pte_write(l2_lo + 0 * 8, l3_lo | TABLE);
	/* L2_hi[256] -> L3_hi: VA 0x60000000 is L2 index (0x60000000>>21)&0x1ff = 0x100 */
	pte_write(l2_hi + 0x100 * 8, l3_hi | TABLE);

	/*
	 * Map a contiguous PA region for low GMU VAs 0x4000..0x7ffff (icache +
	 * dcache including the 0x57000 block). Upstream sizes each ~240 KiB;
	 * 0x7c pages from 0x4000 covers through 0x7ffff.
	 */
	uintptr_t pa_low = buf_base;
	for (unsigned i = 4; i < 0x80; i++) {	/* VA page index 4..127 */
		uint64_t pte = (pa_low + (uint64_t)(i - 4) * 0x1000) |
		               PAGE | ATTR_NC | AP_PRIV_RW | SH_OS | AF;
		pte_write(l3_lo + (uint64_t)i * 8, pte);
	}
	/* High: dummy @0x60000000 + log/HFI pages @0x60001000.. */
	uintptr_t pa_hi = buf_base + 0x80000;
	for (unsigned i = 0; i < 16; i++) {
		uint64_t pte = (pa_hi + (uint64_t)i * 0x1000) |
		               PAGE | (i == 0 ? ATTR_DEV : ATTR_NC) | AP_PRIV_RW | SH_OS | AF;
		pte_write(l3_hi + (uint64_t)i * 8, pte);
	}
	if (!quiet)
		con_hex2("SMMU PT", "l1=", (uint32_t)l1, "buf=", (uint32_t)buf_base);

	/*
	 * Context bank n at base + (num_global_pages + n) * pagesize.
	 * IDR1 NUMPAGENDXB [30:28]: global pages = 1 << (field + 1).
	 * idr1=0x20000005 -> 8 global pages -> CB0 at 0x02CA8000.
	 */
	uint32_t numpagendxb = (id1 >> 28) & 7U;
	uint32_t num_global = 1U << (numpagendxb + 1U);
	uintptr_t cb_base = SMMU_BASE + (uint64_t)pg * (num_global + first_free_cb);
	con_hex2("SMMU CB", "idx=", first_free_cb, "base=", (uint32_t)cb_base);
	if (!quiet)
		con_hex2("SMMU CB", "glob=", num_global, "pgndx=", numpagendxb);

	/* CBA2R first: force AArch64 long-descriptor format (Linux order). */
	uint32_t cba2r_w = CBA2R_VA64;
	wr(SMMU_BASE + pg + SMMU_CBA2R(first_free_cb), cba2r_w);
	uint32_t cba2r_r = rd(SMMU_BASE + pg + SMMU_CBA2R(first_free_cb));
	rb("CBA2R", cba2r_w, cba2r_r);

	/* CBAR: stage-1 with stage-2 bypass */
	uint32_t cbar_w = CBAR_TYPE_S1_S2_BYPASS;
	wr(SMMU_BASE + pg + SMMU_CBAR(first_free_cb), cbar_w);
	uint32_t cbar_r = rd(SMMU_BASE + pg + SMMU_CBAR(first_free_cb));
	rb("CBAR", cbar_w, cbar_r);

	/* TCR: 4K TG0, T0SZ=25 (39-bit), SH0=outer, ORGN/IRGN=NC, EPD1=1 */
	uint32_t tcr = (25U << 0) |		/* T0SZ */
	               (0U << 8) |		/* IRGN0 NC */
	               (0U << 10) |		/* ORGN0 NC */
	               (2U << 12) |		/* SH0 outer */
	               (0U << 14) |		/* TG0 4K */
	               (1U << 23);		/* EPD1 */
	wr(cb_base + SMMU_CB_TTBCR, tcr);
	uint32_t tcr_r = rd(cb_base + SMMU_CB_TTBCR);
	rb("TCR", tcr, tcr_r);

	/* TTBR0 = L1 PA */
	wr(cb_base + SMMU_CB_TTBR0, (uint32_t)l1);
	wr(cb_base + SMMU_CB_TTBR0 + 4, (uint32_t)(l1 >> 32));
	uint32_t ttbr_lo = rd(cb_base + SMMU_CB_TTBR0);
	uint32_t ttbr_hi = rd(cb_base + SMMU_CB_TTBR0 + 4);
	rb("TTBR0 lo", (uint32_t)l1, ttbr_lo);
	rb("TTBR0 hi", (uint32_t)(l1 >> 32), ttbr_hi);

	/* MAIR: Attr0 Device-nGnRnE, Attr1 Normal NC */
	uint32_t mair0 = 0x00004400U;
	wr(cb_base + SMMU_CB_MAIR0, mair0);
	wr(cb_base + SMMU_CB_MAIR1, 0);
	rb("MAIR0", mair0, rd(cb_base + SMMU_CB_MAIR0));

	/* SCTLR: M=1, CFRE/CFIE, HUPCF; CFCFG=0 so faults terminate not stall */
	/* No CFIE: faults are recorded in FSR but raise no interrupt (nothing here
	 * handles one; HackiOS redwood_adreno.c also leaves it off). */
	uint32_t sctlr = SCTLR_M | SCTLR_CFRE | SCTLR_HUPCF;
	wr(cb_base + SMMU_CB_SCTLR, sctlr);
	uint32_t sctlr_r = rd(cb_base + SMMU_CB_SCTLR);
	rb("SCTLR", sctlr, sctlr_r);

	/* SMR: valid + exact SID 5 */
	uint32_t smr_w = SMR_VALID | (GMU_SID_MASK << 16) | GMU_SID_KERNEL;
	wr(SMMU_BASE + SMMU_SMR(first_free_smr), smr_w);
	uint32_t smr_r = rd(SMMU_BASE + SMMU_SMR(first_free_smr));
	rb("SMR", smr_w, smr_r);

	/* S2CR: translate via our CB */
	uint32_t s2cr_w = (S2CR_TYPE_TRANS << 16) | first_free_cb;
	wr(SMMU_BASE + SMMU_S2CR(first_free_smr), s2cr_w);
	uint32_t s2cr_r = rd(SMMU_BASE + SMMU_S2CR(first_free_smr));
	rb("S2CR", s2cr_w, s2cr_r);
	con_hex2("SMMU regs", "written=", rb_n, "mismatch=", rb_bad);

	/* TLB invalidate ASID 0 + sync for this CB */
	wr(cb_base + SMMU_CB_TLBIASID, 0);
	wr(cb_base + SMMU_CB_TLBSYNC, 0);
	int tlb_ok = poll(cb_base + SMMU_CB_TLBSTATUS, 1, 0, 2000);
	con_hex2("TLB", tlb_ok ? "sync ok fsr=" : "sync TO fsr=",
	         rd(cb_base + SMMU_CB_FSR), "sctlr=", sctlr_r);

	/*
	 * ATS self-check only when IDR0.ATOSNS (bit 26) is clear. On this phone
	 * idr0=0x4C017E06 has bit 26 set -> ATS from NS EL1 is not available and
	 * returns a false "ok pa=0". Skip rather than fail.
	 */
	if (id0 & (1U << 26)) {
		con_str("SMMU ATS", "skipped (ATOSNS set)");
	} else {
		con_str("SMMU ATS", "probe 0x4000 / 0x57000 / 0x60001000");
		static const uint32_t ats_va[3] = { 0x4000U, 0x57000U, 0x60001000U };
		for (int i = 0; i < 3; i++) {
			wr(cb_base + SMMU_CB_ATS1PR, ats_va[i]);
			wr(cb_base + SMMU_CB_ATS1PR + 4, 0);
			int done = poll(cb_base + SMMU_CB_ATSR, ATSR_ACTIVE, 0, 2000);
			uint32_t par_lo = rd(cb_base + SMMU_CB_PAR);
			char lab[8];
			label_n(lab, "PAR", (unsigned)i);
			con_hex2(lab, done ? "ok va=" : "TO va=", ats_va[i],
			         (par_lo & PAR_FAULT) ? "FAULT=" : "pa=",
			         (par_lo & PAR_FAULT) ? par_lo : (par_lo & 0xfffff000U));
			if (!done || (par_lo & PAR_FAULT))
				note_fail("SMMU ATS");
		}
	}

	if ((smr_r & SMR_VALID) == 0 || (sctlr_r & SCTLR_M) == 0)
		note_fail("SMMU CB");
	else if (n_fail == 0)
		con_str("SMMU", "GMU CB programmed (SID 5)");
}


/*
 * Step 6: load GMU firmware only (do NOT start the core).
 * Follows upstream a6xx_gmu_fw_load() non-legacy path for A640:
 *   ITCM/DTCM blocks -> CM3 windows (core halted via SYSRESET=1)
 *   icache/dcache    -> scratch RAM behind the step-5 SMMU maps
 */
static uintptr_t gmu_va_to_pa(uint32_t va)
{
	/* Match step5: VA pages 4..0x7f -> buf_base + (page-4)*0x1000 */
	uintptr_t buf_base = SCRATCH_PA + 0x10000;
	if (va >= 0x4000U && va < 0x80000U) {
		uint32_t page = va >> 12;
		return buf_base + (uint64_t)(page - 4) * 0x1000 + (va & 0xfff);
	}
	if (va >= 0x60000000U && va < 0x60000000U + 0x10000U) {
		uintptr_t pa_hi = buf_base + 0x80000;
		return pa_hi + (va - 0x60000000U);
	}
	return 0;
}

static void step6_gmu_fw_load(void)
{
	if (!very_quiet)
		con_str("GPU", "step 6: load GMU FW (no start)");

	if (!very_quiet)
		con_str("GMU rd", "SYSRESET @02C7E000 ...");
	uint32_t sr0 = rd(GMU_REG(GMU_CM3_SYSRESET));
	con_str("GMU wr", "SYSRESET=1 (halt CM3)");
	wr(GMU_REG(GMU_CM3_SYSRESET), 1);
	uint32_t sr1 = rd(GMU_REG(GMU_CM3_SYSRESET));
	con_hex2("SYSRESET", "was=", sr0, "now=", sr1);
	if ((sr1 & 1U) == 0) {
		note_fail("SYSRESET");
		return;
	}

	uint32_t len = (uint32_t)(sweet_gmu_fw_end - (const char *)sweet_gmu_fw);
	uint32_t itcm_w = 0, itcm_bad = 0, itcm_first = 0xffffffffU, itcm_exp = 0, itcm_got = 0;
	uint32_t dtcm_w = 0, dtcm_bad = 0, dtcm_first = 0xffffffffU, dtcm_exp = 0, dtcm_got = 0;
	uint32_t ic_w = 0, ic_bad = 0, ic_first = 0xffffffffU, ic_exp = 0, ic_got = 0;
	uint32_t dc_w = 0, dc_bad = 0, dc_first = 0xffffffffU, dc_exp = 0, dc_got = 0;
	uint32_t other = 0;

	con_str("GMU wr", "FW blocks -> ITCM/DTCM/icache/dcache");
	for (uint32_t off = 0; off + 16 <= len; ) {
		const uint32_t *b = sweet_gmu_fw + off / 4;
		uint32_t addr = b[0], size = b[1], type = b[2], value = b[3];
		(void)type; (void)value;
		const uint32_t *data = b + 4;
		uint32_t words = size / 4;
		if (size == 0) {
			off += 16;
			continue;
		}
		if (off + 16 + size > len)
			break;

		if (addr < GMU_ITCM_VA_BASE + GMU_TCM_SIZE &&
		    addr + size <= GMU_ITCM_VA_BASE + GMU_TCM_SIZE) {
			uint32_t reg = GMU_ITCM_START + ((addr - GMU_ITCM_VA_BASE) >> 2);
			for (uint32_t i = 0; i < words; i++) {
				wr(GMU_REG(reg + i), data[i]);
				uint32_t got = rd(GMU_REG(reg + i));
				itcm_w++;
				if (got != data[i]) {
					if (itcm_bad == 0) {
						itcm_first = addr + i * 4;
						itcm_exp = data[i];
						itcm_got = got;
					}
					itcm_bad++;
				}
			}
		} else if (addr >= GMU_DTCM_VA_BASE &&
		           addr + size <= GMU_DTCM_VA_BASE + GMU_TCM_SIZE) {
			uint32_t reg = GMU_DTCM_START + ((addr - GMU_DTCM_VA_BASE) >> 2);
			for (uint32_t i = 0; i < words; i++) {
				wr(GMU_REG(reg + i), data[i]);
				uint32_t got = rd(GMU_REG(reg + i));
				dtcm_w++;
				if (got != data[i]) {
					if (dtcm_bad == 0) {
						dtcm_first = addr + i * 4;
						dtcm_exp = data[i];
						dtcm_got = got;
					}
					dtcm_bad++;
				}
			}
		} else if (addr >= 0x4000U && addr < 0x44000U) {
			/* icache region */
			for (uint32_t i = 0; i < words; i++) {
				uintptr_t pa = gmu_va_to_pa(addr + i * 4);
				if (!pa) { other++; break; }
				*(volatile uint32_t *)pa = data[i];
				__asm__ volatile("dsb sy" ::: "memory");
				uint32_t got = *(volatile uint32_t *)pa;
				ic_w++;
				if (got != data[i]) {
					if (ic_bad == 0) {
						ic_first = addr + i * 4;
						ic_exp = data[i];
						ic_got = got;
					}
					ic_bad++;
				}
			}
		} else if (addr >= 0x44000U && addr < 0x80000U) {
			/* dcache (includes 0x57000 block) */
			for (uint32_t i = 0; i < words; i++) {
				uintptr_t pa = gmu_va_to_pa(addr + i * 4);
				if (!pa) { other++; break; }
				*(volatile uint32_t *)pa = data[i];
				__asm__ volatile("dsb sy" ::: "memory");
				uint32_t got = *(volatile uint32_t *)pa;
				dc_w++;
				if (got != data[i]) {
					if (dc_bad == 0) {
						dc_first = addr + i * 4;
						dc_exp = data[i];
						dc_got = got;
					}
					dc_bad++;
				}
			}
		} else {
			other++;
		}
		off += 16 + size;
	}

	if (!very_quiet || itcm_bad)
		con_hex2("ITCM", "words=", itcm_w, "bad=", itcm_bad);
	if (itcm_bad) {
		con_hex2("ITCM 1st", "addr=", itcm_first, "exp=", itcm_exp);
		con_hex2("ITCM 1st", "got=", itcm_got, "pad=", 0);
	}
	if (!very_quiet || dtcm_bad)
		con_hex2("DTCM", "words=", dtcm_w, "bad=", dtcm_bad);
	if (dtcm_bad) {
		con_hex2("DTCM 1st", "addr=", dtcm_first, "exp=", dtcm_exp);
		con_hex2("DTCM 1st", "got=", dtcm_got, "pad=", 0);
	}
	if (!very_quiet || ic_bad)
		con_hex2("icache", "words=", ic_w, "bad=", ic_bad);
	if (ic_bad) {
		con_hex2("icache1st", "addr=", ic_first, "exp=", ic_exp);
		con_hex2("icache1st", "got=", ic_got, "pad=", 0);
	}
	if (!very_quiet || dc_bad)
		con_hex2("dcache", "words=", dc_w, "bad=", dc_bad);
	if (dc_bad) {
		con_hex2("dcache1st", "addr=", dc_first, "exp=", dc_exp);
		con_hex2("dcache1st", "got=", dc_got, "pad=", 0);
	}
	if (other)
		con_hex2("FW", "other blks=", other, "len=", len);

	if (itcm_bad || dtcm_bad || ic_bad || dc_bad || itcm_w == 0)
		note_fail("GMU FW load");
	else
		con_str("GMU FW", "loaded (core still halted)");
}


/*
 * Step 7: a6xx_gmu_rpmh_init (A640) + GENERAL_7 + a6xx_rpmh_start.
 * Does NOT release CM3_SYSRESET. Markers before each new MMIO block.
 */
static void rscc_wr(uint32_t d, uint32_t v)
{
	wr(RSCC_REG(d), v);
}
static uint32_t rscc_rd(uint32_t d)
{
	return rd(RSCC_REG(d));
}
static void pdc_cfg_wr(uint32_t d, uint32_t v)
{
	wr(PDC_REG(PDC_CFG_BASE, d), v);
}
static void pdc_seq_wr(uint32_t d, uint32_t v)
{
	wr(PDC_REG(PDC_SEQ_BASE, d), v);
}

static void step7_rpmh(void)
{
	if (!very_quiet)
		con_str("GPU", "step 7: RPMh init + start (A640)");

	/* ---- RSCC config (a6xx_gmu_rpmh_init) ---- */
	con_str("RSCC", "first access @02C8D000 ...");
	uint32_t st0 = rscc_rd(RSCC_RSC_STATUS0_DRV0);
	rscc_wr(RSCC_RSC_STATUS0_DRV0, 1U << 24);	/* disable SDE clock gating */
	rscc_wr(RSCC_PDC_SLAVE_ID_DRV0, 1);
	rscc_wr(RSCC_HIDDEN_TCS_CMD0_DATA, 0);
	rscc_wr(RSCC_HIDDEN_TCS_CMD0_ADDR, 0);
	rscc_wr(RSCC_HIDDEN_TCS_CMD0_DATA + 2, 0);
	rscc_wr(RSCC_HIDDEN_TCS_CMD0_ADDR + 2, 0);
	rscc_wr(RSCC_HIDDEN_TCS_CMD0_DATA + 4, 0x80000000U);
	rscc_wr(RSCC_HIDDEN_TCS_CMD0_ADDR + 4, 0);
	rscc_wr(RSCC_OVERRIDE_START_ADDR, 0);
	rscc_wr(RSCC_PDC_SEQ_START_ADDR, 0x4520);
	rscc_wr(RSCC_PDC_MATCH_VALUE_LO, 0x4510);
	rscc_wr(RSCC_PDC_MATCH_VALUE_HI, 0x4514);
	/* A640 SEQ_MEM (non-a650 branch) */
	rscc_wr(RSCC_SEQ_MEM_0_DRV0 + 0, 0xa7a506a0U);
	rscc_wr(RSCC_SEQ_MEM_0_DRV0 + 1, 0xa1e6a6e7U);
	rscc_wr(RSCC_SEQ_MEM_0_DRV0 + 2, 0xa2e081e1U);
	rscc_wr(RSCC_SEQ_MEM_0_DRV0 + 3, 0xe9a982e2U);
	rscc_wr(RSCC_SEQ_MEM_0_DRV0 + 4, 0x0020e8a8U);
	uint32_t seq0 = rscc_rd(RSCC_SEQ_MEM_0_DRV0);
	con_hex2("RSCC", "st0=", st0, "seq0=", seq0);
	if (seq0 != 0xa7a506a0U)
		note_fail("RSCC SEQ");

	/* ---- PDC seq uCode ---- */
	con_str("PDC seq", "first access @0B490000 ...");
	pdc_seq_wr(PDC_SEQ_MEM_0 + 0, 0xfebea1e1U);
	pdc_seq_wr(PDC_SEQ_MEM_0 + 1, 0xa5a4a3a2U);
	pdc_seq_wr(PDC_SEQ_MEM_0 + 2, 0x8382a6e0U);
	pdc_seq_wr(PDC_SEQ_MEM_0 + 3, 0xbce3e284U);
	pdc_seq_wr(PDC_SEQ_MEM_0 + 4, 0x002081fcU);
	uint32_t pseq = rd(PDC_REG(PDC_SEQ_BASE, PDC_SEQ_MEM_0));
	con_hex2("PDC seq", "mem0=", pseq, "want=", 0xfebea1e1U);
	if (pseq != 0xfebea1e1U)
		note_fail("PDC SEQ");

	/* ---- PDC cfg TCS commands (A640: pdc_address_offset 0x30090) ---- */
	con_str("PDC cfg", "first access @0B290000 ...");
	uint32_t pdc_off = PDC_ADDRESS_OFFSET_A640;
	pdc_cfg_wr(PDC_TCS1_CMD_ENABLE_BANK, 7);
	pdc_cfg_wr(PDC_TCS1_CMD_WAIT, 0);
	pdc_cfg_wr(PDC_TCS1_CONTROL, 0);
	pdc_cfg_wr(PDC_TCS1_CMD0_MSGID, 0x10108);
	pdc_cfg_wr(PDC_TCS1_CMD0_ADDR, 0x30010);
	pdc_cfg_wr(PDC_TCS1_CMD0_DATA, 1);
	pdc_cfg_wr(PDC_TCS1_CMD0_MSGID + 4, 0x10108);
	pdc_cfg_wr(PDC_TCS1_CMD0_ADDR + 4, 0x30000);
	pdc_cfg_wr(PDC_TCS1_CMD0_DATA + 4, 0);
	pdc_cfg_wr(PDC_TCS1_CMD0_MSGID + 8, 0x10108);
	pdc_cfg_wr(PDC_TCS1_CMD0_ADDR + 8, pdc_off);
	pdc_cfg_wr(PDC_TCS1_CMD0_DATA + 8, 0);

	pdc_cfg_wr(PDC_TCS3_CMD_ENABLE_BANK, 7);
	pdc_cfg_wr(PDC_TCS3_CMD_WAIT, 0);
	pdc_cfg_wr(PDC_TCS3_CONTROL, 0);
	pdc_cfg_wr(PDC_TCS3_CMD0_MSGID, 0x10108);
	pdc_cfg_wr(PDC_TCS3_CMD0_ADDR, 0x30010);
	pdc_cfg_wr(PDC_TCS3_CMD0_DATA, 2);
	pdc_cfg_wr(PDC_TCS3_CMD0_MSGID + 4, 0x10108);
	pdc_cfg_wr(PDC_TCS3_CMD0_ADDR + 4, 0x30000);
	pdc_cfg_wr(PDC_TCS3_CMD0_DATA + 4, 0x3);	/* A640 (not a618/a619/a650) */
	pdc_cfg_wr(PDC_TCS3_CMD0_MSGID + 8, 0x10108);
	pdc_cfg_wr(PDC_TCS3_CMD0_ADDR + 8, pdc_off);
	pdc_cfg_wr(PDC_TCS3_CMD0_DATA + 8, 0x3);

	pdc_cfg_wr(PDC_SEQ_START_ADDR, 0);
	pdc_cfg_wr(PDC_ENABLE_PDC, 0x80000001U);
	uint32_t pen = rd(PDC_REG(PDC_CFG_BASE, PDC_ENABLE_PDC));
	con_hex2("PDC cfg", "enable=", pen, "tcs1en=", rd(PDC_REG(PDC_CFG_BASE, PDC_TCS1_CMD_ENABLE_BANK)));
	if (pen != 0x80000001U)
		note_fail("PDC EN");

	/*
	 * Upstream a6xx_gmu_rpmh_init ends with a6xx_rpmh_stop() so the RSC
	 * is in a known sleep state before any later a6xx_rpmh_start().
	 * CONTROL_REQ=1, poll STATUS0_DRV0 bit 16, then REQ=0.
	 */
	con_str("RPMh", "sleep (rpmh_stop) ...");
	wr(GMU_REG(GMU_RSCC_CONTROL_REQ), 1);
	__asm__ volatile("dsb sy" ::: "memory");
	int sleep_ok = 0;
	uint32_t st_sleep = 0;
	for (int i = 0; i < 1000; i++) {
		st_sleep = rscc_rd(RSCC_RSC_STATUS0_DRV0);
		if (st_sleep & (1U << 16)) { sleep_ok = 1; break; }
		udelay(10);
	}
	wr(GMU_REG(GMU_RSCC_CONTROL_REQ), 0);
	uint32_t irq_sleep = rd(GMU_REG(GMU_AO_HOST_INTERRUPT_STATUS));
	con_hex2("RPMh SLEEP", sleep_ok ? "ok st=" : "TO st=", st_sleep,
	         "irq=", irq_sleep);
	if (!sleep_ok)
		note_fail("RPMh SLEEP");

	/* Register retention, then rpmh_start handshake */
	con_str("GMU", "GENERAL_7=1 + RSCC wake");
	wr(GMU_REG(GMU_GENERAL_7), 1);

	uint32_t st_before = rscc_rd(RSCC_RSC_STATUS0_DRV0);
	wr(GMU_REG(GMU_RSCC_CONTROL_REQ), 1U << 1);
	__asm__ volatile("dsb sy" ::: "memory");
	int ack_ok = 0;
	uint32_t ack = 0;
	for (int i = 0; i < 1000; i++) {
		ack = rd(GMU_REG(GMU_RSCC_CONTROL_ACK));
		if (ack & (1U << 1)) { ack_ok = 1; break; }
		udelay(10);
	}
	int busy_ok = 0;
	uint32_t busy = 0xffffffffU;
	for (int i = 0; i < 1000; i++) {
		busy = rscc_rd(RSCC_SEQ_BUSY_DRV0);
		if (busy == 0) { busy_ok = 1; break; }
		udelay(10);
	}
	wr(GMU_REG(GMU_RSCC_CONTROL_REQ), 0);
	uint32_t st_after = rscc_rd(RSCC_RSC_STATUS0_DRV0);
	uint32_t irq_wake = rd(GMU_REG(GMU_AO_HOST_INTERRUPT_STATUS));

	con_hex2("RPMh ACK", ack_ok ? "ok val=" : "TO val=", ack, "st0=", st_before);
	con_hex2("RPMh BUSY", busy_ok ? "ok val=" : "TO val=", busy, "st1=", st_after);
	con_hex2("RPMh IRQ", "wake=", irq_wake, "rscc=", (irq_wake & AO_IRQ_RSCC_COMP) ? 1 : 0);
	if (!ack_ok)
		note_fail("RPMh ACK");
	if (!busy_ok)
		note_fail("RPMh BUSY");
	if (ack_ok && busy_ok)
		con_str("RPMh", "start ok (CM3 still halted)");
}


/*
 * Step 8: finish a6xx_gmu_fw_start + a6xx_gmu_start (release CM3, poll BABEFACE).
 * HFI log @0x60001000, queue table @0x60005000 (step-5 maps).
 * No HFI messaging, no GX power.
 */
static void hfi_mem_init(void)
{
	/* log 16K zeroed at PA of 0x60001000 */
	uintptr_t log_pa = gmu_va_to_pa(GMU_VA_HFI_LOG);
	uintptr_t hfi_pa = gmu_va_to_pa(GMU_VA_HFI_QTBL);
	uint32_t i;

	for (i = 0; i < 0x4000 / 4; i++)
		*(volatile uint32_t *)(log_pa + i * 4) = 0;
	for (i = 0; i < 0x5000 / 4; i++)	/* table + 4 queues (0xE0 + 4 x 4K) */
		*(volatile uint32_t *)(hfi_pa + i * 4) = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	/*
	 * Queue table in the layout of the OnePlus KGSL (kgsl_hfi.c hfi_init),
	 * which this firmware is paired with: header (version 0, size in DWORDS,
	 * qhdr0 offset 6, qhdr size 12, 4 queues, 4 active), then 4 queue
	 * headers: 0 command (prio 10), 1 message (prio 10), 2 debug (prio 40),
	 * 3 dispatch (prio 20, disabled). Queue data at ALIGN(216, 16) = 0xE0 +
	 * i * 4K (HFI_QUEUE_OFFSET). The firmware reads this table at
	 * HFI_CTRL_INIT (step 9), not at boot, so step 8 does not depend on it.
	 */
	volatile uint32_t *table = (volatile uint32_t *)hfi_pa;
	table[0] = 0;			/* version */
	table[1] = (24U + 4U * 48U) / 4U;	/* size: sizeof(hfi_queue_table) >> 2 */
	table[2] = 6;			/* qhdr0_offset (dwords) */
	table[3] = 12;			/* qhdr_size (dwords) */
	table[4] = 4;			/* num_q = HFI_QUEUE_MAX */
	table[5] = 4;			/* num_active_q */
	for (i = 0; i < 4; i++) {
		volatile uint32_t *h = table + 6 + 12 * i;
		uint32_t prio = (i == 2) ? 40U : (i == 3) ? 20U : 10U;
		h[0] = (i == 3) ? 0U : 1U;				/* status */
		h[1] = GMU_VA_HFI_QTBL + 0xE0U + i * 0x1000U;	/* start_addr */
		h[2] = i | (prio << 8);				/* type: id | prio << 8 */
		h[3] = 0x1000U / 4U;				/* queue_size, dwords */
		h[4] = 0; h[5] = 0;				/* msg_size, drop_cnt */
		h[6] = 1; h[7] = 1; h[8] = 1; h[9] = 0;		/* rx_wm tx_wm rx_req tx_req */
		h[10] = 0; h[11] = 0;				/* read_index, write_index */
	}
	__asm__ volatile("dsb sy" ::: "memory");
}


/*
 * RPMh BCM bandwidth vote before GMU start (SH3 = GPU SMMU walker).
 * cmd-db locator @0xc3f000c; apps RSC TCS window @0x18220d00.
 * Layout/trigger match Linux v6.6 cmd-db.c + rpmh-rsc.c (ver 2.7 offsets).
 */
#define CMDDB_LOCATOR		0x0c3f000cUL
#define RSC_DRV_BASE		0x18220000UL
#define RSC_TCS_BASE		0x18220d00UL
#define TCS_STRIDE		672
#define CMD_MSGID_VAL		0x10108U
#define CMD_STATUS_COMPL	(1U << 16)
#define TCS_AMC_MODE_ENABLE	(1U << 16)
#define TCS_AMC_MODE_TRIGGER	(1U << 24)
#define BCM_TCS_CMD_VAL		0x60004001U	/* commit|valid|x=1|y=1 */
#define CMDDB_DATA_START	0x90U		/* end of cmd_db_header */

static int name4_eq(uint32_t id0, const char *name)
{
	int k;
	for (k = 0; name[k] && k < 4; k++)
		if (((id0 >> (8 * k)) & 0xff) != (uint8_t)name[k])
			return 0;
	/* require NUL terminator in the id word so "SH3" != "SH3x..." */
	if (k < 4 && (((id0 >> (8 * k)) & 0xff) != 0))
		return 0;
	return name[0] != 0;
}

static int cmd_db_get_addr(uint32_t db, const char *name, uint32_t *out)
{
	uint32_t mag = rd(db + 4);
	if (mag != 0x0c0330dbU) {
		con_hex2("cmd-db", "bad magic=", mag, "want=", 0x0c0330dbU);
		return 0;
	}
	for (unsigned s = 0; s < 8; s++) {
		uint32_t off = 8 + s * 16;
		uint32_t w0 = rd(db + off);
		uint32_t w1 = rd(db + off + 4);
		/* rsc_hdr: slv_id, header_offset | data_offset, cnt | version ... */
		uint16_t header_offset = (uint16_t)(w0 >> 16);
		uint16_t cnt = (uint16_t)(w1 >> 16);	/* cnt is high half of w1 */
		if (!cnt || cnt > 512)
			continue;
		for (unsigned e = 0; e < cnt; e++) {
			/* entries live at data[] + header_offset, data @ 0x90 */
			uint32_t eo = db + CMDDB_DATA_START + header_offset + e * 24;
			uint32_t id0 = rd(eo);
			if (!name4_eq(id0, name))
				continue;
			*out = rd(eo + 16);
			return 1;
		}
	}
	return 0;
}

static int rpmh_bcm_vote_once(uint32_t cmd_addr, uint32_t data)
{
	uintptr_t tcs = RSC_TCS_BASE;	/* first ACTIVE TCS */
	uint32_t st = 0;
	int idle = 0;
	for (int i = 0; i < 1000; i++) {
		st = rd(tcs + 0x18);	/* STATUS (ver 2.7) */
		if (st & 1U) { idle = 1; break; }
		udelay(10);
	}
	if (!idle) {
		con_hex2("TCS", "busy st=", st, "pad=", 0);
		return 0;
	}
	wr(tcs + 0x08, 1U);		/* IRQ_CLEAR bit0 */
	wr(tcs + 0x30, CMD_MSGID_VAL);	/* CMD0 MSGID */
	wr(tcs + 0x34, cmd_addr);
	wr(tcs + 0x38, data);
	wr(tcs + 0x1c, 1U);		/* CMD_ENABLE bit0 */

	/* __tcs_set_trigger: clear TRIGGER then ENABLE, then ENABLE, then ENABLE|TRIGGER */
	{
		uint32_t ctl = rd(tcs + 0x14);
		wr(tcs + 0x14, ctl & ~TCS_AMC_MODE_TRIGGER);
		ctl = rd(tcs + 0x14);
		wr(tcs + 0x14, ctl & ~TCS_AMC_MODE_ENABLE);
		wr(tcs + 0x14, TCS_AMC_MODE_ENABLE);			/* 0x00010000 */
		wr(tcs + 0x14, TCS_AMC_MODE_ENABLE | TCS_AMC_MODE_TRIGGER); /* 0x01010000 */
	}
	__asm__ volatile("dsb sy" ::: "memory");

	uint32_t cstat = 0;
	int done = 0;
	for (int i = 0; i < 1000; i++) {
		cstat = rd(tcs + 0x3c);
		if (cstat & CMD_STATUS_COMPL) { done = 1; break; }
		udelay(10);
	}
	con_hex2("TCS cmd", done ? "ok st=" : "TO st=", cstat, "addr=", cmd_addr);

	/* tx-done: clear ENABLE|TRIGGER and CMD_ENABLE */
	{
		uint32_t ctl = rd(tcs + 0x14);
		wr(tcs + 0x14, ctl & ~(TCS_AMC_MODE_ENABLE | TCS_AMC_MODE_TRIGGER));
	}
	wr(tcs + 0x1c, 0);
	return done;
}

static int rpmh_vote_gpu_bw(void)
{
	con_str("cmd-db", "locator @0C3F000C ...");
	uint32_t loc0 = rd(CMDDB_LOCATOR);
	uint32_t loc1 = rd(CMDDB_LOCATOR + 4);
	con_hex2("cmd-db", "loc0=", loc0, "loc1=", loc1);
	uint32_t db = loc0;
	if (db < 0x80000000U || db >= 0xf0000000U) {
		if (loc1 >= 0x80000000U && loc1 < 0xf0000000U)
			db = loc1;
		else {
			con_str("cmd-db", "bad base, skip vote");
			return 0;
		}
	}
	con_str("cmd-db", "parse entries ...");
	uint32_t sh3 = 0, sh0 = 0, mc0 = 0;
	cmd_db_get_addr(db, "SH3", &sh3);
	cmd_db_get_addr(db, "SH0", &sh0);
	cmd_db_get_addr(db, "MC0", &mc0);
	con_hex2("cmd-db", "SH3=", sh3, "SH0=", sh0);	/* MC0 looked up, not printed */
	(void)mc0;
	if (!sh3) {
		con_str("cmd-db", "SH3 not found");
		return 0;
	}

	con_str("RSC", "first access @18220D00 ...");
	uint32_t rid = rd(RSC_DRV_BASE);
	uint32_t maj = (rid >> 16) & 0xff;
	con_hex2("RSC", "id=", rid, "maj=", maj);
	/*
	 * Upstream rpmh-rsc.c: major 3 -> ver_3_0 offsets, anything else ->
	 * ver_2_7 offsets (used below). OP7T reads id 0x00010700 = v1.7.
	 */
	if (maj == 3) {
		con_str("RSC", "v3 offset table not supported");
		return 0;
	}

	/* Only SH3: do not touch SH0/MC0 (CPU DDR path). */
	int ok = rpmh_bcm_vote_once(sh3, BCM_TCS_CMD_VAL);
	con_str("RPMh BW", ok ? "SH3 vote complete" : "vote failed");
	return ok;
}

/* Read a GMU DTCM word by its GMU address (0x40000-0x43fff) via the GMU window. */
static uint32_t gmu_dtcm(uint32_t addr)
{
	return rd(GMU_REG(GMU_DTCM_START + ((addr - GMU_DTCM_VA_BASE) >> 2)));
}

/*
 * Print up to 4 words in [lo, hi) of DTCM that look like Thumb return
 * addresses into firmware code (odd, inside 0x200..0x7a58), lowest address
 * (innermost frame) first. Prints "none" if there are none.
 */
static void __attribute__((unused)) dtcm_scan(const char *label, uint32_t lo, uint32_t hi)
{
	unsigned shown = 0;
	for (uint32_t a = lo; a < hi && shown < 4; a += 4) {
		uint32_t v = gmu_dtcm(a);
		if ((v & 1U) && v > 0x200U && v < 0x7a58U) {
			con_hex2(label, "at=", a, "ret=", v);
			shown++;
		}
	}
	if (!shown)
		con_hex2(label, "none lo=", lo, "hi=", hi);
}

/*
 * Like dtcm_scan, but from the top of [lo, hi) downward (outermost frames
 * first): up to max code-looking words, printed as address and value.
 */
static void __attribute__((unused)) dtcm_scan_top(const char *label, uint32_t lo, uint32_t hi, unsigned max)
{
	unsigned shown = 0;
	for (uint32_t a = hi - 4U; a >= lo && shown < max; a -= 4U) {
		uint32_t v = gmu_dtcm(a);
		if ((v & 1U) && v > 0x200U && v < 0x7a58U) {
			con_hex2(label, "at=", a, "ret=", v);
			shown++;
		}
	}
	if (!shown)
		con_hex2(label, "none lo=", lo, "hi=", hi);
}

/* Three DTCM words per console line, label "Sxxxx" = low 16 bits of the address. */
static void dtcm_dump3(uint32_t lo, uint32_t hi)
{
	static const char hx[] = "0123456789ABCDEF";
	for (uint32_t a = lo; a < hi; a += 12U) {
		char lab[8], buf[32];
		int k = 0;
		lab[0] = 'S';
		for (int i = 0; i < 4; i++)
			lab[1 + i] = hx[(a >> (12 - 4 * i)) & 0xf];
		lab[5] = 0;
		for (uint32_t j = 0; j < 3 && a + 4U * j < hi; j++) {
			uint32_t v = gmu_dtcm(a + 4U * j);
			for (int i = 7; i >= 0; i--)
				buf[k++] = hx[(v >> (4 * i)) & 0xf];
			buf[k++] = ' ';
		}
		buf[k] = 0;
		con_str(lab, buf);
	}
}

static void step8_gmu_start(const char *cmdline)
{
	con_str("GPU", "step 8: HFI + start GMU");

	hfi_mem_init();
	con_str("HFI", "log@60001000 qtbl@60005000");

	/* Rest of a6xx_gmu_fw_start after fw_load */
	wr(GMU_REG(GMU_CM3_FW_INIT_RESULT), 0);
	wr(GMU_REG(GMU_CM3_BOOT_CONFIG), 0x02);
	wr(GMU_REG(GMU_HFI_QTBL_ADDR), GMU_VA_HFI_QTBL);
	wr(GMU_REG(GMU_HFI_QTBL_INFO), 1);
	wr(GMU_REG(GMU_AHB_FENCE_RANGE_0), (1U << 31) | (0xaU << 18) | 0xa0U);
	wr(GMU_REG(GMU_CM3_CFG), 0x4052);
	wr(GMU_REG(GMU_HFI_SFR_ADDR), GMU_CHIPID_A640);
	/* log iova | (size/4K - 1) = 0x60001000 | 3 */
	wr(GMU_REG(GMU_PWR_COL_CP_MSG), GMU_VA_HFI_LOG | 3U);

	/* a6xx_gmu_power_config (idle none): bus/icache/dcache + rpmh client bits */
	wr(GMU_REG(GMU_SYS_BUS_CONFIG), 0x1);
	wr(GMU_REG(GMU_ICACHE_CONFIG), 0x1);
	wr(GMU_REG(GMU_DCACHE_CONFIG), 0x1);
	wr(GMU_REG(GMU_PWR_COL_INTER_FRAME_CTRL), 0x9c40400U);
	/* RPMH_CTRL: set interface + votes enable bits (OR in) */
	{
		uint32_t v = rd(GMU_REG(GMU_RPMH_CTRL));
		/* iface|llc|ddr|mx|cx|gfx */
		wr(GMU_REG(GMU_RPMH_CTRL), v | 0x1U | 0x10U | 0x100U | 0x200U | 0x400U | 0x800U);
	}

	/* a6xx_gmu_start: version from DTCM+0xff8 */
	uint32_t ver = rd(GMU_REG(GMU_DTCM_START + 0xff8));
	con_hex2("GMU ver", "dtcm=", ver, "mask=", 0);

	/* Pre-release snapshot */
	{
		uint32_t pre = rd(GMU_REG(GMU_CM3_FW_INIT_RESULT));
		con_hex2("FW pre", "val=", pre, "sysrst=",
		         rd(GMU_REG(GMU_CM3_SYSRESET)));
	}

	unsigned gmu_mode = cmdline_uint(cmdline, "sweetgmu=", 0);
	unsigned us = cmdline_uint(cmdline, "sweetgmuus=", 20);
	/*
	 * Boots 2026-10-22: reading the SMMU context-bank page (CB0) after the
	 * GMU has run sends the phone to Qualcomm crashdump, while GR0 reads fine.
	 * CB0 reads after release only with sweetgmucb=1.
	 */
	unsigned cb_read = cmdline_uint(cmdline, "sweetgmucb=", 0);
	if (us > 1000000U)
		us = 1000000U;

	/*
	 * sweetgmu=6/7: RPMh BCM vote (SH3) before any GMU release.
	 * On vote failure, skip release (do not start GMU).
	 */
	if (gmu_mode == 6 || gmu_mode == 7) {
		if (!rpmh_vote_gpu_bw()) {
			note_fail("RPMh BW");
			con_str("GMU", "skip start: no BW vote");
			return;
		}
	}

	/*
	 * sweetgmu=4/5: KGSL a6xx_gmu_start pre-release setup
	 * (WFI off, IRQ masks, fence allow + KGSL fence range).
	 * Also apply for 6/7 (same as KGSL gmu_start order: vote then start).
	 */
	if (gmu_mode == 4 || gmu_mode == 5 || gmu_mode == 6 || gmu_mode == 7) {
		con_hex2("old", "wfi=", rd(GMU_REG(GMU_CX_WFI_CONFIG)),
		         "fence=", rd(GMU_REG(GMU_AO_AHB_FENCE_CTRL)));
		con_hex2("old irq", "ao=", rd(GMU_REG(GMU_AO_HOST_INTERRUPT_MASK)),
		         "g2h=", rd(GMU_REG(GMU_GMU2HOST_INTR_MASK)));
		wr(GMU_REG(GMU_CX_WFI_CONFIG), 0);
		wr(GMU_REG(GMU_AO_HOST_INTERRUPT_CLR), 0xffffffffU);
		wr(GMU_REG(GMU_AO_HOST_INTERRUPT_MASK), 0xffffffffU);
		wr(GMU_REG(GMU_GMU2HOST_INTR_CLR), 0xffffffffU);
		wr(GMU_REG(GMU_GMU2HOST_INTR_MASK), 0xffffffffU);
		wr(GMU_REG(GMU_AO_AHB_FENCE_CTRL), 0);	/* allow mode */
		wr(GMU_REG(GMU_AHB_FENCE_RANGE_0), GMU_FENCE_RANGE_KGSL);
		con_str("KGSL", "wfi0 irqmask fence set");
	}

	/*
	 * Sentinels in the GMU firmware's dcache .bss (GMU 0x57308, 0x4068
	 * bytes, zeroed by the firmware's own startup code = its first DDR
	 * access through the SMMU). PA = 0xB0010000 + (va - 0x4000).
	 * sweetgmu=3 prints them after the re-halt: still A5A5A5A5 = the GMU
	 * had not written DDR yet; 0 = its SMMU writes reached memory.
	 */
	wr(0xb0063308UL, 0xa5a5a5a5U);		/* first word, va 0x57308 */
	wr(0xb0065000UL, 0xa5a5a5a5U);		/* middle,     va 0x59000 */
	wr(0xb006736cUL, 0xa5a5a5a5U);		/* last word,  va 0x5b36c */

	/*
	 * GPU bus interface halt (a6xx_reg.h GBIF_HALT 0x3c45 / HALT_ACK 0x3c46,
	 * CX side). A halted GBIF would leave the GMU's first DDR access hanging.
	 * KGSL clears it only at GPU start, so expect 0 here; clear it if not.
	 */
	{
		uint32_t gh = rd(0x02c00000UL + 0x3c45U * 4U);
		con_hex2("GBIF", "halt=", gh, "ack=", rd(0x02c00000UL + 0x3c46U * 4U));
		if (gh != 0)
			wr(0x02c00000UL + 0x3c45U * 4U, 0);
	}
	con_str("GMU", "SYSRESET=1 + CP_RESP=0");
	wr(GMU_REG(GMU_CM3_SYSRESET), 1);
	wr(GMU_REG(GMU_PWR_COL_CP_RESP), 0);

	uint64_t t_rel = ticks();
	con_str("GMU", "SYSRESET=0 (release) ...");
	wr(GMU_REG(GMU_CM3_SYSRESET), 0);

	/*
	 * sweetgmu=3 or 5: brief run then re-halt.
	 * No prints or other MMIO between release and re-halt.
	 */
	if (gmu_mode == 3 || gmu_mode == 5 || gmu_mode == 6) {
		udelay(us);
		wr(GMU_REG(GMU_CM3_SYSRESET), 1);
		{
			uint32_t fw = rd(GMU_REG(GMU_CM3_FW_INIT_RESULT));
			uint32_t irq = rd(GMU_REG(GMU_AO_HOST_INTERRUPT_STATUS));
			con_hex2("FW post", "val=", fw, "us=", us);
			con_hex2("AO IRQ", "st=", irq, "mode=", gmu_mode);
		}
		/*
		 * Diagnostic only: CPU-local timer read (cntpct_el0, same
		 * counter udelay() already uses) + console print. Touches
		 * no GPU/SMMU/bus hardware at all. If this line makes it to
		 * the screen but the CB0 SMMU read right after does not,
		 * the hang is specifically tied to touching the SMMU CB0
		 * register page, not just elapsed wall-clock time.
		 */
		{
			uint64_t el = ticks() - t_rel;
			uint32_t el_us = (uint32_t)(el * 10U / 192U);
			con_hex2("T+rel", "us=", el_us, "pad=", 0);
		}
		/*
		 * Boot 2026-10-22: T+rel printed, then the CB0 SMMU read sent the
		 * phone to crashdump. So everything that cannot touch the SMMU comes
		 * first. 1) DRAM only (CPU reads of scratch RAM, no device): did the
		 * GMU's writes through the SMMU land? Sentinels still A5A5A5A5 = no.
		 */
		con_hex2("BSS", "first=", rd(0xb0063308UL), "last=", rd(0xb006736cUL));
		con_hex2("MEM", "log0=", rd(0xb0091000UL), "hfi0=", rd(0xb0095000UL));
		/* 2) GPU CX / GMU / GPUCC registers (same windows as before release). */
		con_hex2("GBIF", "halt=", rd(0x02c00000UL + 0x3c45U * 4U),
		         "ack=", rd(0x02c00000UL + 0x3c46U * 4U));
		con_hex2("RSC ctl", "ack=", rd(GMU_REG(GMU_RSCC_CONTROL_ACK)),
		         "cxbusy=", rd(GMU_REG(0x930cU)));
		con_hex2("RSCC", "st0=", rd(RSCC_REG(RSCC_RSC_STATUS0_DRV0)),
		         "busy=", rd(RSCC_REG(RSCC_SEQ_BUSY_DRV0)));
		con_hex2("CC CX", "gdsc=", rd(GPUCC_CX_GDSCR), "gx=", rd(GPUCC_BASE + 0x100c));
		con_hex2("CC GMU", "cbcr=", rd(GPUCC_CX_GMU), "rcg=", rd(GPUCC_GMU_CFG_RCGR));
		con_str("GMU", "re-halted, safe reads done; SMMU next");
		/*
		 * 3) SMMU last: GR0 (global fault state) before CB0, since GR0 is the
		 * page step 4 reads; then CB0, the read that crashed the last boot.
		 * GR0: sGFSR bit 1 = unmatched stream (only if sCR0.USFCFG bit 10 = 1),
		 * sGFSYNR1[15:0] = its stream ID, sGFAR = its address.
		 */
		con_hex2("GR0", "scr0=", rd(SMMU_BASE + SMMU_SCR0),
		         "gfsr=", rd(SMMU_BASE + 0x48));
		con_hex2("GR0 sid", "syn1=", rd(SMMU_BASE + 0x54),
		         "gfar=", rd(SMMU_BASE + 0x40));
		if (cb_read) {
			uint32_t id1 = rd(SMMU_BASE + SMMU_IDR1);
			uint32_t pg = (id1 & (1U << 31)) ? 0x10000U : 0x1000U;
			uint32_t numpagendxb = (id1 >> 28) & 7U;
			uint32_t num_global = 1U << (numpagendxb + 1U);
			uintptr_t cb0 = SMMU_BASE + (uint64_t)pg * num_global;
			uint32_t fsr = rd(cb0 + SMMU_CB_FSR);
			uint32_t far_lo = rd(cb0 + 0x60);
			uint32_t far_hi = rd(cb0 + 0x64);
			uint32_t fsynr0 = rd(cb0 + 0x68);
			con_hex2("CB0 FSR", "fsr=", fsr, "far=", far_lo);
			con_hex2("CB0 FAR", "hi=", far_hi, "syn0=", fsynr0);
		}
		con_str("GMU", "re-halted, alive");
		return;
	}

	con_str("GMU", "released, wait 1 ms (no MMIO)");
	udelay(1000);

	con_str("GMU", "after wait, first FW_INIT read ...");
	{
		uint32_t first = rd(GMU_REG(GMU_CM3_FW_INIT_RESULT));
		con_hex2("FW 1st", "val=", first, "pad=", 0);
	}

	int ok = 0;
	uint32_t res = 0;
	/* Up to 500 ms, as HackiOS redwood_adreno.c (5000 x 100 us). */
	for (int i = 0; i < 5000; i++) {
		res = rd(GMU_REG(GMU_CM3_FW_INIT_RESULT));
		if (res == 0xbabefaceU) { ok = 1; break; }
		udelay(100);
	}
	con_hex2("FW_INIT", ok ? "ok val=" : "TO val=", res, "want=", 0xbabefaceU);
	/* DRAM only (CPU reads of scratch): how far the firmware got. */
	con_hex2("BSS", "first=", rd(0xb0063308UL), "last=", rd(0xb006736cUL));
	con_hex2("MEM", "log0=", rd(0xb0091000UL), "hfi0=", rd(0xb0095000UL));
	if (!ok) {
		note_fail("FW_INIT");
		/*
		 * Boot 2026-10-22 (sweetgmu=7): BSS fully zeroed and log0=ABCDABCD
		 * (written by the firmware), but no BABEFACE in 500 ms. Safe reads to
		 * see where it waits: the next words of its log buffer (DRAM, VA
		 * 0x60001000 = PA 0xb0091000) and the GMU's status/interrupt registers.
		 */
		/*
		 * Boot 2026-10-22 (sweetgmu=7, log words): log = 4 x ABCDABCD then 0,
		 * IRQ 0/0, RSCC idle. Firmware analysis (a640_gmu.bin, see notes):
		 * BABEFACE is written by an RTOS task (entry 0x71bc, TCB 0x411e0, stack
		 * 0x402c0-0x406e0) that first creates a second task (TCB 0x4196c).
		 * Read-only DTCM scan (same GMU window as the "GMU ver" read): odd words
		 * that look like Thumb return addresses on task A's stack and on the
		 * main stack (top 0x43f80) show where each is blocked.
		 */
		/*
		 * Boot 2026-10-22 (stack scan): task A never ran (TCB 0, no frames),
		 * main stack only holds stale task-create frames, idle loop = WFI.
		 * The firmware's fault/NMI handler (0x4b4 -> 0x46c0) saves a crash
		 * record at DTCM 0x41274 (0 in the image): stacked r0..r3, r12, LR,
		 * PC, xPSR at +0x00..+0x1c, EXC_RETURN +0x24, SP +0x5c, IPSR +0x70.
		 * Asserts (0x1d74) raise an NMI, so they land here too: the stacked
		 * LR points into the function whose check failed.
		 */
		/*
		 * Boot 2026-10-22: crash record at 0x41274 all zero (no assert, no
		 * fault), TCB A/B 0. The firmware waits somewhere. The earlier stack
		 * scan printed the lowest (stale) frames; the live call chain is at the
		 * top of a stack. Scan top-down: main stack (init runs here before the
		 * scheduler starts) and the idle task's stack (0x40b00-0x40d00, frames
		 * there = the scheduler runs and switched to idle).
		 */
		/*
		 * Boot 2026-10-22 (top-down scan): reset -> 0x2c9c -> init 0x1edc ->
		 * step 0x1e68 -> 0x4dc4 (slot 0x43F44 still holds 0x1EAD, so the two
		 * calls after it never ran), and 0x4C9 (return inside the exception
		 * entry 0x4b4 -> 0x46c0) at 0x43F24. Raw dump of the main stack top so
		 * the frames, including any exception frame (even stacked PC), can be
		 * rebuilt: label = low address, three words from there upward.
		 */
		dtcm_dump3(0x43f00U, 0x43f80U);
		con_hex2("IRQ", "ao=", rd(GMU_REG(GMU_AO_HOST_INTERRUPT_STATUS)),
		         "g2h=", rd(GMU_REG(0x5192U)));	/* GMU2HOST_INTR_INFO */
		con_hex2("RSCC", "st0=", rd(RSCC_REG(RSCC_RSC_STATUS0_DRV0)),
		         "busy=", rd(RSCC_REG(RSCC_SEQ_BUSY_DRV0)));
		con_hex2("RSC ctl", "ack=", rd(GMU_REG(GMU_RSCC_CONTROL_ACK)),
		         "cxbusy=", rd(GMU_REG(0x930cU)));
	}

	/* CB0 fault registers after start attempt (sweetgmucb=1 only) */
	if (cb_read) {
		uint32_t id1 = rd(SMMU_BASE + SMMU_IDR1);
		uint32_t pg = (id1 & (1U << 31)) ? 0x10000U : 0x1000U;
		uint32_t numpagendxb = (id1 >> 28) & 7U;
		uint32_t num_global = 1U << (numpagendxb + 1U);
		uintptr_t cb0 = SMMU_BASE + (uint64_t)pg * num_global;	/* CB0 */
		uint32_t fsr = rd(cb0 + SMMU_CB_FSR);
		uint32_t far_lo = rd(cb0 + 0x60);
		uint32_t far_hi = rd(cb0 + 0x64);
		uint32_t fsynr0 = rd(cb0 + 0x68);
		con_hex2("CB0 FSR", "fsr=", fsr, "far=", far_lo);
		con_hex2("CB0 FAR", "hi=", far_hi, "syn0=", fsynr0);
		if (fsr & 0x3fU)	/* real fault bits below format field */
			note_fail("SMMU FSR");
	}

	if (ok)
		con_str("GMU", "running (BABEFACE)");
}

/* HFI state shared by steps 9 and 10. */
static uint32_t hfi_seq;	/* last sequence number sent (step 9 uses 1) */
static uint32_t hfi_ver;	/* HFI_VERSION_INFO read in step 9 */
static int hfi_tables_ok;	/* step 10 completed (CORE_FW_START acked) */
static int gx_on;		/* step 11: GX powered and OOB still held */

/*
 * Step 9: HFI, the message queues between us and the GMU firmware.
 * Needs the GMU running (step 8 with sweetgmu=7: FW_INIT_RESULT = BABEFACE).
 * Order and formats from the OnePlus KGSL (adreno_a6xx_gmu.c
 * a6xx_gmu_hfi_start, kgsl_hfi.c hfi_start / hfi_send_cmd / hfi_queue_write):
 *   1. HFI_CTRL_INIT = 1, poll HFI_CTRL_STATUS bit 0 (100 ms).
 *   2. A640 sends no H2F_MSG_INIT (hfi_start skips it for a640/a680); the
 *      first message is H2F_MSG_FW_VER (id 1) with supported_ver =
 *      GMU_VERSION(2, 0) = 0x20000000 (A640 gpmu_major/minor).
 *      Header = seqnum << 20 | type << 16 | size_dwords << 8 | id, type 0 = CMD.
 *      Write into queue 0, pad to 4 dwords with 0xFAFAFAFA (HFI major >= 2),
 *      update write_index, ring HOST2GMU_INTR_SET bit 0.
 *   3. Poll GMU2HOST_INTR_INFO bit 0 (100 ms), clear it, read queue 1:
 *      [hdr, our request hdr, error, firmware version].
 * Read-only toward anything outside the GMU; changes no clocks or power.
 */
static void step9_hfi(void)
{
	con_str("GPU", "step 9: HFI start + FW version");
	if (rd(GMU_REG(GMU_CM3_FW_INIT_RESULT)) != 0xbabefaceU) {
		con_str("HFI", "GMU not running (need sweetgmu=7): skip");
		note_fail("HFI no GMU");
		return;
	}
	volatile uint32_t *t = (volatile uint32_t *)gmu_va_to_pa(GMU_VA_HFI_QTBL);
	volatile uint32_t *h0 = t + 6, *h1 = t + 6 + 12;
	con_hex2("HFI tbl", "size=", t[1], "nq=", t[4]);

	/* 1. queues up */
	wr(GMU_REG(GMU_GMU2HOST_INTR_CLR), 0xffffffffU);
	wr(GMU_REG(GMU_HFI_CTRL_INIT), 1);
	int up = poll(GMU_REG(GMU_HFI_CTRL_STATUS), 1, 1, 100000);
	uint32_t hver = rd(GMU_REG(GMU_HFI_VERSION_INFO));
	con_hex2(up ? "HFI up" : "HFI TO", "st=", rd(GMU_REG(GMU_HFI_CTRL_STATUS)), "ver=", hver);
	if (!up) {
		note_fail("HFI init");
		return;
	}

	/* 2. H2F_MSG_FW_VER into queue 0 */
	uint32_t qsz = h0[3];
	volatile uint32_t *q0 = (volatile uint32_t *)gmu_va_to_pa(h0[1]);
	volatile uint32_t *q1 = (volatile uint32_t *)gmu_va_to_pa(h1[1]);
	if (!q0 || !q1 || qsz == 0 || h1[3] == 0) {
		con_hex2("HFI", "bad q0=", h0[1], "q1=", h1[1]);
		note_fail("HFI queues");
		return;
	}
	uint32_t seq = 1;
	hfi_seq = seq;
	hfi_ver = hver;
	uint32_t msg_hdr = (seq << 20) | (0U << 16) | (2U << 8) | 1U;
	uint32_t w = h0[11];
	q0[w] = msg_hdr;
	w = (w + 1) % qsz;
	q0[w] = 0x20000000U;			/* GMU_VERSION(2, 0) */
	w = (w + 1) % qsz;
	if ((hver >> 28) >= 2)
		for (; w % 4; w = (w + 1) % qsz)
			q0[w] = 0xFAFAFAFAU;
	h0[11] = w;
	__asm__ volatile("dsb sy" ::: "memory");
	wr(GMU_REG(GMU_HOST2GMU_INTR_SET), 1);
	con_hex2("HFI tx", "hdr=", msg_hdr, "wr=", w);

	/* 3. reply */
	int got = poll(GMU_REG(GMU_GMU2HOST_INTR_INFO), 1, 1, 100000);
	con_hex2(got ? "HFI irq" : "HFI noirq", "info=", rd(GMU_REG(GMU_GMU2HOST_INTR_INFO)),
	         "q1w=", h1[11]);
	if (got)
		wr(GMU_REG(GMU_GMU2HOST_INTR_CLR), 1);
	uint32_t r = h1[10], rw = h1[11], q1sz = h1[3];
	if (r == rw) {
		con_hex2("HFI", "no reply r=", r, "q0r=", h0[10]);
		note_fail("HFI reply");
		return;
	}
	uint32_t a0 = q1[r % q1sz], a1 = q1[(r + 1) % q1sz];
	uint32_t a2 = q1[(r + 2) % q1sz], a3 = q1[(r + 3) % q1sz];
	con_hex2("HFI rx", "hdr=", a0, "req=", a1);
	con_hex2("HFI rx", "err=", a2, "fwver=", a3);
	uint32_t size = (a0 >> 8) & 0xffU;
	r = (r + (size ? size : 1)) % q1sz;
	if ((hver >> 28) >= 2)
		r = ((r + 3U) & ~3U) % q1sz;
	h1[10] = r;
	__asm__ volatile("dsb sy" ::: "memory");
	if (a1 == msg_hdr && a2 == 0)
		con_hex2("HFI OK", "fw ver=", a3, "major=", a3 >> 28);
	else
		note_fail("HFI FW_VER");
}

/*
 * Step 10: the tables KGSL sends right after FW_VER, then CORE_FW_START.
 * Order and formats: OnePlus KGSL kgsl_hfi.c hfi_start() (A640, HFI major 2):
 *   hfi_send_dcvstbl -> hfi_send_bwtbl -> hfi_send_feature_ctrl(ECP, 0, 0)
 *   -> [ACD only if ADRENO_ACD_CTRL; LM only if ADRENO_LM_CTRL: both skipped]
 *   -> hfi_send_core_fw_start(handle 0).
 * Each message: header seq<<20 | 0<<16 | size_dw<<8 | id into queue 0, pad to
 * 4 dwords (v2), doorbell; then wait for the ACK [hdr, req hdr, error, ...].
 *
 * Perf table (struct hfi_dcvstable_cmd, 59 dwords incl. header):
 *   gpu_level_num, gmu_level_num, gx_votes[16] {vote, acd, kHz},
 *   cx_votes[4] {vote, kHz}.
 *   GX levels: DT qcom,gpu-pwrlevels-0 (speed bin 0, a subset of bin 1) with
 *   the opp-microvolt of gpu_opp_table_v2 (the table the kgsl node uses),
 *   minus RPMH_REGULATOR_LEVEL_OFFSET (1), in ascending order as KGSL sends
 *   them (gmu->gpu_freqs[i] = pwrlevels[n-1-i]).
 *   GMU levels: DT qcom,gmu-pwrlevels 0 and 200 MHz; 200 MHz needs vdd_cx
 *   VDD_MIN = MIN_SVS (48) per gpucc-sm8150.c gpu_cc_gmu_clk_src.
 *   Vote = vlvl<<16 | mx index<<8 | rail index (ARC_VOTE_SET), built as in
 *   kgsl_gmu.c setup_volt_dependency_tbl from the cmd-db aux tables
 *   gfx.lvl / cx.lvl (primary) and mx.lvl (secondary).
 * Bus table (struct hfi_bwtable_cmd, 160 dwords incl. header): one "off" level like
 *   upstream Linux for A640; DDR commands SH0, MC0, ACV and CNOC command CN0,
 *   addresses looked up by name in cmd-db (HackiOS does the same).
 * Writes only GMU shared memory and the GMU doorbell. GX stays off.
 */
#define H2F_MSG_BW_VOTE_TBL	3U
#define H2F_MSG_PERF_TBL	4U
#define H2F_MSG_FEATURE_CTRL	11U
#define H2F_MSG_CORE_FW_START	14U
#define HFI_FEATURE_ECP		1U
#define HFI_TYPE_ACK		1U	/* header bits 19:16 */
#define F2H_MSG_ERR		100U
#define HFI_DCVS_DW		59U	/* hdr + 2 + 16*3 + 4*2 */
#define HFI_BW_DW		160U	/* hdr + 5 + 6 + 2*6 + 8 + 16*8 */
#define BCM_OFF			0x40000000U	/* commit, no vote */
#define BCM_CNOC_ON		0x60000001U	/* commit|valid, y=1 */

static const uint32_t gx_khz[]  = { 0, 257000, 345000, 427000, 499200, 585000 };
/*
 * GPU clock: the perf-table index step 11 votes (H2F_MSG_GX_BW_PERF_VOTE). Boot-arg sweetgpufreq=N,
 * N = 1..5 -> 257 / 345 / 427 / 499.2 / 585 MHz (DT gpu_opp_table_v2, speed bin 0, valid for both
 * bins). Default 1 = what every passing boot used. Nothing after the shim changes it (no XNU patch
 * sends a GX perf vote), so this is the GPU's clock for the whole session.
 */
static uint32_t gx_freq_idx = 1;
static const uint32_t gx_vlvl[] = { 0, 64, 128, 192, 224, 256 };
static const uint32_t cx_khz[]  = { 0, 200000 };
static const uint32_t cx_vlvl[] = { 0, 48 };
#define N_GX	(sizeof(gx_khz) / sizeof(gx_khz[0]))
#define N_CX	(sizeof(cx_khz) / sizeof(cx_khz[0]))

/* cmd-db base from the locator (same rule as rpmh_vote_gpu_bw). */
static uint32_t cmd_db_base(void)
{
	uint32_t loc0 = rd(CMDDB_LOCATOR), loc1 = rd(CMDDB_LOCATOR + 4);
	if (loc0 >= 0x80000000U && loc0 < 0xf0000000U)
		return loc0;
	if (loc1 >= 0x80000000U && loc1 < 0xf0000000U)
		return loc1;
	return 0;
}

/* Aligned 32-bit read of a little-endian u16 at any even address (Device memory). */
static uint32_t rd16(uint32_t a)
{
	uint32_t w = rd(a & ~3U);
	return (a & 2U) ? (w >> 16) : (w & 0xffffU);
}

/*
 * Find an entry by its full id (up to 8 chars, NUL padded), as Linux cmd-db.c:
 * entry_header { u8 id[8]; le32 priority[2]; le32 addr; le16 len; le16 offset; }
 * aux data at data[] + rsc_hdr.data_offset + entry.offset, data[] at 0x90.
 */
static int cmd_db_find(uint32_t db, const char *name, uint32_t *addr, uint32_t *len, uint32_t *aux)
{
	uint32_t want0 = 0, want1 = 0;
	for (unsigned k = 0; k < 8 && name[k]; k++) {
		if (k < 4)
			want0 |= (uint32_t)(uint8_t)name[k] << (8 * k);
		else
			want1 |= (uint32_t)(uint8_t)name[k] << (8 * (k - 4));
	}
	if (rd(db + 4) != 0x0c0330dbU)
		return 0;
	for (unsigned s = 0; s < 8; s++) {
		uint32_t w0 = rd(db + 8 + s * 16), w1 = rd(db + 8 + s * 16 + 4);
		uint32_t header_offset = w0 >> 16, data_offset = w1 & 0xffffU, cnt = w1 >> 16;
		if (!cnt || cnt > 512)
			continue;
		for (unsigned e = 0; e < cnt; e++) {
			uint32_t eo = db + CMDDB_DATA_START + header_offset + e * 24;
			if (rd(eo) != want0 || rd(eo + 4) != want1)
				continue;
			uint32_t lo = rd(eo + 20);
			*addr = rd(eo + 16);
			*len = lo & 0xffffU;
			*aux = db + CMDDB_DATA_START + data_offset + (lo >> 16);
			return 1;
		}
	}
	return 0;
}

/* VLVL table of a rail (kgsl_gmu.c rpmh_arc_cmds); returns the count, 0 on error. */
static unsigned arc_load(uint32_t db, const char *name, uint16_t *val)
{
	uint32_t addr = 0, len = 0, aux = 0;
	if (!cmd_db_find(db, name, &addr, &len, &aux) || len < 2 || len > 32 || (aux & 1U))
		return 0;
	unsigned total = len >> 1, num;
	for (unsigned i = 0; i < 16; i++)
		val[i] = (i < total) ? (uint16_t)rd16(aux + 2U * i) : 0;
	for (num = 1; num < total; num++)
		if (val[num - 1] != 0 && val[num] == 0)
			break;
	return num;
}

/* kgsl_gmu.c setup_volt_dependency_tbl for one level. */
static int arc_vote(uint32_t vlvl, const uint16_t *pri, unsigned npri,
		    const uint16_t *sec, unsigned nsec, uint32_t *vote)
{
	unsigned k, j;
	for (k = 0; k < npri; k++)
		if (pri[k] >= vlvl)
			break;
	if (k == npri || nsec == 0)
		return 0;
	uint32_t cur = pri[k];
	for (j = 0; j < nsec; j++)
		if (sec[j] >= cur || j + 1 == nsec)
			break;
	if (j == nsec)
		j = 0;
	*vote = (cur << 16) | (j << 8) | k;
	return 1;
}

/* Queue-0 writer (kgsl_hfi.c hfi_queue_write). */
static volatile uint32_t *hq;
static uint32_t hq_sz, hq_w, hq_start, hq_len;

static void hfi_put(uint32_t v)
{
	hq[hq_w] = v;
	hq_w = (hq_w + 1U) % hq_sz;
}

static int hfi_begin(uint32_t id, uint32_t size_dw, uint32_t *hdr)
{
	volatile uint32_t *h0 = (volatile uint32_t *)gmu_va_to_pa(GMU_VA_HFI_QTBL) + 6;
	hq = (volatile uint32_t *)gmu_va_to_pa(h0[1]);
	hq_sz = h0[3];
	if (!hq || hq_sz == 0 || hq_sz > 0x400U)
		return 0;
	uint32_t r = h0[10] % hq_sz, w = h0[11] % hq_sz;
	uint32_t space = (w >= r) ? hq_sz - (w - r) : r - w;
	if (space <= size_dw + 4U)
		return 0;
	hfi_seq = (hfi_seq + 1U) & 0xfffU;
	*hdr = (hfi_seq << 20) | (0U << 16) | ((size_dw & 0xffU) << 8) | (id & 0xffU);
	hq_w = w;
	hq_start = w;
	hq_len = size_dw;
	hfi_put(*hdr);
	return 1;
}

static int hfi_commit(void)
{
	volatile uint32_t *h0 = (volatile uint32_t *)gmu_va_to_pa(GMU_VA_HFI_QTBL) + 6;
	uint32_t written = (hq_w + hq_sz - hq_start) % hq_sz;

	/* Never ring the doorbell for a message whose length disagrees with its header. */
	if (written != hq_len) {
		con_hex2("HFI len", "wrote=", written, "hdr says=", hq_len);
		return 0;
	}
	if ((hfi_ver >> 28) >= 2)
		for (unsigned i = 0; i < 3 && (hq_w % 4U); i++)
			hfi_put(0xFAFAFAFAU);
	__asm__ volatile("dsb sy" ::: "memory");
	h0[11] = hq_w;
	__asm__ volatile("dsb sy" ::: "memory");
	wr(GMU_REG(GMU_HOST2GMU_INTR_SET), 1);
	return 1;
}

/*
 * Wait for the ACK to `hdr` (same sequence number), draining queue 1.
 * Other messages are printed (max 2). Returns 1 with the verdict in *err.
 */
static int hfi_wait_ack(uint32_t hdr, uint32_t *err)
{
	volatile uint32_t *h1 = (volatile uint32_t *)gmu_va_to_pa(GMU_VA_HFI_QTBL) + 6 + 12;
	volatile uint32_t *q1 = (volatile uint32_t *)gmu_va_to_pa(h1[1]);
	uint32_t q1sz = h1[3];
	unsigned others = 0;
	int found = 0;

	if (!q1 || q1sz == 0 || q1sz > 0x400U)
		return 0;
	for (unsigned round = 0; round < 3 && !found; round++) {
		int got = poll(GMU_REG(GMU_GMU2HOST_INTR_INFO), 1, 1, 100000);
		if (got)
			wr(GMU_REG(GMU_GMU2HOST_INTR_CLR), 1);
		for (unsigned guard = 0; guard < 32 && (h1[10] % q1sz) != (h1[11] % q1sz); guard++) {
			uint32_t r = h1[10] % q1sz;
			uint32_t m0 = q1[r], m1 = q1[(r + 1U) % q1sz];
			uint32_t m2 = q1[(r + 2U) % q1sz], m3 = q1[(r + 3U) % q1sz];
			uint32_t size = (m0 >> 8) & 0xffU;
			r = (r + (size ? size : 1U)) % q1sz;
			if ((hfi_ver >> 28) >= 2)
				r = ((r + 3U) & ~3U) % q1sz;
			h1[10] = r;
			__asm__ volatile("dsb sy" ::: "memory");
			if (((m0 >> 16) & 0xfU) == HFI_TYPE_ACK &&
			    ((m1 >> 20) & 0xfffU) == ((hdr >> 20) & 0xfffU)) {
				*err = m2;
				found = 1;
			} else if (others++ < 2) {
				con_hex2((m0 & 0xffU) == F2H_MSG_ERR ? "HFI F2H ERR" : "HFI other",
					 "hdr=", m0, "w1=", m1);
				con_hex2("HFI   ..", "w2=", m2, "w3=", m3);
			}
		}
		if (!got)
			break;
	}
	return found;
}

/* Send one message whose body is already in queue 0, wait, print one line. */
static int hfi_finish(const char *what, uint32_t hdr)
{
	uint32_t err = 0xffffffffU;
	if (!hfi_commit()) {
		note_fail(what);
		return 0;
	}
	int ok = hfi_wait_ack(hdr, &err);
	if (!ok) {
		con_hex2(what, "NO ACK hdr=", hdr, "fw=", rd(GMU_REG(GMU_CM3_FW_INIT_RESULT)));
		con_hex2(what, "info=", rd(GMU_REG(GMU_GMU2HOST_INTR_INFO)),
			 "ao=", rd(GMU_REG(GMU_AO_HOST_INTERRUPT_STATUS)));
		note_fail(what);
		return 0;
	}
	con_hex2(what, "ack hdr=", hdr, "err=", err);
	if (err) {
		note_fail(what);
		return 0;
	}
	return 1;
}

static void step10_hfi_tables(void)
{
	uint16_t gfx[16], cx[16], mx[16];
	uint32_t gx_vote[N_GX], cx_vote[N_CX];
	uint32_t hdr, a_sh0 = 0, a_mc0 = 0, a_acv = 0, a_cn0 = 0, l, x;
	unsigned n_gfx, n_cx, n_mx, i;

	con_str("GPU", "step 10: perf/bw tables, ECP off, CORE_FW_START");
	if (rd(GMU_REG(GMU_CM3_FW_INIT_RESULT)) != 0xbabefaceU || hfi_seq == 0) {
		con_str("HFI10", "needs step 9 (HFI up): skip");
		note_fail("HFI10 no HFI");
		return;
	}
	uint32_t db = cmd_db_base();
	if (!db) {
		con_str("HFI10", "cmd-db locator bad");
		note_fail("HFI10 cmd-db");
		return;
	}
	n_gfx = arc_load(db, "gfx.lvl", gfx);
	n_cx = arc_load(db, "cx.lvl", cx);
	n_mx = arc_load(db, "mx.lvl", mx);
	con_hex2("ARC", "gfx n=", n_gfx, "cx n=", n_cx);
	con_hex2("ARC", "mx n=", n_mx, "gfx top=", n_gfx ? gfx[n_gfx - 1] : 0);
	if (!n_gfx || !n_cx || !n_mx || N_GX > n_gfx || N_CX > n_cx) {
		note_fail("HFI10 ARC tables");
		return;
	}
	for (i = 0; i < N_GX; i++)
		if (!arc_vote(gx_vlvl[i], gfx, n_gfx, mx, n_mx, &gx_vote[i])) {
			con_hex2("ARC", "no gfx level for vlvl=", gx_vlvl[i], "i=", i);
			note_fail("HFI10 gx vote");
			return;
		}
	for (i = 0; i < N_CX; i++)
		if (!arc_vote(cx_vlvl[i], cx, n_cx, mx, n_mx, &cx_vote[i])) {
			con_hex2("ARC", "no cx level for vlvl=", cx_vlvl[i], "i=", i);
			note_fail("HFI10 cx vote");
			return;
		}
	con_hex2("votes", "gx1=", gx_vote[1], "gxtop=", gx_vote[N_GX - 1]);
	con_hex2("votes", "cx0=", cx_vote[0], "cx1=", cx_vote[1]);

	if (!cmd_db_find(db, "SH0", &a_sh0, &l, &x) || !cmd_db_find(db, "MC0", &a_mc0, &l, &x) ||
	    !cmd_db_find(db, "ACV", &a_acv, &l, &x) || !cmd_db_find(db, "CN0", &a_cn0, &l, &x)) {
		con_hex2("BW", "SH0=", a_sh0, "MC0=", a_mc0);
		con_hex2("BW", "ACV=", a_acv, "CN0=", a_cn0);
		note_fail("HFI10 BCM names");
		return;
	}
	con_hex2("BW", "SH0=", a_sh0, "MC0=", a_mc0);
	con_hex2("BW", "ACV=", a_acv, "CN0=", a_cn0);

	/* 1. H2F_MSG_PERF_TBL (hfi_dcvstable_cmd) */
	if (!hfi_begin(H2F_MSG_PERF_TBL, HFI_DCVS_DW, &hdr)) {
		note_fail("HFI10 q0 full");
		return;
	}
	hfi_put(N_GX);
	hfi_put(N_CX);
	for (i = 0; i < 16; i++) {
		hfi_put(i < N_GX ? gx_vote[i] : 0);
		hfi_put(i < N_GX ? 0xffffffffU : 0);	/* acd: max threshold, unused */
		hfi_put(i < N_GX ? gx_khz[i] : 0);
	}
	for (i = 0; i < 4; i++) {
		hfi_put(i < N_CX ? cx_vote[i] : 0);
		hfi_put(i < N_CX ? cx_khz[i] : 0);
	}
	if (!hfi_finish("HFI perf", hdr))
		return;

	/* 2. H2F_MSG_BW_VOTE_TBL (hfi_bwtable_cmd): one "off" level */
	if (!hfi_begin(H2F_MSG_BW_VOTE_TBL, HFI_BW_DW, &hdr)) {
		note_fail("HFI10 q0 full");
		return;
	}
	hfi_put(1);		/* bw_level_num */
	hfi_put(1);		/* cnoc_cmds_num */
	hfi_put(3);		/* ddr_cmds_num */
	hfi_put(1);		/* cnoc_wait_bitmask */
	hfi_put(1);		/* ddr_wait_bitmask */
	for (i = 0; i < 6; i++)			/* cnoc_cmd_addrs[6] */
		hfi_put(i == 0 ? a_cn0 : 0);
	for (i = 0; i < 2 * 6; i++)		/* cnoc_cmd_data[2][6] */
		hfi_put(i == 0 ? BCM_OFF : (i == 6 ? BCM_CNOC_ON : 0));
	for (i = 0; i < 8; i++)			/* ddr_cmd_addrs[8] */
		hfi_put(i == 0 ? a_sh0 : i == 1 ? a_mc0 : i == 2 ? a_acv : 0);
	for (i = 0; i < 16 * 8; i++)		/* ddr_cmd_data[16][8] */
		hfi_put(i < 3 ? BCM_OFF : 0);
	if (!hfi_finish("HFI bw", hdr))
		return;

	/* 3. H2F_MSG_FEATURE_CTRL: ECP disabled */
	if (!hfi_begin(H2F_MSG_FEATURE_CTRL, 4, &hdr)) {
		note_fail("HFI10 q0 full");
		return;
	}
	hfi_put(HFI_FEATURE_ECP);
	hfi_put(0);
	hfi_put(0);
	if (!hfi_finish("HFI ecp", hdr))
		return;

	/* 4. H2F_MSG_CORE_FW_START, handle 0 */
	if (!hfi_begin(H2F_MSG_CORE_FW_START, 2, &hdr)) {
		note_fail("HFI10 q0 full");
		return;
	}
	hfi_put(0);
	if (!hfi_finish("HFI fwst", hdr))
		return;
	con_hex2("HFI10 OK", "seq=", hfi_seq, "fw=", rd(GMU_REG(GMU_CM3_FW_INIT_RESULT)));
	hfi_tables_ok = 1;
}

/*
 * Step 11: ask the GMU to power the 3D core (GX), then read 3D-core registers.
 * KGSL order after hfi_start() (kgsl_gmu.c gmu_start, adreno.c _adreno_start,
 * adreno_a6xx_gmu.c):
 *   1. kgsl_pwrctrl_set_default_gpu_pwrlevel -> gmu_dcvs_set ->
 *      H2F_MSG_GX_BW_PERF_VOTE (id 30) {ack_type 1 = DCVS_ACK_BLOCK,
 *      freq = index into our perf table, bw = 0xFF = INVALID_DCVS_IDX}.
 *      Index 1 = 257 MHz (lowest real level).
 *   2. a6xx_gmu_oob_set(oob_gpu = 0): not a630/a615 -> set BIT(30 - 2*0),
 *      wait GMU2HOST_INTR_INFO BIT(31 - 0) (100 ms), clear it. Legacy rule
 *      (HFI version branch bits == 0): set/check BIT(16). Our HFI version
 *      0x20000001 has branch 1 -> bits 30/31 (HackiOS uses the same).
 *   3. a6xx_gmu_hfi_start_msg: A640 sends H2F_MSG_START (id 10, header only).
 *   4. GX is on when GMU_SPTPRAC_PWR_CLK_STATUS bits 6 (GX_GDSC_POWER_OFF)
 *      and 7 (GX_CLK_OFF) are clear (a6xx_gmu_gx_is_on). Only then read
 *      RBBM_STATUS (0x210), RBBM_INT_0_STATUS (0x201), RBBM_CLOCK_CNTL (0xae)
 *      from the kgsl_3d0 block at 0x02c00000 (DT reg 0).
 *   5. At sweetgpu=11 release the request again (a6xx_gmu_oob_clear:
 *      BIT(31 - 2*0) into HOST2GMU_INTR_SET) so the GMU may power GX down.
 */
#define H2F_MSG_START		10U
#define H2F_MSG_GX_BW_PERF_VOTE	30U
#define GMU_SPTPRAC_PWR_CLK_STATUS	0x50d0U		/* 0x1F8D0 - 0x1A800 */
#define GMU_RPMH_POWER_STATE	0x50ecU		/* 0x1F8EC - 0x1A800 */
#define GX_OFF_BITS		0xc0U		/* GX_GDSC_POWER_OFF | GX_CLK_OFF */
#define GPU3D_BASE		0x02c00000UL	/* kgsl_3d0_reg_memory */
#define GPU3D_REG(d)		(GPU3D_BASE + ((uint32_t)(d) << 2))
#define A6XX_RBBM_STATUS	0x210U
#define A6XX_RBBM_INT_0_STATUS	0x201U
#define A6XX_RBBM_CLOCK_CNTL	0x0aeU

static void step11_gx_power(unsigned level)
{
	uint32_t hdr, set, check, info = 0;

	con_str("GPU", "step 11: GX vote, OOB gpu, START, RBBM");
	if (!hfi_tables_ok) {
		con_str("GX", "needs step 10 (CORE_FW_START): skip");
		note_fail("GX no step 10");
		return;
	}
	con_hex2("GX before", "sptprac=", rd(GMU_REG(GMU_SPTPRAC_PWR_CLK_STATUS)),
		 "gx_gdscr=", rd(GPUCC_BASE + 0x100c));

	/* 1. default GPU level: perf index 1 (257 MHz), no bus change */
	if (!hfi_begin(H2F_MSG_GX_BW_PERF_VOTE, 4, &hdr)) {
		note_fail("GX q0 full");
		return;
	}
	hfi_put(1);		/* ack_type: DCVS_ACK_BLOCK */
	hfi_put(gx_freq_idx);	/* freq: perf table index (sweetgpufreq=, default 1 = 257 MHz) */
	hfi_put(0xffU);		/* bw: INVALID_DCVS_IDX */
	{
		int was_muted = con_mute;               /* shown even when step 11 is muted (sweetgpu >= 12) */
		con_mute = 0;
		con_hex2("GX clock", "index=", gx_freq_idx, "kHz=", gx_khz[gx_freq_idx]);
		con_mute = was_muted;
	}
	if (!hfi_finish("HFI gxvote", hdr))
		return;

	/* 2. OOB request "gpu" */
	if ((hfi_ver & 0x1fU) == 0) {
		set = 1U << 16;
		check = 1U << 16;
	} else {
		set = 1U << 30;
		check = 1U << 31;
	}
	wr(GMU_REG(GMU_HOST2GMU_INTR_SET), set);
	int granted = poll(GMU_REG(GMU_GMU2HOST_INTR_INFO), check, check, 100000);
	info = rd(GMU_REG(GMU_GMU2HOST_INTR_INFO));
	wr(GMU_REG(GMU_GMU2HOST_INTR_CLR), check);
	uint32_t sp = rd(GMU_REG(GMU_SPTPRAC_PWR_CLK_STATUS));
	con_hex2(granted ? "OOB gpu ok" : "OOB gpu TO", "info=", info, "sptprac=", sp);
	con_hex2("GX after", "gx_gdscr=", rd(GPUCC_BASE + 0x100c),
		 "rpmh_st=", rd(GMU_REG(GMU_RPMH_POWER_STATE)));
	if (!granted || (sp & GX_OFF_BITS)) {
		con_hex2("GX", "not on: fw=", rd(GMU_REG(GMU_CM3_FW_INIT_RESULT)),
			 "ao=", rd(GMU_REG(GMU_AO_HOST_INTERRUPT_STATUS)));
		note_fail(granted ? "GX not on" : "OOB gpu timeout");
		return;		/* never touch 3D-core registers without GX */
	}

	/* 3. H2F_MSG_START (A640) */
	if (!hfi_begin(H2F_MSG_START, 1, &hdr)) {
		note_fail("GX q0 full");
		return;
	}
	if (!hfi_finish("HFI start", hdr))
		return;

	/* 4. first reads of the 3D core; re-check GX right before */
	sp = rd(GMU_REG(GMU_SPTPRAC_PWR_CLK_STATUS));
	if (sp & GX_OFF_BITS) {
		con_hex2("GX", "went off before read sptprac=", sp, "pad=", 0);
		note_fail("GX dropped");
		return;
	}
	con_hex2("RBBM", "status=", rd(GPU3D_REG(A6XX_RBBM_STATUS)),
		 "int0=", rd(GPU3D_REG(A6XX_RBBM_INT_0_STATUS)));
	con_hex2("RBBM", "clock_cntl=", rd(GPU3D_REG(A6XX_RBBM_CLOCK_CNTL)), "sptprac=", sp);
	con_str("GX OK", "3D core powered and readable");
	gx_on = 1;

	/* 5. release the request at sweetgpu=11 (later steps keep it) */
	if (level == 11) {
		wr(GMU_REG(GMU_HOST2GMU_INTR_SET), (hfi_ver & 0x1fU) ? (1U << 31) : (1U << 24));
		con_str("GX", "OOB gpu released");
		gx_on = 0;
	}
}

/*
 * Step 12 (= planned 12a + 12b): the 3D core's own SMMU context, then start
 * the command processor (CP) without sending it any commands yet.
 *
 * 12a, SMMU (same register sequence as step 5, which works for the GMU):
 *   - stream: DT gfx3d_user iommus <smmu 0 0x401> -> SMR id 0 mask 0x401
 *     (no overlap with the GMU's id 5 mask 0x400);
 *   - pages UNPRIVILEGED read/write (AP[2:1] = 01, PTE bit 6): the 3D core's
 *     accesses are unprivileged (HackiOS PTE_UNPRIVILEGED for the GPU
 *     context; the GMU needed the opposite, boot 9);
 *   - tables at scratch +1 MiB (step 5 uses the first MiB), buffers at
 *     scratch +0x110000; GPU VA 0x01000000.. (outside the UCHE GMEM window
 *     0x100000-0x1fffff = uche_gmem_base 1 MiB + GMEM 1 MiB):
 *       0x01000000 SQE microcode (a630_sqe.fw minus its first dword, as
 *                  KGSL _load_firmware), 16 pages
 *       0x01010000 ringbuffer, 1 page (1024 dwords)
 *       0x01011000 scratch page: RPTR shadow at +0, fence at +0x100 (12c)
 * 12b, CP start (KGSL _adreno_start / a6xx_start / a6xx_rb_start order):
 *   - a6xx_start constants that need no feature data: GBIF (a640_gbif),
 *     PERFCTR_GPU_BUSY_MASKED, UCHE ranges/trap/write-thru, UCHE GMEM range,
 *     FILTER/WAYS, ROQ thresholds (a640), MEM_POOL 128, PC_DBG_ECO (a640),
 *     CP_AHB_CNTL 1, PERFCTR_CNTL 1, NC_MODE from DT (hbb 15 -> 2, mal 32 ->
 *     0, ubwc 3 -> mode 0 + amsbc 1), INTERFACE_HANG, UCHE_CLIENT_PF.
 *     Left out: HW clock gating, CP protect, preemption, LM, 64-bit mode
 *     (all our GPU addresses are below 4 GiB).
 *   - a6xx_rb_start: RPTR_ADDR, RB_CNTL (A6XX_CP_RB_CNTL_DEFAULT formula for
 *     a 1024-dword ring = 2<<8 | 9), RB_BASE, SQE_INSTR_BASE, SQE_CNTL = 1.
 *   - adreno_set_unsecured_mode without a zap shader:
 *     RBBM_SECVID_TRUST_CNTL = 0 (notes "Gate 3", direct-write variant).
 * Then 10 ms and read fault/status registers. No commands are submitted.
 */
#define GPU_SID			0U
#define GPU_SID_MASK		0x401U
#define GPU_PT_PA		(SCRATCH_PA + 0x100000UL)	/* L1 +0, L2 +0x1000, L3 +0x2000 */
#define GPU_BUF_PA		(SCRATCH_PA + 0x110000UL)
#define GPU_VA_BASE		0x01000000U
#define GPU_VA_SQE		(GPU_VA_BASE)
#define GPU_VA_RB		(GPU_VA_BASE + 0x10000U)
#define GPU_VA_SCR		(GPU_VA_BASE + 0x11000U)
#define GPU_NPAGES		0x12U
#define GPU_SQE_MAX		0x10000U
#define GPU_RB_CNTL		((2U << 8) | 9U)

/* G2 (step 14): indirect buffer + framebuffer window, mapped in step 12a */
#define FB_PA			0x9c000000UL	/* cont_splash; initialize_screen b=9C000000 */
#define FB_STRIDE		4320U		/* r=0x10E0 */
#define FB_W			1080U
#define FB_H			2400U
#define G2_X0			415U
#define G2_Y0			2100U
#define G2_W			250U
#define G2_H			250U
#define G2_COLOR		0xff00ff00U	/* green in ARGB and in ABGR */
#define G2_BLACK		0xff000000U
#define GPU_VA_IB		0x01100000U	/* L3 index 256..511 of the existing table */
#define GPU_IB_PA		(SCRATCH_PA + 0x200000UL)	/* 1 MiB */
#define GPU_IB_PAGES		256U
#define GPU_VA_FBWIN		0x01200000U	/* L2 index 9 -> new L3 at GPU_PT_PA + 0x3000 */
#define GPU_FBWIN_PAGES		512U		/* 2 MiB window */
#define G2_WIN_PA		((FB_PA + (uintptr_t)G2_Y0 * FB_STRIDE) & ~0xfffUL)

/*
 * G3 (step 15): 2D blitter. Source and destination images, mapped in step 12a
 * (sweetgpu >= 15) in the same L3 table as the ring/IB. Each image has 4 guard
 * pages behind it so a cache-line prefetch past the last row cannot fault.
 */
#define G3_SRC_W		128U
#define G3_SRC_H		128U
#define G3_SRC_PITCH		512U	/* bytes; the 2D engine's pitch fields are in units of 64 */
#define G3_DST_W		256U
#define G3_DST_H		256U
#define G3_DST_PITCH		1024U
#define G3_SRC_PA		(SCRATCH_PA + 0x300000UL)	/* 64 KiB + guard */
#define G3_DST_PA		(SCRATCH_PA + 0x320000UL)	/* 256 KiB + guard */
#define GPU_VA_G3SRC		0x01020000U	/* L3 index 0x20 */
#define GPU_VA_G3DST		0x01040000U	/* L3 index 0x40 */
#define G3_SRC_PAGES		20U		/* 16 + 4 guard */
#define G3_DST_PAGES		68U		/* 64 + 4 guard */
#define G3_IB_OFF_A		0x40000U	/* byte offsets into the IB area; step 14 uses the first 253 KB */
#define G3_IB_OFF_B		0x41000U
#define G3_X0			100U		/* on-screen copy, left of the G2 square (x 415..664) */
#define G3_Y0			2100U		/* must be even: see step 15 */

#define A6XX_GBIF_QSB_SIDE0	0x3c03U
#define A6XX_RBBM_GBIF_CLIENT_QOS_CNTL	0x0011U
#define A6XX_RBBM_PERFCTR_GPU_BUSY_MASKED	0x050bU
#define A6XX_UCHE_WRITE_RANGE_MAX_LO	0x0e05U
#define A6XX_UCHE_WRITE_THRU_BASE_LO	0x0e07U
#define A6XX_UCHE_TRAP_BASE_LO	0x0e09U
#define A6XX_UCHE_GMEM_RANGE_MIN_LO	0x0e0bU
#define A6XX_UCHE_CACHE_WAYS	0x0e17U
#define A6XX_UCHE_FILTER_CNTL	0x0e18U
#define A6XX_UCHE_CLIENT_PF	0x0e19U
#define A6XX_UCHE_MODE_CNTL	0x0e01U
#define A6XX_CP_ROQ_THRESHOLDS_1	0x08c1U
#define A6XX_CP_ROQ_THRESHOLDS_2	0x08c2U
#define A6XX_CP_MEM_POOL_SIZE	0x08c3U
#define A6XX_PC_DBG_ECO_CNTL	0x9e00U
#define A6XX_CP_AHB_CNTL	0x098dU
#define A6XX_RBBM_PERFCTR_CNTL	0x0500U
#define A6XX_RB_NC_MODE_CNTL	0x8e08U
#define A6XX_TPL1_NC_MODE_CNTL	0xb604U
#define A6XX_SP_NC_MODE_CNTL	0xae02U
#define A6XX_RBBM_INTERFACE_HANG_INT_CNTL	0x001fU
#define A6XX_CP_RB_BASE		0x0800U
#define A6XX_CP_RB_BASE_HI	0x0801U
#define A6XX_CP_RB_CNTL		0x0802U
#define A6XX_CP_RB_RPTR_ADDR_LO	0x0804U
#define A6XX_CP_RB_RPTR_ADDR_HI	0x0805U
#define A6XX_CP_RB_RPTR		0x0806U
#define A6XX_CP_RB_WPTR		0x0807U
#define A6XX_CP_SQE_CNTL	0x0808U
#define A6XX_CP_SQE_INSTR_BASE_LO	0x0830U
#define A6XX_CP_SQE_INSTR_BASE_HI	0x0831U
#define A6XX_CP_HW_FAULT	0x0821U
#define A6XX_CP_INTERRUPT_STATUS	0x0823U
#define A6XX_CP_STATUS_1	0x0825U
#define A6XX_CP_SQE_STAT_ADDR	0x0908U
#define A6XX_CP_SQE_STAT_DATA	0x0909U
#define A6XX_RBBM_SECVID_TRUST_CNTL	0xf400U
/* trusted (secure-only) GPU VA range; Linux a6xx_hw_init and HackiOS step 20 set all 0 */
#define A6XX_RBBM_SECVID_TSB_TRUSTED_BASE	0xf800U	/* lo, hi 0xf801 */
#define A6XX_RBBM_SECVID_TSB_TRUSTED_SIZE	0xf802U
#define A6XX_RBBM_SECVID_TSB_CNTL	0xf803U
#define A6XX_RBBM_GBIF_HALT	0x0016U
#define A6XX_RB_DBG_ECO_CNTL	0x8e04U
static uint32_t tsb_old[4] = { 0xffffffffU, 0xffffffffU, 0xffffffffU, 0xffffffffU };	/* base lo/hi, size, cntl */
#define SMMU_CB_FAR		0x060
#define SMMU_CB_FSYNR0		0x068

static int gpu_ctx_ok;		/* step 12a done */
static int cp_started;		/* step 12b: CP_SQE_CNTL=1, no HW fault */
static int g1_ok;		/* step 13: fence seen */
static unsigned gpu_level;	/* sweetgpu=N, for steps that prepare later ones */
static uint32_t ring_w;		/* next free ring dword after step 13 */
static uint32_t gpu_cb_idx = 0xff;

static uintptr_t gpu_va_pa(uint32_t va)
{
	return GPU_BUF_PA + (uintptr_t)(va - GPU_VA_BASE);
}

/* Write a 3D-core register, read it back, count mismatches. */
static unsigned g_wr_n, g_wr_bad;
static uint32_t g_bad_reg[3], g_bad_r[3];
static void gwr(uint32_t reg, uint32_t v)
{
	wr(GPU3D_REG(reg), v);
	uint32_t r = rd(GPU3D_REG(reg));
	g_wr_n++;
	if (r != v) {
		if (g_wr_bad < 3) {
			g_bad_reg[g_wr_bad] = reg;
			g_bad_r[g_wr_bad] = r;
		}
		g_wr_bad++;
	}
}

/*
 * GPU page tables (L1 +0, L2 +0x1000, L3 +0x2000, FB-window L3 +0x3000) and
 * the zeroed buffer pages. Pure memory work, no register access (also run by
 * the host simulation of the CP).
 */
static void gpu_pt_build(void)
{
	uintptr_t l1 = GPU_PT_PA, l2 = GPU_PT_PA + 0x1000, l3 = GPU_PT_PA + 0x2000;
	for (uintptr_t a = GPU_PT_PA; a < GPU_PT_PA + 0x3000; a += 8)
		*(volatile uint64_t *)a = 0;
	for (uintptr_t a = GPU_BUF_PA; a < GPU_BUF_PA + GPU_NPAGES * 0x1000UL; a += 8)
		*(volatile uint64_t *)a = 0;
	__asm__ volatile("dsb sy" ::: "memory");
	const uint64_t ATTR_NC = (1ULL << 2), AP_USER_RW = (1ULL << 6);
	const uint64_t SH_OS = (2ULL << 8), AF = (1ULL << 10), PAGE = 3ULL, TABLE = 3ULL;
	pte_write(l1 + ((GPU_VA_BASE >> 30) & 0x1ffU) * 8, l2 | TABLE);
	pte_write(l2 + ((GPU_VA_BASE >> 21) & 0x1ffU) * 8, l3 | TABLE);
	for (unsigned i = 0; i < GPU_NPAGES; i++) {
		uint32_t l3i = ((GPU_VA_BASE >> 12) & 0x1ffU) + i;
		pte_write(l3 + (uint64_t)l3i * 8,
			  (GPU_BUF_PA + (uint64_t)i * 0x1000) | PAGE | ATTR_NC | AP_USER_RW | SH_OS | AF);
	}
	if (gpu_level >= 14) {
		/* G2: indirect buffer (1 MiB) in the same L3 table, framebuffer window in a new one */
		uintptr_t l3fb = GPU_PT_PA + 0x3000;
		for (uintptr_t a = l3fb; a < l3fb + 0x1000; a += 8)
			*(volatile uint64_t *)a = 0;
		for (unsigned i = 0; i < GPU_IB_PAGES; i++)
			pte_write(l3 + (uint64_t)(((GPU_VA_IB >> 12) & 0x1ffU) + i) * 8,
				  (GPU_IB_PA + (uint64_t)i * 0x1000) | PAGE | ATTR_NC | AP_USER_RW | SH_OS | AF);
		pte_write(l2 + ((GPU_VA_FBWIN >> 21) & 0x1ffU) * 8, l3fb | TABLE);
		for (unsigned i = 0; i < GPU_FBWIN_PAGES; i++)
			pte_write(l3fb + (uint64_t)i * 8,
				  (G2_WIN_PA + (uint64_t)i * 0x1000) | PAGE | ATTR_NC | AP_USER_RW | SH_OS | AF);
	}
	if (gpu_level >= 15) {
		/* G3: source and destination images (+ guard pages) in the same L3 table */
		for (unsigned i = 0; i < G3_SRC_PAGES; i++)
			pte_write(l3 + (uint64_t)(((GPU_VA_G3SRC >> 12) & 0x1ffU) + i) * 8,
				  (G3_SRC_PA + (uint64_t)i * 0x1000) | PAGE | ATTR_NC | AP_USER_RW | SH_OS | AF);
		for (unsigned i = 0; i < G3_DST_PAGES; i++)
			pte_write(l3 + (uint64_t)(((GPU_VA_G3DST >> 12) & 0x1ffU) + i) * 8,
				  (G3_DST_PA + (uint64_t)i * 0x1000) | PAGE | ATTR_NC | AP_USER_RW | SH_OS | AF);
	}
}

static void step12a_gpu_smmu(void)
{
	uint32_t id0 = rd(SMMU_BASE + SMMU_IDR0), id1 = rd(SMMU_BASE + SMMU_IDR1);
	uint32_t scr0 = rd(SMMU_BASE + SMMU_SCR0);
	uint32_t nsmr = id0 & 0xff, ncb = id1 & 0xff;
	uint32_t pg = (id1 & (1U << 31)) ? 0x10000 : 0x1000;
	int exids = (scr0 & SCR0_EXIDENABLE) != 0;

	if (nsmr == 0 || ncb == 0 || nsmr > 128 || ncb > 128 || pg != 0x1000) {
		con_hex2("GSMMU", "bad geometry idr0=", id0, "idr1=", id1);
		note_fail("GSMMU ID");
		return;
	}
	/* free SMR/CB; refuse if something already claims SID 0 */
	uint32_t cb_used[4] = { 0, 0, 0, 0 }, free_smr = 0xff, clash = 0xff;
	for (uint32_t n = 0; n < nsmr; n++) {
		uint32_t smr = rd(SMMU_BASE + SMMU_SMR(n)), s2cr = rd(SMMU_BASE + SMMU_S2CR(n));
		int v = exids ? (s2cr & S2CR_EXIDVALID) != 0 : (smr >> 31) != 0;
		if (!v) {
			if (free_smr == 0xff)
				free_smr = n;
			continue;
		}
		uint32_t type = (s2cr >> 16) & 3, cb = s2cr & 0xff;
		if (type == 0 && cb < 128)
			cb_used[cb / 32] |= 1U << (cb % 32);
		if (smr_matches(smr & 0x7fff, (smr >> 16) & 0x7fff, GPU_SID) && clash == 0xff)
			clash = n;
	}
	uint32_t free_cb = 0xff;
	for (uint32_t c = 0; c < ncb; c++)
		if (!(cb_used[c / 32] & (1U << (c % 32)))) {
			free_cb = c;
			break;
		}
	con_hex2("GSMMU", "free smr=", free_smr, "free cb=", free_cb);
	if (clash != 0xff) {
		uint32_t csmr = rd(SMMU_BASE + SMMU_SMR(clash)), cs2cr = rd(SMMU_BASE + SMMU_S2CR(clash));
		if (csmr == (SMR_VALID | (GPU_SID_MASK << 16) | GPU_SID) &&
		    ((cs2cr >> 16) & 3) == S2CR_TYPE_TRANS && (cs2cr & 0xff) < ncb) {
			/* our own entry from a previous boot (warm reboot): reuse and reprogram */
			free_smr = clash;
			free_cb = cs2cr & 0xff;
			con_hex2("GSMMU", "reuse own smr=", free_smr, "cb=", free_cb);
		} else {
			con_hex2("GSMMU", "SID 0 already in SMR ", clash, "smr=", csmr);
			note_fail("GSMMU SID0 taken");
			return;
		}
	}
	if (free_smr == 0xff || free_cb == 0xff) {
		note_fail("GSMMU free");
		return;
	}

	/* tables + buffers */
	uintptr_t l1 = GPU_PT_PA;
	gpu_pt_build();

	/* SQE microcode without its first dword (KGSL _load_firmware) */
	uint32_t len = (uint32_t)(sweet_sqe_fw_end - (const char *)sweet_sqe_fw);
	if (len < 12 || (len & 3) || len - 4 > GPU_SQE_MAX) {
		con_hex2("GSQE", "bad fw len=", len, "max=", GPU_SQE_MAX + 4);
		note_fail("GSQE len");
		return;
	}
	uint32_t ndw = (len - 4) / 4;
	volatile uint32_t *sqe = (volatile uint32_t *)gpu_va_pa(GPU_VA_SQE);
	/*
	 * Load the source address through a volatile local: otherwise the
	 * compiler may fold "sweet_sqe_fw - destination" into one 64-bit
	 * constant (seen on the x86 check build), which on AArch64 could end up
	 * as an ABS64 literal and break the no-ABS64 rule.
	 */
	const uint32_t *volatile src_v = sweet_sqe_fw;
	const uint32_t *src = src_v;
	for (uint32_t i = 0; i < ndw; i++)
		sqe[i] = src[i + 1];
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2("GSQE", "dwords=", ndw, "first=", sqe[0]);
	if (sqe[0] != src[1] || sqe[ndw - 1] != src[ndw]) {
		note_fail("GSQE copy");
		return;
	}

	/* context bank, same sequence as step 5 */
	uint32_t numpagendxb = (id1 >> 28) & 7U;
	uint32_t num_global = 1U << (numpagendxb + 1U);
	uintptr_t cb = SMMU_BASE + (uint64_t)pg * (num_global + free_cb);
	unsigned bad0 = rb_bad;
	wr(SMMU_BASE + pg + SMMU_CBA2R(free_cb), CBA2R_VA64);
	rb("G CBA2R", CBA2R_VA64, rd(SMMU_BASE + pg + SMMU_CBA2R(free_cb)));
	wr(SMMU_BASE + pg + SMMU_CBAR(free_cb), CBAR_TYPE_S1_S2_BYPASS);
	rb("G CBAR", CBAR_TYPE_S1_S2_BYPASS, rd(SMMU_BASE + pg + SMMU_CBAR(free_cb)));
	uint32_t tcr = 25U | (2U << 12) | (1U << 23);	/* T0SZ 25, SH0 outer, NC, TG0 4K, EPD1 */
	wr(cb + SMMU_CB_TTBCR, tcr);
	rb("G TCR", tcr, rd(cb + SMMU_CB_TTBCR));
	wr(cb + SMMU_CB_TTBR0, (uint32_t)l1);
	wr(cb + SMMU_CB_TTBR0 + 4, (uint32_t)(l1 >> 32));
	rb("G TTBR0", (uint32_t)l1, rd(cb + SMMU_CB_TTBR0));
	wr(cb + SMMU_CB_MAIR0, 0x00004400U);
	wr(cb + SMMU_CB_MAIR1, 0);
	rb("G MAIR0", 0x00004400U, rd(cb + SMMU_CB_MAIR0));
	uint32_t sctlr = SCTLR_M | SCTLR_CFRE | SCTLR_HUPCF;
	wr(cb + SMMU_CB_SCTLR, sctlr);
	rb("G SCTLR", sctlr, rd(cb + SMMU_CB_SCTLR));
	uint32_t smr_w = SMR_VALID | (GPU_SID_MASK << 16) | GPU_SID;
	wr(SMMU_BASE + SMMU_SMR(free_smr), smr_w);
	rb("G SMR", smr_w, rd(SMMU_BASE + SMMU_SMR(free_smr)));
	uint32_t s2cr_w = (S2CR_TYPE_TRANS << 16) | free_cb;
	wr(SMMU_BASE + SMMU_S2CR(free_smr), s2cr_w);
	rb("G S2CR", s2cr_w, rd(SMMU_BASE + SMMU_S2CR(free_smr)));
	wr(cb + SMMU_CB_TLBIASID, 0);
	wr(cb + SMMU_CB_TLBSYNC, 0);
	int tlb_ok = poll(cb + SMMU_CB_TLBSTATUS, 1, 0, 2000);
	con_hex2("GSMMU", tlb_ok ? "cb ok base=" : "TLB TO base=", (uint32_t)cb,
		 "mismatch=", rb_bad - bad0);
	if (!tlb_ok || rb_bad != bad0) {
		note_fail("GSMMU CB");
		return;
	}
	gpu_cb_idx = free_cb;
	gpu_ctx_ok = 1;
}

static void step12b_cp_start(const char *cmdline)
{
	uint32_t sp = rd(GMU_REG(GMU_SPTPRAC_PWR_CLK_STATUS));
	if (sp & GX_OFF_BITS) {
		con_hex2("GCP", "GX off before CP setup sptprac=", sp, "pad=", 0);
		note_fail("GCP GX off");
		return;
	}
	g_wr_n = g_wr_bad = 0;

	if (gpu_level >= 15) {
		/*
		 * Boot 19: TrustZone loaded the zap shader, but the fence after
		 * CP_SET_SECURE_MODE(0) never came. Linux a6xx_hw_init (and HackiOS
		 * step 20) clear the trusted memory range first, "we don't want to
		 * block off part of the virtual memory space": once out of secure
		 * mode the CP may not write a trusted range. Kept for the G3 report.
		 */
		tsb_old[0] = rd(GPU3D_REG(A6XX_RBBM_SECVID_TSB_TRUSTED_BASE));
		tsb_old[1] = rd(GPU3D_REG(A6XX_RBBM_SECVID_TSB_TRUSTED_BASE + 1));
		tsb_old[2] = rd(GPU3D_REG(A6XX_RBBM_SECVID_TSB_TRUSTED_SIZE));
		tsb_old[3] = rd(GPU3D_REG(A6XX_RBBM_SECVID_TSB_CNTL));
		gwr(A6XX_RBBM_SECVID_TSB_CNTL, 0);
		gwr(A6XX_RBBM_SECVID_TSB_TRUSTED_BASE, 0);
		gwr(A6XX_RBBM_SECVID_TSB_TRUSTED_BASE + 1, 0);
		gwr(A6XX_RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
		/*
		 * Boot 20: TSB was already 0; after CP_SET_SECURE_MODE(0) the CP's
		 * memory writes stopped and PC_DCALL stayed busy: the zap shader
		 * (run on the shader cores in a context TrustZone set up) does not
		 * finish. Linux a6xx_hw_init, KGSL a6xx_start and HackiOS step 20
		 * all switch every block to 64-bit addressing before the CP starts;
		 * we had left that out. Same 12 registers as HackiOS step 20
		 * (a6xx.xml *_ADDR_MODE_CNTL, A6XX), plus its GBIF halt release and
		 * RB_DBG_ECO_CNTL (Mesa's A640 value, written kernel-side there).
		 */
		static const uint16_t addr_mode[12] = {
			0x0842, /* CP */   0x0c01, /* VSC */  0x8601, /* GRAS */ 0x8e05, /* RB */
			0x9e01, /* PC */   0xbe05, /* HLSQ */ 0xa601, /* VFD */  0x9601, /* VPC */
			0x0e00, /* UCHE */ 0xae01, /* SP */   0xb601, /* TPL1 */ 0xf810, /* SECVID_TSB */
		};
		gwr(A6XX_RBBM_GBIF_HALT, 0);
		for (unsigned i = 0; i < 12; i++)
			gwr(addr_mode[i], 1);
		gwr(A6XX_RB_DBG_ECO_CNTL, 0x04100000U);
	}

	/* a6xx_start subset (constants only) */
	for (uint32_t i = 0; i < 4; i++)
		gwr(A6XX_GBIF_QSB_SIDE0 + i, 0x00071620U);
	gwr(A6XX_RBBM_GBIF_CLIENT_QOS_CNTL, 0x3);
	gwr(A6XX_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffffU);
	gwr(A6XX_UCHE_WRITE_RANGE_MAX_LO, 0xffffffc0U);
	gwr(A6XX_UCHE_WRITE_RANGE_MAX_LO + 1, 0x0001ffffU);
	gwr(A6XX_UCHE_TRAP_BASE_LO, 0xfffff000U);
	gwr(A6XX_UCHE_TRAP_BASE_LO + 1, 0x0001ffffU);
	gwr(A6XX_UCHE_WRITE_THRU_BASE_LO, 0xfffff000U);
	gwr(A6XX_UCHE_WRITE_THRU_BASE_LO + 1, 0x0001ffffU);
	gwr(A6XX_UCHE_GMEM_RANGE_MIN_LO, 0x00100000U);
	gwr(A6XX_UCHE_GMEM_RANGE_MIN_LO + 1, 0);
	gwr(A6XX_UCHE_GMEM_RANGE_MIN_LO + 2, 0x001fffffU);
	gwr(A6XX_UCHE_GMEM_RANGE_MIN_LO + 3, 0);
	gwr(A6XX_UCHE_FILTER_CNTL, 0x804);
	gwr(A6XX_UCHE_CACHE_WAYS, 0x4);
	gwr(A6XX_CP_ROQ_THRESHOLDS_2, 0x02000140U);
	gwr(A6XX_CP_ROQ_THRESHOLDS_1, 0x8040362cU);
	gwr(A6XX_CP_MEM_POOL_SIZE, 128);
	gwr(A6XX_PC_DBG_ECO_CNTL, 0x400U << 11);
	gwr(A6XX_CP_AHB_CNTL, 0x1);
	gwr(A6XX_RBBM_PERFCTR_CNTL, 0x1);
	gwr(A6XX_RB_NC_MODE_CNTL, (1U << 4) | (2U << 1));
	gwr(A6XX_TPL1_NC_MODE_CNTL, 2U << 1);
	gwr(A6XX_SP_NC_MODE_CNTL, 2U << 1);
	gwr(A6XX_UCHE_MODE_CNTL, 2U << 21);
	gwr(A6XX_RBBM_INTERFACE_HANG_INT_CNTL, (1U << 30) | 0xcfffffU);
	gwr(A6XX_UCHE_CLIENT_PF, 1);
	unsigned start_bad = g_wr_bad;

	/* a6xx_rb_start */
	gwr(A6XX_CP_RB_RPTR_ADDR_LO, GPU_VA_SCR);
	gwr(A6XX_CP_RB_RPTR_ADDR_HI, 0);
	gwr(A6XX_CP_RB_CNTL, GPU_RB_CNTL);
	gwr(A6XX_CP_RB_BASE, GPU_VA_RB);
	gwr(A6XX_CP_RB_BASE_HI, 0);
	gwr(A6XX_CP_SQE_INSTR_BASE_LO, GPU_VA_SQE);
	gwr(A6XX_CP_SQE_INSTR_BASE_HI, 0);
	con_hex2("GCP regs", "written=", g_wr_n, "mismatch=", g_wr_bad);
	/*
	 * Boot 13: 3 mismatches, the first PERFCTR_GPU_BUSY_MASKED (0x50B) reading
	 * back 0x....7FFF for 0xFFFFFFFF: not all bits exist. Informational only.
	 */
	for (unsigned k = 0; k < g_wr_bad && k < 3; k++)
		con_hex2("GCP rb", "reg=", g_bad_reg[k], "reads=", g_bad_r[k]);
	(void)start_bad;

	con_hex2("GCP pre", "hwfault=", rd(GPU3D_REG(A6XX_CP_HW_FAULT)),
		 "int0=", rd(GPU3D_REG(A6XX_RBBM_INT_0_STATUS)));
	/* Clear the SQE halt: the CP fetches its microcode through our context */
	wr(GPU3D_REG(A6XX_CP_SQE_CNTL), 1);
	udelay(10000);
	uint32_t hwf = rd(GPU3D_REG(A6XX_CP_HW_FAULT)), int0 = rd(GPU3D_REG(A6XX_RBBM_INT_0_STATUS));
	con_hex2("GCP run", "hwfault=", hwf, "int0=", int0);
	con_hex2("GCP run", "rbbm=", rd(GPU3D_REG(A6XX_RBBM_STATUS)),
		 "cpstat1=", rd(GPU3D_REG(A6XX_CP_STATUS_1)));
	con_hex2("GCP run", "rptr=", rd(GPU3D_REG(A6XX_CP_RB_RPTR)),
		 "cpint=", rd(GPU3D_REG(A6XX_CP_INTERRUPT_STATUS)));
	uint32_t st[4];
	for (uint32_t i = 0; i < 4; i++) {
		wr(GPU3D_REG(A6XX_CP_SQE_STAT_ADDR), i);
		st[i] = rd(GPU3D_REG(A6XX_CP_SQE_STAT_DATA));
	}
	con_hex2("GSQE stat", "0=", st[0], "1=", st[1]);
	con_hex2("GSQE stat", "2=", st[2], "3=", st[3]);

	/*
	 * adreno_set_unsecured_mode, no-zap branch. Boot 13 (2026-10-22): the phone
	 * switched off right here, after the SQE stat lines and before this line
	 * printed: TZ does not let EL1 touch RBBM_SECVID_TRUST_CNTL (0xF400; read or
	 * write, not known which). Now only with sweettrust=1.
	 */
	uint32_t tc1 = 0;
	if (cmdline_uint(cmdline, "sweettrust=", 0)) {
		uint32_t tc0 = rd(GPU3D_REG(A6XX_RBBM_SECVID_TRUST_CNTL));
		wr(GPU3D_REG(A6XX_RBBM_SECVID_TRUST_CNTL), 0);
		tc1 = rd(GPU3D_REG(A6XX_RBBM_SECVID_TRUST_CNTL));
		con_hex2("GSEC trust", "before=", tc0, "after=", tc1);
	}

	if (cmdline_uint(cmdline, "sweetgpucb=", 0) && gpu_cb_idx != 0xff) {
		uint32_t id1 = rd(SMMU_BASE + SMMU_IDR1);
		uint32_t num_global = 1U << (((id1 >> 28) & 7U) + 1U);
		uintptr_t cb = SMMU_BASE + 0x1000UL * (num_global + gpu_cb_idx);
		con_hex2("GSMMU fsr", "fsr=", rd(cb + SMMU_CB_FSR), "far=", rd(cb + SMMU_CB_FAR));
		con_hex2("GSMMU fsr", "fsynr0=", rd(cb + SMMU_CB_FSYNR0), "far_hi=", rd(cb + SMMU_CB_FAR + 4));
	}

	if (hwf != 0) {
		note_fail("GCP hw fault");
	} else {
		con_str("GCP OK", "CP running, no HW fault");
		cp_started = 1;
	}
	if (tc1 != 0)
		con_str("GSEC", "TRUST_CNTL did not stick (zap needed?)");
}

static void step12_gpu_cp(const char *cmdline)
{
	con_str("GPU", "step 12: GPU SMMU ctx + SQE + CP start");
	if (!gx_on) {
		con_str("GPU12", "needs step 11 with GX held: skip");
		note_fail("GPU12 no GX");
		return;
	}
	step12a_gpu_smmu();
	if (!gpu_ctx_ok)
		return;
	step12b_cp_start(cmdline);
}

/*
 * Step 13 (= planned 12c): the CP's first commands. KGSL a6xx_send_cp_init +
 * _set_ordinals and cp_mem_write (adreno_a6xx.c, adreno_ringbuffer.c,
 * adreno_pm4types.h):
 *   type-7 header = 7<<28 | cnt | parity(cnt)<<15 | (op & 0x7f)<<16 | parity(op)<<23,
 *   parity = (0x9669 >> (xor of the 8 nibbles)) & 1.
 *   CP_ME_INIT (0x48), 11 ordinals. KGSL's CP_INIT_MASK is bits 0,1,2,3,5,6,8;
 *   bit 8 (register init list with spinlock) needs KGSL's power-up register
 *   list, which we do not have, so mask 0x6F: max contexts 3, error detection
 *   0x20000000, header dump 0/0, ucode workaround 0, operation mode 2, then
 *   zero padding to 11 (_set_ordinals emits exactly these for that mask).
 *   CP_MEM_WRITE (0x3D), 3 dwords (64-bit address): lo, hi, value.
 * The ringbuffer gets 16 dwords; CP_RB_WPTR = 16; then poll the fence word and
 * the RPTR (register and the shadow the CP writes to 0x01011000) for 2 s.
 * TRUST_CNTL is NOT touched (boot 13). The GPU starts in secure mode; whether
 * its writes still reach memory without the zap shader is what this tests.
 */
#define CP_OP_ME_INIT		0x48U
#define CP_OP_MEM_WRITE		0x3dU
#define GPU_FENCE_MAGIC		0x600dcafeU
#define CP_INIT_MASK_NOLIST	0x6fU

static uint32_t pm4_parity(uint32_t v)
{
	return (0x9669U >> (0xfU & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^
				    (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1U;
}

static uint32_t cp_type7(uint32_t op, uint32_t cnt)
{
	return (7U << 28) | cnt | (pm4_parity(cnt) << 15) |
	       ((op & 0x7fU) << 16) | (pm4_parity(op) << 23);
}

static void step13_cp_fence(void)
{
	con_str("GPU", "step 13: CP_ME_INIT + MEM_WRITE fence");
	if (!cp_started) {
		con_str("G1", "needs step 12 (CP started): skip");
		note_fail("G1 no CP");
		return;
	}
	volatile uint32_t *ring = (volatile uint32_t *)gpu_va_pa(GPU_VA_RB);
	volatile uint32_t *scr = (volatile uint32_t *)gpu_va_pa(GPU_VA_SCR);
	scr[0] = 0xdeadbeefU;		/* RPTR shadow sentinel */
	scr[0x40] = 0;			/* fence at +0x100 */

	uint32_t n = 0;
	ring[n++] = cp_type7(CP_OP_ME_INIT, 11);
	ring[n++] = CP_INIT_MASK_NOLIST;
	ring[n++] = 0x00000003;		/* max contexts */
	ring[n++] = 0x20000000;		/* error detection */
	ring[n++] = 0;			/* header dump address */
	ring[n++] = 0;			/* header dump enable/size */
	ring[n++] = 0;			/* ucode workaround */
	ring[n++] = 0x00000002;		/* operation mode */
	while (n < 12)
		ring[n++] = 0;
	ring[n++] = cp_type7(CP_OP_MEM_WRITE, 3);
	ring[n++] = GPU_VA_SCR + 0x100U;
	ring[n++] = 0;
	ring[n++] = GPU_FENCE_MAGIC;
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2("G1 ring", "hdr0=", ring[0], "hdr12=", ring[12]);

	uint32_t sp = rd(GMU_REG(GMU_SPTPRAC_PWR_CLK_STATUS));
	if (sp & GX_OFF_BITS) {
		con_hex2("G1", "GX off before submit sptprac=", sp, "pad=", 0);
		note_fail("G1 GX off");
		return;
	}
	wr(GPU3D_REG(A6XX_CP_RB_WPTR), n);

	uint32_t fence = 0, hwf = 0;
	unsigned waited = 0;
	for (; waited < 200000; waited += 10) {	/* 2 s, like adreno_ringbuffer_submit_spin */
		fence = scr[0x40];
		hwf = rd(GPU3D_REG(A6XX_CP_HW_FAULT));
		if (fence == GPU_FENCE_MAGIC || hwf)
			break;
		udelay(10);
	}
	con_hex2("G1 wait", "us=", waited, "fence=", fence);
	con_hex2("G1 rptr", "reg=", rd(GPU3D_REG(A6XX_CP_RB_RPTR)), "shadow=", scr[0]);
	con_hex2("G1 st", "hwfault=", hwf, "int0=", rd(GPU3D_REG(A6XX_RBBM_INT_0_STATUS)));
	con_hex2("G1 st", "rbbm=", rd(GPU3D_REG(A6XX_RBBM_STATUS)),
		 "cpstat1=", rd(GPU3D_REG(A6XX_CP_STATUS_1)));
	con_hex2("G1 st", "cpint=", rd(GPU3D_REG(A6XX_CP_INTERRUPT_STATUS)), "wptr=", n);
	if (fence == GPU_FENCE_MAGIC) {
		con_str("G1 OK", "GPU wrote the fence: GPU ALIVE");
		g1_ok = 1;
		ring_w = n;
	} else
		note_fail(hwf ? "G1 CP fault" : "G1 no fence");
}

/*
 * Step 14: G2, the GPU draws on the screen.
 * - The CPU paints the square black first, so a green square can only come
 *   from the GPU.
 * - Indirect buffer (GPU VA 0x01100000): one CP_MEM_WRITE per row,
 *   type7(0x3D, 2 + W), address lo/hi, W colour words. 250 rows x 253 dwords.
 * - Ring (from where step 13 stopped): CP_INDIRECT_BUFFER_PFE type7(0x3F, 3)
 *   {IB lo, hi, size in dwords} (KGSL adreno_ringbuffer.c), then a CP_MEM_WRITE
 *   fence 0x6002CAFE to scratch +0x104; CP_RB_WPTR = new end.
 * - Then the CPU checks every pixel of the square before printing anything
 *   (console text could overlap it).
 * The framebuffer window (2 MiB from the 4K page holding row G2_Y0) and the
 * IB pages were mapped in step 12a, before the CP started.
 */
#define CP_OP_INDIRECT_BUFFER_PFE	0x3fU
#define G2_FENCE_MAGIC		0x6002cafeU

static void step14_g2_draw(void)
{
	con_str("GPU", "step 14: G2, GPU draws a green square");
	if (!g1_ok) {
		con_str("G2", "needs step 13 (G1): skip");
		note_fail("G2 no G1");
		return;
	}
	volatile uint32_t *fb = (volatile uint32_t *)FB_PA;
	volatile uint32_t *ib = (volatile uint32_t *)GPU_IB_PA;
	volatile uint32_t *ring = (volatile uint32_t *)gpu_va_pa(GPU_VA_RB);
	volatile uint32_t *scr = (volatile uint32_t *)gpu_va_pa(GPU_VA_SCR);
	uint32_t y, x, n = 0;

	/* 1. CPU: black square */
	for (y = G2_Y0; y < G2_Y0 + G2_H; y++)
		for (x = G2_X0; x < G2_X0 + G2_W; x++)
			fb[(y * FB_STRIDE) / 4 + x] = G2_BLACK;

	/* 2. indirect buffer: one CP_MEM_WRITE per row */
	for (y = G2_Y0; y < G2_Y0 + G2_H; y++) {
		uint32_t off = (uint32_t)((FB_PA + (uintptr_t)y * FB_STRIDE + G2_X0 * 4U) - G2_WIN_PA);
		ib[n++] = cp_type7(CP_OP_MEM_WRITE, 2 + G2_W);
		ib[n++] = GPU_VA_FBWIN + off;
		ib[n++] = 0;
		for (x = 0; x < G2_W; x++)
			ib[n++] = G2_COLOR;
	}
	scr[0x41] = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	/* 3. ring: IB call + fence */
	uint32_t w = ring_w;
	if (w + 8 >= 1024) {
		con_hex2("G2", "ring full w=", w, "pad=", 0);
		note_fail("G2 ring");
		return;
	}
	ring[w++] = cp_type7(CP_OP_INDIRECT_BUFFER_PFE, 3);
	ring[w++] = GPU_VA_IB;
	ring[w++] = 0;
	ring[w++] = n;
	ring[w++] = cp_type7(CP_OP_MEM_WRITE, 3);
	ring[w++] = GPU_VA_SCR + 0x104U;
	ring[w++] = 0;
	ring[w++] = G2_FENCE_MAGIC;
	__asm__ volatile("dsb sy" ::: "memory");
	uint32_t sp = rd(GMU_REG(GMU_SPTPRAC_PWR_CLK_STATUS));
	if (sp & GX_OFF_BITS) {
		con_hex2("G2", "GX off before submit sptprac=", sp, "pad=", 0);
		note_fail("G2 GX off");
		return;
	}
	wr(GPU3D_REG(A6XX_CP_RB_WPTR), w);
	ring_w = w;

	uint32_t fence = 0, hwf = 0;
	unsigned waited = 0;
	for (; waited < 200000; waited += 10) {
		fence = scr[0x41];
		hwf = rd(GPU3D_REG(A6XX_CP_HW_FAULT));
		if (fence == G2_FENCE_MAGIC || hwf)
			break;
		udelay(10);
	}

	/* 4. CPU: check the square before printing anything */
	uint32_t good = 0, first_bad = 0xffffffffU;
	for (y = G2_Y0; y < G2_Y0 + G2_H; y++)
		for (x = G2_X0; x < G2_X0 + G2_W; x++) {
			uint32_t v = fb[(y * FB_STRIDE) / 4 + x];
			if (v == G2_COLOR)
				good++;
			else if (first_bad == 0xffffffffU)
				first_bad = v;
		}

	con_hex2("G2 ib", "dwords=", n, "wptr=", w);
	con_hex2("G2 wait", "us=", waited, "fence=", fence);
	con_hex2("G2 rptr", "reg=", rd(GPU3D_REG(A6XX_CP_RB_RPTR)), "hwfault=", hwf);
	con_hex2("G2 pix", "green=", good, "of=", G2_W * G2_H);
	if (good != G2_W * G2_H)
		con_hex2("G2 pix", "first other=", first_bad, "int0=", rd(GPU3D_REG(A6XX_RBBM_INT_0_STATUS)));
	if (fence == G2_FENCE_MAGIC && good == G2_W * G2_H)
		con_str("G2 OK", "GPU drew the green square (bottom)");
	else
		note_fail(hwf ? "G2 CP fault" : fence != G2_FENCE_MAGIC ? "G2 no fence" : "G2 pixels");
}

/*
 * Step 15: G3, the 2D blitter (BLIT_OP_SCALE) copies and scales an image.
 *
 * Sources (the files are in the G3 handoff bundle):
 *   Mesa fd6_blitter.cc  handle_rgba_blit -> emit_setup, emit_blit_texture
 *                        (emit_blit_texture_setup, emit_blit_setup,
 *                        emit_blit_src, emit_blit_dst, emit_blit_fini), then
 *                        fd6_emit_flushes(CCU color/depth, cache, WFI)
 *   Mesa fd6_emit.h      fd6_set_render_mode (CP_SET_MARKER), fd6_cache_inv,
 *                        fd6_set_rb_dbg_eco_mode (only with QCTDD04536579,
 *                        which freedreno_devices.py sets for A630, not A640)
 *   a6xx.xml / a6xx_enums.xml / adreno_pm4.xml  register and packet layouts
 *   freedreno_devices.py A640: magic registers, num_ccu 2, 64 KiB per-CCU
 *                        sysmem depth/color cache
 *   HackiOS redwood_adreno.c step_frame: working 2D fill on an Adreno 642L
 *                        (type-4 header, CCU flush events with a value word)
 *
 * G3a: a 128x128 RGBA8 gradient (bytes x*2, y*2, 0x80, 0xff) is scaled to
 *      256x256 with linear filtering into a scratch image (pitch 1024). The
 *      CPU checks every inner pixel: byte 0 must be about x, byte 1 about y
 *      (+-2 covers the possible sample-position conventions), byte 2 0x80.
 * G3b: the same source is put on the screen at x 100, y 2100 (256x256). The
 *      framebuffer pitch (4320) is not a multiple of 64, which the 2D engine
 *      needs (RB_A2D_DEST_BUFFER_PITCH is in units of 64 bytes). Two passes:
 *      even rows with pitch 8640 (= 2 x 4320 = 135 x 64) from row 2100, odd
 *      rows with pitch 8640 from 32 bytes before row 2101 (64-byte aligned)
 *      with x shifted by 8 pixels. Each pass scales 128x128 to 256x128, so
 *      both rows of a pair show the same source row (2x vertical by
 *      repetition, linear 2x horizontally). This is how the framebuffer copy
 *      in G4 can use the 2D engine without changing the display's pitch.
 *
 * Unknowns (flagged, overridable):
 *   - RB_CCU_CNTL "bypass" value: depth cache at GMEM 0, colour cache after
 *     the 2 x 64 KiB depth cache -> COLOR_OFFSET 0x20000 -> 0x10000000. Mesa's
 *     formula is not in the files we have; HackiOS used 0x10000004 (the same
 *     plus CONCURRENT_RESOLVE) on the 642L: sweetg3ccu=1 selects that.
 *   - The A640 magic registers are written from the IB (Mesa writes them from
 *     its command stream at context start). sweetg3magic=0 skips them.
 *   - FD_LABEL -> DEBUG_LABEL (0x3f), no value word: name from the XML, the
 *     mapping is from memory of freedreno_gpu_event.h.
 *   - TS events are sent HackiOS/Linux style (TIMESTAMP bit clear, so the
 *     value word is written) to have a readable progress word.
 * sweetg3fb=0 runs only G3a (no framebuffer writes).
 */
#define CP_OP_EVENT_WRITE	0x46U
#define CP_OP_SET_MARKER	0x65U
#define CP_OP_BLIT		0x2cU
#define CP_OP_WAIT_FOR_IDLE	0x26U
#define CP_BLIT_OP_SCALE	3U
#define RM6_BLIT2DSCALE		12U

/* vgt_event_type (adreno_pm4.xml, A6XX) */
#define EV_CACHE_FLUSH_TS	0x04U
#define EV_CCU_INV_DEPTH	0x18U
#define EV_CCU_INV_COLOR	0x19U
#define EV_CCU_FLUSH_DEPTH_TS	0x1cU
#define EV_CCU_FLUSH_COLOR_TS	0x1dU
#define EV_CACHE_INVALIDATE	0x31U
#define EV_DEBUG_LABEL		0x3fU

/* registers (a6xx.xml, A6XX variants; dword offsets) */
#define R_GRAS_A2D_BLT_CNTL	0x8400U
#define R_GRAS_A2D_SRC_XMIN	0x8401U	/* XMAX, YMIN, YMAX follow; value << 8 */
#define R_GRAS_A2D_DEST_TL	0x8405U	/* BR follows; X 0-13, Y 16-29, BR inclusive */
#define R_RB_A2D_BLT_CNTL	0x8c00U
#define R_RB_A2D_PIXEL_CNTL	0x8c01U
#define R_RB_A2D_DEST_INFO	0x8c17U	/* BASE lo/hi 0x8c18/19, PITCH 0x8c1a (>> 6) */
#define R_RB_CCU_CNTL		0x8e07U
#define R_SP_A2D_OUTPUT_INFO	0xacc0U
#define R_TPL1_A2D_SRC_INFO	0xb4c0U	/* SIZE b4c1, BASE b4c2/3, PITCH b4c4 */
#define R_TPL1_A2D_SRC_FLAG	0xb4caU	/* FLAG_BASE b4ca/b, FLAG_PITCH b4cc */
/* read-only status, for the failure report (a6xx.xml) */
#define A6XX_CP_PROTECT_STATUS	0x0824U
#define A6XX_CP_IB1_BASE	0x0928U
#define A6XX_CP_IB1_REM_SIZE	0x092aU
#define A6XX_RBBM_STATUS3	0x0213U

/* field values */
#define FMT6_8_8_8_8_UNORM	0x30U	/* a6xx_enums.xml */
/* a6xx_a2d_blt_cntl: ROTATE_0, COLOR_FORMAT 8-15, MASK 0xf at 20-23, IFMT R2D_UNORM8 (0) */
#define G3_BLT_CNTL		((FMT6_8_8_8_8_UNORM << 8) | (0xfU << 20))
/* a6xx_sp_a2d_output_info: IFMT_TYPE FLOAT (0), COLOR_FORMAT 3-10, MASK 0xf at 12-15 */
#define G3_SP_OUT		((FMT6_8_8_8_8_UNORM << 3) | (0xfU << 12))
/* a6xx_a2d_src_texture_info: format, TILE6_LINEAR, WZYX, FILTER (16), UNK20, UNK22 (as Mesa) */
#define G3_TEX_INFO		(FMT6_8_8_8_8_UNORM | (1U << 16) | (1U << 20) | (1U << 22))
/* a6xx_a2d_dest_buffer_info: format, TILE6_LINEAR, WZYX */
#define G3_DST_INFO		FMT6_8_8_8_8_UNORM
#define G3_CCU_BYPASS		0x10000000U	/* COLOR_OFFSET 0x20000 (bits 23-31, >> 12) */
#define G3_CCU_HACKIOS		0x10000004U	/* + CONCURRENT_RESOLVE */

/* scratch page words */
#define G3_TS_COLOR		0x120U
#define G3_TS_DEPTH		0x128U
#define G3_TS_CACHE		0x130U
#define G3_MARK_A		0x140U
#define G3_MARK_B		0x148U
#define G3_PROG			0x150U	/* progress word: 0x6003xxxx after every packet */
#define G3_MAGIC_A		0x6003a0a0U
#define G3_MAGIC_B		0x6003b0b0U

/* A640 raw_magic_regs, freedreno_devices.py (offsets from a6xx.xml) */
static const uint32_t g3_magic[12][2] = {
	{ 0x9804U, 0x0000001fU },	/* PC_MODE_CNTL */
	{ 0x9805U, 0x00000001U },	/* PC_POWER_CNTL */
	{ 0xa0f8U, 0x00000001U },	/* VFD_POWER_CNTL */
	{ 0xb600U, 0x00008000U },	/* TPL1_DBG_ECO_CNTL */
	{ 0x8600U, 0x00000000U },	/* GRAS_DBG_ECO_CNTL */
	{ 0xae03U, 0x00000420U },	/* SP_CHICKEN_BITS */
	{ 0xae00U, 0x00000000U },	/* SP_DBG_ECO_CNTL */
	{ 0xbe04U, 0x00000000U },	/* HLSQ_DBG_ECO_CNTL */
	{ 0x9600U, 0x02000000U },	/* VPC_DBG_ECO_CNTL */
	{ 0x0e12U, 0x00000001U },	/* UCHE_UNKNOWN_0E12 */
	{ 0x8e01U, 0x00000001U },	/* RB_RBP_CNTL */
	{ 0x8e04U, 0x04100000U },	/* RB_DBG_ECO_CNTL */
};

/* type 4 = register write (KGSL cp_type4_packet; HackiOS ring_register) */
static uint32_t cp_type4(uint32_t reg, uint32_t cnt)
{
	return (4U << 28) | cnt | (pm4_parity(cnt) << 7) |
	       ((reg & 0x3ffffU) << 8) | (pm4_parity(reg) << 27);
}

struct g3_ib {
	volatile uint32_t *p;
	uint32_t n, max;
};

static void ib_put(struct g3_ib *ib, uint32_t v)
{
	if (ib->n < ib->max)
		ib->p[ib->n] = v;
	ib->n++;	/* keeps counting: n > max reports the overflow */
}

static void ib_regs(struct g3_ib *ib, uint32_t reg, uint32_t cnt,
		    uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3, uint32_t v4)
{
	ib_put(ib, cp_type4(reg, cnt));
	ib_put(ib, v0);
	if (cnt > 1)
		ib_put(ib, v1);
	if (cnt > 2)
		ib_put(ib, v2);
	if (cnt > 3)
		ib_put(ib, v3);
	if (cnt > 4)
		ib_put(ib, v4);
}

static void ib_reg(struct g3_ib *ib, uint32_t reg, uint32_t v)
{
	ib_regs(ib, reg, 1, v, 0, 0, 0, 0);
}

static void ib_event(struct g3_ib *ib, uint32_t ev)
{
	ib_put(ib, cp_type7(CP_OP_EVENT_WRITE, 1));
	ib_put(ib, ev);
}

/* event that writes a word when it is done: event, address lo/hi, value */
static void ib_event_ts(struct g3_ib *ib, uint32_t ev, uint32_t scr_off, uint32_t val)
{
	ib_put(ib, cp_type7(CP_OP_EVENT_WRITE, 4));
	ib_put(ib, ev);
	ib_put(ib, GPU_VA_SCR + scr_off);
	ib_put(ib, 0);
	ib_put(ib, val);
}

static void ib_wfi(struct g3_ib *ib)
{
	ib_put(ib, cp_type7(CP_OP_WAIT_FOR_IDLE, 0));
}

static void ib_mem_write(struct g3_ib *ib, uint32_t scr_off, uint32_t val)
{
	ib_put(ib, cp_type7(CP_OP_MEM_WRITE, 3));
	ib_put(ib, GPU_VA_SCR + scr_off);
	ib_put(ib, 0);
	ib_put(ib, val);
}

/*
 * Progress word: a CP_MEM_WRITE after every packet. The CP runs the IB in
 * order, so after a hang the last value names the packet that did not finish:
 *   0x60030001        IB started
 *   0x600300MM        magic register MM-0x10 (0x10..0x1b) written
 *   0x6003TTkk        blit with tag TT (A0 = G3a, B0/C0 = G3b even/odd),
 *                     after step kk (see ib_blit)
 */
static void ib_prog(struct g3_ib *ib, uint32_t code)
{
	ib_mem_write(ib, G3_PROG, 0x60030000U | code);
}

struct g3_blit {
	uint32_t src_va, src_w, src_h, src_pitch;
	uint32_t sx0, sy0, sx1, sy1;		/* source rectangle, x1/y1 exclusive */
	uint32_t dst_va, dst_pitch;
	uint32_t dx0, dy0, dx1, dy1;		/* destination rectangle, exclusive */
};

/* one blit, as Mesa's handle_rgba_blit emits it for a linear RGBA8 texture */
static void ib_blit(struct g3_ib *ib, const struct g3_blit *b, uint32_t ccu, uint32_t tag)
{
	uint32_t t = (tag >> 16) & 0xff00U;	/* progress codes 0xTT01.. */

	/* emit_setup: flush + invalidate the CCU, 2D render mode, CCU in bypass layout */
	ib_event_ts(ib, EV_CCU_FLUSH_COLOR_TS, G3_TS_COLOR, tag | 1U);
	ib_prog(ib, t | 0x01U);
	ib_event_ts(ib, EV_CCU_FLUSH_DEPTH_TS, G3_TS_DEPTH, tag | 2U);
	ib_prog(ib, t | 0x02U);
	ib_event(ib, EV_CCU_INV_COLOR);
	ib_prog(ib, t | 0x03U);
	ib_event(ib, EV_CCU_INV_DEPTH);
	ib_prog(ib, t | 0x04U);
	/* fd6_cache_inv's last event: the texture cache may not hold the old source */
	ib_event(ib, EV_CACHE_INVALIDATE);
	ib_prog(ib, t | 0x05U);
	ib_wfi(ib);
	ib_prog(ib, t | 0x06U);
	ib_put(ib, cp_type7(CP_OP_SET_MARKER, 1));
	ib_put(ib, RM6_BLIT2DSCALE);
	ib_prog(ib, t | 0x07U);
	ib_reg(ib, R_RB_CCU_CNTL, ccu);
	ib_prog(ib, t | 0x08U);
	/* emit_blit_texture_setup */
	ib_regs(ib, R_GRAS_A2D_SRC_XMIN, 4, b->sx0 << 8, (b->sx1 - 1U) << 8,
		b->sy0 << 8, (b->sy1 - 1U) << 8, 0);
	ib_regs(ib, R_GRAS_A2D_DEST_TL, 2, b->dx0 | (b->dy0 << 16),
		(b->dx1 - 1U) | ((b->dy1 - 1U) << 16), 0, 0, 0);
	ib_prog(ib, t | 0x09U);
	/* emit_blit_setup */
	ib_reg(ib, R_RB_A2D_BLT_CNTL, G3_BLT_CNTL);
	ib_reg(ib, R_GRAS_A2D_BLT_CNTL, G3_BLT_CNTL);
	ib_reg(ib, R_SP_A2D_OUTPUT_INFO, G3_SP_OUT);
	ib_reg(ib, R_RB_A2D_PIXEL_CNTL, 0);
	ib_prog(ib, t | 0x0aU);
	/* emit_blit_src */
	ib_regs(ib, R_TPL1_A2D_SRC_INFO, 5, G3_TEX_INFO, b->src_w | (b->src_h << 15),
		b->src_va, 0, (b->src_pitch >> 6) << 9);
	ib_regs(ib, R_TPL1_A2D_SRC_FLAG, 3, 0, 0, 0, 0, 0);
	ib_prog(ib, t | 0x0bU);
	/* emit_blit_dst */
	ib_regs(ib, R_RB_A2D_DEST_INFO, 4, G3_DST_INFO, b->dst_va, 0, b->dst_pitch >> 6, 0);
	ib_prog(ib, t | 0x0cU);
	/* emit_blit_fini */
	ib_event(ib, EV_DEBUG_LABEL);
	ib_prog(ib, t | 0x0dU);
	ib_put(ib, cp_type7(CP_OP_BLIT, 1));
	ib_put(ib, CP_BLIT_OP_SCALE);
	ib_prog(ib, t | 0x0eU);
	/* fd6_emit_flushes(FLUSH_CCU_COLOR | FLUSH_CCU_DEPTH | FLUSH_CACHE | WAIT_FOR_IDLE) */
	ib_event_ts(ib, EV_CCU_FLUSH_COLOR_TS, G3_TS_COLOR, tag | 3U);
	ib_prog(ib, t | 0x0fU);
	ib_event_ts(ib, EV_CCU_FLUSH_DEPTH_TS, G3_TS_DEPTH, tag | 4U);
	ib_prog(ib, t | 0x10U);
	ib_event_ts(ib, EV_CACHE_FLUSH_TS, G3_TS_CACHE, tag | 5U);
	ib_prog(ib, t | 0x11U);
	ib_wfi(ib);
	ib_prog(ib, t | 0x12U);
}

/* Call an IB from the ring, then wait for its last word. 1 if it came. */
static int g3_run(uint32_t ib_off, uint32_t ndw, uint32_t mark_off, uint32_t magic,
		  unsigned *waited_us, uint32_t *hwf_out)
{
	volatile uint32_t *ring = (volatile uint32_t *)gpu_va_pa(GPU_VA_RB);
	volatile uint32_t *scr = (volatile uint32_t *)gpu_va_pa(GPU_VA_SCR);
	uint32_t w = ring_w;

	*waited_us = 0;
	*hwf_out = 0;
	if (w + 8 >= 1024) {
		con_hex2("G3", "ring full w=", w, "pad=", 0);
		return 0;
	}
	scr[mark_off / 4] = 0;
	scr[G3_PROG / 4] = 0;
	ring[w++] = cp_type7(CP_OP_INDIRECT_BUFFER_PFE, 3);
	ring[w++] = GPU_VA_IB + ib_off;
	ring[w++] = 0;
	ring[w++] = ndw;
	__asm__ volatile("dsb sy" ::: "memory");
	uint32_t sp = rd(GMU_REG(GMU_SPTPRAC_PWR_CLK_STATUS));
	if (sp & GX_OFF_BITS) {
		con_hex2("G3", "GX off before submit sptprac=", sp, "pad=", 0);
		return 0;
	}
	wr(GPU3D_REG(A6XX_CP_RB_WPTR), w);
	ring_w = w;

	uint32_t mark = 0, hwf = 0;
	unsigned waited = 0;
	for (; waited < 200000; waited += 10) {
		mark = scr[mark_off / 4];
		hwf = rd(GPU3D_REG(A6XX_CP_HW_FAULT));
		if (mark == magic || hwf)
			break;
		udelay(10);
	}
	*waited_us = waited;
	*hwf_out = hwf;
	return mark == magic;
}

static uint32_t g3_diff(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

/* bytes 0..2 against the expected gradient; alpha is not part of the test */
static int g3_pix_ok(uint32_t v, uint32_t e0, uint32_t e1)
{
	return g3_diff(v & 0xffU, e0) <= 2U && g3_diff((v >> 8) & 0xffU, e1) <= 2U &&
	       g3_diff((v >> 16) & 0xffU, 0x80U) <= 1U;
}

static void g3_status(const char *label, uint32_t hwf, int ok)
{
	volatile uint32_t *scr = (volatile uint32_t *)gpu_va_pa(GPU_VA_SCR);
	con_hex2(label, "rptr=", rd(GPU3D_REG(A6XX_CP_RB_RPTR)), "hwfault=", hwf);
	con_hex2(label, "ts color=", scr[G3_TS_COLOR / 4], "cache=", scr[G3_TS_CACHE / 4]);
	if (ok)
		return;
	/* where the CP stopped and which blocks are busy (all read-only) */
	con_hex2(label, "prog=", scr[G3_PROG / 4], "ib1 rem=", rd(GPU3D_REG(A6XX_CP_IB1_REM_SIZE)));
	con_hex2(label, "ib1 base=", rd(GPU3D_REG(A6XX_CP_IB1_BASE)), "depth ts=", scr[G3_TS_DEPTH / 4]);
	con_hex2(label, "rbbm=", rd(GPU3D_REG(A6XX_RBBM_STATUS)), "rbbm3=", rd(GPU3D_REG(A6XX_RBBM_STATUS3)));
	con_hex2(label, "int0=", rd(GPU3D_REG(A6XX_RBBM_INT_0_STATUS)), "cpint=", rd(GPU3D_REG(A6XX_CP_INTERRUPT_STATUS)));
	con_hex2(label, "protect=", rd(GPU3D_REG(A6XX_CP_PROTECT_STATUS)), "cpstat1=", rd(GPU3D_REG(A6XX_CP_STATUS_1)));
}

/*
 * sweetg3magic=2: write the A640 magic registers one per IB call and read
 * CP_INTERRUPT_STATUS after each, to find the one that raises CP_AHB_ERROR
 * (bit 5). Stops at the first error or hang. No blit.
 */
static void g3_magic_probe(void)
{
	volatile uint32_t *scr = (volatile uint32_t *)gpu_va_pa(GPU_VA_SCR);
	uint32_t before = rd(GPU3D_REG(A6XX_CP_INTERRUPT_STATUS));
	for (unsigned i = 0; i < 12; i++) {
		struct g3_ib ib = { (volatile uint32_t *)(GPU_IB_PA + G3_IB_OFF_A + i * 0x40U), 0, 0x40U / 4 };
		ib_reg(&ib, g3_magic[i][0], g3_magic[i][1]);
		ib_prog(&ib, 0x10U + i);
		__asm__ volatile("dsb sy" ::: "memory");
		unsigned waited;
		uint32_t hwf;
		int got = g3_run(G3_IB_OFF_A + i * 0x40U, ib.n, G3_PROG, 0x60030010U + i, &waited, &hwf);
		udelay(1000);	/* let a posted write's error arrive */
		uint32_t now = rd(GPU3D_REG(A6XX_CP_INTERRUPT_STATUS));
		if (!got || hwf || now != before) {
			con_hex2("G3m stop reg=", "", g3_magic[i][0], "val=", g3_magic[i][1]);
			con_hex2("G3m", "done=", got, "cpint=", now);
			con_hex2("G3m", "rbbm=", rd(GPU3D_REG(A6XX_RBBM_STATUS)), "prog=", scr[G3_PROG / 4]);
			note_fail(got ? "G3m AHB error" : "G3m hang");
			return;
		}
	}
	con_hex2("G3m", "all 12 written, cpint=", before, "rbbm=", rd(GPU3D_REG(A6XX_RBBM_STATUS)));
}

/*
 * Out of secure mode (before the G3 blits). Boots 17/18: in secure mode the
 * first CCU flush event raised CP_AHB_ERROR and the CP stopped (G1/G2's plain
 * CP_MEM_WRITEs worked). Writing RBBM_SECVID_TRUST_CNTL directly resets the
 * phone (boot 13), so: the zap shader, as Linux (a6xx_hw_init ->
 * a6xx_zap_shader_init, after the CP runs) and HackiOS (steps 12, 19, 21) do:
 *   TrustZone PAS id 13 (live DT qcom,kgsl-hyp qcom,pas-id = <0xd>):
 *   1. PAS_INIT_IMAGE(13, PA of a640_zap.mdt)  -- the header + hashes
 *   2. PAS_MEM_SETUP(13, 0x98b15000, 0x2000)   -- live DT pil_gpu_region
 *   3. copy a640_zap.b02 (the one PT_LOAD, 0x7b0 bytes, relocatable) to the
 *      start of that region, zero the rest
 *   4. PAS_AUTH_AND_RESET(13)
 *   then CP_SET_SECURE_MODE(0) on the ring (KGSL/Linux put it in the ring).
 * SMC convention (HackiOS scm(), Linux qcom_scm-smc.c): x0 = 64-bit SIP
 * yielding call id, x1 = argument description, x2.. arguments, x6 session;
 * x0 == 1 (interrupted) -> call again with x0 = 1 and the returned x6.
 * After step 4 the region belongs to TrustZone: never touch it again.
 */
#define SCM_64			(1ULL << 30)
#define SCM_SIP			(2ULL << 24)
#define SCM_PAS(cmd)		(SCM_64 | SCM_SIP | (2ULL << 8) | (cmd))
#define SCM_PAS_INIT_IMAGE	SCM_PAS(1)
#define SCM_PAS_MEM_SETUP	SCM_PAS(2)
#define SCM_PAS_AUTH_AND_RESET	SCM_PAS(5)
#define SCM_ARGS_VAL		1ULL			/* one plain value */
#define SCM_ARGS_VAL_BUF	(2ULL | (2ULL << 6))	/* a value, then a buffer TZ reads/writes */
#define SCM_ARGS_3VAL		3ULL
#define SCM_INTERRUPTED		1ULL
#define PAS_GPU			13U
#define ZAP_PA			0x98b15000UL	/* live DT reserved-memory pil_gpu_region */
#define ZAP_SIZE		0x2000U
#define ZAP_MDT_PA		(SCRATCH_PA + 0x380000UL)	/* 2 pages, for TrustZone to read */
#define CP_OP_SET_SECURE_MODE	0x66U
#define G3_ZAP_FENCE		0x158U
#define G3_MAGIC_Z		0x6003eeeeU

static int32_t scm_call(uint64_t fn, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c,
			uint64_t *answer)
{
	uint64_t session = 0;
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? fn : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arginfo;
		register uint64_t x2 __asm__("x2") = a;
		register uint64_t x3 __asm__("x3") = b;
		register uint64_t x4 __asm__("x4") = c;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;
		__asm__ volatile("smc #0"
				 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
				 :
				 : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18",
				   "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			*answer = x1;
			return (int32_t)x0;
		}
	}
	*answer = ~0ULL;
	return -1;
}

/* 1 when the CP has left secure mode */
static int g3_zap(void)
{
	/* volatile locals: keep the firmware addresses out of 64-bit literals (no ABS64) */
	const uint32_t *volatile mdt_v = sweet_zapmdt_fw;
	const uint32_t *volatile seg_v = sweet_zapseg_fw;
	const char *volatile mdt_end_v = sweet_zapmdt_fw_end;
	const char *volatile seg_end_v = sweet_zapseg_fw_end;
	const uint32_t *mdt = mdt_v, *seg = seg_v;
	uint32_t mlen = (uint32_t)(mdt_end_v - (const char *)mdt);
	uint32_t slen = (uint32_t)(seg_end_v - (const char *)seg);

	/* the header as TrustZone takes it: 32-bit ELF, table at 52, then only the hashes */
	uint32_t nph = mlen >= 52 ? mdt[44 / 4] & 0xffffU : 0;
	if (mlen < 52 + 2 * 32 || mlen > 0x2000U || mdt[0] != 0x464c457fU || (mdt[4 / 4] & 0xffU) != 1U ||
	    nph < 2 || nph > 8 || 52 + 32 * nph > mlen ||
	    52 + 32 * nph + mdt[(52 + 32 + 16) / 4] != mlen) {
		con_hex2("G3z", "bad a640_zap.mdt len=", mlen, "phnum=", nph);
		return 0;
	}
	uint32_t load_sz = 0, nload = 0;
	for (uint32_t i = 0; i < nph; i++)
		if (mdt[(52 + 32 * i) / 4] == 1U) {	/* PT_LOAD */
			load_sz = mdt[(52 + 32 * i + 16) / 4];
			nload++;
		}
	if (nload != 1 || load_sz != slen || slen == 0 || slen > ZAP_SIZE || (slen & 3U)) {
		con_hex2("G3z", "bad a640_zap.b02 len=", slen, "phdr says=", load_sz);
		return 0;
	}

	volatile uint32_t *buf = (volatile uint32_t *)ZAP_MDT_PA;
	for (uint32_t i = 0; i < 0x2000U / 4; i++)
		buf[i] = i < (mlen + 3) / 4 ? mdt[i] : 0;
	__asm__ volatile("dsb sy" ::: "memory");

	uint64_t a_init = 0, a_mem = 0, a_auth = 0;
	int32_t r_init = scm_call(SCM_PAS_INIT_IMAGE, SCM_ARGS_VAL_BUF, PAS_GPU, ZAP_MDT_PA, 0, &a_init);
	int32_t r_mem = -1, r_auth = -1;
	if (r_init == 0 && a_init == 0)
		r_mem = scm_call(SCM_PAS_MEM_SETUP, SCM_ARGS_3VAL, PAS_GPU, ZAP_PA, ZAP_SIZE, &a_mem);
	if (r_mem == 0 && a_mem == 0) {
		volatile uint32_t *zap = (volatile uint32_t *)ZAP_PA;
		for (uint32_t i = 0; i < ZAP_SIZE / 4; i++)
			zap[i] = i < slen / 4 ? seg[i] : 0;
		__asm__ volatile("dsb sy" ::: "memory");
		r_auth = scm_call(SCM_PAS_AUTH_AND_RESET, SCM_ARGS_VAL, PAS_GPU, 0, 0, &a_auth);
	}
	con_hex2("G3z tz", "init=", (uint32_t)r_init | (uint32_t)a_init, "mem=", (uint32_t)r_mem | (uint32_t)a_mem);
	con_hex2("G3z tz", "auth=", (uint32_t)r_auth, "ans=", (uint32_t)a_auth);
	if (r_auth != 0 || a_auth != 0)
		return 0;

	/* trusted range as found before step 12 cleared it (0xFFFFFFFF = not read) */
	con_hex2("G3z tsb", "base=", tsb_old[0], "size=", tsb_old[2]);
	con_hex2("G3z tsb", "cntl=", tsb_old[3], "base hi=", tsb_old[1]);

	/* CP_SET_SECURE_MODE(0) + a fence, straight in the ring */
	volatile uint32_t *ring = (volatile uint32_t *)gpu_va_pa(GPU_VA_RB);
	volatile uint32_t *scr = (volatile uint32_t *)gpu_va_pa(GPU_VA_SCR);
	uint32_t w = ring_w;
	if (w + 8 >= 1024)
		return 0;
	scr[G3_ZAP_FENCE / 4] = 0;
	ring[w++] = cp_type7(CP_OP_SET_SECURE_MODE, 1);
	ring[w++] = 0;
	ring[w++] = cp_type7(CP_OP_MEM_WRITE, 3);
	ring[w++] = GPU_VA_SCR + G3_ZAP_FENCE;
	ring[w++] = 0;
	ring[w++] = G3_MAGIC_Z;
	__asm__ volatile("dsb sy" ::: "memory");
	wr(GPU3D_REG(A6XX_CP_RB_WPTR), w);
	ring_w = w;
	uint32_t f = 0, hwf = 0;
	unsigned waited = 0;
	for (; waited < 200000; waited += 10) {
		f = scr[G3_ZAP_FENCE / 4];
		hwf = rd(GPU3D_REG(A6XX_CP_HW_FAULT));
		if (f == G3_MAGIC_Z || hwf)
			break;
		udelay(10);
	}
	int ok = f == G3_MAGIC_Z && !hwf;
	con_hex2("G3z", ok ? "secure off us=" : "NO FENCE us=", waited, "hwf=", hwf);
	if (!ok) {
		con_hex2("G3z", "rptr=", rd(GPU3D_REG(A6XX_CP_RB_RPTR)), "wptr=", w);
		con_hex2("G3z", "rbbm=", rd(GPU3D_REG(A6XX_RBBM_STATUS)), "rbbm3=", rd(GPU3D_REG(A6XX_RBBM_STATUS3)));
		con_hex2("G3z", "int0=", rd(GPU3D_REG(A6XX_RBBM_INT_0_STATUS)), "cpint=", rd(GPU3D_REG(A6XX_CP_INTERRUPT_STATUS)));
		con_hex2("G3z", "protect=", rd(GPU3D_REG(A6XX_CP_PROTECT_STATUS)), "cpstat1=", rd(GPU3D_REG(A6XX_CP_STATUS_1)));
		con_hex2("G3z", "fence=", f, "shadow rptr=", scr[0]);
	}
	return ok;
}

static void step15_g3_blit(const char *cmdline)
{
	con_str("GPU", "step 15: G3, 2D blitter scale 128->256");
	if (!g1_ok) {
		con_str("G3", "needs step 13 (G1): skip");
		note_fail("G3 no G1");
		return;
	}
	uint32_t ccu = cmdline_uint(cmdline, "sweetg3ccu=", 0) ? G3_CCU_HACKIOS : G3_CCU_BYPASS;
	/* boot 17: with the magic registers the first CCU flush event stuck in PC
	 * (RBBM_STATUS PC_DCALL_BUSY) and CP_AHB_ERROR was set: default off now.
	 * sweetg3magic=1 writes them as before; =2 probes them one by one. */
	unsigned magic = cmdline_uint(cmdline, "sweetg3magic=", 0);
	unsigned do_fb = cmdline_uint(cmdline, "sweetg3fb=", 1);
	volatile uint32_t *src = (volatile uint32_t *)G3_SRC_PA;
	volatile uint32_t *dst = (volatile uint32_t *)G3_DST_PA;
	volatile uint32_t *fb = (volatile uint32_t *)FB_PA;
	uint32_t x, y;

	/* 1. CPU: source gradient, destination filled with a value no blit makes */
	for (y = 0; y < G3_SRC_H; y++)
		for (x = 0; x < G3_SRC_W; x++)
			src[y * (G3_SRC_PITCH / 4) + x] = 0xff800000U | ((y * 2U) << 8) | (x * 2U);
	for (y = 0; y < G3_DST_H; y++)
		for (x = 0; x < G3_DST_W; x++)
			dst[y * (G3_DST_PITCH / 4) + x] = 0x5a5a5a5aU;

	/* baseline: is an AHB error already latched before G3 sends anything? */
	con_hex2("G3 pre", "cpint=", rd(GPU3D_REG(A6XX_CP_INTERRUPT_STATUS)),
		 "rbbm=", rd(GPU3D_REG(A6XX_RBBM_STATUS)));
	if (cmdline_uint(cmdline, "sweetg3zap=", 1) && !g3_zap()) {
		note_fail("G3 zap / secure mode");
		return;
	}
	if (magic == 2) {
		g3_magic_probe();
		return;
	}

	/* 2. G3a: scratch-to-scratch scale */
	struct g3_ib ib = { (volatile uint32_t *)(GPU_IB_PA + G3_IB_OFF_A), 0, 0x1000U / 4 };
	ib_prog(&ib, 0x01U);
	if (magic == 1)
		for (unsigned i = 0; i < 12; i++) {
			ib_reg(&ib, g3_magic[i][0], g3_magic[i][1]);
			ib_prog(&ib, 0x10U + i);
		}
	struct g3_blit b;
	b.src_va = GPU_VA_G3SRC;
	b.src_w = G3_SRC_W;
	b.src_h = G3_SRC_H;
	b.src_pitch = G3_SRC_PITCH;
	b.sx0 = 0;
	b.sy0 = 0;
	b.sx1 = G3_SRC_W;
	b.sy1 = G3_SRC_H;
	b.dst_va = GPU_VA_G3DST;
	b.dst_pitch = G3_DST_PITCH;
	b.dx0 = 0;
	b.dy0 = 0;
	b.dx1 = G3_DST_W;
	b.dy1 = G3_DST_H;
	ib_blit(&ib, &b, ccu, 0xa0000000U);
	ib_mem_write(&ib, G3_MARK_A, G3_MAGIC_A);
	if (ib.n > ib.max) {
		con_hex2("G3a", "IB overflow n=", ib.n, "max=", ib.max);
		note_fail("G3a IB");
		return;
	}
	con_hex2("G3a ib", "dwords=", ib.n, "ccu=", ccu);
	__asm__ volatile("dsb sy" ::: "memory");

	unsigned waited;
	uint32_t hwf;
	int got = g3_run(G3_IB_OFF_A, ib.n, G3_MARK_A, G3_MAGIC_A, &waited, &hwf);

	/* CPU check of the inner pixels (edges depend on the clamp convention) */
	uint32_t good = 0, total = 0, untouched = 0, first_bad = 0, first_at = 0;
	for (y = 2; y < G3_DST_H - 2; y++)
		for (x = 2; x < G3_DST_W - 2; x++) {
			uint32_t v = dst[y * (G3_DST_PITCH / 4) + x];
			total++;
			if (g3_pix_ok(v, x, y))
				good++;
			else {
				if (v == 0x5a5a5a5aU)
					untouched++;
				if (first_at == 0) {
					first_bad = v;
					first_at = (y << 16) | x;
				}
			}
		}
	con_hex2("G3a wait", "us=", waited, "mark=", got ? G3_MAGIC_A : 0);
	g3_status("G3a", hwf, got);
	con_hex2("G3a pix", "ok=", good, "of=", total);
	if (got && good != total) {
		con_hex2("G3a bad", "first=", first_bad, "at y<<16|x=", first_at);
		con_hex2("G3a bad", "untouched=", untouched, "pad=", 0);
	}
	if (!got || good != total) {
		note_fail(hwf ? "G3a CP fault" : !got ? "G3a no mark" : "G3a pixels");
		return;
	}
	con_hex2("G3a OK", "2D engine scaled in us=", waited, "pad=", 0);
	if (!do_fb)
		return;

	/* 3. G3b: onto the framebuffer, even and odd rows (see above) */
	uint32_t base_e = G3_Y0 * FB_STRIDE;			/* byte offset in the FB */
	uint32_t base_o = (G3_Y0 + 1U) * FB_STRIDE - 32U;
	uint32_t end = (G3_Y0 + G3_DST_H) * FB_STRIDE;
	if ((G3_Y0 & 1U) || (base_e & 63U) || (base_o & 63U) || ((2U * FB_STRIDE) & 63U) ||
	    FB_PA + base_e < G2_WIN_PA ||
	    FB_PA + end > G2_WIN_PA + (uintptr_t)GPU_FBWIN_PAGES * 0x1000U) {
		con_hex2("G3b", "bad geometry e=", base_e, "o=", base_o);
		note_fail("G3b geometry");
		return;
	}
	for (y = G3_Y0; y < G3_Y0 + G3_DST_H; y++)
		for (x = G3_X0; x < G3_X0 + G3_DST_W; x++)
			fb[(y * FB_STRIDE) / 4 + x] = G2_BLACK;

	ib.p = (volatile uint32_t *)(GPU_IB_PA + G3_IB_OFF_B);
	ib.n = 0;
	ib_prog(&ib, 0x01U);
	b.dst_pitch = 2U * FB_STRIDE;
	b.dy0 = 0;
	b.dy1 = G3_DST_H / 2U;
	/* even rows */
	b.dst_va = GPU_VA_FBWIN + (uint32_t)(FB_PA + base_e - G2_WIN_PA);
	b.dx0 = G3_X0;
	b.dx1 = G3_X0 + G3_DST_W;
	ib_blit(&ib, &b, ccu, 0xb0000000U);
	/* odd rows: base 8 pixels early, x 8 pixels on */
	b.dst_va = GPU_VA_FBWIN + (uint32_t)(FB_PA + base_o - G2_WIN_PA);
	b.dx0 = G3_X0 + 8U;
	b.dx1 = G3_X0 + 8U + G3_DST_W;
	ib_blit(&ib, &b, ccu, 0xc0000000U);
	ib_mem_write(&ib, G3_MARK_B, G3_MAGIC_B);
	if (ib.n > ib.max) {
		con_hex2("G3b", "IB overflow n=", ib.n, "max=", ib.max);
		note_fail("G3b IB");
		return;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	got = g3_run(G3_IB_OFF_B, ib.n, G3_MARK_B, G3_MAGIC_B, &waited, &hwf);

	/* CPU check, rows by parity, before printing (text could overlap) */
	uint32_t good_e = 0, good_o = 0;
	first_at = 0;
	for (y = 0; y < G3_DST_H; y++)
		for (x = 2; x < G3_DST_W - 2; x++) {
			uint32_t v = fb[((G3_Y0 + y) * FB_STRIDE) / 4 + G3_X0 + x];
			if (g3_pix_ok(v, x, (y >> 1) * 2U)) {
				if (y & 1U)
					good_o++;
				else
					good_e++;
			} else if (first_at == 0) {
				first_bad = v;
				first_at = (y << 16) | x;
			}
		}
	uint32_t half = (G3_DST_H / 2U) * (G3_DST_W - 4U);
	con_hex2("G3b wait", "us=", waited, "mark=", got ? G3_MAGIC_B : 0);
	g3_status("G3b", hwf, got);
	con_hex2("G3b pix", "even ok=", good_e, "odd ok=", good_o);
	if (good_e != half || good_o != half)
		con_hex2("G3b bad", "first=", first_bad, "at y<<16|x=", first_at);
	if (got && good_e == half && good_o == half)
		con_str("G3 OK", "GPU scaled the gradient onto the screen (left of G2)");
	else
		note_fail(hwf ? "G3b CP fault" : !got ? "G3b no mark" : "G3b pixels");
}


/*
 * ---- Clocks: CPU boost (sweetcpufreq=1) and a real GPU clock readout ----
 * CPU: OSM, the hardware CPU DVFS block (OnePlus clk-cpu-osm.c, offsets verified there): one block per
 * cluster, frequency table FREQ_REG 0x110 + i*32 (40 entries; src = bits 31:30, 0 -> 300 MHz, else
 * 19.2 MHz * lval bits 7:0; two equal entries end the table), DCVS_PERF_STATE_DESIRED_REG(n) 0x920 + 4n.
 * Writing the highest index there is what Linux's cpufreq does for the "performance" governor; OSM itself
 * sets the voltage (CPR) and LMH still caps for thermals. Done before xnu, from CPU0 (MMU off).
 * GPU: the GMU programs GPU PLL0 (GPUCC +0x0, a Trion PLL; clk-alpha-pll.c: L at +0x04, alpha at +0x40,
 * 16-bit) for the voted clock: VCO = 19.2 MHz * (L + alpha / 65536). The core clock is the VCO or an
 * integer fraction of it (post-divider), so compare boots: it should scale with sweetgpufreq.
 */
#define OSM_FREQ_REG(i)		(0x110U + (uint32_t)(i) * 32U)
#define OSM_DESIRED(n)		(0x920U + 4U * (uint32_t)(n))

static uint32_t osm_khz(uint32_t w)
{
	uint32_t src = (w >> 30) & 3U, lval = w & 0xffU;
	return src ? 19200U * lval : 300000U;
}

/* highest-frequency index of one cluster's table; *khz gets that frequency */
static unsigned osm_top(uintptr_t base, uint32_t *khz)
{
	uint32_t prev = 0, best_k = 0;
	unsigned best = 0;

	for (unsigned i = 0; i < 40; i++) {
		uint32_t k = osm_khz(rd(base + OSM_FREQ_REG(i)));
		if (i > 0 && k == prev)
			break;
		if (k >= best_k) {
			best_k = k;
			best = i;
		}
		prev = k;
	}
	*khz = best_k;
	return best;
}

static void osm_boost_one(const char *label, uintptr_t base, unsigned ncores)
{
	uint32_t khz = 0, before = rd(base + OSM_DESIRED(0)), bad = 0;
	unsigned top = osm_top(base, &khz);

	if (khz == 0 || top == 0) {
		con_hex2(label, "table unreadable top=", top, "kHz=", khz);
		return;
	}
	for (unsigned n = 0; n < ncores; n++) {
		wr(base + OSM_DESIRED(n), top);
		if (rd(base + OSM_DESIRED(n)) != top)
			bad++;
	}
	con_hex2(label, "MHz=", khz / 1000U, "index was/now=", (before << 16) | (top & 0xffffU));
	if (bad)
		con_hex2(label, "readback mismatch cores=", bad, "of=", ncores);
}

static void cpu_boost(void)
{
	/* L3 0x18321000, little 0x18323000 (cpu0-3), big 0x18325800 (cpu4-6), prime 0x18327800 (cpu7) */
	osm_boost_one("CPU L3", 0x18321000UL, 1);
	osm_boost_one("CPU 0-3", 0x18323000UL, 4);
	osm_boost_one("CPU 4-6", 0x18325800UL, 3);
	osm_boost_one("CPU 7", 0x18327800UL, 1);
}

static void gpu_clock_report(void)
{
	uint32_t l = rd(GPUCC_BASE + 0x04) & 0xffffU, alpha = rd(GPUCC_BASE + 0x40) & 0xffffU;
	uint32_t vco_mhz = (uint32_t)(((uint64_t)19200U * ((uint64_t)l * 65536U + alpha)) / 65536U / 1000U);

	con_hex2("GPU clock", "voted MHz=", gx_khz[gx_freq_idx] / 1000U, "PLL0 VCO MHz=", vco_mhz);
	con_hex2("GPU PLL0", "L=", l, "alpha=", alpha);
}

void gpu_stage(const char *cmdline)
{
	unsigned level = cmdline_uint(cmdline, "sweetgpu=", 0);
	if (level == 0)
		return;
	gpu_level = level;
	{
		unsigned f = cmdline_uint(cmdline, "sweetgpufreq=", 1);
		gx_freq_idx = (f >= 1 && f < N_GX) ? f : 1;     /* 0 = GPU off and out-of-range: keep 257 MHz */
	}
	if (cmdline_uint(cmdline, "sweetcpufreq=", 0))
		cpu_boost();

	n_fail = 0;
	quiet = level >= 5;
	very_quiet = level >= 8;
	rb_n = rb_bad = 0;
	con_skip();
	con_hex2("GPU", "sweetgpu=", level, "cmd=", cmdline ? 1 : 0);
	step1_power();
	if (level >= 2)
		step2_firmware();
	if (level >= 3 && n_fail == 0)
		step3_gmu_clock();
	else if (level >= 3)
		con_str("GPU", "step 3 skipped: earlier step failed");
	/* At sweetgpu=5, skip the verbose step-4 dump so step-5 results fit
	 * on the non-scrolling framebuffer console. Step 5 re-reads geometry. */
	if (level == 4 && n_fail == 0)
		step4_smmu_dump();
	else if (level == 4)
		con_str("GPU", "step 4 skipped: earlier step failed");
	if (level >= 5 && n_fail == 0) {
		if (quiet)
			con_str("GPU", "steps 1-3 ok (quiet, failures only)");
		con_mute = very_quiet;	/* steps 5-7 proven: summary only at >=8 */
		step5_smmu_gmu();
	}
	else if (level >= 5)
		con_str("GPU", "step 5 skipped: earlier step failed");
	if (level >= 6 && n_fail == 0) {
		step6_gmu_fw_load();
		if (very_quiet && n_fail == 0)
			con_str("GPU", "step 6 ok (quiet)");
	} else if (level >= 6)
		con_str("GPU", "step 6 skipped: earlier step failed");
	if (level >= 7 && n_fail == 0) {
		step7_rpmh();
		if (very_quiet && n_fail == 0)
			con_str("GPU", "step 7 ok (quiet)");
	} else if (level >= 7)
		con_str("GPU", "step 7 skipped: earlier step failed");
	if (con_mute) {
		con_mute = 0;
		con_str("GPU", n_fail ? "steps 5-7 FAILED (rerun sweetgpu=7)"
				      : "steps 5-7 ok (muted)");
	}
	if (level >= 8 && n_fail == 0) {
		con_mute = (level >= 15);	/* step 8 proven: summary only at >= 15 */
		step8_gmu_start(cmdline);
		if (con_mute) {
			con_mute = 0;
			con_str("GPU", n_fail ? "step 8 FAILED (rerun sweetgpu=8)" : "step 8 ok, GMU running (muted)");
		}
	} else if (level >= 8)
		con_str("GPU", "step 8 skipped: earlier step failed");
	if (level >= 9 && n_fail == 0) {
		con_mute = (level >= 10);	/* step 9 proven: summary only at >= 10 */
		step9_hfi();
		if (con_mute) {
			con_mute = 0;
			con_str("GPU", n_fail ? "step 9 FAILED (rerun sweetgpu=9)" : "step 9 ok (muted)");
		}
	} else if (level >= 9)
		con_str("GPU", "step 9 skipped: earlier step failed");
	if (level >= 10 && n_fail == 0) {
		con_mute = (level >= 11);	/* step 10 proven: summary only at >= 11 */
		step10_hfi_tables();
		if (con_mute) {
			con_mute = 0;
			con_str("GPU", n_fail ? "step 10 FAILED (rerun sweetgpu=10)" : "step 10 ok (muted)");
		}
	} else if (level >= 10)
		con_str("GPU", "step 10 skipped: earlier step failed");
	if (level >= 11 && n_fail == 0) {
		con_mute = (level >= 12);	/* step 11 proven: summary only at >= 12 */
		step11_gx_power(level);
		if (con_mute) {
			con_mute = 0;
			con_str("GPU", n_fail ? "step 11 FAILED (rerun sweetgpu=11)" : "step 11 ok, GX held (muted)");
		}
	} else if (level >= 11)
		con_str("GPU", "step 11 skipped: earlier step failed");
	if (level >= 12 && n_fail == 0) {
		con_mute = (level >= 13);	/* step 12 proven: summary only at >= 13 */
		step12_gpu_cp(cmdline);
		if (con_mute) {
			con_mute = 0;
			con_str("GPU", n_fail ? "step 12 FAILED (rerun sweetgpu=12)" : "step 12 ok, CP started (muted)");
		}
	} else if (level >= 12)
		con_str("GPU", "step 12 skipped: earlier step failed");
	if (level >= 13 && n_fail == 0) {
		con_mute = (level >= 14);	/* step 13 proven (G1): summary only at >= 14 */
		step13_cp_fence();
		if (con_mute) {
			con_mute = 0;
			con_str("GPU", n_fail ? "step 13 FAILED (rerun sweetgpu=13)" : "step 13 ok, G1 fence (muted)");
		}
	} else if (level >= 13)
		con_str("GPU", "step 13 skipped: earlier step failed");
	if (level >= 14 && n_fail == 0) {
		con_mute = (level >= 15);	/* step 14 proven (G2): summary only at >= 15 */
		step14_g2_draw();
		if (con_mute) {
			con_mute = 0;
			con_str("GPU", n_fail ? "step 14 FAILED (rerun sweetgpu=14)" : "step 14 ok, G2 square (muted)");
		}
	} else if (level >= 14)
		con_str("GPU", "step 14 skipped: earlier step failed");
	if (level >= 15 && n_fail == 0)
		step15_g3_blit(cmdline);
	else if (level >= 15)
		con_str("GPU", "step 15 skipped: earlier step failed");
	if (gx_on)
		gpu_clock_report();             /* unmuted: the real GPU clock after the vote */
	if (n_fail == 0)
		con_str("GPU OK", "all steps ok");
	for (int i = 0; i < n_fail && i < 4; i++)
		con_str("GPU FAIL", fail_name[i]);
	con_str("GPU", "holding screen 10 s, then xnu");
	con_skip();
	udelay(HOLD_S * 1000000U);
}
