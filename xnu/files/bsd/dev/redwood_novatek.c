/*
 * The touch screen of the Redmi Note 8T (willow): a Novatek NT36672A -- the panel's own display
 * driver, which does the touch as well -- on the SPI bus of the SoC's serial engine 2 (QUP 0), as a
 * source of touches for /dev/redwood-touch (sys/redwood_input.h). Only with boot-arg touchscreen=1
 * and device=willow (xnuloader puts that on the command line): every address below belongs to
 * this phone.
 *
 * The chip is powered when the kernel starts: its supplies are the panel's, which the firmware has
 * left on to show the screen. But its touch half has no flash of its own: whoever drives it loads
 * its firmware into its memory at every start (Android's driver from /vendor/firmware), and after
 * the reboot into UEFI nobody has. So the driver does what the vendor's Linux driver does at probe:
 * the serial engine's clocks, by vote in the global clock controller; its four pins; the reset pin
 * high; the chip identified by its trim id; the firmware -- fw/novatek_ts_fw.bin of the small image,
 * the phone's own /vendor/firmware/novatek_ts_tianma_fw.bin (userland/build.sh copies fw-touch/
 * there; it is Novatek's, not this repository's) -- written into its memory partition by
 * partition, with the checksums of the first two for its boot loader to check, and started.
 * Then the report is read: SPI in FIFO mode, polled, as in bsd/dev/redwood_goodix.c.
 *
 * The protocol (Novatek's nt36xxx_spi Linux driver): the chip's memory is seen through a window of
 * 128 bytes, moved with a write of 0xff and two bytes of the address (a "page"); a read is the
 * address's low seven bits, a dummy byte, and the data. The touch report is at 0x21c00 in the
 * NT36672A's map: 6 bytes a finger, ten fingers; the firmware's facts (its version, the
 * coordinates' range) at 0x21c78.
 *
 * No interrupt is used: a thread reads the report 100 times a second (touch_poll_us=N: every N
 * microseconds instead) and compares it with the last one. The interrupt line (GPIO 88) is only
 * looked at to say what it does.
 *
 * Register layouts: Linux drivers/spi/spi-geni-qcom.c, drivers/clk/qcom/gcc-sm6125.c,
 * drivers/pinctrl/qcom/pinctrl-sm6125.c, Novatek's nt36xxx.c, nt36xxx_fw_update.c and
 * nt36xxx_mem_map.h (Xiaomi's ginkgo-q-oss, drivers/input/touchscreen/nt36xxx_spi_c3j), and the
 * phone's own device tree (spi@4a88000, touchpad; qupv3_se2_spi_active).
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
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

/* Every line to the loader's log as well, as bsd/dev/redwood_buttons.c does: a touch is to be seen when it happens. */
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

static vm_offset_t tlmm, gcc, se;

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

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x00500000UL    /* the "west" tile: every pin used here is in it */
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0

#define PIN_SPI_FIRST           6       /* 6 to 9: the serial engine's four */
#define PIN_RESET               87      /* low = in reset */
#define PIN_INT                 88      /* the chip pulls it low when it has a report */
#define PIN_FUNCTION_SPI        1       /* "qup02" */

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_describe(const char *name, unsigned pin)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	printf("novatek: %s (GPIO %u): function %u, pull %u, %u mA, %s; reads %u%s\n", name, pin, cfg >> 2 & 0xf, cfg & 3,
	    ((cfg >> 6 & 7) + 1) * 2, (cfg & (1u << 9)) ? "output" : "input", io & 1,
	    (cfg & (1u << 9)) ? ((io & 2) ? ", driven high" : ", driven low") : "");
}

/* ---- clocks (GCC) -------------------------------------------------------------------------------- */

