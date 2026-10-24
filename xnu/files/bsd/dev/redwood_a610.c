/*
 * The GPU of the Redmi Note 8T (willow, SM6125), an Adreno 610, looked at step by step: the first
 * of what will wake it, as bsd/dev/redwood_adreno.c did for redwood's Adreno 642L. Only with
 * boot-arg a610=N and device=willow (xnuloader's): steps 1 to N, every address below being this
 * phone's.
 *
 * The A610 is the first kind of Adreno 6xx and has no GMU, the power management processor the
 * 642L has: the GPU is powered the way the rest of the SoC is, by its two power switches ("GDSC")
 * in its own clock controller -- CX, the half that is not the 3D core (bus interface, SMMU, the
 * registers the kernel talks to), and GX, the 3D core -- and its clocks, some in the global clock
 * controller and some in its own. Its supplies are the SoC's CX rail and the GPU's own rail, both
 * the RPM's to keep, which the firmware has running. Its SMMU (0x59a0000) is set up under
 * Qualcomm's hypervisor ("qcom,hyp_secure_alloc"); its "zap shader" (a610_zap, PAS id 13) is
 * TrustZone's to load.
 *
 * Reading a register of the GPU block while its switch is off does not answer: the bus hangs.
 * So the first steps only read what is always there; the GPU's own registers come after it has
 * been switched on, and at first at the crystal's 19.2 MHz: the 3D core's clock is the CX rail's
 * like the rest (vdd_cx in gpucc-trinket.c, no rail of its own), and so slow a clock asks nothing
 * of the rail's voltage that the firmware has not set already.
 *
 *   1  reads   the GPU's clocks in the global clock controller
 *   2  reads   the GPU's own clock controller: its two PLLs, its two clock sources, its clocks,
 *              and both power switches (the firmware leaves all of it off)
 *   3  reads   the fuse that says which bin of the chip this is (the speeds it is good for)
 *   4  writes  the GPU's bus clocks on in the global clock controller -- the ones the vendor's
 *              kgsl node names: mem (bimc_gpu_axi), mem_iface (gpu_memnoc_gfx), alt_mem_iface
 *              (sys_noc_compute_sf_axi), and snoc_dvm_gfx -- then CX's power switch, then the
 *              clocks of that half: iface (gpu_cc_ahb), rbbmtimer (gpu_cc_cxo), the GMU wrapper's
 *              (gpu_cc_cx_gmu, with the vendor's wake and sleep counts) and the SMMU's vote
 *   5  writes  GX's power switch, and the 3D core's clock from the crystal (its source set to
 *              input 0 undivided, then the clock on); with a610_mhz=N, from one of the GPU's PLLs
 *              instead, the rails MX and CX asked of the RPM first (core_speed)
 *   6  reads   the first registers of the GPU block
 *   7  reads   the GPU's SMMU (0x59a0000): its ID registers and stream table, which the hypervisor
 *              answers for, and the first translation context, as the firmware and hypervisor left it
 *   8  asks    TrustZone what Linux asks first: whether it takes calls, whether it has the calls
 *              that authenticate and start a firmware image, and whether it "supports" the GPU's
 *              zap shader (PAS id 13) -- calls that pass no memory
 *   9  asks    TrustZone to check the zap shader's signature: its header (fw/a610_zap.mdt of the
 *              small image, the phone's own /vendor/firmware/a610_zap.mdt: the ELF header and the
 *              block of hashes and certificates) goes into a page of ours, which TrustZone reads
 *              (PAS_INIT_IMAGE); then, as the rest of Linux's qcom_mdt_load, its code into the
 *              device tree's gpu_region (PAS_MEM_SETUP first) and PAS_AUTH_AND_RESET (zap_load).
 *              Only with a610_zap=1. The call came back "interrupted" for ever until the timer's
 *              interrupt was the right one (19, not 27: gic_v3_irq.c); since then it takes 3 resumptions
 *  10  writes  memory of ours for the 3D core, physically in one piece: its ring of commands, its
 *              microcode (fw/a630_sqe.fw, the phone's own, less the file's first word) and a page
 *              to be written on command. The core is given physical addresses
 *  11  writes  the 3D core's registers as Linux's a6xx_hw_init does for an A610 before it starts
 *              the command processor, then starts it and gives it "initialize" (a6xx_cp_init).
 *              With a610_bypass=1 first the SMMU: none of its stream match registers is valid
 *              (step 7) and sCR0 has USFCFG set, so every stream of the core is refused -- the
 *              device tree's kgsl-smmu has "qcom,skip-init", it is Linux that programs the GPU's
 *              streams (gfx3d_user: SID 0 mask 1, gfx3d_secure: SID 2). One match for SIDs 0-3 goes
 *              to context 4 with its translation off, as arm-smmu-qcom.c's bypass_quirk does: a
 *              plain bypass S2CR is turned into "fault" by Qualcomm's hypervisor. With the zap
 *              shader in (step 9), CP_SET_SECURE_MODE 0 after "initialize", as a6xx_hw_init does:
 *              RBBM_SECVID_TRUST_CNTL goes from 1 to 0, the core is out of secure mode
 *  12  commands the core to write a word into our page, first as it is (in secure mode: TRUST_CNTL
 *              reads 1), and if that does not land, after CP_SET_SECURE_MODE 0 -- the packet Linux
 *              sends once the zap shader is in. Writing RBBM_SECVID_TRUST_CNTL = 0 from here, as
 *              Linux does where there is no zap shader at all, hung the bus on this phone (the
 *              write never came back, and the whole kernel stopped some 15 s later): not done
 *
 *  13  commands a picture filled with three bands of colour by the 2D engine and shows it on
 *              the panel: only with a610_smmu=1, which (in step 11, instead of a610_bypass=1) gives
 *              the core addresses of its own through page tables of ours in the SMMU's context 0
 *
 * Steps 9 to 12 go one after the other whatever 9 says. Without the zap shader the core stays in
 * secure mode: CP_MEM_WRITE lands, but what the 2D engine draws (step 13) goes nowhere of ours --
 * in secure mode its colour units write as the secure stream. With it (a610_zap=1, 2026-10-25)
 * step 13 draws: its three bands are on the panel.
 *
 * Register layouts: Xiaomi's ginkgo-q-oss kernel, drivers/clk/qcom/gpucc-trinket.c and
 * gcc-trinket.c, drivers/gpu/msm/adreno_a6xx.c; the phone's own device tree (qcom,kgsl-3d0@5900000,
 * qcom,gdsc@599106c and @599100c, qcom,kgsl-iommu@59a0000).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
#include <vm/vm_kern.h>
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

static int say(const char *format, ...) __printflike(1, 2);

/* every line to the loader's log as well: a run that ends in a hang still has said what it was about to do */
static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

#define GCC_PHYS                0x01400000UL
#define GCC_SIZE                0x80000UL
#define GPUCC_PHYS              0x05990000UL
#define GPUCC_SIZE              0x9000UL
#define GPU_PHYS                0x05900000UL
#define GPU_SIZE                0x90000UL
#define SMMU_PHYS               0x059a0000UL
#define SMMU_SIZE               0x10000UL
#define QFPROM_PHYS             0x01b40000UL
#define QFPROM_SIZE             0x7000UL

#define CLOCK_OFF               (1u << 31)      /* a branch's control register: the clock does not run */
#define CLOCK_ENABLE            (1u << 0)
#define GDSC_ON                 (1u << 31)      /* a power switch's: it is on */
#define GDSC_SW_COLLAPSE        (1u << 0)       /* ... and asked to be off */

static vm_offset_t gcc, gpucc, gpu, qfprom, smmu;

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__builtin_arm_dsb(0xf);
}

