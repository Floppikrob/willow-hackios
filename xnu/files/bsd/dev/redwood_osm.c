/*
 * The CPU clusters' frequency hardware of the Redmi Note 8T (willow, SM6125), read and nothing
 * else: boot-arg osm_dump=1 with device=willow.
 *
 * Each cluster has an OSM ("operating state manager", dt: qcom,cpucc@f521000, "osm_pwrcl_base"
 * 0xf521000 for Silver, "osm_perfcl_base" 0xf523000 for Gold) that the firmware sets up and Linux
 * only asks for a row of its table (clk-cpu-osm.c). The firmware has Silver's (enable 3, a
 * sequencer, a PLL), not Gold's (enable 2, which then stays at 300 MHz whatever row is asked
 * for). Before anything is written there, this says what is there: once the Gold cores are on
 * (osm_dump_after=S, default 70 s), every word of both blocks that is not zero, side by side, and
 * those of the limits hardware both clusters' "qcom,msm-hw-limits" name (0xf550800, of which
 * Linux's msm_lmh_dcvs.c reads the frequency cap it imposes at +0x704 and the floor at +0x3c0).
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
extern processor_t cpu_to_processor(int cpu);
extern processor_t thread_bind(processor_t processor);
extern wait_result_t thread_block(thread_continue_t continuation);
extern int cpu_number(void);

static int say(const char *format, ...) __printflike(1, 2);

/* every line to the loader's log as well */
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

#define OSM_PHYS                0x0f520000UL    /* a page below Silver's block, to Gold's end */
#define OSM_SIZE                0x5000UL
#define OSM_SILVER              0x1000          /* 0xf521000 */
#define OSM_GOLD                0x3000          /* 0xf523000 */
#define OSM_BLOCK               0x1400
#define LMH_PHYS                0x0f550000UL
#define LMH_SIZE                0x2000UL
#define LMH_AT                  0x800           /* 0xf550800 */

static void
osm_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t after = 70, lines = 0;
	vm_offset_t osm, lmh;

	PE_parse_boot_argn("osm_dump_after", &after, sizeof(after));
	delay_for_interval(after, NSEC_PER_SEC);
	osm = ml_io_map(OSM_PHYS, OSM_SIZE);
	printf("osm: the clusters' OSM blocks, %u s after the start; offset: Silver (0xf521000) | Gold (0xf523000), "
	    "where either is not zero\n", after);
	for (uint32_t at = 0; at < OSM_BLOCK; at += 16) {
		uint32_t s[4], g[4], any = 0;

		for (int i = 0; i < 4; i++) {
			s[i] = rd(osm + OSM_SILVER + at + 4 * i);
			g[i] = rd(osm + OSM_GOLD + at + 4 * i);
			any |= s[i] | g[i];
		}
		if (any != 0 && lines++ < 320) {
			printf("osm: %#06x: %08x %08x %08x %08x | %08x %08x %08x %08x%s\n", at, s[0], s[1], s[2], s[3], g[0], g[1],
			    g[2], g[3], (s[0] != g[0] || s[1] != g[1] || s[2] != g[2] || s[3] != g[3]) ? "  *" : "");
		}
	}
	printf("osm: %u lines of the blocks not zero\n", lines);
	(void)lmh;
	/*
	 * The clusters' speed, by their own cycle counters (clk-cpu-osm.c, OSM_CYCLE_COUNTER_STATUS_REG 0x9c0:
	 * what Linux gives the scheduler as a core's cycles), read 1 s apart, ten times. The limits
	 * hardware (0xf550800) is not read: the first run's read of it never came back.
	 */
	for (int i = 0; i < 10; i++) {
		uint32_t s0 = rd(osm + OSM_SILVER + 0x9c0), g0 = rd(osm + OSM_GOLD + 0x9c0);
		uint64_t t0 = mach_absolute_time(), ns;

		delay_for_interval(1, NSEC_PER_SEC);
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		printf("osm: in %llu ms: Silver %u cycles, Gold %u: %llu MHz and %llu MHz; rows asked %u and %u, now %u and %u\n",
		    ns / 1000000, rd(osm + OSM_SILVER + 0x9c0) - s0, rd(osm + OSM_GOLD + 0x9c0) - g0,
		    (uint64_t)(rd(osm + OSM_SILVER + 0x9c0) - s0) * 1000 / ns, (uint64_t)(rd(osm + OSM_GOLD + 0x9c0) - g0) * 1000 / ns,
		    rd(osm + OSM_SILVER + 0x920), rd(osm + OSM_GOLD + 0x920), rd(osm + OSM_SILVER + 0x840), rd(osm + OSM_GOLD + 0x840));
	}
	/*
	 * What OSM's counters say is what it believes it set. What a core does: the same loop, 100 million
	 * times a subtract and a branch, on each processor in turn (bound to it), timed.
	 */
	for (int cpu = 0; cpu < 8; cpu++) {
		processor_t processor = cpu_to_processor(cpu);
		uint64_t n = 100000000, t0, ns;

		if (processor == PROCESSOR_NULL) {
			continue;
		}
		thread_bind(processor);
		thread_block(THREAD_CONTINUE_NULL);
		t0 = mach_absolute_time();
		__asm__ volatile ("1: subs %0, %0, #1\n\tb.ne 1b" : "+r" (n));
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		printf("osm: cpu %d (on cpu %d): 100M loops in %llu us: %llu million a second\n", cpu, cpu_number(), ns / 1000,
		    ns ? 100000000000ull / ns : 0);
	}
	thread_bind(PROCESSOR_NULL);
	thread_terminate(current_thread());
}

void
redwood_osm_init(void)
{
	uint32_t dump = 0;
	char device[16] = "";
	thread_t thread;

	if (!PE_parse_boot_argn("osm_dump", &dump, sizeof(dump)) || !dump ||
	    !PE_parse_boot_arg_str("device", device, sizeof(device)) || strcmp(device, "willow") != 0) {
		return;
	}
	if (kernel_thread_start(osm_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
