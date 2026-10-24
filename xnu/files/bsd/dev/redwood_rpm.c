/*
 * The RPM of the Redmi Note 8T (willow, SM6125): the processor that keeps the SoC's shared clocks
 * and buses -- the memory buses the GPU reaches memory through (bimc, snoc), the crypto engine's
 * clock -- switched on for whoever asks. Asking is a message on its "rpm_requests" channel, which
 * is GLINK over a pair of FIFOs in the RPM's message RAM, with a doorbell in the application
 * processors' IPC register. Only with boot-arg rpm=N and device=willow: steps 1 to N.
 *
 * The UEFI this kernel starts from has its own GLINK client of the same FIFOs (GLinkDxe), and its
 * loader goes on running on core 0 under that UEFI. Step 1 found its session there: versions
 * exchanged, "rpm_requests" open, ours as channel 1 and the RPM's as 2, both FIFOs empty. So
 * nothing is negotiated again: the requests go behind UEFI's on its open channel, as GLINK data
 * of channel 1 (the RPM's GLINK is "intentless": no receive intents to ask for first), and the
 * RPM is rung through the IPC register (mailbox@F111000, offset 8, bit 0).
 *
 *   1  reads   the message RAM's table of contents (its last 256 bytes, "grt0"), both FIFOs'
 *              head and tail, and what is already in each, packet by packet: what UEFI asked
 *              the RPM for since the boot, and what the RPM answered
 *   2  asks    the RPM for the memory bus (bimc, "clk2" 0) at 400 MHz; what it answers is read
 *              back out of the FIFO from it and said
 *   3  asks    for the system bus (snoc, "clk1" 2: on SM6125 "clk1" 1 is the config bus, cnoc) at
 *              200 MHz and the crypto engine (ce1, "ce" 0) at 100 MHz
 *
 * The first run of steps 1-3 (2026-10-24) had all three answered "msg#" with their ids and no
 * "err", and gcc_ce1_clk ran after it (it had been off): the channel works.
 *
 * The GPU's steps (redwood_a610.c, a610=N) wait for these: its path to memory runs on these buses.
 *
 * Layouts: Linux drivers/rpmsg/qcom_glink_rpm.c, qcom_glink_native.c, drivers/soc/qcom/smd-rpm.c;
 * the phone's device tree (memory@045F0000 "qcom,rpm-msg-ram", mailbox@0F111000, rpm-glink).
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

static int say(const char *format, ...) __printflike(1, 2);

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

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

#define MSG_RAM_PHYS            0x045f0000UL
#define MSG_RAM_SIZE            0x7000UL
#define TOC_SIZE                256
#define TOC_MAGIC               0x67727430      /* "grt0" */
#define TX_FIFO_ID              0x61703272      /* "ap2r": from us to the RPM */
#define RX_FIFO_ID              0x72326170      /* "r2ap": from the RPM to us */

static vm_offset_t msg_ram, ipc;

#define IPC_PHYS                0x0f111000UL
#define IPC_DOORBELL            8               /* bit 0: the RPM */
#define GLINK_CMD_TX_DATA       9
#define UEFI_CHANNEL            1               /* "rpm_requests" as UEFI opened it (step 1) */
#define RPM_REQUEST             0x00716572      /* "req" */
#define RPM_ACTIVE_STATE        0
#define KEY_KHZ                 0x007a484b      /* "KHz" */
#define TYPE_BUS_CLK            0x316b6c63      /* "clk1" */
#define TYPE_MEM_CLK            0x326b6c63      /* "clk2" */
#define TYPE_CE_CLK             0x00006563      /* "ce" */

/* redwood_a610.c waits for this: the RPM's requests are done, whatever they came to (or rpm= was not given) */
volatile int redwood_rpm_done;

struct fifo {
	const char *name;
	uint32_t    offset, size;   /* in the message RAM: tail, head, then the data */
	bool        found;
};
static struct fifo tx = { .name = "to the RPM" }, rx = { .name = "from the RPM" };

static void
fifo_say(const struct fifo *f)
{
	uint32_t tail = rd(msg_ram + f->offset), head = rd(msg_ram + f->offset + 4);
	vm_offset_t data = msg_ram + f->offset + 8;

	printf("rpm: FIFO %s at %#x, %u bytes: tail %u, head %u (%u bytes in it)\n", f->name, f->offset, f->size, tail, head,
	    head >= tail ? head - tail : f->size - tail + head);
	printf("rpm: ... its first words: %08x %08x %08x %08x %08x %08x %08x %08x\n", rd(data), rd(data + 4), rd(data + 8),
	    rd(data + 12), rd(data + 16), rd(data + 20), rd(data + 24), rd(data + 28));
}

/* a resource type or a key as the text it is ("clk2", "KHz"), its bytes from the lowest */
static const char *
fourcc(uint32_t value, char text[5])
{
	for (int i = 0; i < 4; i++) {
		char c = (char)(value >> (8 * i));

		text[i] = c >= 0x20 && c < 0x7f ? c : c == 0 ? ' ' : '?';
	}
	text[4] = 0;
	return text;
}

