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
 *              input 0 undivided, then the clock on)
 *   6  reads   the first registers of the GPU block
 *   7  reads   the GPU's SMMU (0x59a0000): its ID registers and stream table, which the hypervisor
 *              answers for, and the first translation context, as the firmware and hypervisor left it
 *   8  asks    TrustZone what Linux asks first: whether it takes calls, whether it has the calls
 *              that authenticate and start a firmware image, and whether it "supports" the GPU's
 *              zap shader (PAS id 13) -- calls that pass no memory
 *   9  asks    TrustZone to check the zap shader's signature: its header (fw/a610_zap.mdt of the
 *              small image, the phone's own /vendor/firmware/a610_zap.mdt: the ELF header and the
 *              block of hashes and certificates) goes into a page of ours, which TrustZone reads
 *              (the first of the three calls of Linux's qcom_mdt_pas_init; nothing of the GPU)
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
 *              plain bypass S2CR is turned into "fault" by Qualcomm's hypervisor
 *  12  commands the core to write a word into our page, first as it is (in secure mode: TRUST_CNTL
 *              reads 1), and if that does not land, after CP_SET_SECURE_MODE 0 -- the packet Linux
 *              sends once the zap shader is in. Writing RBBM_SECVID_TRUST_CNTL = 0 from here, as
 *              Linux does where there is no zap shader at all, hung the bus on this phone (the
 *              write never came back, and the whole kernel stopped some 15 s later): not done
 *
 * Steps 9 to 12 go one after the other whatever 9 says: TrustZone does not take the zap shader's
 * signature here (it answers "not supported", SMCCC's -1), and 12 is the way round it, if there is one.
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

static uint32_t
odd_parity(uint32_t v)
{
	return 0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28)) & 1;
}

static void
ring(uint32_t word)
{
	((uint32_t *)core_memory)[ring_at] = word;
	ring_at = (ring_at + 1) % RING_WORDS;
}

static void
ring_command(uint32_t command, uint32_t words)
{
	ring(7u << 28 | words | odd_parity(words) << 15 | command << 16 | odd_parity(command) << 23);
}

/* Let the core have what is in the ring, and wait until it has taken all of it and rests. */
static bool
ring_run(const char *what)
{
	uint32_t taken = ~0u, status = ~0u;

	printf("a610: about to have the 3D core %s: the ring up to word %u\n", what, ring_at);
	to_memory(core_memory + 4 * ring_sent, 4 * (ring_at - ring_sent));
	ring_sent = ring_at;
	wr(gpu + CP_RB_WPTR, ring_at);
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

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t *answer)
{
	return scm2(what, function, arguments, first, 0, answer);
}

static int32_t
scm2(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t *answer)
{
	uint64_t session = 0;

	printf("a610: about to ask TrustZone %s: smc %#llx (%#llx)\n", what, function, first);
	uint64_t deadline;

	/*
	 * A call that takes TrustZone a while (checking a signature) comes back "interrupted" whenever
	 * an interrupt is due, and is resumed so: as often as it takes, up to 20 s of it.
	 */
	clock_interval_to_deadline(20, NSEC_PER_SEC, &deadline);
	for (uint64_t i = 0; i == 0 || mach_absolute_time() < deadline; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = 0;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("a610: TrustZone returns %d, answer %#llx (after %llu resumptions)\n", (int32_t)x0, x1, i);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("a610: TrustZone still says the call was interrupted after 20 s\n");
	return -1;
}

extern volatile int redwood_rpm_done;

static void
a610_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter;
	bool cx_on, cx_clocks;

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
	if (!clock_on("gpu_cc_gx_gfx3d_clk", gpucc, 0x1054, true)) {
		redwood_status(REDWOOD_STATUS_GPU, "FAILED at step 5: the core clock does not run");
		goto out;
	}
	redwood_status(REDWOOD_STATUS_GPU, "CX and GX on, core at 19.2 MHz");
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
		uint32_t bypass = 0;

		PE_parse_boot_argn("a610_bypass", &bypass, sizeof(bypass));
		if (bypass && smmu != 0) {
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
		wr(gpu + RBBM_INTERFACE_HANG_INT_CNTL, 1u << 30 | 0x3ffff);    /* the A610's */
		wr(gpu + UCHE_CLIENT_PF, 1u << 7 | 1);
		put64(gpu + CP_SQE_INSTR_BASE, core_phys + CORE_SQE_OFFSET);
		put64(gpu + CP_RB_BASE, core_phys);
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
	}
	if (steps < 12) {
		goto out;
	}

	printf("a610: step 12: a word written on command, in secure mode and then out of it by packet\n");
	redwood_status(REDWOOD_STATUS_GPU, "step 12: first write");
	{
		volatile uint32_t *page = (volatile uint32_t *)(core_memory + CORE_SCRATCH_OFFSET);
		uint64_t target = core_phys + CORE_SCRATCH_OFFSET;
		static const uint32_t words[] = { 0x600dc0de, 0x600dc0d1 };
		bool wrote = false;

		printf("a610: RBBM_SECVID_TRUST_CNTL is %#x (not written: that hangs the bus here)\n",
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
			if (!ring_run(i == 0 ? "write a word to memory, in secure mode" : "write a word to memory, out of it")) {
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
	}
out:
	printf("a610: stopping after step %u (a610=%u)\n", steps < 12 ? steps : 12, steps);
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