#define GCC_PHYS                0x01400000UL
#define GCC_SIZE                0x80000UL
#define GCC_VOTE                (gcc + 0x7900c)         /* the application processor's votes */
#define VOTE_QUP0_M_AHB         (1u << 6)
#define VOTE_QUP0_S_AHB         (1u << 7)
#define VOTE_QUP0_CORE          (1u << 8)
#define VOTE_QUP0_CORE_2X       (1u << 9)
#define VOTE_QUP0_SE2           (1u << 12)
#define GCC_QUP0_M_AHB_CBCR     (gcc + 0x1f004)         /* bit 31: the clock is off */
#define GCC_QUP0_S_AHB_CBCR     (gcc + 0x1f008)
#define GCC_QUP0_SE2_CBCR       (gcc + 0x1f3a4)
#define GCC_QUP0_SE2_CMD        (gcc + 0x1f3a8)         /* bit 0: take the new configuration */
#define GCC_QUP0_SE2_CFG        (gcc + 0x1f3ac)         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define GCC_QUP0_SE2_DFSR       (gcc + 0x1f3bc)         /* bit 0: the engine chooses among preset rates itself */
#define GCC_QUP0_SE2_PERF_CFG(l) (gcc + 0x1f3c4 + 4 * (l))
#define GCC_QUP0_SE2_PERF_M(l)  (gcc + 0x1f404 + 4 * (l))
#define GCC_QUP0_SE2_PERF_N(l)  (gcc + 0x1f444 + 4 * (l))
#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

static uint32_t se_source_hz, se_clock_select;

static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	/* gcc_parent_map_1: the crystal, GPLL0 (600 MHz), GPLL0 / 2, -, GPLL6 / 2 (384 MHz) */
	static const uint32_t parent[8] = { XO_HZ, 600000000, 300000000, 0, 384000000, 0, 0, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static bool
clocks_on(void)
{
	printf("novatek: clocks before: votes %#x, m-ahb %#x, s-ahb %#x, engine %#x, its source %#x (preset rates %s)\n",
	    rd(GCC_VOTE), rd(GCC_QUP0_M_AHB_CBCR), rd(GCC_QUP0_S_AHB_CBCR), rd(GCC_QUP0_SE2_CBCR), rd(GCC_QUP0_SE2_CFG),
	    (rd(GCC_QUP0_SE2_DFSR) & 1) ? "on" : "off");
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP0_CORE_2X | VOTE_QUP0_CORE | VOTE_QUP0_M_AHB | VOTE_QUP0_S_AHB);
	if (rd(GCC_QUP0_SE2_DFSR) & 1) {
		/* preset rates: the lowest that is at least 4 MHz */
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_QUP0_SE2_PERF_CFG(level)), rd(GCC_QUP0_SE2_PERF_M(level)),
			    rd(GCC_QUP0_SE2_PERF_N(level)));

			if (hz >= 4000000 && (se_source_hz == 0 || hz < se_source_hz)) {
				se_source_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_source_hz == 0) {
			printf("novatek: no usable preset clock rate\n");
			return false;
		}
	} else {
		/* the crystal, undivided: nothing else has to be running */
		wr(GCC_QUP0_SE2_CFG, (rd(GCC_QUP0_SE2_CFG) & ~0x371fu) | 1);
		wr(GCC_QUP0_SE2_CMD, rd(GCC_QUP0_SE2_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_QUP0_SE2_CMD) & 1); i++) {
			sleep_us(10);
		}
		if (rd(GCC_QUP0_SE2_CMD) & 1) {
			printf("novatek: the serial engine's clock does not take its configuration (%#x)\n", rd(GCC_QUP0_SE2_CMD));
			return false;
		}
		se_source_hz = XO_HZ;
		se_clock_select = 0;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP0_SE2);
	for (int i = 0; i < 1000; i++) {
		if (!((rd(GCC_QUP0_M_AHB_CBCR) | rd(GCC_QUP0_S_AHB_CBCR) | rd(GCC_QUP0_SE2_CBCR)) & CLOCK_OFF)) {
			printf("novatek: clocks on; the engine's source runs at %u kHz (preset %u)\n", se_source_hz / 1000,
			    se_clock_select);
			return true;
		}
		sleep_us(10);
	}
	printf("novatek: clocks stay off: %#x %#x %#x\n", rd(GCC_QUP0_M_AHB_CBCR), rd(GCC_QUP0_S_AHB_CBCR),
	    rd(GCC_QUP0_SE2_CBCR));
	return false;
}