/* A branch clock on, and whether it is seen running within 2 ms (`voted`: its halt bit may not say). */
static bool
clock_on(const char *name, vm_offset_t base, uint32_t offset, bool check)
{
	wr(base + offset, rd(base + offset) | CLOCK_ENABLE);
	for (int i = 0; check && i < 200; i++) {
		if (!(rd(base + offset) & CLOCK_OFF)) {
			printf("a610: %s on (%#x)\n", name, rd(base + offset));
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	printf("a610: %s: %s (%#x)\n", name, check ? "DOES NOT RUN" : "asked for", rd(base + offset));
	return !check;
}

/* A power switch on: its "collapse" bit cleared, then up to 5 ms for it to say it is on. */
static bool
gdsc_on(const char *name, uint32_t offset)
{
	wr(gpucc + offset, rd(gpucc + offset) & ~GDSC_SW_COLLAPSE);
	for (int i = 0; i < 500; i++) {
		if (rd(gpucc + offset) & GDSC_ON) {
			printf("a610: power switch %s is ON (%#x) after %d us\n", name, rd(gpucc + offset), 10 * i);
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	printf("a610: power switch %s DOES NOT COME ON (%#x)\n", name, rd(gpucc + offset));
	return false;
}

struct named {
	const char *name;
	uint32_t    offset;
};

static void
branches(const char *where, vm_offset_t base, const struct named *list, unsigned count)
{
	for (unsigned i = 0; i < count; i++) {
		uint32_t value = rd(base + list[i].offset);

		printf("a610: %s %-28s (%#06x) = %#010x  %s\n", where, list[i].name, list[i].offset, value,
		    (value & CLOCK_OFF) ? "off" : "RUNS");
	}
}

static bool
gdsc(const char *name, uint32_t offset)
{
	uint32_t value = rd(gpucc + offset);

	printf("a610: power switch %s (%#x) = %#010x: %s%s\n", name, offset, value, (value & GDSC_ON) ? "ON" : "off",
	    (value & GDSC_SW_COLLAPSE) ? ", asked to be off" : "");
	return (value & GDSC_ON) != 0;
}

/*
 * a610_mhz=N (with rpm=2 or more): the 3D core at one of the speeds of gpucc-trinket.c's table
 * instead of the crystal's 19.2 MHz. The rails first, through the RPM: MX for the PLL, CX for the
 * core, at the level the table gives -- but NOM at least on these first runs: nothing on this
 * side has asked the RPM for CX before, and a lower level than the firmware's would be lower for
 * every other clock the AP's half of the SoC runs on too. Then one of the GPU's two alpha PLLs at
 * twice the speed (the source divides by 2), its aux2 output, as clk_alpha_pll_configure and
 * clk_alpha_pll_enable do it; then the source switched to it, forced on while it changes
 * (FORCE_ENABLE_RCG). PLL1 (its VCO 500 MHz-1 GHz) for 320 and 465, PLL0 (1-2 GHz) above.
 * Returns the speed set, or 0 (the core stays on the crystal).
 */
extern bool redwood_rpm_level(const char *what, bool mx, uint32_t level);

static const struct {
	uint32_t mhz, level;
} core_speeds[] = {
	{ 320, 64 }, { 465, 128 }, { 600, 192 }, { 745, 256 }, { 820, 320 }, { 900, 384 }, { 950, 416 },
};
#define LEVEL_NOM               256

#define PLL_MODE                0x00
#define PLL_L                   0x04
#define PLL_ALPHA               0x08
#define PLL_ALPHA_U             0x0c
#define PLL_USER                0x10
#define PLL_CONFIG              0x18
#define PLL_OUTCTRL             (1u << 0)
#define PLL_BYPASSNL            (1u << 1)
#define PLL_RESET_N             (1u << 2)
#define PLL_LOCK_DET            (1u << 31)
#define PLL_AUX2                (1u << 2)       /* the user control's aux2 output */
#define PLL_ALPHA_EN            (1u << 24)
#define PLL_CONFIG_VALUE        0x4001055b      /* gpu_pll0_config's and gpu_pll1_config's */
#define RCG_UPDATE              (1u << 0)
#define RCG_ROOT_EN             (1u << 1)
#define RCG_ROOT_OFF            (1u << 31)

static uint32_t core_mhz;               /* 0: the crystal's 19.2 */

static uint32_t
core_speed(uint32_t mhz)
{
	uint32_t level = 0, pll = 2 * mhz >= 1000 ? 0 : 1, source = pll == 0 ? 2 : 4;  /* gpu_cc_parent_map_1 */
	uint32_t l = 20 * mhz / 192;            /* twice the speed over 19.2 MHz: L, and 40 bits of fraction */
	uint64_t fraction = ((uint64_t)(20 * mhz % 192) << 40) / 192;
	vm_offset_t p = gpucc + 0x100 * pll;
	int i;

	for (unsigned k = 0; k < sizeof(core_speeds) / sizeof(core_speeds[0]); k++) {
		if (core_speeds[k].mhz == mhz) {
			level = core_speeds[k].level;
		}
	}
	if (level == 0) {
		printf("a610: a610_mhz=%u is not one of 320 465 600 745 820 900 950: the core stays on the crystal\n", mhz);
		return 0;
	}
	printf("a610: next: the core at %u MHz: rails MX and CX at level %u (the table's %u), PLL%u at %u MHz "
	    "(L %#x, alpha %#llx), source input %u divided by 2\n", mhz, level < LEVEL_NOM ? LEVEL_NOM : level, level, pll,
	    2 * mhz, l, fraction, source);
	if (level < LEVEL_NOM) {
		level = LEVEL_NOM;
	}
	if (!redwood_rpm_level("the rail MX", true, level) || !redwood_rpm_level("the rail CX", false, level)) {
		printf("a610: the RPM has not given the rails: the core stays on the crystal\n");
		return 0;
	}
	if (rd(p + PLL_MODE) & PLL_OUTCTRL) {
		printf("a610: PLL%u is already running (mode %#x): not touched, the core stays on the crystal\n", pll,
		    rd(p + PLL_MODE));
		return 0;
	}
	wr(p + PLL_L, l);
	wr(p + PLL_ALPHA, (uint32_t)fraction);
	wr(p + PLL_ALPHA_U, (uint32_t)(fraction >> 32));
	wr(p + PLL_CONFIG, PLL_CONFIG_VALUE);
	wr(p + PLL_USER, (rd(p + PLL_USER) & ~(3u << 20)) | (pll == 0 ? 0u : 2u) << 20 | PLL_ALPHA_EN | PLL_AUX2);
	printf("a610: PLL%u set: L %#x, alpha %#x %#x, user %#x, config %#x\n", pll, rd(p + PLL_L), rd(p + PLL_ALPHA),
	    rd(p + PLL_ALPHA_U), rd(p + PLL_USER), rd(p + PLL_CONFIG));
	/* out of bypass, 5 us, out of reset, its lock, then its outputs */
	wr(p + PLL_MODE, rd(p + PLL_MODE) | PLL_BYPASSNL);
	delay_for_interval(5, NSEC_PER_USEC);
	wr(p + PLL_MODE, rd(p + PLL_MODE) | PLL_RESET_N);
	for (i = 0; i < 200 && !(rd(p + PLL_MODE) & PLL_LOCK_DET); i++) {
		delay_for_interval(10, NSEC_PER_USEC);
	}
	if (!(rd(p + PLL_MODE) & PLL_LOCK_DET)) {
		printf("a610: PLL%u DOES NOT LOCK in 2 ms (mode %#x): off again, the core stays on the crystal\n", pll,
		    rd(p + PLL_MODE));
		wr(p + PLL_MODE, 0);
		return 0;
	}
	wr(p + PLL_MODE, rd(p + PLL_MODE) | PLL_OUTCTRL);
	printf("a610: PLL%u locked after %d us, its outputs on (mode %#x)\n", pll, 10 * i, rd(p + PLL_MODE));

	wr(gpucc + 0x101c, rd(gpucc + 0x101c) | RCG_ROOT_EN);
	for (i = 0; i < 100 && (rd(gpucc + 0x101c) & RCG_ROOT_OFF); i++) {
		delay_for_interval(10, NSEC_PER_USEC);
	}
	wr(gpucc + 0x1020, (rd(gpucc + 0x1020) & ~0x71fu) | source << 8 | (2 * 2 - 1));
	wr(gpucc + 0x101c, rd(gpucc + 0x101c) | RCG_UPDATE);
	for (i = 0; i < 500 && (rd(gpucc + 0x101c) & RCG_UPDATE); i++) {
		delay_for_interval(10, NSEC_PER_USEC);
	}
	printf("a610: the 3D core's clock source: command %#x, configuration %#x (%s)\n", rd(gpucc + 0x101c),
	    rd(gpucc + 0x1020), rd(gpucc + 0x101c) & RCG_UPDATE ? "the switch DID NOT TAKE" : "switched");
	if (rd(gpucc + 0x101c) & RCG_UPDATE) {
		return 0;
	}
	return mhz;
}

/*
 * The core's speed as its own counter says: CP's first performance counter counting every cycle
 * (PERF_CP_ALWAYS_COUNT, set in step 11 as Linux's hw_init does) over 20 ms of the timebase.
 */
#define RBBM_PERFCTR_CP_0       (4u * 0x0400)   /* low word, then high */
#define CP_PERFCTR_CP_SEL_0     (4u * 0x08d0)
#define PERF_CP_ALWAYS_COUNT    0

static uint64_t
cycles(void)
{
	uint32_t high, low;

	do {
		high = rd(gpu + RBBM_PERFCTR_CP_0 + 4);
		low = rd(gpu + RBBM_PERFCTR_CP_0);
	} while (rd(gpu + RBBM_PERFCTR_CP_0 + 4) != high);
	return (uint64_t)high << 32 | low;
}

static void
core_speed_measured(void)
{
	uint64_t c0, c1, t0, t1, ns;

	c0 = cycles();
	t0 = mach_absolute_time();
	delay_for_interval(20, NSEC_PER_MSEC);
	c1 = cycles();
	t1 = mach_absolute_time();
	absolutetime_to_nanoseconds(t1 - t0, &ns);
	printf("a610: THE CORE'S SPEED, by its own counter: %llu cycles in %llu us = %llu MHz (asked for: %u)\n", c1 - c0,
	    ns / 1000, ns ? (c1 - c0) * 1000 / ns : 0, core_mhz ? core_mhz : 19);
	redwood_status(REDWOOD_STATUS_GPU, "commands RUN, core at %llu MHz", ns ? (c1 - c0) * 1000 / ns : 0);
}

/* ---- the 3D core: its registers (in words), its ring, its first commands (as bsd/dev/redwood_adreno.c) ---- */

#define GPU_WORD(n)             (4u * (n))
#define RBBM_GBIF_CLIENT_QOS_CNTL GPU_WORD(0x0011)
#define RBBM_GBIF_HALT          GPU_WORD(0x0016)
#define RBBM_GBIF_HALT_ACK      GPU_WORD(0x0017)
#define RBBM_INTERFACE_HANG_INT_CNTL GPU_WORD(0x001f)
#define UCHE_CLIENT_PF          GPU_WORD(0x0e19)
#define GBIF_QSB_SIDE0          GPU_WORD(0x3c03)        /* to 3: the bus interface's four queues */
#define GBIF_HALT               GPU_WORD(0x3c45)
#define GBIF_HALT_ACK           GPU_WORD(0x3c46)
#define RBBM_INT_0_STATUS       GPU_WORD(0x0201)
#define RBBM_STATUS             GPU_WORD(0x0210)
#define RBBM_PERFCTR_CNTL       GPU_WORD(0x0500)
#define RBBM_PERFCTR_GPU_BUSY_MASKED GPU_WORD(0x050b)
#define CP_RB_BASE              GPU_WORD(0x0800)
#define CP_RB_CNTL              GPU_WORD(0x0802)
#define CP_RB_RPTR              GPU_WORD(0x0806)
#define CP_RB_WPTR              GPU_WORD(0x0807)
#define CP_SQE_CNTL             GPU_WORD(0x0808)
#define CP_HW_FAULT             GPU_WORD(0x0821)
#define CP_INTERRUPT_STATUS     GPU_WORD(0x0823)
#define CP_PROTECT_STATUS       GPU_WORD(0x0824)
#define CP_SQE_INSTR_BASE       GPU_WORD(0x0830)
#define CP_ROQ_THRESHOLDS_1     GPU_WORD(0x08c1)
#define CP_ROQ_THRESHOLDS_2     GPU_WORD(0x08c2)
#define CP_MEM_POOL_SIZE        GPU_WORD(0x08c3)
#define CP_AHB_CNTL             GPU_WORD(0x098d)
#define UCHE_WRITE_RANGE_MAX    GPU_WORD(0x0e05)
#define UCHE_WRITE_THRU_BASE    GPU_WORD(0x0e07)
#define UCHE_TRAP_BASE          GPU_WORD(0x0e09)
#define UCHE_GMEM_RANGE_MIN     GPU_WORD(0x0e0b)
#define UCHE_GMEM_RANGE_MAX     GPU_WORD(0x0e0d)
#define UCHE_CACHE_WAYS         GPU_WORD(0x0e17)
#define UCHE_FILTER_CNTL        GPU_WORD(0x0e18)
#define PC_DBG_ECO_CNTL         GPU_WORD(0x9e00)
#define RBBM_SECVID_TRUST_CNTL  GPU_WORD(0xf400)
#define RBBM_SECVID_TSB_TRUSTED_BASE GPU_WORD(0xf800)
#define RBBM_SECVID_TSB_TRUSTED_SIZE GPU_WORD(0xf802)
#define RBBM_SECVID_TSB_CNTL    GPU_WORD(0xf803)
#define CP_RB_CNTL_VALUE        (12u | 2u << 8 | 1u << 27)      /* 2^12 pairs of words; no read position copied out */

#define CP_MEM_WRITE            0x3d
#define CP_ME_INIT              0x48
#define CP_SET_SECURE_MODE      0x66

#define CORE_RING_SIZE          0x8000u
#define CORE_SQE_OFFSET         0x8000u
#define CORE_SQE_MAX            0x10000u
#define CORE_SCRATCH_OFFSET     0x18000u
#define CORE_MEMORY_SIZE        0x20000u
#define RING_WORDS              (CORE_RING_SIZE / 4)

static vm_offset_t core_memory;         /* the ring, the microcode, the page: one piece */
static uint64_t core_phys;
static uint64_t core_address;           /* ... where the 3D core sees it: core_phys, or CORE_ADDRESS with a610_smmu=1 */
static uint32_t ring_at, ring_sent;

static void
to_memory(vm_offset_t start, vm_size_t size)
{
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__builtin_arm_dsb(0xf);
}

static void
put64(vm_offset_t reg, uint64_t value)
{
	wr(reg, (uint32_t)value);
	wr(reg + 4, (uint32_t)(value >> 32));
}

/*
 * ---- the 3D core's own addresses (a610_smmu=1) ----
 *
 * Context 0 of the GPU's SMMU with page tables of ours, as bsd/dev/redwood_adreno.c makes them
 * for redwood: three levels of 4K pages, 39 bits (dt: qcom,use-3-lvl-tables), stage 1 with stage
 * 2 bypassed, the GPU's stream (SID 0 mask 1, the one match the hypervisor takes) sent through
 * it. The core then needs no memory in one piece: the picture of step 13 is 10 MB of pages.
 * Formats: Linux drivers/iommu/io-pgtable-arm.c and arm-smmu.c.
 */
#define CORE_ADDRESS            0x01000000ull   /* the ring, the microcode, the page (redwood's CORE_RING_ADDRESS) */
#define TARGET_ADDRESS          0x10000000ull   /* step 13's picture */
#define PTE_TABLE               3ull
#define PTE_PAGE                3ull
#define PTE_CORE                (1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)  /* unprivileged, shared, accessed, not global; uncached */
#define PTE_ADDRESS             0x0000fffffffff000ull
#define SPACE_TABLES            1024            /* 2 GB of addresses, were they all of the last level (as redwood_adreno.c) */
#define PTE_READ_ONLY           (1ull << 7)     /* Linux gives the core its microcode so (MSM_BO_GPU_READONLY) */
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)       /* stage 2 bypassed; our tables alone decide */
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)   /* 4K pages, cached walks, no TTBR1 */
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)       /* 36 bits of physical address, 16 of context tag */
#define SMMU_MAIR0              0xf404ff44      /* Linux's: 0 uncached, 1 cached, 2 device */
#define SMMU_FSR_ALL            0xc00001fe
#define SMMU_GPU_SMR            (1u << 31 | 1u << 16 | 0)       /* dt gfx3d_user: SID 0 mask 1 */

static struct {
	uint32_t count;
	uint64_t *table[SPACE_TABLES];
	uint64_t phys[SPACE_TABLES];
	bool changed[SPACE_TABLES];             /* to be written out of this processor's cache (space_commit()) */
	uint64_t *last_entry, *last_table;      /* the level-2 entry looked up last, and its table: a buffer's pages in a row */
} space;

/* The table an entry of ours is in has changed. */
static void
space_changed(const uint64_t *entry)
{
	static uint32_t last;                   /* (a buffer's pages are in one table after another) */

	if (last < space.count && entry >= space.table[last] && entry < space.table[last] + 512) {
		space.changed[last] = true;
		return;
	}
	for (uint32_t i = 0; i < space.count; i++) {
		if (entry >= space.table[i] && entry < space.table[i] + 512) {
			space.changed[i] = true;
			last = i;
			return;
		}
	}
}

static uint64_t *
space_table(uint64_t *entry)
{
	uint64_t *table;
	vm_offset_t page;

	if (entry != NULL && (*entry & 1)) {
		for (uint32_t i = 0; i < space.count; i++) {
			if (space.phys[i] == (*entry & PTE_ADDRESS)) {
				return space.table[i];
			}
		}
		return NULL;
	}
	if (space.count == SPACE_TABLES || kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return NULL;
	}
	table = (uint64_t *)page;       /* a 4K table at the start of a page of ours (16K pages hold four; one is enough) */
	space.table[space.count] = table;
	space.changed[space.count] = true;
	space.phys[space.count++] = kvtophys(page);
	if (entry != NULL) {
		*entry = kvtophys(page) | PTE_TABLE;
		space_changed(entry);
	}
	return table;
}

/* One 4K page of the core's addresses to one of ours, with these attributes (0 and 0: no page there). */
static bool
space_map_as(uint64_t address, uint64_t phys, uint64_t attributes)
{
	uint64_t *level2, *level3;

	if (address >> 39 || (space.count == 0 && space_table(NULL) == NULL)) {
		return false;
	}
	if ((level2 = space_table(&space.table[0][address >> 30 & 0x1ff])) == NULL) {
		return false;
	}
	if (space.last_entry == &level2[address >> 21 & 0x1ff]) {
		level3 = space.last_table;
	} else if ((level3 = space_table(&level2[address >> 21 & 0x1ff])) != NULL) {
		space.last_entry = &level2[address >> 21 & 0x1ff];
		space.last_table = level3;
	} else {
		space.last_entry = NULL;
		return false;
	}
	level3[address >> 12 & 0x1ff] = phys == 0 && attributes == 0 ? 0 : phys | attributes | PTE_PAGE;
	space_changed(&level3[address >> 12 & 0x1ff]);
	return true;
}

static bool
space_map(uint64_t address, uint64_t phys)
{
	return space_map_as(address, phys, PTE_CORE);
}

/* The tables that have changed, out of this processor's cache to where the SMMU reads them. */
static void
space_commit(void)
{
	for (uint32_t i = 0; i < space.count; i++) {
		if (space.changed[i]) {
			to_memory((vm_offset_t)space.table[i], 4096);
			space.changed[i] = false;
		}
	}
}

/* Memory of ours, `size` bytes of pages, at an address of the core's. */
static bool
space_block(uint64_t address, vm_offset_t memory, uint32_t size)
{
	for (uint32_t offset = 0; offset < size; offset += 4096) {
		if (!space_map(address + offset, kvtophys(memory + offset))) {
			printf("a610: no room for the tables at %#llx\n", address + offset);
			return false;
		}
	}
	return true;
}

/* Context 0 with our tables, and the GPU's stream through it; what the SMMU made of each write is checked. */
static bool
space_on(void)
{
	vm_offset_t cb = smmu + 0x8000;         /* 8 pages of 4K before the contexts' own (step 7) */
	uint64_t root = space.phys[0];          /* context tag 0 */

	for (uint32_t i = 0; i < space.count; i++) {
		to_memory((vm_offset_t)space.table[i], 4096);
	}
	wr(cb + 0x00, 0);
	wr(smmu + 0x1800, 1);                   /* CBA2R 0: 64-bit */
	wr(smmu + 0x1000, SMMU_CBAR_STAGE_1);   /* CBAR 0 */
	wr(cb + SMMU_CB_TCR2, SMMU_TCR2);
	wr(cb + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	put64(cb + SMMU_CB_TTBR0, root);
	put64(cb + SMMU_CB_TTBR1, 0);
	wr(cb + SMMU_CB_MAIR0, SMMU_MAIR0);
	wr(cb + SMMU_CB_MAIR1, 0);
	wr(cb + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr(cb + SMMU_CB_TLBIASID, 0);
	wr(cb + SMMU_CB_TLBSYNC, 1);
	for (int i = 0; i < 1000 && (rd(cb + SMMU_CB_TLBSTATUS) & 1); i++) {
		delay_for_interval(10, NSEC_PER_USEC);
	}
	wr(cb + 0x00, SMMU_SCTLR_ON);
	wr(smmu + 0xc00, 0);                    /* S2CR 0: translate, context 0 */
	wr(smmu + 0x800, SMMU_GPU_SMR);
	__builtin_arm_dsb(0xf);
	printf("a610: SMMU context 0: CBAR %#x, CBA2R %#x, SCTLR %#x, TCR %#x, TCR2 %#x, TTBR0 %#x:%#x (ours %#llx), TLB %#x; "
	    "stream 0: SMR %#x, S2CR %#x\n", rd(smmu + 0x1000), rd(smmu + 0x1800), rd(cb), rd(cb + SMMU_CB_TCR),
	    rd(cb + SMMU_CB_TCR2), rd(cb + SMMU_CB_TTBR0 + 4), rd(cb + SMMU_CB_TTBR0), root, rd(cb + SMMU_CB_TLBSTATUS),
	    rd(smmu + 0x800), rd(smmu + 0xc00));
	if (rd(smmu + 0x800) != SMMU_GPU_SMR || (rd(smmu + 0xc00) & 0x300ff) != 0 || !(rd(cb) & 1) ||
	    (rd(smmu + 0x1000) & 0x30000) != (SMMU_CBAR_STAGE_1 & 0x30000) || rd(cb + SMMU_CB_TTBR0) != (uint32_t)root ||
	    rd(cb + SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32)) {
		printf("a610: the SMMU did NOT take it: the GPU's stream is not going through our tables\n");
		return false;
	}
	printf("a610: the GPU's stream goes through our tables (%u of them)\n", space.count);
	return true;
}

/* What context 0 has recorded of a request it could not serve. */
static bool
space_faulted(const char *when)
{
	vm_offset_t cb = smmu + 0x8000;

	if ((rd(cb + SMMU_CB_FSR) & SMMU_FSR_ALL) == 0) {
		return false;
	}
	printf("a610: THE SMMU REFUSED the core %s: status %#x, address %#x:%#x, syndrome %#x\n", when, rd(cb + SMMU_CB_FSR),
	    rd(cb + SMMU_CB_FAR + 4), rd(cb + SMMU_CB_FAR), rd(cb + SMMU_CB_FSYNR0));
	return true;
}

static uint32_t
odd_parity(uint32_t v)
{
	return 0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28)) & 1;
}

static uint32_t *emit_to, emit_count;    /* set: what ring() is given goes into a command buffer instead */

static void
ring(uint32_t word)
{
	if (emit_to != NULL) {
		emit_to[emit_count++] = word;
		return;
	}
	((uint32_t *)core_memory)[ring_at] = word;
	ring_at = (ring_at + 1) % RING_WORDS;
}

/* What has been put into the ring since the last time, out to memory and to the core; no waiting. */
static void
ring_send(void)
{
	if (ring_at >= ring_sent) {
		to_memory(core_memory + 4 * ring_sent, 4 * (ring_at - ring_sent));
	} else {
		to_memory(core_memory + 4 * ring_sent, 4 * (RING_WORDS - ring_sent));
		to_memory(core_memory, 4 * ring_at);
	}
	ring_sent = ring_at;
	wr(gpu + CP_RB_WPTR, ring_at);
}

static void
ring_command(uint32_t command, uint32_t words)
{
	ring(7u << 28 | words | odd_parity(words) << 15 | command << 16 | odd_parity(command) << 23);
}

/* A register of the 3D core (by its word number) written by command, in the order of the other commands. */
static void
ring_register(uint32_t number, uint32_t value)
{
	ring(4u << 28 | 1 | odd_parity(1) << 7 | number << 8 | odd_parity(number) << 27);
	ring(value);
}

/*
 * Step 13: the 2D engine filling a rectangle with one colour, as redwood_adreno.c's step 22 (Mesa's
 * fd6_blitter: fd6_clear_surface; registers under their a6xx.xml names). Rows of a multiple of 64
 * bytes. What differs on the A610 is the colour cache's offset when it draws straight into memory:
 * Mesa's ccu_offset_bypass = num_ccu (1 here, freedreno_devices.py) x 64 KB, in RB_CCU_CNTL's
 * COLOR_OFFSET [31:23] in 4K -- the A630's two make the 0x10000000 Mesa once had as a constant.
 */
#define CP_WAIT_FOR_IDLE        0x26
#define CP_BLIT                 0x2c
#define CP_EVENT_WRITE          0x46
#define CP_SET_MARKER           0x65
#define RB_CCU_CNTL             0x8e07
#define A610_CCU_CNTL_BYPASS    (0x10000u >> 12 << 23)
#define GRAS_2D_BLIT_CNTL       0x8400
#define GRAS_2D_DST_TL          0x8405
#define GRAS_2D_DST_BR          0x8406
#define RB_2D_BLIT_CNTL         0x8c00
#define RB_2D_PIXEL_CNTL        0x8c01
#define RB_2D_DST_INFO          0x8c17
#define RB_2D_DST               0x8c18
#define RB_2D_DST_PITCH         0x8c1a
#define RB_2D_SRC_SOLID_C0      0x8c2c
#define SP_2D_DST_FORMAT        0xacc0
#define FORMAT_8888             0x30
#define BLIT_SOLID_8888         (1u << 7 | FORMAT_8888 << 8 | 0xfu << 20 | 0x10u << 24)
#define DST_BGRA_LINEAR         (FORMAT_8888 | 1u << 10)
#define EVENT_FLUSH_COLOR       0x1d
#define EVENT_CACHE_FLUSH       0x04
#define PICTURE_WIDTH           1080
#define PICTURE_HEIGHT          2340
#define PICTURE_ROW             4352            /* 1088 pixels: the next multiple of 64 bytes */

extern void MiniIOMobileFramebufferShow(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t row_bytes);

static void
ring_fill_at(uint64_t target, uint32_t top, uint32_t bottom, uint32_t red, uint32_t green, uint32_t blue)
{
	ring_register(GRAS_2D_DST_TL, top << 16);
	ring_register(GRAS_2D_DST_BR, (PICTURE_WIDTH - 1) | (bottom - 1) << 16);
	ring_register(RB_2D_SRC_SOLID_C0, red);
	ring_register(RB_2D_SRC_SOLID_C0 + 1, green);
	ring_register(RB_2D_SRC_SOLID_C0 + 2, blue);
	ring_register(RB_2D_SRC_SOLID_C0 + 3, 255);
	ring_register(RB_2D_BLIT_CNTL, BLIT_SOLID_8888);
	ring_register(GRAS_2D_BLIT_CNTL, BLIT_SOLID_8888);
	ring_register(SP_2D_DST_FORMAT, 1 | FORMAT_8888 << 3 | 0xfu << 12);
	ring_register(RB_2D_PIXEL_CNTL, 0);
	ring_register(RB_2D_DST_INFO, DST_BGRA_LINEAR);
	ring_register(RB_2D_DST, (uint32_t)target);
	ring_register(RB_2D_DST + 1, (uint32_t)(target >> 32));
	ring_register(RB_2D_DST_PITCH, PICTURE_ROW >> 6);
	ring_command(CP_WAIT_FOR_IDLE, 0);
	ring_command(CP_BLIT, 1);
	ring(3);                                /* "scale": the 2D engine's one operation */
}

static void
ring_fill(uint32_t top, uint32_t bottom, uint32_t red, uint32_t green, uint32_t blue)
{
	ring_fill_at(TARGET_ADDRESS, top, bottom, red, green, blue);
}

/* Let the core have what is in the ring, and wait until it has taken all of it and rests. */
static bool
ring_run(const char *what)
{
	uint32_t taken = ~0u, status = ~0u;

	printf("a610: about to have the 3D core %s: the ring up to word %u\n", what, ring_at);
	ring_send();
	for (int i = 0; i < 20000 && (taken != ring_at || (status & ~1u)); i++) {
		delay_for_interval(100, NSEC_PER_USEC);
		taken = rd(gpu + CP_RB_RPTR);
		status = rd(gpu + RBBM_STATUS);
	}
	if (taken == ring_at && !(status & ~1u) && rd(gpu + CP_HW_FAULT) == 0) {
		printf("a610: the 3D core has done it (ring read up to %u, status %#x)\n", taken, status);
		return true;
	}
	printf("a610: the 3D core does not get through: ring read up to %u of %u, status %#x, interrupts %#x, command "
	    "processor: fault %#x, interrupts %#x, protection %#x\n", taken, ring_at, status, rd(gpu + RBBM_INT_0_STATUS),
	    rd(gpu + CP_HW_FAULT), rd(gpu + CP_INTERRUPT_STATUS), rd(gpu + CP_PROTECT_STATUS));
	return false;
}

/* What the SMMU's global and context 0 fault registers say (the core's requests, if refused, are seen there). */
static void
smmu_faults(const char *when)
{
	if (smmu == 0) {
		return;
	}
	printf("a610: SMMU %s: sGFSR %#x, sGFSYNR0 %#x, sGFSYNR1 %#x; context 0: FSR %#x, FAR %#x:%#x, FSYNR0 %#x\n", when,
	    rd(smmu + 0x48), rd(smmu + 0x50), rd(smmu + 0x54), rd(smmu + 0x8000 + 0x58), rd(smmu + 0x8000 + 0x64),
	    rd(smmu + 0x8000 + 0x60), rd(smmu + 0x8000 + 0x68));
}

/* ---- TrustZone (Linux drivers/firmware/qcom_scm-smc.c, as bsd/dev/redwood_adreno.c calls it) ---- */

#define SCM_FAST                (1ull << 31)
#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_IS_CALL_AVAILABLE   SCM_CALL(6, 1)
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_IS_SUPPORTED    SCM_CALL(2, 7)
#define SCM_ONE_VALUE           1
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)            /* a number, and the address of memory TrustZone may read and write */
#define SCM_INTERRUPTED         1
#define PAS_GPU                 13

