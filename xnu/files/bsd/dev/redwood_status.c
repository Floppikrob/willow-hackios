/*
 * What each part of the phone is doing, in a line each, for the panel the framebuffer draws over
 * the picture (fb_hud=1, iokit/Kernel/MiniIOMobileFramebuffer.cpp): the drivers say how they are
 * (redwood_status()), the panel shows the lines. The parts are a fixed list, so that one whose
 * driver has said nothing -- or that has no driver at all -- is seen as such. The log is not
 * replaced: every driver says what it says there as before.
 */
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

#define STATUS_TEXT             56

/* each core's clock as cpu_speed_probe() measured it as the core came up (osfmk/arm/arm_init.c, cpuprobe=1) */
extern uint32_t redwood_cpu_mhz[8];
extern unsigned int vm_page_free_count, memorystatus_available_pages;
extern _Atomic uint64_t ramfs_pages_in_use;

static const char *const status_parts[REDWOOD_STATUS_PARTS] = {
	"Screen", "CPU", "Memory", "GPU", "Touch", "Keys", "Storage", "Wi-Fi", "Sound", "Battery",
};

static struct {
	char     text[STATUS_TEXT];
	bool     said;
} status_lines[REDWOOD_STATUS_PARTS];

static LCK_GRP_DECLARE(status_lck_grp, "redwood-status");
static LCK_SPIN_DECLARE(status_lock, &status_lck_grp);

void
redwood_status(unsigned part, const char *format, ...)
{
	char text[STATUS_TEXT];
	va_list list;

	if (part >= REDWOOD_STATUS_PARTS) {
		return;
	}
	va_start(list, format);
	vsnprintf(text, sizeof(text), format, list);
	va_end(list);
	lck_spin_lock(&status_lock);
	memcpy(status_lines[part].text, text, sizeof(text));
	status_lines[part].said = true;
	lck_spin_unlock(&status_lock);
}

/* Line `part` as the panel shows it: "Touch    ok, 3 fingers"; false past the last. */
bool
redwood_status_line(unsigned part, char *line, size_t size)
{
	char text[STATUS_TEXT];
	bool said;

	if (part >= REDWOOD_STATUS_PARTS) {
		return false;
	}
	lck_spin_lock(&status_lock);
	memcpy(text, status_lines[part].text, sizeof(text));
	said = status_lines[part].said;
	lck_spin_unlock(&status_lock);
	if (part == REDWOOD_STATUS_CPU && !said && (redwood_cpu_mhz[0] | redwood_cpu_mhz[4]) != 0) {
		snprintf(text, sizeof(text), "%u %u %u %u | %u %u %u %u MHz (at start)", redwood_cpu_mhz[0], redwood_cpu_mhz[1],
		    redwood_cpu_mhz[2], redwood_cpu_mhz[3], redwood_cpu_mhz[4], redwood_cpu_mhz[5], redwood_cpu_mhz[6],
		    redwood_cpu_mhz[7]);
		said = true;
	} else if (part == REDWOOD_STATUS_MEMORY) {
		snprintf(text, sizeof(text), "free %u MB, available %u MB, /var %u MB",
		    (unsigned)(((uint64_t)vm_page_free_count * PAGE_SIZE) >> 20),
		    (unsigned)(((uint64_t)memorystatus_available_pages * PAGE_SIZE) >> 20),
		    (unsigned)((__c11_atomic_load(&ramfs_pages_in_use, __ATOMIC_RELAXED) * PAGE_SIZE) >> 20));
		said = true;
	}
	snprintf(line, size, "%-8s %s", status_parts[part], said ? text : "no driver");
	return true;
}