/*
 * What is already in a FIFO from its start up to its head, packet by packet. Every boot starts its
 * FIFOs from 0 (step 1 has always found the same heads), so what lies before the head is all that
 * was sent since: UEFI's GLINK session and requests, or the RPM's answers.
 */
static void
fifo_history(const struct fifo *f)
{
	uint32_t head = rd(msg_ram + f->offset + 4), at = 0;
	vm_offset_t data = msg_ram + f->offset + 8;
	char t1[5], t2[5];

	printf("rpm: what is in the FIFO %s, %u bytes:\n", f->name, head);
	while (at + 8 <= head && head <= f->size) {
		uint32_t w0 = rd(data + at), w1 = rd(data + at + 4), cmd = w0 & 0xffff, param1 = w0 >> 16;

		if (cmd == 0 || cmd == 1) {
			printf("rpm:   %4u: VERSION%s %u, features %#x\n", at, cmd ? "_ACK" : "", param1, w1);
			at += 8;
		} else if (cmd == 2) {
			printf("rpm:   %4u: OPEN channel %u, a name of %u bytes\n", at, param1, w1);
			at += 8 + ((w1 + 7) & ~7u);
		} else if (cmd == 4) {
			printf("rpm:   %4u: OPEN_ACK of channel %u\n", at, param1);
			at += 8;
		} else if (cmd == GLINK_CMD_TX_DATA && at + 16 <= head) {
			uint32_t chunk = rd(data + at + 8), p = at + 16, service = rd(data + p);

			if (service == RPM_REQUEST && chunk >= 28) {
				/* a request: service, length, id, set, type, resource id, data length, then key-values */
				uint32_t length = rd(data + p + 24);

				printf("rpm:   %4u: data of channel %u: request %#x, set %u, \"%s\" %u:", at, param1, rd(data + p + 8),
				    rd(data + p + 12), fourcc(rd(data + p + 16), t1), rd(data + p + 20));
				for (uint32_t k = 0; k + 12 <= length && 28 + k + 12 <= chunk; k += 8 + ((rd(data + p + 32 + k) + 3) & ~3u)) {
					printf(" %s=%u", fourcc(rd(data + p + 28 + k), t2), rd(data + p + 36 + k));
				}
				printf("\n");
			} else if (chunk >= 20) {
				/* an answer: service, length, then a message ("msg#" and the id it answers, or "err" and a text) */
				printf("rpm:   %4u: data of channel %u: \"%s\", %u bytes, \"%s\" %08x %08x\n", at, param1, fourcc(service, t1),
				    rd(data + p + 4), fourcc(rd(data + p + 8), t2), rd(data + p + 16), rd(data + p + 20));
			} else {
				printf("rpm:   %4u: data of channel %u, %u bytes\n", at, param1, chunk);
			}
			at += 16 + ((chunk + 7) & ~7u);
		} else {
			printf("rpm:   %4u: command %u (%#x %#x), the rest not read\n", at, cmd, w0, w1);
			break;
		}
	}
}

/* `count` bytes (a multiple of 4) into the FIFO to the RPM at its head; false if there is not room */
static bool
fifo_put(const uint32_t *words, uint32_t count)
{
	uint32_t tail = rd(msg_ram + tx.offset), head = rd(msg_ram + tx.offset + 4);
	uint32_t room = tail <= head ? tx.size - head + tail : tail - head;
	vm_offset_t data = msg_ram + tx.offset + 8;

	if (count + 8 > room || head >= tx.size || (head & 3)) {
		printf("rpm: no room in the FIFO to the RPM (tail %u, head %u)\n", tail, head);
		return false;
	}
	for (uint32_t i = 0; i < count / 4; i++) {
		wr(data + head, words[i]);
		head = (head + 4) % tx.size;
	}
	__builtin_arm_dsb(0xf);
	wr(msg_ram + tx.offset + 4, head);
	__builtin_arm_dsb(0xf);
	return true;
}