static int32_t
scm2(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t *answer);

static uint32_t scm_before_counts[1024];

/* which interrupts this kernel took while the call went on (gic_v3_irq.c counts them all) */
static void
scm_interrupts(void)
{
	extern uint32_t gic_intid_counts[1024];
	uint32_t shown = 0;

	for (uint32_t n = 0; n < 1024; n++) {
		uint32_t more = gic_intid_counts[n] - scm_before_counts[n];

		if (more != 0 && shown++ < 12) {
			printf("a610: ... meanwhile interrupt %u came %u times\n", n, more);
		}
	}
	if (shown == 0) {
		printf("a610: ... meanwhile this kernel took NO interrupt at all\n");
	}
}

static int32_t
scm3(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t *answer);

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t *answer)
{
	return scm3(what, function, arguments, first, 0, 0, answer);
}

static int32_t
scm2(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t *answer)
{
	return scm3(what, function, arguments, first, second, 0, answer);
}

static int32_t
scm3(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t *answer)
{
	/*
	 * Resumed as mainline's qcom_scm-smc.c does it: the call's own x1-x5, x6 the session TrustZone
	 * gave back (ARM_SMCCC_QUIRK_QCOM_A6).
	 */
	uint64_t session = 0;
	extern uint32_t gic_intid_counts[1024];

	memcpy(scm_before_counts, gic_intid_counts, sizeof(scm_before_counts));

	printf("a610: about to ask TrustZone %s: smc %#llx (%#llx)\n", what, function, first);
	uint64_t deadline;

	/*
	 * A call that takes TrustZone a while (checking a signature) comes back "interrupted" whenever
	 * an interrupt is due, and is resumed so: as often as it takes, up to 20 s of it.
	 */
	/*
	 * Should it go on saying "interrupted": an interrupt pending for this side that keeps coming
	 * would do that -- on willow it was PPI 27, taken for the timer and asserted for ever (see
	 * gic_v3_irq.c's gic_vtimer); the interrupts taken meanwhile are said at the end. The first few times and the last say how soon it came back and what this
	 * processor has pending (ICC_HPPIR1_EL1: the highest group 1 interrupt; ISR_EL1; DAIF).
	 */
	uint64_t started = mach_absolute_time(), before = started, i = 0;

	clock_interval_to_deadline(20, NSEC_PER_SEC, &deadline);
	for (i = 0; i == 0 || mach_absolute_time() < deadline; i++) {
		if (i > 0 && (i <= 4 || (i & (i - 1)) == 0)) {
			uint64_t now = mach_absolute_time(), ns;

			absolutetime_to_nanoseconds(now - before, &ns);
			printf("a610: ... interrupted (%llu): %llu us since the last; pending %#llx, ISR %#llx, DAIF %#llx, on cpu %d\n", i,
			    ns / 1000, __builtin_arm_rsr64("S3_0_C12_C12_2"), __builtin_arm_rsr64("S3_0_C12_C1_0"),
			    __builtin_arm_rsr64("DAIF"), cpu_number());
		}
		before = mach_absolute_time();
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("a610: TrustZone returns %d, answer %#llx (after %llu resumptions)\n", (int32_t)x0, x1, i);
			if (i > 0) {
				scm_interrupts();
			}
			*answer = x1;
			return (int32_t)x0;
		}
		if (i < 4) {
			printf("a610: ... TrustZone gave back x1 %#llx x2 %#llx x3 %#llx x4 %#llx x5 %#llx x6 %#llx\n", x1, x2, x3,
			    x4, x5, x6);
		}
	}
	printf("a610: TrustZone still says the call was interrupted after 20 s (%llu times; pending %#llx, ISR %#llx)\n", i,
	    __builtin_arm_rsr64("S3_0_C12_C12_2"), __builtin_arm_rsr64("S3_0_C12_C1_0"));
	scm_interrupts();
	(void)started;
	return -1;
}