/* ---- SPI through the serial engine (GENI) ---------------------------------------------------------- */

#define SE_PHYS                 0x04a88000UL
#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_IF_DISABLE_RO      (se + 0x064)    /* bit 0: no FIFO interface */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 1 = SPI */
#define GENI_CLK_SEL            (se + 0x07c)
#define SPI_CPHA                (se + 0x224)
#define SPI_LOOPBACK            (se + 0x22c)
#define SPI_CPOL                (se + 0x230)
#define SPI_DEMUX_OUTPUT_INV    (se + 0x24c)
#define SPI_DEMUX_SEL           (se + 0x250)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define SPI_TRANS_CFG           (se + 0x25c)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define SPI_WORD_LEN            (se + 0x268)
#define SPI_TX_TRANS_LEN        (se + 0x26c)
#define SPI_RX_TRANS_LEN        (se + 0x270)
#define SPI_DELAY_COUNTERS      (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] operation */
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)    /* [27:0] words */
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* bit 31 last, [30:28] bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define SPI_FULL_DUPLEX         (3u << 27)

static uint32_t fifo_depth;

/* NULL = ready; otherwise why the engine cannot be used */
static const char *
geni_spi_init(uint32_t hz)
{
	uint32_t divider;

	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 1) {
		return "the serial engine has no SPI firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	/* the status bits we poll follow these enables; the interrupt itself stays masked at the GIC */
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* mode 0 (nt36xxx sets SPI_MODE_0 over the device tree's spi-cs-high), chip select 0, 8 bits, most significant first */
	wr(SPI_LOOPBACK, 0);
	wr(SPI_DEMUX_SEL, 0);
	wr(SPI_CPHA, 0);
	wr(SPI_CPOL, 0);
	wr(SPI_DEMUX_OUTPUT_INV, 0);
	wr(SPI_TRANS_CFG, rd(SPI_TRANS_CFG) & ~2u);
	wr(SPI_DELAY_COUNTERS, 0);
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);
	wr(SPI_WORD_LEN, 8 - 4);

	divider = (se_source_hz + hz - 1) / hz;
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, (divider ? divider : 1) << 4 | 1);
	printf("novatek: SPI at %u kHz (divider %u), FIFO of %u words\n", se_source_hz / (divider ? divider : 1) / 1000,
	    divider, fifo_depth);
	return NULL;
}

static void
geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;

		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				if (rx) {
					rx[*received] = (uint8_t)(word >> (8 * j));
				}
				(*received)++;
			}
		}
	}
}

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* One transfer with the chip selected throughout: `length` bytes out and as many in. */
static bool
spi_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	static unsigned complaints;
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_recover();
			break;
		}
		sleep_us(100);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SPI_TX_TRANS_LEN, length);
	wr(SPI_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, SPI_FULL_DUPLEX);
	clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < length && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
			uint32_t word = 0;

			for (int j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)tx[sent] << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		status = rd(GENI_M_IRQ_STATUS);
		geni_drain(rx, length, &received);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			geni_drain(rx, length, &received);
			wr(GENI_M_IRQ_CLEAR, status);
			if (received == length) {
				return true;
			}
			break;
		}
	}
	if (complaints++ < 8) {
		printf("novatek: SPI transfer of %u bytes failed: status %#x, sent %u, received %u\n", length, status, sent,
		    received);
	}
	geni_recover();
	return false;
}

/* ---- the Novatek chip ------------------------------------------------------------------------------ */