/* What the RPM has put into the FIFO from it, said and taken out; false if nothing came in 200 ms. */
static bool
fifo_take(void)
{
	uint32_t tail, head, n, words[24];
	vm_offset_t data = msg_ram + rx.offset + 8;

	for (int i = 0; i < 2000; i++) {
		if (rd(msg_ram + rx.offset + 4) != rd(msg_ram + rx.offset)) {
			break;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	delay_for_interval(2, NSEC_PER_MSEC);   /* (the rest of an answer that is still being written) */
	tail = rd(msg_ram + rx.offset);
	head = rd(msg_ram + rx.offset + 4);
	n = head >= tail ? head - tail : rx.size - tail + head;
	if (n == 0) {
		printf("rpm: no answer in 200 ms\n");
		return false;
	}
	for (uint32_t i = 0; i < 24 && 4 * i < n; i++) {
		words[i] = rd(data + (tail + 4 * i) % rx.size);
	}
	printf("rpm: answer, %u bytes: %08x %08x %08x %08x | %08x %08x %08x %08x %08x %08x %08x %08x\n", n, words[0], words[1],
	    words[2], words[3], words[4], words[5], words[6], words[7], words[8], words[9], words[10], words[11]);
	if (n >= 40 && (words[0] & 0xffff) == GLINK_CMD_TX_DATA) {
		/* GLINK data: then the RPM's header (service, length) and its messages: "msg#" with our id, or "err" and a text */
		printf("rpm: ... GLINK data of channel %u, %u bytes: service %#x, first message %#x (%s), id or text %08x %08x\n",
		    words[0] >> 16, words[2], words[4], words[6], words[6] == 0x2367736d ? "msg#: done" : words[6] == 0x00727265 ?
		    "err" : "?", words[8], words[9]);
	}
	wr(msg_ram + rx.offset, head);          /* taken */
	__builtin_arm_dsb(0xf);
	return true;
}

/* One key-value request (rate in kHz) for a resource of the RPM, active set; its answer said. */
static bool
rpm_ask(const char *what, uint32_t type, uint32_t id, uint32_t khz)
{
	static uint32_t msg_id = 0x7000;        /* (far from UEFI's own numbers) */
	uint32_t m[14];

	m[0] = GLINK_CMD_TX_DATA | UEFI_CHANNEL << 16;
	m[1] = 0;                               /* no intent: the RPM's GLINK is intentless */
	m[2] = 40;                              /* this chunk */
	m[3] = 0;                               /* nothing left after it */
	m[4] = RPM_REQUEST;
	m[5] = 20 + 12;                         /* the request and its one key-value */
	m[6] = msg_id++;
	m[7] = RPM_ACTIVE_STATE;
	m[8] = type;
	m[9] = id;
	m[10] = 12;
	m[11] = KEY_KHZ;
	m[12] = 4;
	m[13] = khz;
	printf("rpm: next: ask for %s at %u kHz (message %#x)\n", what, khz, m[6]);
	if (!fifo_put(m, sizeof(m))) {
		return false;
	}
	wr(ipc + IPC_DOORBELL, 1);
	__builtin_arm_dsb(0xf);
	return fifo_take();
}

static void
rpm_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter, count;
	vm_offset_t toc;

	printf("rpm: step 1: the message RAM (%#lx) and its table of contents\n", MSG_RAM_PHYS);
	msg_ram = ml_io_map(MSG_RAM_PHYS, MSG_RAM_SIZE);
	toc = msg_ram + MSG_RAM_SIZE - TOC_SIZE;
	count = rd(toc + 4);
	printf("rpm: table of contents: magic %#x (%s), %u entries\n", rd(toc), rd(toc) == TOC_MAGIC ? "grt0" : "NOT grt0",
	    count);
	if (rd(toc) != TOC_MAGIC || count > (TOC_SIZE - 8) / 12) {
		goto out;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint32_t id = rd(toc + 8 + 12 * i), offset = rd(toc + 12 + 12 * i), size = rd(toc + 16 + 12 * i);
		struct fifo *f = id == TX_FIFO_ID ? &tx : id == RX_FIFO_ID ? &rx : NULL;

		printf("rpm: entry %u: id %#x, offset %#x, %u bytes%s\n", i, id, offset, size, f ? (f == &tx ? " (ap2r)" : " (r2ap)") : "");
		if (f != NULL && offset + size <= MSG_RAM_SIZE) {
			f->offset = offset;
			f->size = size;
			f->found = true;
		}
	}
	if (!tx.found || !rx.found) {
		printf("rpm: the two FIFOs are not both there\n");
		goto out;
	}
	fifo_say(&tx);
	fifo_say(&rx);
	fifo_history(&tx);
	fifo_history(&rx);
	if (steps < 2) {
		goto out;
	}

	printf("rpm: step 2: the memory bus\n");
	ipc = ml_io_map(IPC_PHYS, 0x1000);
	if (!rpm_ask("the memory bus (bimc)", TYPE_MEM_CLK, 0, 400000)) {
		goto out;
	}
	fifo_say(&tx);
	if (steps < 3) {
		goto out;
	}

	printf("rpm: step 3: the system bus and the crypto engine\n");
	rpm_ask("the system bus (snoc)", TYPE_BUS_CLK, 2, 200000);
	rpm_ask("the crypto engine (ce1)", TYPE_CE_CLK, 0, 100000);
out:
	printf("rpm: stopping after step %u (rpm=%u)\n", steps < 3 ? steps : 3, steps);
	redwood_rpm_done = 1;
	thread_terminate(current_thread());
}

void
redwood_rpm_init(void)
{
	uint32_t steps = 0;
	char device[16] = "";
	thread_t thread;

	if (!PE_parse_boot_argn("rpm", &steps, sizeof(steps)) || steps == 0 ||
	    !PE_parse_boot_arg_str("device", device, sizeof(device)) || strcmp(device, "willow") != 0) {
		return;
	}
	if (kernel_thread_start(rpm_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