/*
 * The rest of Linux's qcom_mdt_load for the zap shader, once TrustZone has taken its signature
 * (PAS_INIT_IMAGE). Its one loadable segment is relocatable (QCOM_MDT_RELOCATABLE, bit 27 of its
 * flags), so it goes where the device tree's pil_gpu_mem has room for it: gpu_region, 8 KB at
 * 0x57515000 ("no-map"; UEFI's map has it reserved). TrustZone is told where and how much first
 * (PAS_MEM_SETUP), the segments are copied there from their files (.b00 to .b02 of the small
 * image, zeros past what a file has up to the segment's memory size), and then TrustZone
 * authenticates and starts it (PAS_AUTH_AND_RESET). After that CP_SET_SECURE_MODE 0 takes the
 * core out of secure mode, as a6xx_hw_init has it (step 11).
 */
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_THREE_VALUES        3
#define ZAP_REGION_PHYS         0x57515000UL
#define ZAP_REGION_SIZE         0x2000UL
#define MDT_RELOCATABLE         (1u << 27)
#define MDT_TYPE(flags)         (((flags) >> 24) & 7)
#define MDT_TYPE_HASH           2

static bool zap_loaded;

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void
zap_load(const uint8_t *header, uint32_t segments)
{
	uint32_t low = ~0u, high = 0;
	bool relocatable = false;
	uint64_t answer = ~0ull;
	vm_offset_t region;

	for (uint32_t i = 0; i < segments; i++) {
		const uint8_t *ph = header + 52 + 32 * i;
		uint32_t paddr = le32(ph + 12), memsz = le32(ph + 20), flags = le32(ph + 24);

		if (le32(ph) != 1 || memsz == 0 || MDT_TYPE(flags) == MDT_TYPE_HASH) {
			continue;
		}
		relocatable |= (flags & MDT_RELOCATABLE) != 0;
		low = paddr < low ? paddr : low;
		high = paddr + memsz > high ? paddr + memsz : high;
	}
	high = (high + 0xfff) & ~0xfffu;
	printf("a610: the zap shader's segments: %#x to %#x, %s\n", low, high, relocatable ? "relocatable" : "NOT relocatable");
	if (low >= high || !relocatable || high - low > ZAP_REGION_SIZE) {
		printf("a610: not a zap shader that fits gpu_region (%#lx bytes at %#lx)\n", ZAP_REGION_SIZE, ZAP_REGION_PHYS);
		redwood_status(REDWOOD_STATUS_GPU, "FAILED 9: zap does not fit its region");
		return;
	}
	if (scm3("where the zap shader is to go", SCM_64 | SCM_SIP | SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_GPU, ZAP_REGION_PHYS,
	    high - low, &answer) != 0 || answer != 0) {
		redwood_status(REDWOOD_STATUS_GPU, "FAILED 9: TZ refuses the zap region (%#llx)", answer);
		return;
	}
	region = ml_io_map(ZAP_REGION_PHYS, ZAP_REGION_SIZE);
	for (uint32_t i = 0; i < segments; i++) {
		const uint8_t *ph = header + 52 + 32 * i;
		uint32_t paddr = le32(ph + 12), filesz = le32(ph + 16), memsz = le32(ph + 20), flags = le32(ph + 24);
		vm_offset_t to = region + (paddr - low);
		char name[24];
		uint64_t size = 0;
		const uint8_t *data;

		if (le32(ph) != 1 || memsz == 0 || MDT_TYPE(flags) == MDT_TYPE_HASH) {
			continue;
		}
		snprintf(name, sizeof(name), "a610_zap.b%02u", i);
		data = redwood_image_file("fw", name, &size, NULL);
		if (data == NULL || size < filesz) {
			printf("a610: no fw/%s of %u bytes in the small image\n", name, filesz);
			redwood_status(REDWOOD_STATUS_GPU, "FAILED 9: no %s", name);
			return;
		}
		for (uint32_t at = 0; at < memsz; at += 4) {
			uint32_t word = 0;

			for (uint32_t b = 0; b < 4; b++) {
				word |= at + b < filesz ? (uint32_t)data[at + b] << (8 * b) : 0;
			}
			*(volatile uint32_t *)(to + at) = word;
		}
		__builtin_arm_dsb(0xf);
		printf("a610: segment %u (%s, %u bytes, %u in memory) at %#lx: its first words read back %#x %#x\n", i, name,
		    filesz, memsz, ZAP_REGION_PHYS + (paddr - low), rd(to), rd(to + 4));
	}
	answer = ~0ull;
	if (scm("to authenticate and start the zap shader", SCM_64 | SCM_SIP | SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_GPU,
	    &answer) != 0 || answer != 0) {
		printf("a610: TrustZone does not start the zap shader (answer %#llx)\n", answer);
		redwood_status(REDWOOD_STATUS_GPU, "FAILED 9: TZ does not start zap (%#llx)", answer);
		return;
	}
	zap_loaded = true;
	printf("a610: THE ZAP SHADER IS IN: TrustZone has authenticated and started it\n");
	redwood_status(REDWOOD_STATUS_GPU, "zap shader IN");
}

/*
 * ---- the GPU kept on, for the system to draw with (a610_keep=1) ----
 *
 * As redwood_adreno.c's "GPU kept on": once the steps have the core out of secure mode and
 * writing memory through our tables (a610=12 or 13, a610_zap=1, a610_smmu=1), it is left so,
 * and the service RedwoodGPU (iokit/Kernel/RedwoodGPU.cpp) hands it to programs through the
 * interface of Linux's msm driver. redwood_adreno.c keeps what is not the GPU's -- the addresses
 * handed out, the boot-args gpu_* -- and asks here for the rest while a610_kept is set: the
 * parameters that are the A610's, pages entered into context 0, submissions and how far the
 * core has got. The caller holds one lock around all of these.
 *
 * A submission is what Linux's a6xx_submit puts in the ring, as redwood_adreno.c's: the time,
 * the colour and depth caches invalidated, the program's command buffers (CP_INDIRECT_BUFFER),
 * the time again, and its number written to the scratch page once the caches are in memory.
 *
 * a610_selftest=1: before the service starts, the picture of step 13 once more, but made the
 * way a program has it made -- addresses from redwood_gpu_address(), pages entered one by one,
 * the commands in a command buffer of their own, and a submission waited for by its number.
 */
#define GPU_CHIP_ID_A610        0x06010000ull   /* Linux's adreno_device.c, Mesa's freedreno_devices.py */
#define GPU_GMEM_A610           (128u * 1024 + 4096)    /* adreno_device.c: SZ_128K + SZ_4K */
#define KEPT_FENCE_AT           0x40            /* in the scratch page */
#define KEPT_TIMES_AT           0x400           /* ... and when the core began and ended its last submissions */
#define KEPT_TIMES              64
#define CP_ALWAYS_ON_COUNTER    0x0980          /* ticks of the 19.2 MHz crystal, 64 bits */
#define CP_REG_TO_MEM           0x3e
#define CP_INDIRECT_BUFFER      0x3f
#define CP_SCRATCH_REG2         0x0885
#define CP_IB1_BASE             GPU_WORD(0x0928)
#define CP_IB1_REM_SIZE         GPU_WORD(0x092a)
#define EVENT_INVALIDATE_DEPTH  0x18
#define EVENT_INVALIDATE_COLOR  0x19

bool a610_kept;
static uint32_t kept_fence, kept_timed, kept_timed_count, kept_timed_by[7];
static uint64_t kept_ticks;

extern uint64_t redwood_gpu_address(uint64_t size);
extern void redwood_gpu_keep(void);
extern void RedwoodGPUStart(void);

static vm_offset_t
kept_scratch(void)
{
	return core_memory + CORE_SCRATCH_OFFSET;
}

/* The answers to msm's GET_PARAM that are the A610's; the rest are redwood_adreno.c's (false here). */
bool
a610_gpu_parameter(uint32_t which, uint64_t *value)
{
	switch (which) {
	case 0x02:                              /* GMEM_SIZE */
		*value = GPU_GMEM_A610;
		return true;
	case 0x03:                              /* CHIP_ID */
		*value = GPU_CHIP_ID_A610;
		return true;
	case 0x04:                              /* MAX_FREQ */
		*value = (core_mhz ? core_mhz : 19) * 1000000ull;
		return true;
	case 0x10:                              /* HIGHEST_BANK_BIT: a6xx_calc_ubwc_config for the A610 */
		*value = 13;
		return true;
	case 0x12:                              /* UBWC_SWIZZLE */
		*value = 7;
		return true;
	case 0x13:                              /* MACROTILE_MODE */
		*value = 0;
		return true;
	default:
		return false;
	}
}

bool
a610_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	return space_map_as(address, phys, PTE_CORE | (read_only ? PTE_READ_ONLY : 0));
}

void
a610_gpu_mapped(void)
{
	space_commit();
}

void
a610_gpu_unmap(uint64_t address, uint64_t size)
{
	vm_offset_t cb = smmu + 0x8000;

	for (uint64_t offset = 0; offset < size; offset += 4096) {
		space_map_as(address + offset, 0, 0);
	}
	space_commit();
	wr(cb + SMMU_CB_TLBIASID, 0);
	wr(cb + SMMU_CB_TLBSYNC, 1);
	for (int i = 0; i < 1000 && (rd(cb + SMMU_CB_TLBSTATUS) & 1); i++) {
	}
}

bool
a610_gpu_done(uint32_t fence)
{
	volatile uint32_t *word = (volatile uint32_t *)(kept_scratch() + KEPT_FENCE_AT);

	to_memory((vm_offset_t)word, 4);
	return (int32_t)(*word - fence) >= 0;
}

uint32_t
a610_gpu_last(void)
{
	return kept_fence;
}