#define NVT_EVENT_BUF           0x21c00         /* NT36672A_memory_map: EVENT_BUF_ADDR */
#define NVT_EVENT_HOST_CMD      0x50            /* ... + these */
#define NVT_EVENT_RESET_STATE   0x60
#define NVT_EVENT_FWINFO        0x78            /* the firmware's facts */
#define NVT_RESET_INIT          0xa0            /* what RESET_STATE says once the firmware has started, up to 0xaf */
#define NVT_ENG_RST             0x7fff80        /* the vendor driver's ENG_RST_ADDR */
#define NVT_SWRST_N8            0x3f0fe         /* the device tree's novatek,swrst-n8-addr */
#define NVT_SPI_RD_FAST         0x3f310         /* ... and novatek,spi-rd-fast-addr */
#define NVT_TRIM_ID             0x1f600         /* + 0x4e: six bytes that say which chip */
#define NVT_BOOT_RDY            0x3f10d
#define NVT_ILM_LENGTH          0x3f118
#define NVT_DLM_LENGTH          0x3f130
#define NVT_ILM_DES             0x3f128
#define NVT_DLM_DES             0x3f12c
#define NVT_G_ILM_CHECKSUM      0x3f100
#define NVT_G_DLM_CHECKSUM      0x3f104
#define NVT_BLD_CRC_EN          0x3f30e
#define NVT_MAX_READ            128
#define NVT_MAX_WRITE           1024            /* bytes of the firmware a transfer */
#define NVT_FINGERS             10
#define NVT_POINT_BYTES         (6 * NVT_FINGERS + 5)   /* POINT_DATA_LEN, the bytes nt36xxx reads a report */

static uint8_t spi_tx[NVT_MAX_WRITE + 2], spi_rx[NVT_MAX_WRITE + 2];
static uint32_t max_x = 1080, max_y = 2340;     /* the device tree's display-coords; the chip says its own */

/* Move the window to the page of `address`. */
static bool
nvt_page(uint32_t address)
{
	uint8_t tx[3] = { 0xff, (uint8_t)(address >> 15), (uint8_t)(address >> 7) }, rx[3];

	return spi_transfer(tx, rx, sizeof(tx));
}

/* `length` bytes into the chip's memory from `address` on, within the page already set (or past it: it counts on). */
static bool
nvt_write_here(uint32_t address, const uint8_t *data, uint32_t length)
{
	if (length > NVT_MAX_WRITE) {
		return false;
	}
	spi_tx[0] = (uint8_t)(address & 0x7f) | 0x80;   /* bit 7 set: a write */
	memcpy(spi_tx + 1, data, length);
	return spi_transfer(spi_tx, spi_rx, length + 1);
}

static bool
nvt_write_byte(uint32_t address, uint8_t value)
{
	return nvt_page(address) && nvt_write_here(address, &value, 1);
}

/* `length` bytes of the chip's memory from `address` on. */
static bool
nvt_read(uint32_t address, uint8_t *data, uint32_t length)
{
	if (length > NVT_MAX_READ || !nvt_page(address)) {
		return false;
	}
	memset(spi_tx, 0, length + 2);
	spi_tx[0] = (uint8_t)(address & 0x7f);  /* bit 7 clear: a read */
	if (!spi_transfer(spi_tx, spi_rx, length + 2)) {
		return false;
	}
	memcpy(data, spi_rx + 2, length);       /* after the address and the dummy byte */
	return true;
}

static void
hex_line(const char *what, const uint8_t *bytes, uint32_t length)
{
	char text[3 * 64 + 1];
	uint32_t n = 0;

	for (uint32_t i = 0; i < length && i < 64; i++) {
		n += (uint32_t)snprintf(text + n, sizeof(text) - n, " %02x", bytes[i]);
	}
	printf("novatek: %s:%s\n", what, text);
}

/* The boot loader's reset: the chip's processor starts over in the boot loader, ready to be given a firmware. */
static void
nvt_bootloader_reset(void)
{
	nvt_write_byte(NVT_SWRST_N8, 0x69);
	sleep_ms(5);
	nvt_write_byte(NVT_SPI_RD_FAST, 0x00);
}

/* nt36xxx_mem_map.h's trim_id_table, the rows that are an NT36672A: byte 0 and bytes 3 to 5 */
static const uint8_t nvt_trim_ids[][4] = {
	{ 0x0b, 0x72, 0x66, 0x03 }, { 0x0b, 0x82, 0x66, 0x03 }, { 0x0b, 0x25, 0x65, 0x03 }, { 0x0a, 0x72, 0x65, 0x03 },
	{ 0x0a, 0x72, 0x66, 0x03 }, { 0x0a, 0x82, 0x66, 0x03 }, { 0x0a, 0x70, 0x66, 0x03 },
};

static bool
nvt_check_chip(void)
{
	uint8_t id[6];

	for (int attempt = 0; attempt < 5; attempt++) {
		nvt_bootloader_reset();
		if (nvt_read(NVT_TRIM_ID + 0x4e, id, sizeof(id))) {
			hex_line("chip id", id, sizeof(id));
			for (unsigned i = 0; i < sizeof(nvt_trim_ids) / sizeof(nvt_trim_ids[0]); i++) {
				if (id[0] == nvt_trim_ids[i][0] && id[3] == nvt_trim_ids[i][1] && id[4] == nvt_trim_ids[i][2] &&
				    id[5] == nvt_trim_ids[i][3]) {
					printf("novatek: an NT36672A\n");
					return true;
				}
			}
		}
		sleep_ms(10);
	}
	printf("novatek: not an NT36672A that this driver knows\n");
	return false;
}