/* The core is to write down what time it is: at one end of submission `fence`. */
static void
kept_time(uint32_t fence, uint32_t end)
{
	uint64_t at = core_address + CORE_SCRATCH_OFFSET + KEPT_TIMES_AT + 16 * (fence % KEPT_TIMES) + 8 * end;

	ring_command(CP_REG_TO_MEM, 3);
	ring(CP_ALWAYS_ON_COUNTER | 2u << 18 | 1u << 30);       /* two words, a 64-bit register */
	ring((uint32_t)at);
	ring((uint32_t)(at >> 32));
}

/* Add up the times of the submissions the core has finished since this was last done (redwood_adreno.c's gpu_times). */
static void
kept_times(void)
{
	volatile uint64_t *times = (volatile uint64_t *)(kept_scratch() + KEPT_TIMES_AT);
	uint32_t done;

	to_memory(kept_scratch(), 4096);
	done = *(volatile uint32_t *)(kept_scratch() + KEPT_FENCE_AT);
	if (done - kept_timed > KEPT_TIMES) {
		kept_timed = done - KEPT_TIMES;
	}
	while (kept_timed != done) {
		uint32_t slot = ++kept_timed % KEPT_TIMES;

		if (kept_timed != 0 && times[2 * slot + 1] > times[2 * slot]) {
			static const uint32_t under[6] = { 1920, 4800, 9600, 19200, 38400, 76800 };
			uint64_t ticks = times[2 * slot + 1] - times[2 * slot];
			unsigned by = 0;

			while (by < 6 && ticks >= under[by]) {
				by++;
			}
			kept_timed_by[by]++;
			kept_ticks += ticks;
			kept_timed_count++;
		}
	}
}

void
a610_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
	kept_times();
	*microseconds = kept_ticks * 10 / 192;
	*submissions = kept_timed_count;
	memcpy(by, kept_timed_by, sizeof(kept_timed_by));
	bzero(kept_timed_by, sizeof(kept_timed_by));
	kept_ticks = 0;
	kept_timed_count = 0;
}