static uint32_t
word_at(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The ILM and DLM partitions' boot loader registers: where each goes, how long it is, its checksum. */
static void
nvt_crc_bank(uint32_t des, uint32_t sram, uint32_t length_at, uint32_t size, uint32_t checksum_at, uint32_t crc)
{
	uint8_t b[4];

	nvt_page(des);
	b[0] = (uint8_t)sram, b[1] = (uint8_t)(sram >> 8), b[2] = (uint8_t)(sram >> 16);
	nvt_write_here(des, b, 3);
	b[0] = (uint8_t)size, b[1] = (uint8_t)(size >> 8);
	nvt_write_here(length_at, b, 2);
	b[0] = (uint8_t)crc, b[1] = (uint8_t)(crc >> 8), b[2] = (uint8_t)(crc >> 16), b[3] = (uint8_t)(crc >> 24);
	nvt_write_here(checksum_at, b, 4);
}

/*
 * nvt_download_firmware_hw_crc: the boot loader reset, every partition the file's header lists
 * written where it says (each one byte longer than it says, as the vendor's driver writes them),
 * the ILM's and DLM's checksums given to the boot loader and its check switched on, the
 * firmware's own check as well, "boot ready" -- and then the firmware is to say it has started.
 */
static bool
nvt_download(const uint8_t *fw, uint64_t fw_size)
{
	struct { uint32_t bin, sram, size, crc; } part[32];
	uint32_t header_end, parts = 0, info, overlays, written = 0;
	uint8_t state[5];

	if (fw_size < 0x1a002 || (uint8_t)(fw[0x1a000] + fw[0x1a001]) != 0xff) {
		/* FW_BIN_VER_OFFSET and FW_BIN_VER_BAR_OFFSET (0x1A000, 0x1A001) */
		printf("novatek: the firmware file is not one (%llu bytes)\n", fw_size);
		return false;
	}
	header_end = word_at(fw);
	info = header_end > 0x30 ? (header_end - 0x30 + 15) / 16 : 0;
	overlays = (fw[0x28] & 0x10) ? (fw[0x28] & 0x0f) : 0;
	if (2 + info + overlays > 32) {
		printf("novatek: the firmware's header lists %u partitions: too many\n", 2 + info + overlays);
		return false;
	}
	for (uint32_t i = 0; i < 2; i++) {             /* ILM and DLM: BIN address, SRAM address, size; their CRCs at 0x18 */
		part[parts++] = (typeof(part[0])){ word_at(fw + 12 * i), word_at(fw + 4 + 12 * i), word_at(fw + 8 + 12 * i),
			                           word_at(fw + 0x18 + 4 * i) };
	}
	for (uint32_t i = 0; i < info; i++) {          /* the others: SRAM address, size, BIN address, CRC */
		const uint8_t *h = fw + 0x30 + 16 * i;

		part[parts++] = (typeof(part[0])){ word_at(h + 8), word_at(h), word_at(h + 4), word_at(h + 12) };
	}
	for (uint32_t i = 0; i < overlays; i++) {      /* the overlays: listed at the start of the DLM */
		const uint8_t *h = fw + part[1].bin + 16 * i;

		if (part[1].bin + 16 * i + 16 > fw_size) {
			return false;
		}
		part[parts++] = (typeof(part[0])){ word_at(h + 8), word_at(h), word_at(h + 4), word_at(h + 12) };
	}
	for (uint32_t i = 0; i < parts; i++) {
		if ((uint64_t)part[i].bin + part[i].size + 1 > fw_size) {
			printf("novatek: partition %u of the firmware goes past the end of the file\n", i);
			return false;
		}
	}
	printf("novatek: the firmware: version %u, %u partitions (ILM %u bytes at %#x, DLM %u at %#x)\n", fw[0x1a000], parts,
	    part[0].size, part[0].sram, part[1].size, part[1].sram);

	for (int attempt = 0; attempt < 3; attempt++) {
		nvt_bootloader_reset();
		for (uint32_t i = 0; i < parts; i++) {
			uint32_t left = part[i].size ? part[i].size + 1 : 0, sram = part[i].sram, bin = part[i].bin;

			while (left > 0) {
				uint32_t n = left < NVT_MAX_WRITE ? left : NVT_MAX_WRITE;

				if (!nvt_page(sram) || !nvt_write_here(sram, fw + bin, n)) {
					printf("novatek: writing the firmware failed at %#x\n", sram);
					return false;
				}
				sram += n, bin += n, left -= n, written += n;
			}
		}
		nvt_crc_bank(NVT_ILM_DES, part[0].sram, NVT_ILM_LENGTH, part[0].size, NVT_G_ILM_CHECKSUM, part[0].crc);
		nvt_crc_bank(NVT_DLM_DES, part[1].sram, NVT_DLM_LENGTH, part[1].size, NVT_G_DLM_CHECKSUM, part[1].crc);
		/* the boot loader's check on: bit 7 of BLD_CRC_EN */
		if (nvt_read(NVT_BLD_CRC_EN, state, 1)) {
			nvt_write_byte(NVT_BLD_CRC_EN, state[0] | 0x80);
		}
		/* the firmware's own: its reset state cleared, and the host command 0xae */
		nvt_write_byte(NVT_EVENT_BUF + NVT_EVENT_RESET_STATE, 0x00);
		nvt_write_byte(NVT_EVENT_BUF + NVT_EVENT_HOST_CMD, 0xae);
		nvt_write_byte(NVT_BOOT_RDY, 1);
		sleep_ms(5);
		for (int look = 0; look < 20; look++) {
			if (nvt_read(NVT_EVENT_BUF + NVT_EVENT_RESET_STATE, state, sizeof(state)) && state[0] >= NVT_RESET_INIT &&
			    state[0] <= 0xaf) {
				printf("novatek: the firmware has started (%u bytes written, reset state %#x)\n", written, state[0]);
				return true;
			}
			sleep_ms(10);
		}
		hex_line("the firmware did not start; its reset state", state, sizeof(state));
		written = 0;
	}
	return false;
}

/* The firmware's version and the coordinates' range; false if what comes back is not a Novatek's. */
static bool
nvt_identify(void)
{
	uint8_t info[16];

	for (int attempt = 0; attempt < 5; attempt++) {
		if (!nvt_read(NVT_EVENT_BUF + NVT_EVENT_FWINFO, info, sizeof(info))) {
			return false;
		}
		hex_line("firmware facts", info, sizeof(info));
		if ((uint8_t)(info[0] + info[1]) == 0xff) {
			uint32_t x = (uint32_t)info[4] << 8 | info[5], y = (uint32_t)info[6] << 8 | info[7];

			printf("novatek: firmware version %u, %u x %u sensing lines, coordinates up to %u x %u\n", info[0],
			    info[2], info[3], x, y);
			if (x >= 100 && x <= 8192 && y >= 100 && y <= 8192) {
				max_x = x;
				max_y = y;
			}
			return true;
		}
		sleep_ms(20);
	}
	printf("novatek: the version and its complement do not agree: not the chip, or not running its firmware\n");
	return false;
}

static struct finger {
	bool     down;
	uint32_t x, y;
} fingers[NVT_FINGERS];

static uint64_t reports, posted, failures;

static uint32_t
scale(uint32_t value, uint32_t range)
{
	if (value >= range) {
		value = range - 1;
	}
	return (uint32_t)((uint64_t)value * 65535 / (range - 1));
}

/* One look at the report: a record for each finger that has landed, moved or lifted since the last. */
static void
nvt_poll(void)
{
	uint8_t report[NVT_POINT_BYTES];
	bool seen[NVT_FINGERS] = { false };

	if (!nvt_read(NVT_EVENT_BUF, report, sizeof(report))) {
		failures++;
		return;
	}
	reports++;
	for (unsigned i = 0; i < NVT_FINGERS; i++) {
		const uint8_t *p = report + 6 * i;
		unsigned id = p[0] >> 3, status = p[0] & 7;
		uint32_t x, y;

		if (id == 0 || id > NVT_FINGERS || (status != 1 && status != 2)) {
			continue;
		}
		x = (uint32_t)p[1] << 4 | p[3] >> 4;
		y = (uint32_t)p[2] << 4 | (p[3] & 0xf);
		if (x > max_x || y > max_y) {
			continue;
		}
		id--;
		seen[id] = true;
		x = scale(x, max_x);
		y = scale(y, max_y);
		if (!fingers[id].down) {
			fingers[id] = (struct finger){ .down = true, .x = x, .y = y };
			redwood_touch_post(REDWOOD_TOUCH_DOWN, id, x, y);
			posted++;
			redwood_status(REDWOOD_STATUS_TOUCH, "ok, finger %u at %u,%u; %llu events", id, x * max_x / 65535,
			    y * max_y / 65535, posted);
			if (posted <= 20) {
				printf("novatek: finger %u down at %u, %u (of 65535)\n", id, x, y);
			}
		} else if (fingers[id].x != x || fingers[id].y != y) {
			fingers[id].x = x;
			fingers[id].y = y;
			redwood_touch_post(REDWOOD_TOUCH_MOVE, id, x, y);
			posted++;
		}
	}
	for (unsigned id = 0; id < NVT_FINGERS; id++) {
		if (fingers[id].down && !seen[id]) {
			fingers[id].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, id, fingers[id].x, fingers[id].y);
			posted++;
		}
	}
}