/* Command buffers for the core to run, in order: each an address and a count of words. 0: the ring stays full. */
uint32_t
a610_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	uint32_t wanted = 30 + 4 * count, room = 0;
	uint64_t fence_at = core_address + CORE_SCRATCH_OFFSET + KEPT_FENCE_AT;

	for (int i = 0; i < 20000; i++) {
		room = (rd(gpu + CP_RB_RPTR) + RING_WORDS - ring_at - 1) % RING_WORDS;
		if (room >= wanted) {
			break;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	if (room < wanted) {
		return 0;
	}
	kept_times();
	if (++kept_fence == 0) {
		kept_fence = 1;
	}
	kept_time(kept_fence, 0);
	ring_command(CP_EVENT_WRITE, 1);
	ring(EVENT_INVALIDATE_DEPTH);
	ring_command(CP_EVENT_WRITE, 1);
	ring(EVENT_INVALIDATE_COLOR);
	for (uint32_t i = 0; i < count; i++) {
		ring_command(CP_INDIRECT_BUFFER, 3);
		ring((uint32_t)addresses[i]);
		ring((uint32_t)(addresses[i] >> 32));
		ring(words[i]);
	}
	ring_command(CP_WAIT_FOR_IDLE, 0);
	kept_time(kept_fence, 1);
	ring_register(CP_SCRATCH_REG2, kept_fence);
	ring_command(CP_EVENT_WRITE, 4);
	ring(EVENT_CACHE_FLUSH);
	ring((uint32_t)fence_at);
	ring((uint32_t)(fence_at >> 32));
	ring(kept_fence);
	ring_send();
	return kept_fence;
}

void
a610_gpu_report(const char *why)
{
	printf("a610: gpu: %s: submission %u is the last, the core has done %u; ring: read up to %u, written up to %u; status "
	    "%#x, interrupts %#x; command processor: fault %#x, interrupts %#x, protection %#x; in a command buffer at %#llx, "
	    "%u words to go\n", why, kept_fence, *(volatile uint32_t *)(kept_scratch() + KEPT_FENCE_AT), rd(gpu + CP_RB_RPTR),
	    ring_at, rd(gpu + RBBM_STATUS), rd(gpu + RBBM_INT_0_STATUS), rd(gpu + CP_HW_FAULT), rd(gpu + CP_INTERRUPT_STATUS),
	    rd(gpu + CP_PROTECT_STATUS), (uint64_t)rd(gpu + CP_IB1_BASE + 4) << 32 | rd(gpu + CP_IB1_BASE),
	    rd(gpu + CP_IB1_REM_SIZE));
	(void)space_faulted("the submission");
}

/*
 * The run ends (redwood_gpu_stop(), from RedwoodGPUStop()): what the core was given it finishes
 * (half a second at most); then its command processor is stopped and context 0 switched off, so
 * that nothing of the GPU reads this kernel's memory any more.
 */
void
a610_gpu_quiesce(void)
{
	if (!a610_kept) {
		return;
	}
	a610_kept = false;
	wr(gpu + CP_SQE_CNTL, 0);
	wr(smmu + 0x8000, 0);                   /* context 0's SCTLR */
	wr(smmu + 0x800, 0);                    /* SMR 0: the GPU's stream matched no more */
}

void
a610_gpu_stop(void)
{
	uint32_t waited = 0;

	if (!a610_kept) {
		return;
	}
	while (!a610_gpu_done(kept_fence) && waited < 5000) {
		delay_for_interval(100, NSEC_PER_USEC);
		waited++;
	}
	printf("a610: the run is about to end: the GPU is given back (%s after %u us)\n",
	    a610_gpu_done(kept_fence) ? "it had finished what it was given" : "STILL AT WORK", 100 * waited);
	a610_gpu_quiesce();
}

/* Pages of ours, kernel memory, at addresses the allocator gives (a buffer as RedwoodGPU makes one). */
static uint64_t
kept_buffer(vm_offset_t *memory, uint32_t size)
{
	uint64_t address;

	if (kmem_alloc(kernel_map, memory, round_page(size), KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS || (address = redwood_gpu_address(size)) == 0) {
		return 0;
	}
	for (uint32_t offset = 0; offset < size; offset += 4096) {
		if (!a610_gpu_map(address + offset, kvtophys(*memory + offset), false)) {
			return 0;
		}
	}
	a610_gpu_mapped();
	return address;
}

static const uint32_t gen1_magic[][2] = {
	/* Mesa's a6xx_gen1_low_raw_magic_regs, the A610's */
	{ 0x9804, 0xf }, { 0x9805, 0 }, { 0xa0f8, 0 }, { 0xb600, 0 }, { 0x8600, 0 }, { 0xae03, 0 }, { 0xae00, 0 },
	{ 0xbe04, 0 }, { 0x9600, 0 }, { 0x0e12, 0x10000000 }, { 0x8e01, 0x1 }, { 0x8e04, 0x04100000 },
};

static void
kept_selftest(void)
{
	vm_offset_t picture, commands;
	uint64_t picture_at, commands_at, began, ended, ns;
	uint32_t size = PICTURE_ROW * PICTURE_HEIGHT, fence, count;
	const volatile uint8_t *pixel;
	bool done = false;

	printf("a610: self-test: the picture of step 13 as a program would have it drawn\n");
	if ((picture_at = kept_buffer(&picture, size)) == 0 || (commands_at = kept_buffer(&commands, 16384)) == 0) {
		printf("a610: self-test: no buffers\n");
		return;
	}
	to_memory(picture, size);
	emit_to = (uint32_t *)commands;
	emit_count = 0;
	for (unsigned i = 0; i < sizeof(gen1_magic) / sizeof(gen1_magic[0]); i++) {
		ring_register(gen1_magic[i][0], gen1_magic[i][1]);
	}
	ring_command(CP_SET_MARKER, 1);
	ring(12);
	ring_register(RB_CCU_CNTL, A610_CCU_CNTL_BYPASS | 1);
	ring_fill_at(picture_at, 0, 780, 0, 160, 0);                    /* green */
	ring_fill_at(picture_at, 780, 1560, 255, 255, 255);
	ring_fill_at(picture_at, 1560, PICTURE_HEIGHT, 255, 0, 160);    /* pink */
	ring_command(CP_EVENT_WRITE, 4);
	ring(EVENT_FLUSH_COLOR);
	ring((uint32_t)(core_address + CORE_SCRATCH_OFFSET + 32));
	ring((uint32_t)((core_address + CORE_SCRATCH_OFFSET + 32) >> 32));
	ring(0x5e1f7e57);
	count = emit_count;
	emit_to = NULL;
	to_memory(commands, 4 * count);
	printf("a610: self-test: the picture at %#llx, %u words of commands at %#llx; the tables: %u\n", picture_at, count,
	    commands_at, space.count);
	began = mach_absolute_time();
	fence = a610_gpu_submit(&commands_at, &count, 1);
	for (int i = 0; fence != 0 && i < 20000 && !(done = a610_gpu_done(fence)); i++) {
		delay_for_interval(100, NSEC_PER_USEC);
	}
	ended = mach_absolute_time();
	absolutetime_to_nanoseconds(ended - began, &ns);
	if (!done) {
		a610_gpu_report("self-test");
		redwood_status(REDWOOD_STATUS_GPU, "self-test FAILED: submission %u not done", fence);
		return;
	}
	to_memory(picture, size);
	pixel = (const volatile uint8_t *)picture;
	printf("a610: self-test: submission %u done in %llu us; first pixel %02x %02x %02x %02x (told 00 a0 00 ff), middle "
	    "%02x %02x %02x %02x, last row %02x %02x %02x %02x (told a0 00 ff ff)\n", fence, ns / 1000, pixel[0], pixel[1],
	    pixel[2], pixel[3], pixel[1170 * PICTURE_ROW + 2160], pixel[1170 * PICTURE_ROW + 2161],
	    pixel[1170 * PICTURE_ROW + 2162], pixel[1170 * PICTURE_ROW + 2163], pixel[2339 * PICTURE_ROW + 4316],
	    pixel[2339 * PICTURE_ROW + 4317], pixel[2339 * PICTURE_ROW + 4318], pixel[2339 * PICTURE_ROW + 4319]);
	if (pixel[1] == 0xa0 && pixel[2339 * PICTURE_ROW + 4316] == 0xa0 && pixel[2339 * PICTURE_ROW + 4318] == 0xff) {
		MiniIOMobileFramebufferShow((const uint8_t *)picture, PICTURE_WIDTH, PICTURE_HEIGHT, PICTURE_ROW);
		printf("a610: SELF-TEST PASSED: a submission through a command buffer drew its picture (green, white, pink)\n");
		redwood_status(REDWOOD_STATUS_GPU, "kept on; self-test DRAWN");
	} else {
		redwood_status(REDWOOD_STATUS_GPU, "kept on; self-test picture wrong");
	}
}

/* At the end of the steps: the GPU kept on, if it is in a state for that and a610_keep=1 asks. */
static void
kept_start(bool good)
{
	uint32_t keep = 0, selftest = 0;

	if (!PE_parse_boot_argn("a610_keep", &keep, sizeof(keep)) || !keep) {
		return;
	}
	if (!good || !zap_loaded || core_address != CORE_ADDRESS) {
		printf("a610: not kept: the core must take commands out of secure mode through our tables "
		    "(a610=12 or more, a610_zap=1, a610_smmu=1)\n");
		return;
	}
	bzero((void *)kept_scratch(), 4096);
	to_memory(kept_scratch(), 4096);
	redwood_gpu_keep();                     /* the addresses, and the gpu_* boot-args */
	a610_kept = true;
	printf("a610: THE GPU STAYS ON for the system to draw with: the service RedwoodGPU\n");
	redwood_status(REDWOOD_STATUS_GPU, "kept on at %u MHz", core_mhz ? core_mhz : 19);
	if (PE_parse_boot_argn("a610_selftest", &selftest, sizeof(selftest)) && selftest) {
		kept_selftest();
	}
	RedwoodGPUStart();
}

extern volatile int redwood_rpm_done;

static void
a610_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter;
	bool cx_on, cx_clocks, core_good = false;

	{
		/* with rpm=N: the RPM's requests first (bsd/dev/redwood_rpm.c), the GPU's path to memory runs on its buses */
		uint32_t rpm = 0;

		if (PE_parse_boot_argn("rpm", &rpm, sizeof(rpm)) && rpm != 0) {
			for (int i = 0; i < 500 && !redwood_rpm_done; i++) {
				delay_for_interval(10, NSEC_PER_MSEC);
			}
			printf("a610: the RPM's requests are %s\n", redwood_rpm_done ? "done" : "not done after 5 s");
		}
	}
	redwood_status(REDWOOD_STATUS_GPU, "probing (a610=%u)", steps);
	printf("a610: step 1: the GPU's clocks in the global clock controller (%#lx)\n", GCC_PHYS);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	{
		static const struct named list[] = {
			{ "gcc_gpu_cfg_ahb_clk", 0x36004 }, { "gcc_gpu_memnoc_gfx_clk", 0x3600c },
			{ "gcc_gpu_snoc_dvm_gfx_clk", 0x36018 }, { "gcc_gpu_throttle_xo_clk", 0x36044 },
			{ "gcc_bimc_gpu_axi_clk", 0x71154 },
			/* the crypto engine TrustZone checks signatures with (step 9): votes 3-5 at 0x79004, its core clock the RPM's */
			{ "gcc_ce1_clk", 0x27004 }, { "gcc_ce1_axi_clk", 0x27008 }, { "gcc_ce1_ahb_clk", 0x2700c },
		};

		branches("gcc", gcc, list, sizeof(list) / sizeof(list[0]));
		printf("a610: gcc votes at 0x79004 = %#010x (GPLL0 to the GPU: bits 15, 16; throttle core: 14)\n",
		    rd(gcc + 0x79004));
	}
	if (steps < 2) {
		goto out;
	}

	printf("a610: step 2: the GPU's own clock controller (%#lx)\n", GPUCC_PHYS);
	gpucc = ml_io_map(GPUCC_PHYS, GPUCC_SIZE);
	for (uint32_t pll = 0; pll < 2; pll++) {
		vm_offset_t p = gpucc + 0x100 * pll;

		printf("a610: PLL%u: mode %#010x, L %#x, alpha %#x %#x, user %#x %#x, config %#x %#x, status %#x\n", pll,
		    rd(p + 0x00), rd(p + 0x04), rd(p + 0x08), rd(p + 0x0c), rd(p + 0x10), rd(p + 0x14), rd(p + 0x18),
		    rd(p + 0x1c), rd(p + 0x24));
	}
	printf("a610: clock source gx_gfx3d (0x101c): command %#x, configuration %#x\n", rd(gpucc + 0x101c),
	    rd(gpucc + 0x1020));
	printf("a610: clock source gmu (0x1120): command %#x, configuration %#x\n", rd(gpucc + 0x1120), rd(gpucc + 0x1124));
	{
		static const struct named list[] = {
			{ "gpu_cc_cxo_aon_clk", 0x1004 }, { "gpu_cc_gx_gfx3d_clk", 0x1054 }, { "gpu_cc_ahb_clk", 0x1078 },
			{ "gpu_cc_crc_ahb_clk", 0x107c }, { "gpu_cc_cx_apb_clk", 0x1088 }, { "gpu_cc_cx_snoc_dvm_clk", 0x108c },
			{ "gpu_cc_sleep_clk", 0x1090 }, { "gpu_cc_cx_gmu_clk", 0x1098 }, { "gpu_cc_cxo_clk", 0x109c },
			{ "gpu_cc_cx_gfx3d_clk", 0x10a4 }, { "gpu_cc_hlos1_vote_gpu_smmu", 0x5000 },
		};

		branches("gpucc", gpucc, list, sizeof(list) / sizeof(list[0]));
	}
	cx_on = gdsc("CX", 0x106c);
	redwood_status(REDWOOD_STATUS_GPU, "probe: CX %s, GX %s; not drawing yet", cx_on ? "on" : "off",
	    gdsc("GX", 0x100c) ? "on" : "off");
	printf("a610: CX's hardware control (0x1540) = %#x\n", rd(gpucc + 0x1540));
	cx_clocks = !(rd(gpucc + 0x1078) & CLOCK_OFF) && !(rd(gcc + 0x36004) & CLOCK_OFF);
	if (steps < 3) {
		goto out;
	}

	printf("a610: step 3: the fuses (%#lx)\n", QFPROM_PHYS);
	qfprom = ml_io_map(QFPROM_PHYS, QFPROM_SIZE);
	/* qcom,gpu-gaming-bin = <0x6018 0x80 7>: a bit at 0x6018 */
	printf("a610: fuse word 0x6018 = %#010x (the gaming bin: bit 7 is %u)\n", rd(qfprom + 0x6018),
	    (rd(qfprom + 0x6018) >> 7) & 1);
	if (steps < 4) {
		goto out;
	}

	printf("a610: step 4: the bus clocks, CX's power switch, the clocks of the CX half\n");
	redwood_status(REDWOOD_STATUS_GPU, "step 4: CX power");
	(void)cx_clocks;
	if (!clock_on("gcc_gpu_memnoc_gfx_clk", gcc, 0x3600c, false) || !clock_on("gcc_bimc_gpu_axi_clk", gcc, 0x71154, false) ||
	    !clock_on("gcc_sys_noc_compute_sf_axi_clk", gcc, 0x1050c, true) || !clock_on("gcc_gpu_snoc_dvm_gfx_clk", gcc, 0x36018, false)) {
		redwood_status(REDWOOD_STATUS_GPU, "FAILED at step 4: a bus clock");
		goto out;
	}
	if (!cx_on && !gdsc_on("CX", 0x106c)) {
		redwood_status(REDWOOD_STATUS_GPU, "FAILED at step 4: CX does not come on");
		goto out;
	}
	/*
	 * Linux (kgsl) has CX's power switch on before it enables these, and before it they did not run
	 * even with the RPM's buses asked for: so once more, now that CX is on. bimc_gpu_axi has no
	 * status to go by (BRANCH_HALT_DELAY in gcc-sm6125.c); memnoc_gfx and snoc_dvm_gfx do.
	 */
	delay_for_interval(100, NSEC_PER_USEC);
	(void)clock_on("gcc_gpu_memnoc_gfx_clk, with CX on", gcc, 0x3600c, true);
	(void)clock_on("gcc_gpu_snoc_dvm_gfx_clk, with CX on", gcc, 0x36018, true);
	(void)clock_on("gcc_bimc_gpu_axi_clk, with CX on", gcc, 0x71154, false);
	/* the GMU wrapper's clock: the vendor's wake and sleep counts (0xf each) before it is switched on */
	wr(gpucc + 0x1098, (rd(gpucc + 0x1098) & ~0xff0u) | 0xf << 8 | 0xf << 4);
	if (!clock_on("gpu_cc_ahb_clk", gpucc, 0x1078, true) || !clock_on("gpu_cc_cxo_clk", gpucc, 0x109c, true) ||
	    !clock_on("gpu_cc_cx_gmu_clk", gpucc, 0x1098, false) || !clock_on("gpu_cc_hlos1_vote_gpu_smmu", gpucc, 0x5000, false)) {
		redwood_status(REDWOOD_STATUS_GPU, "FAILED at step 4: a CX clock");
		goto out;
	}
	redwood_status(REDWOOD_STATUS_GPU, "CX on; GX off");
	if (steps < 5) {
		goto out;
	}

	printf("a610: step 5: GX's power switch, and the 3D core's clock from the crystal\n");
	redwood_status(REDWOOD_STATUS_GPU, "step 5: GX power");
	if (!gdsc_on("GX", 0x100c)) {
		redwood_status(REDWOOD_STATUS_GPU, "FAILED at step 5: GX does not come on");
		goto out;
	}
	/* gx_gfx3d_clk_src: input 0 (the crystal), no divider; then "update" and wait for it to take */
	wr(gpucc + 0x1020, rd(gpucc + 0x1020) & ~0x71fu);
	wr(gpucc + 0x101c, rd(gpucc + 0x101c) | 1);
	for (int i = 0; i < 500 && (rd(gpucc + 0x101c) & 1); i++) {
		delay_for_interval(10, NSEC_PER_USEC);
	}
	printf("a610: the 3D core's clock source: command %#x, configuration %#x\n", rd(gpucc + 0x101c), rd(gpucc + 0x1020));
	{
		uint32_t mhz = 0;

		if (PE_parse_boot_argn("a610_mhz", &mhz, sizeof(mhz)) && mhz != 0) {
			core_mhz = core_speed(mhz);
		}
	}
	if (!clock_on("gpu_cc_gx_gfx3d_clk", gpucc, 0x1054, true)) {
		redwood_status(REDWOOD_STATUS_GPU, "FAILED at step 5: the core clock does not run");
		goto out;
	}
	if (core_mhz != 0) {
		/* the branch keeps the source on from here: no longer forced (as clk_rcg2_clear_force_enable) */
		wr(gpucc + 0x101c, rd(gpucc + 0x101c) & ~RCG_ROOT_EN);
		printf("a610: the 3D core's clock source after: command %#x, configuration %#x\n", rd(gpucc + 0x101c),
		    rd(gpucc + 0x1020));
		redwood_status(REDWOOD_STATUS_GPU, "CX and GX on, core at %u MHz", core_mhz);
	} else {
		redwood_status(REDWOOD_STATUS_GPU, "CX and GX on, core at 19.2 MHz");
	}
	if (steps < 6) {
		goto out;
	}

	printf("a610: step 6: the first registers of the GPU block (%#lx)\n", GPU_PHYS);
	gpu = ml_io_map(GPU_PHYS, GPU_SIZE);
	/* in words: A6XX_RBBM_STATUS 0x210, A6XX_RBBM_STATUS3 0x213, A6XX_CP_SQE_INSTR_BASE 0x830 */
	printf("a610: RBBM_STATUS %#x, RBBM_STATUS3 %#x\n", rd(gpu + 4 * 0x210), rd(gpu + 4 * 0x213));
	printf("a610: CP_SQE_INSTR_BASE %#x:%#x\n", rd(gpu + 4 * 0x830), rd(gpu + 4 * 0x831));
	redwood_status(REDWOOD_STATUS_GPU, "on at 19.2 MHz, registers answer (RBBM %#x)", rd(gpu + 4 * 0x210));
	if (steps < 7) {
		goto out;
	}

	printf("a610: step 7: the GPU's SMMU (%#lx), as the hypervisor and the firmware left it\n", SMMU_PHYS);
	redwood_status(REDWOOD_STATUS_GPU, "step 7: reading its SMMU");
	smmu = ml_io_map(SMMU_PHYS, SMMU_SIZE);
	{
		/* ARM SMMU v2: IDR0 [7:0] stream match registers; IDR1 [31] 64 KB pages, [30:28] log2 pages - 1, [7:0] contexts */
		uint32_t idr0 = rd(smmu + 0x20), idr1 = rd(smmu + 0x24), page = idr1 >> 31 ? 0x10000 : 0x1000;
		uint32_t pages = 2u << (idr1 >> 28 & 7), contexts = idr1 & 0xff, matches = idr0 & 0xff;

		printf("a610: SMMU sCR0 %#x, IDR0 %#x, IDR1 %#x, IDR2 %#x, IDR7 %#x, sGFSR %#x\n", rd(smmu + 0x0), idr0, idr1,
		    rd(smmu + 0x28), rd(smmu + 0x3c), rd(smmu + 0x48));
		printf("a610: SMMU: %u stream match registers, %u contexts, %u pages of %#x before the contexts' own\n", matches,
		    contexts, pages, page);
		if (2 * pages * page != SMMU_SIZE) {
			printf("a610: that makes %#x of registers, the device tree says %#lx: not reading further\n", 2 * pages * page,
			    SMMU_SIZE);
			redwood_status(REDWOOD_STATUS_GPU, "SMMU: layout not as expected");
			goto out;
		}
		for (uint32_t i = 0; i < matches && i < 16; i++) {
			printf("a610: SMMU stream %u: SMR %#x, S2CR %#x (%s)\n", i, rd(smmu + 0x800 + 4 * i), rd(smmu + 0xc00 + 4 * i),
			    (rd(smmu + 0xc00 + 4 * i) >> 16 & 3) == 0 ? "translate" : (rd(smmu + 0xc00 + 4 * i) >> 16 & 3) == 1 ?
			    "bypass" : "fault");
		}
		for (uint32_t i = 0; i < contexts && i < 16; i++) {
			printf("a610: SMMU context %u: CBAR %#x\n", i, rd(smmu + page + 4 * i));
		}
		{
			vm_offset_t cb0 = smmu + pages * page;

			printf("a610: SMMU context 0: SCTLR %#x, TTBR0 %#x:%#x, TCR %#x, FSR %#x\n", rd(cb0 + 0x00), rd(cb0 + 0x24),
			    rd(cb0 + 0x20), rd(cb0 + 0x30), rd(cb0 + 0x58));
		}
		redwood_status(REDWOOD_STATUS_GPU, "on at 19.2 MHz; SMMU: %u contexts, read", contexts);
	}
	if (steps < 8) {
		goto out;
	}

	printf("a610: step 8: questions to TrustZone that pass no memory\n");
	redwood_status(REDWOOD_STATUS_GPU, "step 8: asking TrustZone");
	{
		uint64_t answer = 0, how = SCM_FAST | SCM_64 | SCM_SIP;
		uint32_t supported = 0;

		if (scm("whether it takes calls", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE, SCM_SIP | SCM_IS_CALL_AVAILABLE,
		    &answer) != 0 || answer != 1) {
			how = SCM_FAST | SCM_SIP;
			if (scm("the same, the 32-bit way", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
			    SCM_SIP | SCM_IS_CALL_AVAILABLE, &answer) != 0 || answer != 1) {
				printf("a610: TrustZone does not answer the way it answers Linux\n");
				redwood_status(REDWOOD_STATUS_GPU, "FAILED at step 8: TrustZone does not answer");
				goto out;
			}
		}
		scm("for the call that takes an image's signature", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
		    SCM_SIP | SCM_PAS_INIT_IMAGE, &answer);
		scm("for the call that authenticates and starts an image", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
		    SCM_SIP | SCM_PAS_AUTH_AND_RESET, &answer);
		answer = 0;
		if (scm("whether it supports the GPU's zap shader", how | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, PAS_GPU, &answer) == 0 &&
		    answer) {
			supported = 1;
		}
		answer = 0;
		if (scm("the same, as a call that may be preempted", (how & ~SCM_FAST) | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, PAS_GPU,
		    &answer) == 0 && answer) {
			supported |= 2;
		}
		printf("a610: TrustZone %s the GPU's zap shader (%u)\n", supported ? "lists" : "does not list", supported);
		{
			/* which images does it list at all: a "no" for 13 means something only if it says yes to others */
			char list[96];
			unsigned n = 0;

			list[0] = 0;
			for (uint64_t id = 0; id < 25; id++) {
				answer = 0;
				if (scm("whether it supports image", (how & ~SCM_FAST) | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, id, &answer) == 0 &&
				    answer && n + 4 < sizeof(list)) {
					n += (unsigned)snprintf(list + n, sizeof(list) - n, " %llu", id);
				}
			}
			printf("a610: TrustZone supports the images:%s\n", n ? list : " none at all");
			redwood_status(REDWOOD_STATUS_GPU, "TZ supports images:%s", n ? list : " none");
		}
		redwood_status(REDWOOD_STATUS_GPU, "on at 19.2 MHz; TrustZone %s its zap shader", supported ? "lists" : "does NOT list");
	}
	if (steps < 9) {
		goto out;
	}

	printf("a610: step 9: TrustZone checks the zap shader's signature\n");
	{
		/* a610_zap=1: here TrustZone answers "interrupted" for 20 s and more, so the step is left out unless asked for */
		uint32_t zap = 0;

		PE_parse_boot_argn("a610_zap", &zap, sizeof(zap));
		if (!zap) {
			printf("a610: left out (a610_zap=1 to ask)\n");
			goto step10;
		}
	}
	{
		/* the timer as every processor has taken it so far: TrustZone stops for what this side has pending */
		extern void gic_v3_timer_report(int (*out)(const char *, ...));

		gic_v3_timer_report(say);
	}
	redwood_status(REDWOOD_STATUS_GPU, "step 9: zap shader signature");
	{
		uint64_t size = 0, answer = ~0ull;
		const uint8_t *header = redwood_image_file("fw", "a610_zap.mdt", &size, NULL);
		uint32_t segments, hashes;
		vm_offset_t page = 0;
		int32_t result;

		if (header == NULL) {
			printf("a610: no fw/a610_zap.mdt in the small image: put the phone's /vendor/firmware/a610_zap.* into fw-a610/\n");
			redwood_status(REDWOOD_STATUS_GPU, "FAILED at step 9: no zap shader file");
			goto out;
		}
		/* a 32-bit ELF: segments at 44, the table at 52 (32 bytes each); the first segment, the hashes, its size at +16 */
		segments = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
		memcpy(&hashes, header + 52 + 16, sizeof(hashes));
		if (memcmp(header, "\177ELF\1", 5) != 0 || segments < 2 || size > PAGE_SIZE) {
			printf("a610: fw/a610_zap.mdt is not such a header\n");
			redwood_status(REDWOOD_STATUS_GPU, "FAILED at step 9: zap header not an ELF");
			goto out;
		}
		printf("a610: an ELF header of %u segments and %llu bytes after its table\n", segments, size - (52 + 32 * segments));
		if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			goto out;
		}
		memcpy((void *)page, header, size);
		/* TrustZone reads memory, not this processor's cache */
		for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		__builtin_arm_dsb(0xf);
		/* never freed: TrustZone protects the page while it works, and it is not touched again */
		result = scm2("to take the zap shader's signature", SCM_64 | SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_GPU,
		    kvtophys(page), &answer);
		if (result == -1 && answer == ~0ull && kvtophys(page) < (1ull << 32)) {
			/*
			 * -1 in both words is SMCCC's "not supported": the call itself, not the image. The fast calls of step 8
			 * went the 64-bit way; this one is asked again the 32-bit way (the page is below 4 GB).
			 */
			answer = ~0ull;
			result = scm2("the same, the 32-bit way", SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_GPU,
			    kvtophys(page), &answer);
		}
		if (result == 0 && answer == 0) {
			printf("a610: TRUSTZONE ACCEPTS THE ZAP SHADER'S SIGNATURE\n");
			redwood_status(REDWOOD_STATUS_GPU, "on; TrustZone ACCEPTS the zap shader's signature");
			zap_load(header, segments);
		} else {
			printf("a610: TrustZone does not take it: the call returns %d, its answer %#llx\n", result, answer);
			redwood_status(REDWOOD_STATUS_GPU, "FAILED 9: TZ refuses zap: %d, %#llx", result, answer);
		}
		/* (the steps after this one go on either way: 12 does without the zap shader) */
	}
	if (steps < 10) {
		goto out;
	}

step10:
	printf("a610: step 10: memory for the 3D core: its ring, its microcode, a page to write to\n");
	redwood_status(REDWOOD_STATUS_GPU, "step 10: ring and microcode");
	{
		uint64_t size = 0;
		const uint8_t *microcode = redwood_image_file("fw", "a630_sqe.fw", &size, NULL);

		if (microcode == NULL || size < 8 || size - 4 > CORE_SQE_MAX - 0) {
			printf("a610: no usable fw/a630_sqe.fw in the small image\n");
			redwood_status(REDWOOD_STATUS_GPU, "FAILED 10: no microcode file");
			goto out;
		}
		if (kmem_alloc_contig(kernel_map, &core_memory, CORE_MEMORY_SIZE, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("a610: no memory in one piece\n");
			redwood_status(REDWOOD_STATUS_GPU, "FAILED 10: no memory");
			goto out;
		}
		core_phys = kvtophys(core_memory);
		/* one piece in physical memory as well: the last page where the first says */
		if (kvtophys(core_memory + CORE_MEMORY_SIZE - PAGE_SIZE) != core_phys + CORE_MEMORY_SIZE - PAGE_SIZE) {
			printf("a610: the memory is not in one piece physically\n");
			goto out;
		}
		memcpy((void *)(core_memory + CORE_SQE_OFFSET), microcode + 4, size - 4);  /* the first word is not microcode */
		to_memory(core_memory, CORE_MEMORY_SIZE);
		printf("a610: ring at %#llx, microcode (%llu bytes) at %#llx, the page at %#llx\n", core_phys, size - 4,
		    core_phys + CORE_SQE_OFFSET, core_phys + CORE_SCRATCH_OFFSET);
	}
	if (steps < 11) {
		goto out;
	}

	printf("a610: step 11: the 3D core's registers as Linux writes them for an A610, then its command processor started\n");
	redwood_status(REDWOOD_STATUS_GPU, "step 11: starting the command processor");
	{
		static const uint32_t address_modes[] = { 0x0842, 0x0c01, 0x8601, 0x8e05, 0x9e01, 0xbe05, 0xa601, 0x9601,
			                                  0x0e00, 0xae01, 0xb601, 0xf810 };

		/*
		 * The bus interface out of halt, both of its halves (Linux's hw_init, a6xx_has_gbif): while it is
		 * halted nothing of the core reaches memory, and a command processor waiting for its microcode
		 * or its ring says nothing about it.
		 */
		uint32_t bypass = 0, translate = 0;

		PE_parse_boot_argn("a610_bypass", &bypass, sizeof(bypass));
		PE_parse_boot_argn("a610_smmu", &translate, sizeof(translate));
		core_address = core_phys;
		if (translate && smmu != 0) {
			/* our tables: the ring, the microcode and the page at CORE_ADDRESS, nothing else yet */
			if (!space_block(CORE_ADDRESS, core_memory, CORE_MEMORY_SIZE) || !space_on()) {
				redwood_status(REDWOOD_STATUS_GPU, "FAILED 11: the SMMU does not take our tables");
				goto out;
			}
			core_address = CORE_ADDRESS;
		} else if (bypass && smmu != 0) {
			/*
			 * SMMU v2: S2CR TYPE [17:16] (0 translate, 1 bypass), CBNDX [7:0]; SMR VALID [31], MASK [30:16],
			 * ID [14:0]. A bypass S2CR (the first try) came back as "fault" and the SMR not taken: Qualcomm's
			 * hypervisor does not allow bypass. Linux's way round it (arm-smmu-qcom.c, bypass_quirk): the
			 * last context, of type "stage 1 translation, stage 2 bypass" with its translation off
			 * (SCTLR 0), and the streams sent to it as to any context.
			 */
			uint32_t cb = 4;
			vm_offset_t cbase = smmu + 0x8000 + 0x1000 * cb;

			printf("a610: SMMU stream 0 before: SMR %#x, S2CR %#x; context %u: CBAR %#x, SCTLR %#x; sCR0 %#x\n",
			    rd(smmu + 0x800), rd(smmu + 0xc00), cb, rd(smmu + 0x1000 + 4 * cb), rd(cbase), rd(smmu + 0x0));
			wr(cbase + 0x00, 0);                            /* SCTLR: translation off */
			wr(cbase + 0x58, rd(cbase + 0x58));             /* FSR: clear what is there */
			wr(smmu + 0x1800 + 4 * cb, 1);                  /* CBA2R: 64-bit */
			wr(smmu + 0x1000 + 4 * cb, 1u << 16);           /* CBAR: S1 translation, S2 bypass */
			wr(smmu + 0xc00, cb);                           /* S2CR: translate, context 4 */
			/*
			 * SMR: SIDs 0-3 (mask 3) was not taken, the rest was -- SID 2 is the secure stream, not the
			 * HLOS's. So the device tree's own gfx3d_user (SID 0 mask 1), then SID 0 alone.
			 */
			{
				static const uint32_t smrs[] = { 1u << 31 | 1u << 16 | 0, 1u << 31 | 0 };

				for (unsigned i = 0; i < 2; i++) {
					wr(smmu + 0x800, smrs[i]);
					__builtin_arm_dsb(0xf);
					printf("a610: SMMU SMR 0 = %#x: reads %#x (%s)\n", smrs[i], rd(smmu + 0x800),
					    rd(smmu + 0x800) == smrs[i] ? "taken" : "NOT taken");
					if (rd(smmu + 0x800) == smrs[i]) {
						break;
					}
				}
			}
			printf("a610: SMMU stream 0 after: SMR %#x, S2CR %#x; context %u: CBAR %#x, CBA2R %#x, SCTLR %#x (%s); sGFSR %#x\n",
			    rd(smmu + 0x800), rd(smmu + 0xc00), cb, rd(smmu + 0x1000 + 4 * cb), rd(smmu + 0x1800 + 4 * cb), rd(cbase),
			    (rd(smmu + 0x800) >> 31) && (rd(smmu + 0xc00) & 0x300ff) == cb ?
			    "the GPU's stream to a context that does not translate" : "the SMMU did NOT take it", rd(smmu + 0x48));
		}
		printf("a610: bus interface before: GBIF halt %#x (ack %#x), RBBM's halt %#x (ack %#x)\n", rd(gpu + GBIF_HALT),
		    rd(gpu + GBIF_HALT_ACK), rd(gpu + RBBM_GBIF_HALT), rd(gpu + RBBM_GBIF_HALT_ACK));
		wr(gpu + GBIF_HALT, 0);
		(void)rd(gpu + GBIF_HALT);
		wr(gpu + RBBM_GBIF_HALT, 0);
		(void)rd(gpu + RBBM_GBIF_HALT);
		printf("a610: bus interface after: GBIF halt %#x (ack %#x), RBBM's halt %#x (ack %#x)\n", rd(gpu + GBIF_HALT),
		    rd(gpu + GBIF_HALT_ACK), rd(gpu + RBBM_GBIF_HALT), rd(gpu + RBBM_GBIF_HALT_ACK));
		wr(gpu + RBBM_SECVID_TSB_CNTL, 0);
		put64(gpu + RBBM_SECVID_TSB_TRUSTED_BASE, 0);
		wr(gpu + RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
		for (unsigned i = 0; i < sizeof(address_modes) / sizeof(address_modes[0]); i++) {
			wr(gpu + GPU_WORD(address_modes[i]), 1);        /* 64-bit addresses */
		}
		for (unsigned i = 0; i < 4; i++) {
			wr(gpu + GBIF_QSB_SIDE0 + 4 * i, 0x00071620);
		}
		wr(gpu + RBBM_GBIF_CLIENT_QOS_CNTL, 3);
		wr(gpu + RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
		put64(gpu + UCHE_WRITE_RANGE_MAX, 0x0001ffffffffffc0ull);
		put64(gpu + UCHE_TRAP_BASE, 0x0001fffffffff000ull);
		put64(gpu + UCHE_WRITE_THRU_BASE, 0x0001fffffffff000ull);
		put64(gpu + UCHE_GMEM_RANGE_MIN, 0x00100000);
		put64(gpu + UCHE_GMEM_RANGE_MAX, 0x00100000 + 0x20000 - 1);     /* the A610's 128 KB of GMEM */
		wr(gpu + UCHE_FILTER_CNTL, 0x804);
		wr(gpu + UCHE_CACHE_WAYS, 0x4);
		wr(gpu + CP_ROQ_THRESHOLDS_2, 0x00800060);                     /* the A610's */
		wr(gpu + CP_ROQ_THRESHOLDS_1, 0x40201b16);
		wr(gpu + CP_MEM_POOL_SIZE, 48);
		wr(gpu + PC_DBG_ECO_CNTL, 0x00080000);
		wr(gpu + CP_AHB_CNTL, 1);
		wr(gpu + RBBM_PERFCTR_CNTL, 1);
		wr(gpu + CP_PERFCTR_CP_SEL_0, PERF_CP_ALWAYS_COUNT);
		wr(gpu + RBBM_INTERFACE_HANG_INT_CNTL, 1u << 30 | 0x3ffff);    /* the A610's */
		wr(gpu + UCHE_CLIENT_PF, 1u << 7 | 1);
		put64(gpu + CP_SQE_INSTR_BASE, core_address + CORE_SQE_OFFSET);
		put64(gpu + CP_RB_BASE, core_address);
		wr(gpu + CP_RB_CNTL, CP_RB_CNTL_VALUE);
		printf("a610: microcode at %#x:%#x, ring at %#x:%#x, ring control %#x, status %#x\n", rd(gpu + CP_SQE_INSTR_BASE + 4),
		    rd(gpu + CP_SQE_INSTR_BASE), rd(gpu + CP_RB_BASE + 4), rd(gpu + CP_RB_BASE), rd(gpu + CP_RB_CNTL),
		    rd(gpu + RBBM_STATUS));
		smmu_faults("before");
		printf("a610: next: start the command processor (CP_SQE_CNTL = 1)\n");
		wr(gpu + CP_SQE_CNTL, 1);
		delay_for_interval(20, NSEC_PER_MSEC);
		printf("a610: alive after it; status %#x, interrupts %#x, command processor: fault %#x, interrupts %#x, "
		    "protection %#x, ring read up to %u\n", rd(gpu + RBBM_STATUS), rd(gpu + RBBM_INT_0_STATUS), rd(gpu + CP_HW_FAULT),
		    rd(gpu + CP_INTERRUPT_STATUS), rd(gpu + CP_PROTECT_STATUS), rd(gpu + CP_RB_RPTR));
		smmu_faults("after the start");
		ring_command(CP_ME_INIT, 8);            /* a6xx_cp_init */
		ring(0x0000002f);
		ring(0x00000003);                       /* several hardware contexts */
		ring(0x20000000);                       /* error detection */
		for (int i = 0; i < 5; i++) {
			ring(0);
		}
		if (!ring_run("initialize its command processor")) {
			smmu_faults("after initialize");
			redwood_status(REDWOOD_STATUS_GPU, "FAILED 11: CP does not run (%#x)", rd(gpu + RBBM_STATUS));
			goto out;
		}
		printf("a610: THE 3D CORE TAKES COMMANDS\n");
		redwood_status(REDWOOD_STATUS_GPU, "command processor RUNS");
		core_speed_measured();
		if (zap_loaded) {
			/* a6xx_hw_init, with the zap shader in: out of secure mode by packet */
			printf("a610: RBBM_SECVID_TRUST_CNTL is %#x before CP_SET_SECURE_MODE 0\n", rd(gpu + RBBM_SECVID_TRUST_CNTL));
			ring_command(CP_SET_SECURE_MODE, 1);
			ring(0);
			if (!ring_run("leave secure mode, the zap shader in (CP_SET_SECURE_MODE 0)")) {
				smmu_faults("after CP_SET_SECURE_MODE");
				redwood_status(REDWOOD_STATUS_GPU, "FAILED 11: CP_SET_SECURE_MODE 0 does not go through");
				goto out;
			}
			printf("a610: RBBM_SECVID_TRUST_CNTL is %#x after it%s\n", rd(gpu + RBBM_SECVID_TRUST_CNTL),
			    rd(gpu + RBBM_SECVID_TRUST_CNTL) == 0 ? ": THE 3D CORE IS OUT OF SECURE MODE" : "");
		}
	}
	if (steps < 12) {
		goto out;
	}

	printf("a610: step 12: a word written on command, in secure mode and then out of it by packet\n");
	redwood_status(REDWOOD_STATUS_GPU, "step 12: first write");
	{
		volatile uint32_t *page = (volatile uint32_t *)(core_memory + CORE_SCRATCH_OFFSET);
		uint64_t target = core_address + CORE_SCRATCH_OFFSET;
		static const uint32_t words[] = { 0x600dc0de, 0x600dc0d1 };
		bool wrote = false;

		printf("a610: RBBM_SECVID_TRUST_CNTL is %#x (never written: that hangs the bus here)\n",
		    rd(gpu + RBBM_SECVID_TRUST_CNTL));
		for (unsigned i = 0; i < 2 && !wrote; i++) {
			if (i == 1) {
				ring_command(CP_SET_SECURE_MODE, 1);
				ring(0);
				if (!ring_run("leave secure mode (CP_SET_SECURE_MODE 0)")) {
					smmu_faults("after CP_SET_SECURE_MODE");
					redwood_status(REDWOOD_STATUS_GPU, "FAILED 12: CP_SET_SECURE_MODE 0 does not go through");
					goto out;
				}
				printf("a610: RBBM_SECVID_TRUST_CNTL is %#x after it\n", rd(gpu + RBBM_SECVID_TRUST_CNTL));
			}
			page[0] = 0;
			to_memory((vm_offset_t)page, 64);
			ring_command(CP_MEM_WRITE, 3);
			ring((uint32_t)target);
			ring((uint32_t)(target >> 32));
			ring(words[i]);
			if (!ring_run(i == 0 && !zap_loaded ? "write a word to memory, in secure mode" : "write a word to memory, out of it")) {
				smmu_faults("after the write");
				redwood_status(REDWOOD_STATUS_GPU, "FAILED 12: write %u does not go through", i + 1);
				goto out;
			}
			to_memory((vm_offset_t)page, 64);
			wrote = page[0] == words[i];
			printf("a610: the page reads %#x: %s\n", page[0], wrote ? "THE 3D CORE WRITES OUR MEMORY ON COMMAND" :
			    "not what it was told to write");
			smmu_faults("after the write");
		}
		redwood_status(REDWOOD_STATUS_GPU, wrote ? "GPU RUNS COMMANDS and writes memory" : "FAILED 12: page reads %#x",
		    page[0]);
		core_good = wrote;
		if (!wrote) {
			goto out;
		}
	}
	if (steps < 13) {
		goto out;
	}

	printf("a610: step 13: a picture filled by the 2D engine, on the panel\n");
	redwood_status(REDWOOD_STATUS_GPU, "step 13: a picture");
	if (core_address != CORE_ADDRESS) {
		printf("a610: left out: 10 MB of pages need the SMMU's tables (a610_smmu=1)\n");
		goto out;
	}
	{
		vm_offset_t picture;
		const volatile uint8_t *pixel;
		volatile uint32_t *page = (volatile uint32_t *)(core_memory + CORE_SCRATCH_OFFSET);

		if (kmem_alloc(kernel_map, &picture, round_page(PICTURE_ROW * PICTURE_HEIGHT), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS || !space_block(TARGET_ADDRESS, picture, PICTURE_ROW * PICTURE_HEIGHT)) {
			printf("a610: no memory for the picture\n");
			goto out;
		}
		to_memory(picture, PICTURE_ROW * PICTURE_HEIGHT);
		/* the new tables (and new entries of old ones) out to memory; the SMMU has not seen these addresses */
		for (uint32_t i = 0; i < space.count; i++) {
			to_memory((vm_offset_t)space.table[i], 4096);
		}
		printf("a610: the picture: %u x %u, rows of %u bytes, at %#llx for the core (%u tables)\n", PICTURE_WIDTH,
		    PICTURE_HEIGHT, PICTURE_ROW, TARGET_ADDRESS, space.count);
		/*
		 * The first run had every command taken and both flush words written, no SMMU fault, and the
		 * picture all zeros. Two things are tried, in turn: Mesa's raw "magic" registers for the A610
		 * (a6xx_gen1_low_raw_magic_regs) with the core as it is, in secure mode; then, should the
		 * picture still be empty, the core out of secure mode by packet (CP_SET_SECURE_MODE 0) and the
		 * same once more -- in secure mode the colour units may write as the secure stream (SID 2),
		 * which goes nowhere of ours.
		 */
		bool drawn = false;

		for (unsigned attempt = 0; attempt < 2 && !drawn; attempt++) {
			if (attempt == 1) {
				ring_command(CP_SET_SECURE_MODE, 1);
				ring(0);
				if (!ring_run("leave secure mode (CP_SET_SECURE_MODE 0)")) {
					space_faulted("leaving secure mode");
					redwood_status(REDWOOD_STATUS_GPU, "FAILED 13: CP_SET_SECURE_MODE 0 does not go through");
					goto out;
				}
				printf("a610: RBBM_SECVID_TRUST_CNTL is %#x after it\n", rd(gpu + RBBM_SECVID_TRUST_CNTL));
			}
			for (unsigned i = 0; i < sizeof(gen1_magic) / sizeof(gen1_magic[0]); i++) {
				ring_register(gen1_magic[i][0], gen1_magic[i][1]);
			}
			ring_command(CP_SET_MARKER, 1);
			ring(12);                               /* "2D blits follow" */
			ring_register(RB_CCU_CNTL, A610_CCU_CNTL_BYPASS | 1);   /* bit 0: no GMEM fast clear on the A610 */
			ring_fill(0, 780, 255, 128, 0);         /* orange */
			ring_fill(780, 1560, 255, 255, 255);
			ring_fill(1560, PICTURE_HEIGHT, 0, 128, 255);
			/* what it has drawn out of its caches into memory: each event writes a word of ours when done */
			ring_command(CP_EVENT_WRITE, 4);
			ring(EVENT_FLUSH_COLOR);
			ring((uint32_t)(core_address + CORE_SCRATCH_OFFSET + 32));
			ring(0);
			ring(0x0c0c0c0c + attempt);
			ring_command(CP_EVENT_WRITE, 4);
			ring(EVENT_CACHE_FLUSH);
			ring((uint32_t)(core_address + CORE_SCRATCH_OFFSET + 16));
			ring(0);
			ring(0x0d0e0d0e + attempt);
			ring_command(CP_WAIT_FOR_IDLE, 0);
			if (!ring_run(attempt == 0 && !zap_loaded ? "fill a picture, in secure mode" : "fill a picture, out of secure mode")) {
				space_faulted("filling the picture");
				redwood_status(REDWOOD_STATUS_GPU, "FAILED 13: the fill does not go through");
				core_good = false;
				goto out;
			}
			space_faulted("filling the picture");
			to_memory(core_memory + CORE_SCRATCH_OFFSET, 64);
			printf("a610: the words it writes when its caches are in memory: %#x, %#x (told: %#x, %#x)\n", page[8], page[4],
			    0x0c0c0c0c + attempt, 0x0d0e0d0e + attempt);
			to_memory(picture, PICTURE_ROW * PICTURE_HEIGHT);
			pixel = (const volatile uint8_t *)picture;
			printf("a610: the picture's first pixel is %02x %02x %02x %02x (blue green red alpha; told: 00 80 ff ff), one in "
			    "the middle %02x %02x %02x %02x, one in the last row %02x %02x %02x %02x\n", pixel[0], pixel[1], pixel[2],
			    pixel[3], pixel[1170 * PICTURE_ROW + 2160], pixel[1170 * PICTURE_ROW + 2161], pixel[1170 * PICTURE_ROW + 2162],
			    pixel[1170 * PICTURE_ROW + 2163], pixel[2339 * PICTURE_ROW + 4316], pixel[2339 * PICTURE_ROW + 4317],
			    pixel[2339 * PICTURE_ROW + 4318], pixel[2339 * PICTURE_ROW + 4319]);
			drawn = (pixel[0] | pixel[1] | pixel[2]) != 0 && pixel[1170 * PICTURE_ROW + 2160] != 0 &&
			    (pixel[2339 * PICTURE_ROW + 4316] | pixel[2339 * PICTURE_ROW + 4317] | pixel[2339 * PICTURE_ROW + 4318]) != 0;
			printf("a610: attempt %u (%s): %s\n", attempt + 1, zap_loaded ? "out of secure mode, the zap shader in" :
			    attempt == 0 ? "secure mode" : "after CP_SET_SECURE_MODE 0",
			    drawn ? "DRAWN" : "the picture is not what the 3D core was told to draw");
		}
		if (!drawn) {
			redwood_status(REDWOOD_STATUS_GPU, "FAILED 13: the picture is not drawn");
			goto out;
		}
		MiniIOMobileFramebufferShow((const uint8_t *)picture, PICTURE_WIDTH, PICTURE_HEIGHT, PICTURE_ROW);
		printf("a610: A PICTURE DRAWN BY THE GPU IS ON THE PANEL: orange, white, blue from the top\n");
		redwood_status(REDWOOD_STATUS_GPU, "GPU DRAWS: its picture is on the panel");
	}
out:
	printf("a610: stopping after step %u (a610=%u)\n", steps < 13 ? steps : 13, steps);
	kept_start(core_good);
	thread_terminate(current_thread());
}

void
redwood_a610_init(void)
{
	uint32_t steps = 0;
	char device[16] = "";
	thread_t thread;

	if (!PE_parse_boot_argn("a610", &steps, sizeof(steps)) || steps == 0 ||
	    !PE_parse_boot_arg_str("device", device, sizeof(device)) || strcmp(device, "willow") != 0) {
		return;
	}
	if (kernel_thread_start(a610_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