static void
novatek_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t poll_us = 10000;
	const char *problem;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	if (poll_us < 1000) {
		poll_us = 1000;
	}
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);

	redwood_status(REDWOOD_STATUS_TOUCH, "starting");
	printf("novatek: the pins as the firmware left them\n");
	for (unsigned pin = PIN_SPI_FIRST; pin < PIN_SPI_FIRST + 4; pin++) {
		gpio_describe("SPI", pin);
	}
	gpio_describe("reset", PIN_RESET);
	gpio_describe("interrupt", PIN_INT);

	if (!clocks_on()) {
		redwood_status(REDWOOD_STATUS_TOUCH, "FAILED: SPI clocks stay off");
		goto out;
	}
	for (unsigned pin = PIN_SPI_FIRST; pin < PIN_SPI_FIRST + 4; pin++) {
		gpio_configure(pin, PIN_FUNCTION_SPI, PULL_NONE, 6, false);
	}
	gpio_configure(PIN_INT, 0, PULL_NONE, 2, false);
	problem = geni_spi_init(4000000);
	if (problem != NULL) {
		printf("novatek: %s: no touch screen\n", problem);
		redwood_status(REDWOOD_STATUS_TOUCH, "FAILED: %s", problem);
		goto out;
	}
	/* as the vendor's probe: the "engineering" reset, the reset line high, 10 ms; then which chip it is */
	nvt_write_byte(NVT_ENG_RST, 0x5a);
	sleep_ms(1);
	if (!((rd(GPIO_CFG(PIN_RESET)) & (1u << 9)) && (rd(GPIO_IO(PIN_RESET)) & 2))) {
		printf("novatek: the reset line is not driven high: driving it high\n");
		gpio_set(PIN_RESET, true);
		gpio_configure(PIN_RESET, 0, PULL_NONE, 2, true);
	}
	sleep_ms(10);
	if (!nvt_check_chip()) {
		redwood_status(REDWOOD_STATUS_TOUCH, "FAILED: no NT36672A answers");
		goto out;
	}
	{
		uint64_t size = 0;
		const uint8_t *fw = redwood_image_file("fw", "novatek_ts_fw.bin", &size, NULL);

		if (fw == NULL) {
			printf("novatek: no fw/novatek_ts_fw.bin in the small image: put the phone's "
			    "/vendor/firmware/novatek_ts_tianma_fw.bin into fw-touch/ as novatek_ts_fw.bin and run userland/build.sh\n");
		}
		if (fw == NULL || !nvt_download(fw, size)) {
			redwood_status(REDWOOD_STATUS_TOUCH, "FAILED: %s", fw == NULL ? "no firmware file" : "firmware did not start");
			goto out;
		}
	}
	sleep_ms(50);
	if (!nvt_identify()) {
		goto out;
	}

	printf("novatek: watching, every %u us\n", poll_us);
	redwood_status(REDWOOD_STATUS_TOUCH, "ok, NT36672A, %u Hz", 1000000 / poll_us);
	for (unsigned tick = 1;; tick++) {
		nvt_poll();
		if (tick == 1) {
			uint8_t report[NVT_POINT_BYTES];

			if (nvt_read(NVT_EVENT_BUF, report, sizeof(report))) {
				hex_line("the first report", report, 24);
			}
		}
		if (tick % (60 * 1000000 / poll_us) == 0) {
			printf("novatek: %llu reports read, %llu failed, %llu records posted; interrupt line %s\n", reports,
			    failures, posted, gpio_get(PIN_INT) ? "high" : "low");
		}
		if (failures >= 1000 && failures * 2 > reports) {
			printf("novatek: more than half of the reads have failed: giving up\n");
			redwood_status(REDWOOD_STATUS_TOUCH, "FAILED: reads fail, gave up");
			break;
		}
		delay_for_interval(poll_us, NSEC_PER_USEC);
	}
out:
	thread_terminate(current_thread());
}

void
redwood_novatek_init(void)
{
	uint32_t enabled = 0;
	char device[16] = "";
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled ||
	    !PE_parse_boot_arg_str("device", device, sizeof(device)) || strcmp(device, "willow") != 0) {
		return;
	}
	if (kernel_thread_start(novatek_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
