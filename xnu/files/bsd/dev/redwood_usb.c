/*
 * The USB port of the Redmi Note 8T (willow, SM6125), looked at step by step on the way to a
 * network over the cable: Synopsys' DWC3 controller in Qualcomm's wrapper ("qcom,dwc-usb3-msm",
 * ssusb@4e00000) and its high-speed PHY, a QUSB2 (qusb@1613000). Only with boot-arg usb=N and
 * device=willow: steps 1 to N, every address below being this phone's.
 *
 * The willow's UEFI has no USB of its own, so nothing has the controller set up when the kernel
 * starts: the phone's boot loader (ABL) used it for fastboot and let it go. As with the GPU
 * (redwood_a610.c), reads come first, a register of the block is not read before its power
 * switch and clocks are on, and every step says what it is about to touch.
 *
 *   1  reads   the USB clocks in the global clock controller, their two sources, the block's
 *              power switch (usb30_prim_gdsc), the resets of the controller and the PHY, the TCSR
 *              clamp of the PHY's 1.8 V side, and the fuse with the PHY's tuning
 *   2  asks    the RPM for the PHY's 3.3 V: ldoa 15 (its 0.925 V, ldoa 7, and its 1.8 V, ldoa 10,
 *              UEFI already asks for: redwood_rpm.c's step 1 shows it)
 *   3  writes  the power switch on; the controller's clock at 133.33 MHz (GPLL0's early output /
 *              4.5, gcc-trinket.c's table) and the "mock UTMI" clock from the crystal; then the
 *              clocks on: the controller's, its sleep and UTMI clocks, both bus clocks, the PHY's
 *              reference and its register clock (ahb2phy)
 *   4  writes  the resets: the controller (GCC_USB30_PRIM_BCR) and the PHY (GCC_QUSB2PHY_PRIM_BCR)
 *              held for 100 us and let go
 *   5  reads   the controller: DWC3's ID (GSNPSID, 0x5533xxxx) and hardware parameters, its global
 *              and device control and status; Qualcomm's wrapper (QSCRATCH); the PHY's registers
 *   6  writes  the PHY as Linux's phy-msm-qusb.c (qusb_phy_init) does: the 1.8 V clamp let go,
 *              powered down, the device tree's tuning sequence and the fuse's tune2, powered up,
 *              and whether its PLL locks
 *   7  reads   the application processors' SMMU (apps-smmu@c600000, "qcom,skip-init": the firmware's
 *              to set up) for the controller's stream (dt: iommus <... 0x100 0x00>): what becomes of
 *              its DMA -- the event buffer and every packet go through it
 *   8  writes  the controller into device mode and onto the cable, as Linux's dwc3_core_init and
 *              dwc3_gadget_start with dwc3-msm.c's: the UTMI clock as the missing USB 3 PHY's (the
 *              port is high-speed only), the device tree's PHY quirks, a soft reset, high speed, an
 *              event buffer, endpoint 0 both ways and a transfer for the first SETUP packet, the
 *              events wanted, Run/Stop. Then for usb_watch=S seconds (default 60) its events are
 *              read and said: the host's bus reset, the speed it connects at, its SETUP packets,
 *              and endpoint 0 answers them (ep0_setup()): the descriptors of a device of the
 *              vendor's own class (1209:0001, "HackiOS", "Redmi Note 8T (iOS)"), its address, its
 *              configuration; what it does not know it stalls
 *   9  writes  ... and the device is a network adapter instead, for good: CDC-NCM (usbncm.sys in Windows 10
 *              and later, cdc_ncm in Linux, macOS's own), en0 on top of it (redwood_net.c). A function of
 *              two interfaces under an IAD: the communication interface with its notification endpoint
 *              (1 IN, interrupt), the data interface whose second setting has the bulk endpoints (2 IN,
 *              2 OUT). Frames go both ways in NTB16 blocks; the host's choice of that second setting
 *              is the cable plugged in. Instead of usb_watch the controller is then looked at every
 *              millisecond until the phone stops
 *
 * Addresses: the phone's device tree (ssusb@4e00000, dwc3@4e00000, qusb@1613000, gdsc@141a004,
 * rpm-regulator-ldoa15); layouts: Xiaomi's ginkgo-q-oss kernel, drivers/clk/qcom/gcc-trinket.c,
 * drivers/usb/phy/phy-msm-qusb.c, drivers/usb/dwc3/core.h and dwc3-msm.c.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/kpi_mbuf.h>
#include <sys/redwood_input.h>
#include <sys/redwood_net.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern volatile int redwood_rpm_done;
extern bool redwood_rpm_ldo(const char *what, uint32_t id, uint32_t microvolts);

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

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__builtin_arm_dsb(0xf);
}

#define GCC_PHYS                0x01400000UL
#define GCC_SIZE                0x90000UL
#define USB_PHYS                0x04e00000UL    /* dt: ssusb@4e00000, the controller and its wrapper */
#define USB_SIZE                0x100000UL
#define QUSB_PHYS               0x01613000UL    /* dt: qusb@1613000 */
#define QUSB_SIZE               0x1000UL
#define TCSR_CLAMP_PHYS         0x003cb250UL    /* dt: tcsr_clamp_dig_n_1p8 */
#define TUNE2_FUSE_PHYS         0x01b44258UL    /* dt: tune2_efuse_addr, bits 25-28 */

/* GCC (gcc-trinket.c) */
#define USB30_GDSC              0x1a004
#define USB30_BCR               0x1a000
#define QUSB2PHY_BCR            0x1c000
#define MASTER_RCG              0x1a01c         /* gcc_usb30_prim_master_clk_src: command, then configuration */
#define MOCK_UTMI_RCG           0x1a034
#define RCG_UPDATE              (1u << 0)
#define RCG_ROOT_OFF            (1u << 31)
#define CLOCK_OFF               (1u << 31)
#define CLOCK_ENABLE            (1u << 0)
#define GDSC_ON                 (1u << 31)
#define GDSC_SW_COLLAPSE        (1u << 0)

/* DWC3 (core.h), from the start of the block */
#define GCTL                    0xc110
#define GSTS                    0xc118
#define GSNPSID                 0xc120
#define GUID                    0xc128
#define GHWPARAMS(n)            (0xc140 + 4 * (n))
#define GUSB2PHYCFG             0xc200
#define DCFG                    0xc700
#define DCTL                    0xc704
#define DSTS                    0xc70c
#define QSCRATCH                0xf8800         /* dwc3-msm.c: QSCRATCH_REG_OFFSET */

/* QUSB2 v1 (phy-msm-qusb.c) */
#define QUSB_PLL_STATUS         0x38
#define QUSB_PLL_LOCK           (1u << 5)
#define QUSB_TUNE2              0x84
#define QUSB_POWERDOWN          0xb4
#define QUSB_CLAMP_N_EN         (1u << 5)
#define QUSB_FREEZIO_N          (1u << 1)
#define QUSB_POWER_DOWN         (1u << 0)

/* the apps SMMU (arm-smmu.h, SMMUv2) */
#define APPS_SMMU_PHYS          0x0c600000UL
#define APPS_SMMU_SIZE          0x80000UL
#define USB_STREAM              0x100           /* dt: iommus = <&apps_smmu 0x100 0x00> */

/* DWC3, device side (core.h, gadget.h) */
#define GUSB3PIPECTL            0xc2c0
#define GEVNTADRLO              0xc400
#define GEVNTADRHI              0xc404
#define GEVNTSIZ                0xc408
#define GEVNTCOUNT              0xc40c
#define DEVTEN                  0xc708
#define DALEPENA                0xc720
#define DEPCMDPAR2(n)           (0xc800 + 16 * (n))
#define DEPCMDPAR1(n)           (0xc804 + 16 * (n))
#define DEPCMDPAR0(n)           (0xc808 + 16 * (n))
#define DEPCMD(n)               (0xc80c + 16 * (n))
#define DEPCMD_ACTIVE           (1u << 10)
#define DEPCMD_SETEPCONFIG      0x01
#define DEPCMD_SETTRANSFRESOURCE 0x02
#define DEPCMD_SETSTALL         0x04
#define DEPCMD_STARTTRANSFER    0x06
#define DEPCMD_DEPSTARTCFG      0x09
#define DCTL_RUN_STOP           (1u << 31)
#define DCTL_CSFTRST            (1u << 30)
#define DSTS_DEVCTRLHLT         (1u << 22)
#define GCTL_PRTCAPDIR_MASK     (3u << 12)
#define GCTL_PRTCAP_DEVICE      (2u << 12)
#define GCTL_DSBLCLKGTNG        (1u << 0)
#define GUSB2PHYCFG_SUSPHY      (1u << 6)
#define GUSB2PHYCFG_ENBLSLPM    (1u << 8)
#define QSCRATCH_GENERAL_CFG    (QSCRATCH + 0x08)
#define HS_PHY_CTRL             (QSCRATCH + 0x10)
#define UTMI_OTG_VBUS_VALID     (1u << 20)
#define SW_SESSVLD_SEL          (1u << 28)
#define PIPE_UTMI_CLK_SEL       (1u << 0)
#define PIPE3_PHYSTATUS_SW      (1u << 3)
#define PIPE_UTMI_CLK_DIS       (1u << 8)
#define TRB_HWO                 (1u << 0)
#define TRB_LST                 (1u << 1)
#define TRB_CONTROL_SETUP       (2u << 4)
#define TRB_ISP_IMI             (1u << 10)
#define TRB_IOC                 (1u << 11)
#define EVENT_BUFFER_SIZE       4096
#define DMA_SETUP_TRB           4096            /* in our DMA page: after the event buffer */
#define DMA_SETUP_PACKET        (4096 + 64)
#define DMA_EP0_TRB(n)          (4096 + 128 + 64 * (n))         /* endpoint 0's data and status TRBs, OUT and IN */
#define DMA_EP0_DATA            (4096 + 1024)                   /* what goes either way in a data stage */
#define EP0_DATA_MAX            1024
#define TRB_CONTROL_STATUS2     (3u << 4)
#define TRB_CONTROL_STATUS3     (4u << 4)
#define TRB_CONTROL_DATA        (5u << 4)
#define DEPCMD_ENDTRANSFER      0x08
#define DEPCMD_HIPRI_FORCERM    (1u << 11)
#define DEPEVT_CONTROL_STATUS   2               /* "transfer not ready" for a control endpoint: the host is at the status stage */
#define DEPCMD_CLEARSTALL       0x05            /* ... which also starts the endpoint's data toggle over */
#define DEPCMD_PARAM(n)         ((n) << 16)
#define TRB_NORMAL              (1u << 4)
#define GTXFIFOSIZ(n)           (0xc300 + 4 * (n))

static vm_offset_t gcc, usb, qusb, tcsr_clamp, fuse, apps_smmu;
static vm_offset_t dma;                 /* one page of ours for the controller: events, the SETUP TRB and packet */
static uint64_t dma_phys;
static uint32_t event_at;               /* where in the event buffer the next event is */

extern uint64_t kvtophys(vm_offset_t va);
#include <vm/vm_kern.h>

static void
to_memory(vm_offset_t start, vm_size_t size)
{
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__builtin_arm_dsb(0xf);
}

/* An endpoint command (physical endpoint n) with its three parameters; its status (bits 15:12), or ~0 if it never ends. */
static uint32_t
ep_cmd(uint32_t n, uint32_t command, uint32_t p0, uint32_t p1, uint32_t p2)
{
	int i;

	wr(usb + DEPCMDPAR0(n), p0);
	wr(usb + DEPCMDPAR1(n), p1);
	wr(usb + DEPCMDPAR2(n), p2);
	wr(usb + DEPCMD(n), command | DEPCMD_ACTIVE);
	for (i = 0; i < 1000 && (rd(usb + DEPCMD(n)) & DEPCMD_ACTIVE); i++) {
		delay_for_interval(10, NSEC_PER_USEC);
	}
	if (rd(usb + DEPCMD(n)) & DEPCMD_ACTIVE) {
		printf("usb: endpoint %u: command %#x DOES NOT END (%#x)\n", n, command, rd(usb + DEPCMD(n)));
		return ~0u;
	}
	return rd(usb + DEPCMD(n)) >> 12 & 0xf;
}

/* A transfer on endpoint 0 OUT for the next SETUP packet: one TRB of 8 bytes. */
static uint32_t ep0_resource[2];        /* each direction's transfer resource, for an end to it */

static uint32_t
ep0_start(uint32_t n, uint32_t trb_at, uint64_t buffer, uint32_t size, uint32_t control)
{
	volatile uint32_t *trb = (volatile uint32_t *)(dma + trb_at);
	uint64_t trb_phys = dma_phys + trb_at;
	uint32_t status;

	trb[0] = (uint32_t)buffer;
	trb[1] = (uint32_t)(buffer >> 32);
	trb[2] = size;
	trb[3] = TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC | control;
	to_memory(dma + trb_at, 16);
	status = ep_cmd(n, DEPCMD_STARTTRANSFER, (uint32_t)(trb_phys >> 32), (uint32_t)trb_phys, 0);
	ep0_resource[n] = rd(usb + DEPCMD(n)) >> 16 & 0x7f;
	return status;
}

static uint32_t
ep0_setup_start(void)
{
	bzero((void *)(dma + DMA_SETUP_PACKET), 8);
	to_memory(dma + DMA_SETUP_PACKET, 8);
	return ep0_start(0, DMA_SETUP_TRB, dma_phys + DMA_SETUP_PACKET, 8, TRB_CONTROL_SETUP);
}

/*
 * ---- endpoint 0: control transfers (Linux's drivers/usb/dwc3/ep0.c) ----
 *
 * SETUP (a transfer on 0 OUT), then a data stage if the request has one -- started right away, in
 * the direction it names -- then the status stage, which the controller asks for itself: "transfer
 * not ready" with the status code, on the endpoint of the status stage's direction, and the TRB is
 * CONTROL_STATUS3 after a data stage and CONTROL_STATUS2 without one. When that completes, the
 * next SETUP is awaited. A request not understood is stalled.
 */
enum ep0_state { EP0_SETUP, EP0_DATA, EP0_STATUS_WAIT, EP0_STATUS };
static enum ep0_state ep0_state;
static bool ep0_three_stage;
static uint32_t ep0_configuration;

/* The device and what it is made of (usb_20.pdf chapter 9). Vendor 0x1209 is pid.codes', 0x0001 its PID for tests. */
static const uint8_t device_descriptor[18] = {
	18, 1, 0x00, 0x02,                      /* USB 2.0 */
	0x00, 0x00, 0x00, 64,                   /* the class is the interface's; packets of 64 on endpoint 0 */
	0x09, 0x12, 0x01, 0x00,                 /* 1209:0001 */
	0x00, 0x01, 1, 2,                       /* release 1.00, strings 1 (maker) and 2 (product) */
	3, 1,                                   /* 3 the serial number; one configuration */
};
static const uint8_t configuration_descriptor[] = {
	9, 2, 18, 0, 1, 1, 0, 0x80, 250,        /* 18 bytes in all, one interface, configuration 1, bus powered, 500 mA */
	9, 4, 0, 0, 0, 0xff, 0, 0, 2,           /* interface 0: no endpoints yet, vendor's own class, string 2 */
};
static const char *const strings[] = { NULL, "HackiOS", "Redmi Note 8T (iOS)", "willow0001", "0248694F5301" };

/*
 * Step 9's device instead: a network adapter, CDC-NCM (usbncm11.pdf), as Linux's f_ncm.c describes
 * one. Class EF/02/01 so that the host reads the IAD and gives the function to its NCM driver.
 * Release 2.00 and not 1.00: Windows keeps what it decided about a VID:PID:release, and 1.00 was
 * the device without a driver of steps 8.
 */
static const uint8_t ncm_device_descriptor[18] = {
	18, 1, 0x10, 0x02,                      /* USB 2.10: a BOS descriptor is there to be asked for */
	0xef, 0x02, 0x01, 64,                   /* "miscellaneous: interface association" */
	0x09, 0x12, 0x01, 0x00,                 /* 1209:0001 */
	0x01, 0x02, 1, 2,                       /* release 2.01: Windows decided about 2.00 without OS descriptors */
	3, 1,
};

/*
 * Microsoft OS descriptors (MS_OS_2_0_desc.docx; OS_Desc_Intro, OS_Desc_CompatID for 1.0): Windows 10's
 * own NCM driver, UsbNcm.sys, is installed by usbncm.inf for the compatible ID USB\MS_COMP_WINNCM alone --
 * the line that would match any NCM interface by class is commented out -- and a device names that ID
 * for a function through these. willow-ncm1: Windows asked for string 0xEE, was stalled, and left the
 * function without a driver. Both kinds: 2.0, which Windows 8.1 and later read from the BOS of a USB
 * 2.10 device, and 1.0 (string 0xEE), which older Windows read.
 */
#define MSOS_VENDOR_CODE        0x20            /* the vendor request Windows sends for them */
#define MSOS20_SET_SIZE         46
static const uint8_t bos_descriptor[40] = {
	5, 0x0f, 40, 0, 2,                      /* BOS: 40 bytes, two capabilities */
	7, 0x10, 0x02, 0, 0, 0, 0,              /* USB 2.0 extension: no LPM */
	28, 0x10, 0x05, 0,                      /* platform capability: */
	0xdf, 0x60, 0xdd, 0xd8, 0x89, 0x45, 0xc7, 0x4c, /* ... MS OS 2.0, {D8DD60DF-4589-4CC7-9CD2-659D9E648A9F} */
	0x9c, 0xd2, 0x65, 0x9d, 0x9e, 0x64, 0x8a, 0x9f,
	0x00, 0x00, 0x03, 0x06,                 /* Windows 8.1 (0x06030000) and later */
	MSOS20_SET_SIZE, 0, MSOS_VENDOR_CODE, 0, /* the set's length, the vendor code, no alternate enumeration */
};
static const uint8_t msos20_set[MSOS20_SET_SIZE] = {
	10, 0, 0x00, 0, 0x00, 0x00, 0x03, 0x06, MSOS20_SET_SIZE, 0,     /* set header */
	8, 0, 0x01, 0, 0, 0, MSOS20_SET_SIZE - 10, 0,                   /* configuration subset: the first configuration */
	8, 0, 0x02, 0, 0, 0, MSOS20_SET_SIZE - 18, 0,                   /* function subset: from interface 0 */
	20, 0, 0x03, 0, 'W', 'I', 'N', 'N', 'C', 'M', 0, 0,             /* compatible ID WINNCM */
	0, 0, 0, 0, 0, 0, 0, 0,
};
static const uint8_t msos10_compat_id[40] = {
	40, 0, 0, 0, 0x00, 0x01, 0x04, 0x00, 1, 0, 0, 0, 0, 0, 0, 0,    /* header: 40 bytes, 1.00, index 4, one function */
	0, 0x01, 'W', 'I', 'N', 'N', 'C', 'M', 0, 0,                    /* interface 0: WINNCM */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
#define NCM_CONFIGURATION_SIZE  94
static const uint8_t ncm_configuration_descriptor[NCM_CONFIGURATION_SIZE] = {
	9, 2, NCM_CONFIGURATION_SIZE, 0, 2, 1, 0, 0x80, 250,
	8, 0x0b, 0, 2, 0x02, 0x0d, 0x00, 0,     /* IAD: interfaces 0 and 1, CDC NCM */
	9, 4, 0, 0, 1, 0x02, 0x0d, 0x00, 0,     /* interface 0, communication: CDC NCM, one endpoint */
	5, 0x24, 0x00, 0x10, 0x01,              /* CDC header, 1.10 */
	5, 0x24, 0x06, 0, 1,                    /* union: 0 controls 1 */
	13, 0x24, 0x0f, 4, 0, 0, 0, 0,          /* Ethernet: the host side's address is string 4; no statistics */
	0xea, 0x05, 0, 0, 0,                    /* ... 1514-byte frames, no multicast or power filters */
	6, 0x24, 0x1a, 0x00, 0x01, 0x01,        /* NCM 1.00: SET_ETHERNET_PACKET_FILTER is understood */
	7, 5, 0x81, 3, 16, 0, 9,                /* endpoint 1 IN, interrupt, 16 bytes, every 2^8 microframes (32 ms) */
	9, 4, 1, 0, 0, 0x0a, 0x00, 0x01, 0,     /* interface 1, data, setting 0: no endpoints (the cable unplugged) */
	9, 4, 1, 1, 2, 0x0a, 0x00, 0x01, 0,     /* ... setting 1: the frames */
	7, 5, 0x82, 2, 0x00, 0x02, 0,           /* endpoint 2 IN, bulk, 512 */
	7, 5, 0x02, 2, 0x00, 0x02, 0,           /* endpoint 2 OUT, bulk, 512 */
};

/* The physical endpoints of step 9 (DWC3 numbers them 2 * number + IN) and the memory they move through */
#define EP_NOTIFY               3
#define EP_BULK_OUT             4
#define EP_BULK_IN              5
#define NCM_NTB_MAX             16384           /* the largest block either way; the buffer for each */
#define NCM_OUT                 0               /* offsets in ncm_dma */
#define NCM_IN                  NCM_NTB_MAX
#define NCM_NOTIFICATION        (2 * NCM_NTB_MAX)
#define NCM_TRB(n)              (2 * NCM_NTB_MAX + 1024 + 64 * (n))
#define NCM_DMA_SIZE            (3 * NCM_NTB_MAX)
#define NTH16_SIGNATURE         0x484d434eu     /* "NCMH" */
#define NDP16_SIGNATURE         0x304d434eu     /* "NCM0": no CRC */
#define NCM_NDP_MAX             16              /* datagrams in one block we send */
#define NCM_TX_QUEUE            64

static bool ncm;                        /* step 9: the network adapter */
static vm_offset_t ncm_dma;
static uint64_t ncm_dma_phys;
static uint32_t ncm_alternate;          /* the data interface's setting: 1 is the cable in */
static bool ncm_configured;
static uint32_t ncm_in_max = NCM_NTB_MAX;       /* what the host takes at most (SET_NTB_INPUT_SIZE) */
static uint32_t ncm_resource[6];
static bool ncm_busy[6];
static uint32_t ncm_notified;           /* how many of the two notifications have gone */
static uint16_t ncm_sequence;
static uint32_t ep0_out_request;        /* the class request whose data is coming in on endpoint 0 */
static struct {
	uint64_t blocks_in, blocks_out, frames_in, frames_out, bad_in, dropped_out;
} ncm_count;
static struct ncm_frame {
	uint16_t length;
	uint8_t  bytes[REDWOOD_NET_FRAME_MAX];
} *ncm_queue;
static uint32_t ncm_queue_in, ncm_queue_out;
static LCK_GRP_DECLARE(usb_lck_grp, "redwood-usb");
static LCK_MTX_DECLARE(ncm_queue_lock, &usb_lck_grp);
static bool ncm_transmit(mbuf_t packet, size_t length);
static struct redwood_net_link ncm_link = {
	.name = "USB (CDC-NCM)",
	.address = { 0x02, 0x48, 0x69, 0x4f, 0x53, 0x02 },     /* the host side is ...:01 (string 4) */
	.transmit = ncm_transmit,
};
static bool ncm_attached;

/* A string descriptor of `text` into `out`: UTF-16, as many characters as fit. */
static uint32_t
string_descriptor(const char *text, uint8_t *out)
{
	uint32_t n = 0;

	while (text[n] != 0 && 2 + 2 * (n + 1) <= 254) {
		out[2 + 2 * n] = (uint8_t)text[n];
		out[3 + 2 * n] = 0;
		n++;
	}
	out[0] = (uint8_t)(2 + 2 * n);
	out[1] = 3;
	return out[0];
}

/*
 * ---- step 9: the network adapter (usbncm11.pdf; Linux's f_ncm.c for what a host expects) ----
 *
 * Every transfer is one TRB with LST, started again when it completes: the bulk OUT one always
 * waiting for the host's next block, the bulk IN one whenever frames are queued and it is free.
 */
static uint32_t
ncm_start(uint32_t n, uint32_t at, uint32_t size)
{
	volatile uint32_t *trb = (volatile uint32_t *)(ncm_dma + NCM_TRB(n));
	uint64_t buffer = ncm_dma_phys + at, trb_phys = ncm_dma_phys + NCM_TRB(n);
	uint32_t status;

	trb[0] = (uint32_t)buffer;
	trb[1] = (uint32_t)(buffer >> 32);
	trb[2] = size;
	trb[3] = TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC | TRB_NORMAL;
	to_memory(ncm_dma + NCM_TRB(n), 16);
	status = ep_cmd(n, DEPCMD_STARTTRANSFER, (uint32_t)(trb_phys >> 32), (uint32_t)trb_phys, 0);
	ncm_resource[n] = rd(usb + DEPCMD(n)) >> 16 & 0x7f;
	ncm_busy[n] = status == 0;
	if (status != 0) {
		printf("usb: endpoint %u: a transfer does not start (%#x)\n", n, status);
	}
	return status;
}

static void
ncm_end(uint32_t n)
{
	if (ncm_busy[n]) {
		ep_cmd(n, DEPCMD_ENDTRANSFER | DEPCMD_HIPRI_FORCERM | DEPCMD_PARAM(ncm_resource[n]), 0, 0, 0);
		delay_for_interval(100, NSEC_PER_USEC);
		ncm_busy[n] = false;
	}
}

/* The host's next block in: the buffer handed back to the controller clean, so that no line of it is written over the DMA. */
static void
ncm_receive_start(void)
{
	to_memory(ncm_dma + NCM_OUT, NCM_NTB_MAX);
	ncm_start(EP_BULK_OUT, NCM_OUT, NCM_NTB_MAX);
}

/* The two notifications the host waits for, one after the other: the speed, then "connected". */
static void
ncm_notify(void)
{
	uint8_t *n = (uint8_t *)(ncm_dma + NCM_NOTIFICATION);
	static const uint8_t speed[16] = { 0xa1, 0x2a, 0, 0, 0, 0, 8, 0, 0x00, 0x38, 0x9c, 0x1c, 0x00, 0x38, 0x9c, 0x1c };
	static const uint8_t connected[8] = { 0xa1, 0x00, 1, 0, 0, 0, 0, 0 };       /* NETWORK_CONNECTION, wValue 1 */

	if (ncm_busy[EP_NOTIFY] || ncm_notified >= 2 || ncm_alternate != 1) {
		return;
	}
	if (ncm_notified == 0) {
		memcpy(n, speed, sizeof(speed));        /* 480 Mbit/s each way */
	} else {
		memcpy(n, connected, sizeof(connected));
	}
	to_memory(ncm_dma + NCM_NOTIFICATION, 16);
	ncm_start(EP_NOTIFY, NCM_NOTIFICATION, ncm_notified == 0 ? sizeof(speed) : sizeof(connected));
}

static void
ncm_link_state(bool up)
{
	if (ncm_attached) {
		redwood_net_link_state(up);
	}
}

/* SET_CONFIGURATION 1: the three endpoints, as dwc3_gadget_start_config and __dwc3_gadget_ep_enable do. */
static void
ncm_configure(void)
{
	static const struct {
		uint32_t n, type, size, fifo, interval;
	} eps[] = {
		{ EP_NOTIFY, 3, 16, 1, 9 }, { EP_BULK_OUT, 2, 512, 0, 0 }, { EP_BULK_IN, 2, 512, 2, 0 },
	};
	uint32_t status;

	if (ncm_configured) {
		return;
	}
	/* the transfer resources of every endpoint but 0's given back, before any of them is configured */
	status = ep_cmd(0, DEPCMD_DEPSTARTCFG | DEPCMD_PARAM(2), 0, 0, 0);
	for (unsigned i = 0; i < sizeof(eps) / sizeof(eps[0]) && status == 0; i++) {
		status = ep_cmd(eps[i].n, DEPCMD_SETEPCONFIG, eps[i].type << 1 | eps[i].size << 3 | eps[i].fifo << 17,
		    1u << 8 | (eps[i].interval ? eps[i].interval - 1 : 0) << 16 | eps[i].n << 25, 0);
		if (status == 0) {
			status = ep_cmd(eps[i].n, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0);
		}
	}
	printf("usb: the network adapter's endpoints configured: status %#x; TX FIFOs %#x %#x %#x %#x\n", status,
	    rd(usb + GTXFIFOSIZ(0)), rd(usb + GTXFIFOSIZ(1)), rd(usb + GTXFIFOSIZ(2)), rd(usb + GTXFIFOSIZ(3)));
	if (status != 0) {
		return;
	}
	wr(usb + DALEPENA, rd(usb + DALEPENA) | 1u << EP_NOTIFY | 1u << EP_BULK_OUT | 1u << EP_BULK_IN);
	ncm_configured = true;
}

/* The data interface's setting: 1 is the cable plugged in, 0 unplugged (and the host's way of starting over). */
static void
ncm_set_alternate(uint32_t alternate)
{
	ncm_end(EP_BULK_OUT);
	ncm_end(EP_BULK_IN);
	ncm_end(EP_NOTIFY);
	ncm_alternate = alternate;
	ncm_notified = 0;
	ncm_sequence = 0;
	lck_mtx_lock(&ncm_queue_lock);
	ncm_queue_out = ncm_queue_in;
	lck_mtx_unlock(&ncm_queue_lock);
	if (alternate == 1 && ncm_configured) {
		/* (a new setting starts every endpoint of it at DATA0) */
		ep_cmd(EP_BULK_OUT, DEPCMD_CLEARSTALL, 0, 0, 0);
		ep_cmd(EP_BULK_IN, DEPCMD_CLEARSTALL, 0, 0, 0);
		ncm_receive_start();
		ncm_notify();
	}
	printf("usb: the network adapter's data interface: setting %u (%s)\n", alternate,
	    alternate == 1 ? "THE CABLE IS IN" : "unplugged");
	ncm_link_state(alternate == 1 && ncm_configured);
}

/* A bus reset or a new configuration: nothing configured, nothing connected. */
static void
ncm_reset(void)
{
	if (ncm_configured) {
		ncm_set_alternate(0);
		wr(usb + DALEPENA, 3);
		ncm_configured = false;
	}
}

static inline uint32_t
le16(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8;
}

static inline uint32_t
le32(const uint8_t *p)
{
	return le16(p) | le16(p + 2) << 16;
}

static inline void
put16(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static inline void
put32(uint8_t *p, uint32_t value)
{
	put16(p, value);
	put16(p + 2, value >> 16);
}

/* A block from the host: NTH16, then its NDP16s, each a list of (offset, length) of the frames. */
static void
ncm_receive(const uint8_t *block, uint32_t length)
{
	uint32_t ndp, frames = 0;

	ncm_count.blocks_in++;
	if (length < 12 || le32(block) != NTH16_SIGNATURE) {
		if (ncm_count.bad_in++ < 10) {
			printf("usb: a block of %u bytes from the host that is not an NTB16 (%#x)\n", length, length >= 4 ? le32(block) : 0);
		}
		return;
	}
	ndp = le16(block + 10);
	for (int chained = 0; ndp != 0 && chained < 8; chained++) {
		uint32_t ndp_length;

		if (ndp + 16 > length || le32(block + ndp) != NDP16_SIGNATURE) {
			if (ncm_count.bad_in++ < 10) {
				printf("usb: a block whose NDP at %u is not an NDP16 (%#x)\n", ndp, ndp + 4 <= length ? le32(block + ndp) : 0);
			}
			break;
		}
		ndp_length = le16(block + ndp + 4);
		for (uint32_t entry = ndp + 8; entry + 4 <= ndp + ndp_length && entry + 4 <= length; entry += 4) {
			uint32_t at = le16(block + entry), size = le16(block + entry + 2);

			if (at == 0 || size == 0) {
				break;
			}
			if (at + size <= length && size <= REDWOOD_NET_FRAME_MAX) {
				frames++;
				redwood_net_input(block + at, size);
			}
		}
		ndp = le16(block + ndp + 6);
	}
	ncm_count.frames_in += frames;
}

/* en0 has a frame to send: into the queue, which the USB thread empties into blocks. */
static bool
ncm_transmit(mbuf_t packet, size_t length)
{
	bool queued = false;

	lck_mtx_lock(&ncm_queue_lock);
	if (ncm_alternate == 1 && length <= REDWOOD_NET_FRAME_MAX && ncm_queue_in - ncm_queue_out < NCM_TX_QUEUE) {
		struct ncm_frame *frame = &ncm_queue[ncm_queue_in % NCM_TX_QUEUE];

		if (mbuf_copydata(packet, 0, length, frame->bytes) == 0) {
			frame->length = (uint16_t)length;
			ncm_queue_in++;
			queued = true;
		}
	}
	lck_mtx_unlock(&ncm_queue_lock);
	if (!queued) {
		ncm_count.dropped_out++;
	}
	return queued;
}

/* If the IN endpoint is free and frames wait: as many as fit into one block, sent. */
static bool
ncm_send(void)
{
	uint8_t *block = (uint8_t *)(ncm_dma + NCM_IN);
	uint32_t at = (12 + 8 + 4 * (NCM_NDP_MAX + 1) + 3) & ~3u, frames = 0, limit;

	if (ncm_alternate != 1 || ncm_busy[EP_BULK_IN] || ncm_queue_in == ncm_queue_out) {
		return false;
	}
	limit = (ncm_in_max < NCM_NTB_MAX ? ncm_in_max : NCM_NTB_MAX) - 1;      /* (- 1: room for the byte below) */
	lck_mtx_lock(&ncm_queue_lock);
	while (ncm_queue_out != ncm_queue_in && frames < NCM_NDP_MAX) {
		struct ncm_frame *frame = &ncm_queue[ncm_queue_out % NCM_TX_QUEUE];

		if (at + frame->length > limit) {
			break;
		}
		memcpy(block + at, frame->bytes, frame->length);
		put16(block + 12 + 8 + 4 * frames, at);
		put16(block + 12 + 8 + 4 * frames + 2, frame->length);
		at = (at + frame->length + 3) & ~3u;    /* wNdpInDivisor 4 */
		frames++;
		ncm_queue_out++;
	}
	lck_mtx_unlock(&ncm_queue_lock);
	if (frames == 0) {
		return false;
	}
	put32(block + 12 + 8 + 4 * frames, 0);  /* the list's end */
	/* a block of a whole number of packets would need a zero-length one after it: one byte more instead */
	if (at % 512 == 0) {
		block[at++] = 0;
	}
	put32(block, NTH16_SIGNATURE);
	put16(block + 4, 12);
	put16(block + 6, ncm_sequence++);
	put16(block + 8, at);
	put16(block + 10, 12);                  /* the NDP right after the header */
	put32(block + 12, NDP16_SIGNATURE);
	put16(block + 16, 8 + 4 * (frames + 1));
	put16(block + 18, 0);
	to_memory(ncm_dma + NCM_IN, at);
	if (ncm_start(EP_BULK_IN, NCM_IN, at) == 0) {
		ncm_count.blocks_out++;
		ncm_count.frames_out += frames;
	}
	return true;
}

/* An event of one of step 9's endpoints. */
static void
ncm_event(uint32_t ep, uint32_t type)
{
	if (type != 1 || ep > EP_BULK_IN || !ncm_busy[ep]) {
		return;
	}
	ncm_busy[ep] = false;
	if (ep == EP_BULK_OUT) {
		volatile uint32_t *trb = (volatile uint32_t *)(ncm_dma + NCM_TRB(ep));
		uint32_t length;

		to_memory(ncm_dma + NCM_TRB(ep), 16);
		length = NCM_NTB_MAX - (trb[2] & 0xffffff);
		to_memory(ncm_dma + NCM_OUT, length);
		ncm_receive((const uint8_t *)(ncm_dma + NCM_OUT), length);
		if (ncm_alternate == 1) {
			ncm_receive_start();
		}
	} else if (ep == EP_NOTIFY) {
		ncm_notified++;
		if (ncm_notified == 2) {
			printf("usb: the host has been told the adapter is connected\n");
		}
		ncm_notify();
	}
}

/* The data of a class request that came in on endpoint 0 (SET_NTB_INPUT_SIZE is the one with any). */
static void
ep0_out_data(void)
{
	const uint8_t *data = (const uint8_t *)(dma + DMA_EP0_DATA);

	to_memory(dma + DMA_EP0_DATA, 64);
	if (ep0_out_request == 0x86) {
		ncm_in_max = le32(data);
		printf("usb: the host takes blocks of up to %u bytes\n", ncm_in_max);
	}
}

/* NCM's own requests (the communication interface's): their answer's length, or -1. */
static int
ncm_answer(const uint8_t *setup, uint8_t *out)
{
	uint32_t type = setup[0], request = setup[1];

	if (type == 0xa1 && request == 0x80) {  /* GET_NTB_PARAMETERS */
		bzero(out, 28);
		put16(out, 28);
		put16(out + 2, 1);              /* NTB16 only */
		put32(out + 4, NCM_NTB_MAX);    /* dwNtbInMaxSize */
		put16(out + 8, 4);              /* wNdpInDivisor */
		put16(out + 10, 0);
		put16(out + 12, 4);             /* wNdpInAlignment */
		put32(out + 16, NCM_NTB_MAX);   /* dwNtbOutMaxSize */
		put16(out + 20, 4);             /* wNdpOutDivisor */
		put16(out + 22, 0);
		put16(out + 24, 4);             /* wNdpOutAlignment */
		put16(out + 26, NCM_NDP_MAX);   /* wNtbOutMaxDatagrams */
		return 28;
	}
	if (type == 0xa1 && request == 0x85) {  /* GET_NTB_INPUT_SIZE */
		put32(out, ncm_in_max);
		return 4;
	}
	if (type == 0xa1 && request == 0x83) {  /* GET_NTB_FORMAT: 16-bit */
		put16(out, 0);
		return 2;
	}
	if (type == 0xa1 && request == 0x87) {  /* GET_MAX_DATAGRAM_SIZE */
		put16(out, REDWOOD_NET_FRAME_MAX);
		return 2;
	}
	if (type == 0x21 && (request == 0x43 || request == 0x84 || request == 0x88 || request == 0x8a)) {
		return 0;                       /* SET_ETHERNET_PACKET_FILTER, SET_NTB_FORMAT, ..._DATAGRAM_SIZE, ..._CRC_MODE */
	}
	return -1;
}

/* The answer to a request, into the data buffer; its length, or -1 to stall it. */
static int
ep0_answer(const uint8_t *setup, uint8_t *out)
{
	uint32_t type = setup[0], request = setup[1], value = setup[2] | setup[3] << 8, index = setup[4] | setup[5] << 8;

	if (ncm && (type & 0x60) == 0x20) {     /* a class request: the network adapter's */
		return ncm_answer(setup, out);
	}
	if (ncm && (type == 0xc0 || type == 0xc1) && request == MSOS_VENDOR_CODE) {
		/* Windows' questions about the Microsoft OS descriptors: 7 the 2.0 set, 4 the 1.0 compatible ID */
		if (index == 7) {
			memcpy(out, msos20_set, sizeof(msos20_set));
			printf("usb: Windows asks for the MS OS 2.0 set: WINNCM for interface 0\n");
			return sizeof(msos20_set);
		}
		if (index == 4) {
			memcpy(out, msos10_compat_id, sizeof(msos10_compat_id));
			printf("usb: Windows asks for the MS OS 1.0 compatible ID: WINNCM for interface 0\n");
			return sizeof(msos10_compat_id);
		}
		return -1;
	}
	if (ncm && type == 0x01 && request == 11) {     /* SET_INTERFACE */
		if (index == 1) {
			ncm_set_alternate(value);
		}
		return 0;
	}
	if (ncm && type == 0x81 && request == 10) {     /* GET_INTERFACE */
		out[0] = index == 1 ? (uint8_t)ncm_alternate : 0;
		return 1;
	}
	if (type == 0x80 && request == 6) {     /* GET_DESCRIPTOR */
		switch (value >> 8) {
		case 1:
			memcpy(out, ncm ? ncm_device_descriptor : device_descriptor, sizeof(device_descriptor));
			return sizeof(device_descriptor);
		case 2:
			if (ncm) {
				memcpy(out, ncm_configuration_descriptor, sizeof(ncm_configuration_descriptor));
				return sizeof(ncm_configuration_descriptor);
			}
			memcpy(out, configuration_descriptor, sizeof(configuration_descriptor));
			return sizeof(configuration_descriptor);
		case 15:                                /* BOS: the MS OS 2.0 capability (the adapter only) */
			if (!ncm) {
				return -1;
			}
			memcpy(out, bos_descriptor, sizeof(bos_descriptor));
			return sizeof(bos_descriptor);
		case 3:
			if (ncm && (value & 0xff) == 0xee) {    /* MS OS 1.0: "MSFT100" and the vendor code */
				static const char sig[] = "MSFT100";

				out[0] = 18;
				out[1] = 3;
				for (int i = 0; i < 7; i++) {
					out[2 + 2 * i] = (uint8_t)sig[i];
					out[3 + 2 * i] = 0;
				}
				out[16] = MSOS_VENDOR_CODE;
				out[17] = 0;
				return 18;
			}
			if ((value & 0xff) == 0) {
				out[0] = 4;
				out[1] = 3;
				out[2] = 0x09;          /* English (United States) */
				out[3] = 0x04;
				return 4;
			}
			if ((value & 0xff) < sizeof(strings) / sizeof(strings[0]) - (ncm ? 0 : 1)) {
				return (int)string_descriptor(strings[value & 0xff], out);
			}
			return -1;
		default:
			return -1;                      /* device qualifier, BOS, other speed: none -- a high-speed-only device */
		}
	}
	if ((type & 0x80) && request == 0) {    /* GET_STATUS: of the device, an interface or an endpoint */
		out[0] = 0;
		out[1] = 0;
		return 2;
	}
	if (type == 0x80 && request == 8) {     /* GET_CONFIGURATION */
		out[0] = (uint8_t)ep0_configuration;
		return 1;
	}
	if (type == 0x00 && request == 5) {     /* SET_ADDRESS: the controller takes it now, and uses it after the status stage */
		wr(usb + DCFG, (rd(usb + DCFG) & ~(0x7fu << 3)) | (value & 0x7f) << 3);
		printf("usb: the host gives us address %u\n", value & 0x7f);
		return 0;
	}
	if (type == 0x00 && request == 9) {     /* SET_CONFIGURATION */
		ep0_configuration = value & 0xff;
		printf("usb: CONFIGURATION %u SET: the host has taken the device\n", ep0_configuration);
		if (ncm) {
			ncm_reset();
			if (ep0_configuration == 1) {
				ncm_configure();
			}
		}
		return 0;
	}
	if ((type == 0x01 && request == 11) || ((type & 0x7f) <= 2 && (request == 1 || request == 3))) {
		return 0;                               /* SET_INTERFACE, CLEAR_FEATURE, SET_FEATURE: taken, nothing to do */
	}
	return -1;
}

static void
ep0_stall_and_restart(void)
{
	ep_cmd(0, DEPCMD_SETSTALL, 0, 0, 0);
	ep0_state = EP0_SETUP;
	ep0_setup_start();
}

/* A SETUP packet has come: answered, its data stage started, or stalled. */
static void
ep0_setup(void)
{
	volatile uint8_t *packet = (volatile uint8_t *)(dma + DMA_SETUP_PACKET);
	uint8_t setup[8];
	uint32_t length;
	int answer;

	to_memory(dma + DMA_SETUP_PACKET, 8);
	for (int i = 0; i < 8; i++) {
		setup[i] = packet[i];
	}
	length = setup[6] | setup[7] << 8;
	if ((setup[0] & 0x80) == 0 && length > 0) {
		/*
		 * Data from the host: only a class request of the network adapter's has any
		 * (SET_NTB_INPUT_SIZE). The data stage's TRB is a whole number of packets (DWC3 wants
		 * an OUT transfer so), and what came is looked at when it completes (ep0_out_data()).
		 */
		if (ncm && setup[0] == 0x21 && length <= EP0_DATA_MAX - 64) {
			printf("usb: SETUP %02x %02x %04x %04x %u: its data awaited\n", setup[0], setup[1], setup[2] | setup[3] << 8,
			    setup[4] | setup[5] << 8, length);
			ep0_out_request = setup[1];
			ep0_three_stage = true;
			ep0_state = EP0_DATA;
			to_memory(dma + DMA_EP0_DATA, EP0_DATA_MAX);
			ep0_start(0, DMA_EP0_TRB(0), dma_phys + DMA_EP0_DATA, (length + 63) & ~63u, TRB_CONTROL_DATA);
			return;
		}
		answer = -1;
	} else {
		answer = ep0_answer(setup, (uint8_t *)(dma + DMA_EP0_DATA));
	}
	printf("usb: SETUP %02x %02x %04x %04x %u: %s", setup[0], setup[1], setup[2] | setup[3] << 8, setup[4] | setup[5] << 8,
	    length, answer < 0 ? "STALLED\n" : "");
	if (answer < 0) {
		ep0_stall_and_restart();
		return;
	}
	ep0_three_stage = length > 0;
	if (!ep0_three_stage) {
		printf("taken\n");
		ep0_state = EP0_STATUS_WAIT;
		return;
	}
	if ((uint32_t)answer > length) {
		answer = (int)length;
	}
	printf("%d bytes back\n", answer);
	to_memory(dma + DMA_EP0_DATA, EP0_DATA_MAX);
	ep0_state = EP0_DATA;
	ep0_start(1, DMA_EP0_TRB(1), dma_phys + DMA_EP0_DATA, (uint32_t)answer, TRB_CONTROL_DATA);
}

/* An event of endpoint 0's, either way. */
static void
ep0_event(uint32_t ep, uint32_t type, uint32_t status)
{
	if (type == 1) {                        /* transfer complete */
		if (ep0_state == EP0_SETUP && ep == 0) {
			ep0_setup();
		} else if (ep0_state == EP0_DATA) {
			if (ep == 0) {
				ep0_out_data();
			}
			ep0_state = EP0_STATUS_WAIT;
		} else if (ep0_state == EP0_STATUS) {
			ep0_state = EP0_SETUP;
			ep0_setup_start();
		}
	} else if (type == 3 && (status & 3) == DEPEVT_CONTROL_STATUS && ep0_state == EP0_STATUS_WAIT) {
		ep0_state = EP0_STATUS;
		ep0_start(ep, DMA_EP0_TRB(ep), dma_phys + DMA_SETUP_PACKET, 0,
		    ep0_three_stage ? TRB_CONTROL_STATUS3 : TRB_CONTROL_STATUS2);
	}
}

/* A bus reset: address 0, and endpoint 0 back to waiting for a SETUP (what was under way given up). */
static void
ep0_reset(void)
{
	wr(usb + DCFG, rd(usb + DCFG) & ~(0x7fu << 3));
	ep0_configuration = 0;
	if (ncm) {
		ncm_reset();
	}
	if (ep0_state != EP0_SETUP) {
		for (uint32_t n = 0; n < 2; n++) {
			ep_cmd(n, DEPCMD_ENDTRANSFER | DEPCMD_HIPRI_FORCERM | ep0_resource[n] << 16, 0, 0, 0);
		}
		ep0_state = EP0_SETUP;
		ep0_setup_start();
	}
}

static const char *const device_events[16] = {
	"disconnect", "USB RESET", "CONNECT DONE", "link state change", "wakeup", "hibernation request", "suspend",
	"start of frame", "?8", "erratic error", "command complete", "event buffer overflow", "vendor test", "?13", "?14", "?15",
};
static const char *const endpoint_events[16] = {
	"?0", "transfer complete", "transfer in progress", "transfer not ready", "?4", "?5", "stream", "command complete",
	"?8", "?9", "?10", "?11", "?12", "?13", "?14", "?15",
};
static const char *const speeds[8] = { "high speed", "full speed", "low speed", "full speed (48 MHz)", "SuperSpeed",
	                               "SuperSpeed Plus", "?6", "?7" };

/* What has come since the last time, said; a SETUP packet on endpoint 0 is shown, stalled, and the next one awaited. */
static unsigned
events_take(unsigned said)
{
	uint32_t count = rd(usb + GEVNTCOUNT) & 0xffff, taken = 0;

	while (taken < count) {
		uint32_t e;

		to_memory(dma + event_at, 4);
		e = *(volatile uint32_t *)(dma + event_at);
		event_at = (event_at + 4) % EVENT_BUFFER_SIZE;
		taken += 4;
		if (said++ >= 200) {
			continue;                       /* (counted, not said: the log has had enough) */
		}
		if (e & 1) {
			uint32_t type = e >> 8 & 0xf;

			printf("usb: event %#010x: the device: %s", e, device_events[type]);
			if (type == 2) {
				printf(": at %s (DSTS %#x)", speeds[rd(usb + DSTS) & 7], rd(usb + DSTS));
			} else if (type == 3) {
				printf(": link state %u", e >> 16 & 0xf);
			}
			printf("\n");
			if (type == 1) {
				ep0_reset();
			}
		} else {
			uint32_t ep = e >> 1 & 0x1f, type = e >> 6 & 0xf;

			if (type != 1 && type != 3) {
				printf("usb: event %#010x: endpoint %u (%s %s): %s, status %#x\n", e, ep, ep >> 1 ? "number" : "control",
				    ep & 1 ? "IN" : "OUT", endpoint_events[type], e >> 12 & 0xf);
			}
			if (ep <= 1) {
				ep0_event(ep, type, e >> 12 & 0xf);
			} else if (ncm) {
				ncm_event(ep, type);
			}
		}
	}
	if (count != 0) {
		wr(usb + GEVNTCOUNT, count);
	}
	return said;
}

struct named {
	const char *name;
	uint32_t    offset;
};

static const struct named clocks[] = {
	/* the device tree's order: core, iface, bus_aggr, xo, sleep, utmi; then the PHY's cfg_ahb */
	{ "gcc_usb30_prim_master_clk", 0x1a010 }, { "gcc_cfg_noc_usb3_prim_axi_clk", 0x1a084 },
	{ "gcc_sys_noc_usb3_prim_axi_clk", 0x1a080 }, { "gcc_usb3_prim_clkref_clk", 0x80278 },
	{ "gcc_usb30_prim_sleep_clk", 0x1a014 }, { "gcc_usb30_prim_mock_utmi_clk", 0x1a018 },
	{ "gcc_ahb2phy_usb_clk", 0x1d008 },
};
#define CLOCKS                  (sizeof(clocks) / sizeof(clocks[0]))

/* the device tree's qcom,qusb-phy-init-seq: value, offset */
static const uint8_t qusb_init[][2] = {
	{ 0xf8, 0x80 }, { 0xb3, 0x84 }, { 0x81, 0x88 }, { 0xc0, 0x8c }, { 0x30, 0x08 }, { 0x79, 0x0c },
	{ 0x21, 0x10 }, { 0x14, 0x9c }, { 0x80, 0x04 }, { 0x9f, 0x1c }, { 0x00, 0x18 },
};

static void
gcc_say(const char *when)
{
	printf("usb: %s: power switch (%#x) %#010x: %s; controller reset %#x, PHY reset %#x\n", when, USB30_GDSC,
	    rd(gcc + USB30_GDSC), (rd(gcc + USB30_GDSC) & GDSC_ON) ? "ON" : "off", rd(gcc + USB30_BCR), rd(gcc + QUSB2PHY_BCR));
	printf("usb: %s: sources: master command %#x configuration %#x (M %#x N %#x D %#x), mock UTMI command %#x "
	    "configuration %#x\n", when, rd(gcc + MASTER_RCG), rd(gcc + MASTER_RCG + 4), rd(gcc + MASTER_RCG + 8),
	    rd(gcc + MASTER_RCG + 12), rd(gcc + MASTER_RCG + 16), rd(gcc + MOCK_UTMI_RCG), rd(gcc + MOCK_UTMI_RCG + 4));
	for (unsigned i = 0; i < CLOCKS; i++) {
		uint32_t value = rd(gcc + clocks[i].offset);

		printf("usb: %s: %-30s (%#07x) = %#010x  %s\n", when, clocks[i].name, clocks[i].offset, value,
		    (value & CLOCK_OFF) ? "off" : "RUNS");
	}
}

/* A clock source to input `source` divided by `divider` (as hid: 2 * d - 1), "update", and whether it took. */
static bool
rcg_set(const char *name, uint32_t rcg, uint32_t source, uint32_t divider)
{
	int i;

	wr(gcc + rcg + 4, (rd(gcc + rcg + 4) & ~(0x71fu | 3u << 12)) | source << 8 | divider);
	wr(gcc + rcg, rd(gcc + rcg) | RCG_UPDATE);
	for (i = 0; i < 500 && (rd(gcc + rcg) & RCG_UPDATE); i++) {
		delay_for_interval(10, NSEC_PER_USEC);
	}
	printf("usb: %s: command %#x, configuration %#x (%s)\n", name, rd(gcc + rcg), rd(gcc + rcg + 4),
	    (rd(gcc + rcg) & RCG_UPDATE) ? "the switch DID NOT TAKE" : "set");
	return !(rd(gcc + rcg) & RCG_UPDATE);
}

static void
usb_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter, step = 0;

	printf("usb: step 1: the USB clocks, sources, power switch and resets in the global clock controller (%#lx)\n", GCC_PHYS);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	gcc_say("as found");
	tcsr_clamp = ml_io_map(TCSR_CLAMP_PHYS & ~0xfffUL, 0x1000) + (TCSR_CLAMP_PHYS & 0xfff);
	fuse = ml_io_map(TUNE2_FUSE_PHYS & ~0xfffUL, 0x1000) + (TUNE2_FUSE_PHYS & 0xfff);
	printf("usb: the PHY's 1.8 V clamp (TCSR %#lx) = %#x; the tune2 fuse (%#lx) = %#010x: tune2 %#x\n", TCSR_CLAMP_PHYS,
	    rd(tcsr_clamp), TUNE2_FUSE_PHYS, rd(fuse), (rd(fuse) >> 25) & 0xf);
	if (steps < ++step + 1) {
		goto out;
	}

	printf("usb: step 2: the PHY's 3.3 V from the RPM\n");
	for (int i = 0; i < 500 && !redwood_rpm_done; i++) {
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	/* the device tree: regulator-min-microvolt 3104000 for pm6125_l15 (phy-msm-qusb.c asks for 3.075 V at least) */
	if (!redwood_rpm_ldo("the USB PHY's 3.3 V", 15, 3104000)) {
		printf("usb: FAILED at step 2: the RPM does not give the 3.3 V\n");
		goto out;
	}
	if (steps < ++step + 1) {
		goto out;
	}

	printf("usb: step 3: the power switch, the clock sources, the clocks\n");
	if (!(rd(gcc + USB30_GDSC) & GDSC_ON)) {
		int i;

		wr(gcc + USB30_GDSC, rd(gcc + USB30_GDSC) & ~GDSC_SW_COLLAPSE);
		for (i = 0; i < 500 && !(rd(gcc + USB30_GDSC) & GDSC_ON); i++) {
			delay_for_interval(10, NSEC_PER_USEC);
		}
		printf("usb: power switch: %#010x (%s after %d us)\n", rd(gcc + USB30_GDSC),
		    (rd(gcc + USB30_GDSC) & GDSC_ON) ? "ON" : "DOES NOT COME ON", 10 * i);
		if (!(rd(gcc + USB30_GDSC) & GDSC_ON)) {
			goto out;
		}
	}
	/* gcc_parent_map_0: 0 the crystal, 1 GPLL0's early output (600 MHz), 2 its aux2 */
	if (!rcg_set("the controller's clock, 133.33 MHz", MASTER_RCG, 1, 2 * 9 / 2 - 1) ||
	    !rcg_set("the mock UTMI clock, 19.2 MHz", MOCK_UTMI_RCG, 0, 0)) {
		printf("usb: FAILED at step 3: a clock source\n");
		goto out;
	}
	for (unsigned i = 0; i < CLOCKS; i++) {
		int t;

		wr(gcc + clocks[i].offset, rd(gcc + clocks[i].offset) | CLOCK_ENABLE);
		for (t = 0; t < 200 && (rd(gcc + clocks[i].offset) & CLOCK_OFF); t++) {
			delay_for_interval(10, NSEC_PER_USEC);
		}
		printf("usb: %s on: %#010x (%s)\n", clocks[i].name, rd(gcc + clocks[i].offset),
		    (rd(gcc + clocks[i].offset) & CLOCK_OFF) ? "DOES NOT RUN" : "runs");
		if (rd(gcc + clocks[i].offset) & CLOCK_OFF) {
			printf("usb: FAILED at step 3: a clock\n");
			goto out;
		}
	}
	if (steps < ++step + 1) {
		goto out;
	}

	printf("usb: step 4: the controller's and the PHY's resets, held and let go\n");
	wr(gcc + USB30_BCR, rd(gcc + USB30_BCR) | 1);
	wr(gcc + QUSB2PHY_BCR, rd(gcc + QUSB2PHY_BCR) | 1);
	delay_for_interval(100, NSEC_PER_USEC);
	wr(gcc + QUSB2PHY_BCR, rd(gcc + QUSB2PHY_BCR) & ~1u);
	wr(gcc + USB30_BCR, rd(gcc + USB30_BCR) & ~1u);
	delay_for_interval(100, NSEC_PER_USEC);
	gcc_say("after the resets");
	if (steps < ++step + 1) {
		goto out;
	}

	printf("usb: step 5: the controller's registers (%#lx) and the PHY's (%#lx)\n", USB_PHYS, QUSB_PHYS);
	usb = ml_io_map(USB_PHYS, USB_SIZE);
	printf("usb: DWC3: GSNPSID %#010x (%s), GUID %#x; GCTL %#x, GSTS %#x, GUSB2PHYCFG %#x; DCFG %#x, DCTL %#x, DSTS %#x\n",
	    rd(usb + GSNPSID), (rd(usb + GSNPSID) >> 16) == 0x5533 ? "Synopsys USB 3.0" : (rd(usb + GSNPSID) >> 16) == 0x3331 ?
	    "Synopsys USB 3.1" : "NOT a DWC3's", rd(usb + GUID), rd(usb + GCTL), rd(usb + GSTS), rd(usb + GUSB2PHYCFG),
	    rd(usb + DCFG), rd(usb + DCTL), rd(usb + DSTS));
	printf("usb: DWC3: hardware parameters %#x %#x %#x %#x %#x %#x %#x %#x\n", rd(usb + GHWPARAMS(0)),
	    rd(usb + GHWPARAMS(1)), rd(usb + GHWPARAMS(2)), rd(usb + GHWPARAMS(3)), rd(usb + GHWPARAMS(4)),
	    rd(usb + GHWPARAMS(5)), rd(usb + GHWPARAMS(6)), rd(usb + GHWPARAMS(7)));
	printf("usb: wrapper (QSCRATCH %#x): %#x %#x %#x %#x %#x %#x %#x %#x | %#x %#x %#x %#x\n", QSCRATCH,
	    rd(usb + QSCRATCH), rd(usb + QSCRATCH + 4), rd(usb + QSCRATCH + 8), rd(usb + QSCRATCH + 12),
	    rd(usb + QSCRATCH + 16), rd(usb + QSCRATCH + 20), rd(usb + QSCRATCH + 24), rd(usb + QSCRATCH + 28),
	    rd(usb + QSCRATCH + 0x50), rd(usb + QSCRATCH + 0x54), rd(usb + QSCRATCH + 0x58), rd(usb + QSCRATCH + 0x5c));
	qusb = ml_io_map(QUSB_PHYS, QUSB_SIZE);
	printf("usb: PHY: PLL status %#x, power-down %#x, tune 1-5 %#x %#x %#x %#x %#x, words at 0x04-0x1c %#x %#x %#x %#x "
	    "%#x %#x %#x\n", rd(qusb + QUSB_PLL_STATUS), rd(qusb + QUSB_POWERDOWN), rd(qusb + 0x80), rd(qusb + 0x84),
	    rd(qusb + 0x88), rd(qusb + 0x8c), rd(qusb + 0x90), rd(qusb + 0x04), rd(qusb + 0x08), rd(qusb + 0x0c),
	    rd(qusb + 0x10), rd(qusb + 0x14), rd(qusb + 0x18), rd(qusb + 0x1c));
	if ((rd(usb + GSNPSID) >> 16) != 0x5533 && (rd(usb + GSNPSID) >> 16) != 0x3331) {
		printf("usb: FAILED at step 5: the controller does not say it is a DWC3\n");
		goto out;
	}
	if (steps < ++step + 1) {
		goto out;
	}

	printf("usb: step 6: the PHY, as Linux's qusb_phy_init\n");
	{
		uint32_t tune2 = (rd(fuse) >> 25) & 0xf;

		wr(tcsr_clamp, 1);                      /* the 1.8 V side's clamp let go */
		wr(qusb + QUSB_POWERDOWN, QUSB_CLAMP_N_EN | QUSB_FREEZIO_N | QUSB_POWER_DOWN);
		for (unsigned i = 0; i < sizeof(qusb_init) / sizeof(qusb_init[0]); i++) {
			wr(qusb + qusb_init[i][1], qusb_init[i][0]);
		}
		if (tune2 != 0) {
			/* the fuse's value as the high nibble of TUNE2, the sequence's low one kept */
			wr(qusb + QUSB_TUNE2, tune2 << 4 | (0xb3 & 0xf));
		}
		wr(qusb + QUSB_POWERDOWN, QUSB_CLAMP_N_EN | QUSB_FREEZIO_N);
		delay_for_interval(160, NSEC_PER_USEC);
		printf("usb: PHY: power-down %#x, tune2 %#x, PLL status %#x: %s\n", rd(qusb + QUSB_POWERDOWN), rd(qusb + QUSB_TUNE2),
		    rd(qusb + QUSB_PLL_STATUS), (rd(qusb + QUSB_PLL_STATUS) & QUSB_PLL_LOCK) ? "THE PHY'S PLL IS LOCKED" :
		    "its PLL does NOT lock");
	}
	if (!(rd(qusb + QUSB_PLL_STATUS) & QUSB_PLL_LOCK) || steps < ++step + 1) {
		goto out;
	}

	printf("usb: step 7: the apps SMMU (%#lx) for the controller's stream %#x\n", APPS_SMMU_PHYS, USB_STREAM);
	apps_smmu = ml_io_map(APPS_SMMU_PHYS, APPS_SMMU_SIZE);
	{
		uint32_t idr0 = rd(apps_smmu + 0x20), idr1 = rd(apps_smmu + 0x24), matches = 0;
		uint32_t smrs = idr0 & 0xff, contexts = idr1 & 0xff, page = idr1 >> 31 ? 0x10000 : 0x1000;
		uint32_t pages = 1u << ((idr1 >> 28 & 7) + 1);

		printf("usb: SMMU: sCR0 %#x (client faults on unmatched streams: %s), IDR0 %#x, IDR1 %#x: %u stream matches, "
		    "%u contexts\n", rd(apps_smmu), (rd(apps_smmu) >> 10 & 1) ? "yes" : "no, they pass", idr0, idr1, smrs, contexts);
		for (uint32_t i = 0; i < smrs && i < 128; i++) {
			uint32_t smr = rd(apps_smmu + 0x800 + 4 * i), mask = smr >> 16 & 0x7fff, id = smr & 0x7fff;

			if ((smr >> 31) && ((USB_STREAM ^ id) & ~mask) == 0) {
				uint32_t s2cr = rd(apps_smmu + 0xc00 + 4 * i), cb = s2cr & 0xff;
				vm_offset_t cbase = apps_smmu + pages * page + cb * page;

				matches++;
				printf("usb: SMMU: match %u: SMR %#x (stream %#x mask %#x), S2CR %#x: %s", i, smr, id, mask, s2cr,
				    (s2cr >> 16 & 3) == 1 ? "BYPASS" : (s2cr >> 16 & 3) == 2 ? "FAULT" : "translated");
				if ((s2cr >> 16 & 3) == 0 && cb < contexts) {
					printf(" by context %u: CBAR %#x, SCTLR %#x, TTBR0 %#x:%#x, TCR %#x", cb, rd(apps_smmu + 0x1000 + 4 * cb),
					    rd(cbase), rd(cbase + 0x24), rd(cbase + 0x20), rd(cbase + 0x30));
				}
				printf("\n");
			}
		}
		if (matches == 0) {
			printf("usb: SMMU: no stream match takes %#x: it goes %s\n", USB_STREAM,
			    (rd(apps_smmu) >> 10 & 1) ? "NOWHERE (faulted)" : "through unmatched (bypass)");
		}
	}
	if (steps < ++step + 1) {
		goto out;
	}

	printf("usb: step 8: the controller into device mode, and onto the cable\n");
	if (kmem_alloc(kernel_map, &dma, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		goto out;
	}
	dma_phys = kvtophys(dma);
	to_memory(dma, PAGE_SIZE);
	ncm = steps >= 9;
	if (ncm) {
		/* physically one piece: a block's buffer is one TRB, 16 KB, and the controller reads it by its physical address */
		if (ncm_queue == NULL || kmem_alloc_contig(kernel_map, &ncm_dma, NCM_DMA_SIZE, PAGE_MASK, 0, 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("usb: FAILED at step 9: no memory for the network adapter\n");
			goto out;
		}
		ncm_dma_phys = kvtophys(ncm_dma);
		to_memory(ncm_dma, NCM_DMA_SIZE);
		printf("usb: step 9: the device is a network adapter (CDC-NCM); its memory at %#llx; en0 %s\n", ncm_dma_phys,
		    ncm_attached ? "on it" : "HAS ANOTHER LINK: the frames go nowhere");
	}
	{
		uint32_t status, watch = 60, said = 0;
		int i;

		/* dwc3-msm.c, dwc3_msm_switch_utmi: no USB 3 PHY to give a pipe clock, so UTMI's stands in */
		wr(usb + QSCRATCH_GENERAL_CFG, rd(usb + QSCRATCH_GENERAL_CFG) | PIPE_UTMI_CLK_DIS);
		delay_for_interval(10, NSEC_PER_USEC);
		wr(usb + QSCRATCH_GENERAL_CFG, rd(usb + QSCRATCH_GENERAL_CFG) | PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW);
		wr(usb + QSCRATCH_GENERAL_CFG, rd(usb + QSCRATCH_GENERAL_CFG) & ~PIPE_UTMI_CLK_DIS);
		/* dt: snps,dis_u2_susphy_quirk, snps,dis_enblslpm_quirk, snps,disable-clk-gating */
		wr(usb + GUSB2PHYCFG, rd(usb + GUSB2PHYCFG) & ~(GUSB2PHYCFG_SUSPHY | GUSB2PHYCFG_ENBLSLPM));
		wr(usb + GCTL, (rd(usb + GCTL) & ~GCTL_PRTCAPDIR_MASK) | GCTL_PRTCAP_DEVICE | GCTL_DSBLCLKGTNG);
		printf("usb: wrapper's GENERAL_CFG %#x; GUSB2PHYCFG %#x; GCTL %#x; next: the controller's soft reset (DCTL.CSFTRST)\n",
		    rd(usb + QSCRATCH_GENERAL_CFG), rd(usb + GUSB2PHYCFG), rd(usb + GCTL));
		wr(usb + DCTL, rd(usb + DCTL) | DCTL_CSFTRST);
		for (i = 0; i < 500 && (rd(usb + DCTL) & DCTL_CSFTRST); i++) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
		printf("usb: soft reset: %s after %d00 us (DCTL %#x)\n", (rd(usb + DCTL) & DCTL_CSFTRST) ? "DOES NOT END" : "done", i,
		    rd(usb + DCTL));
		if (rd(usb + DCTL) & DCTL_CSFTRST) {
			goto out;
		}
		wr(usb + DCFG, rd(usb + DCFG) & ~(7u | 0x7fu << 3));    /* high speed, address 0 */
		wr(usb + GEVNTADRLO, (uint32_t)dma_phys);
		wr(usb + GEVNTADRHI, (uint32_t)(dma_phys >> 32));
		wr(usb + GEVNTSIZ, EVENT_BUFFER_SIZE);                  /* (bit 31 clear: the event interrupt not masked) */
		wr(usb + GEVNTCOUNT, rd(usb + GEVNTCOUNT) & 0xffff);
		event_at = 0;
		status = ep_cmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0);
		printf("usb: start of configuration: status %#x\n", status);
		/* endpoint 0 both ways: control, 64-byte packets, IN on FIFO 0; transfer complete and not ready events */
		for (uint32_t n = 0; n < 2 && status == 0; n++) {
			status = ep_cmd(n, DEPCMD_SETEPCONFIG, 64u << 3, 1u << 8 | 1u << 10 | n << 25, 0);
			if (status == 0) {
				status = ep_cmd(n, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0);
			}
			printf("usb: endpoint 0 %s configured: status %#x\n", n ? "IN" : "OUT", status);
		}
		ep0_state = EP0_SETUP;
		if (status != 0 || (status = ep0_setup_start()) != 0) {
			printf("usb: FAILED at step 8: endpoint 0 (%#x)\n", status);
			goto out;
		}
		printf("usb: a transfer waits for the first SETUP packet (its resource %u)\n", rd(usb + DEPCMD(0)) >> 16 & 0x7f);
		wr(usb + DALEPENA, 3);
		wr(usb + DEVTEN, 0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x200u | 0x800u);
		/*
		 * dwc3-msm.c, dwc3_override_vbus_status: the controller connects (its pull-up on D+) only with a
		 * valid session, and here nothing tells it of the cable's VBUS but this: the wrapper's override.
		 * The first run without it had Run/Stop on and not one event in 60 s.
		 */
		wr(usb + HS_PHY_CTRL, rd(usb + HS_PHY_CTRL) | UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL);
		printf("usb: VBUS valid told to the controller (HS_PHY_CTRL %#x)\n", rd(usb + HS_PHY_CTRL));
		printf("usb: next: Run/Stop (DCTL %#x, DSTS %#x): the pull-up on D+, the host should see a device now\n",
		    rd(usb + DCTL), rd(usb + DSTS));
		wr(usb + DCTL, rd(usb + DCTL) | DCTL_RUN_STOP);
		for (i = 0; i < 500 && (rd(usb + DSTS) & DSTS_DEVCTRLHLT); i++) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
		printf("usb: Run/Stop: DCTL %#x, DSTS %#x (%s)\n", rd(usb + DCTL), rd(usb + DSTS),
		    (rd(usb + DSTS) & DSTS_DEVCTRLHLT) ? "the controller is still halted" : "RUNNING, ON THE CABLE");
		PE_parse_boot_argn("usb_watch", &watch, sizeof(watch));
		/* (step 9: for good, every millisecond, and a line about the traffic once a minute while there is any) */
		for (uint32_t ms = 0, last_dsts = ~0u, changes = 0, tick = ncm ? 1 : 5; ncm || ms < 1000 * watch; ms += tick) {
			/* DSTS: link state (21:18), the speed connected at (2:0); the frame number (16:3) left out */
			uint32_t dsts = rd(usb + DSTS);
			static uint64_t blocks_said;

			said = events_take(said);
			if ((dsts & ~(0x3fffu << 3)) != last_dsts && changes++ < 40) {
				last_dsts = dsts & ~(0x3fffu << 3);
				printf("usb: at %u ms: DSTS %#x (link state %u, speed %u), events waiting %u bytes\n", ms,
				    dsts, dsts >> 18 & 0xf, dsts & 7, rd(usb + GEVNTCOUNT) & 0xffff);
			}
			if (ncm) {
				while (ncm_send()) {
					said = events_take(said);
				}
				if (ms % 60000 == 0 && ncm_count.blocks_in + ncm_count.blocks_out != blocks_said) {
					blocks_said = ncm_count.blocks_in + ncm_count.blocks_out;
					printf("usb: network: %llu blocks in (%llu frames, %llu bad), %llu out (%llu frames, %llu dropped)\n",
					    ncm_count.blocks_in, ncm_count.frames_in, ncm_count.bad_in, ncm_count.blocks_out,
					    ncm_count.frames_out, ncm_count.dropped_out);
				}
			}
			delay_for_interval(tick, NSEC_PER_MSEC);
		}
		printf("usb: %u events in %u s; Run/Stop off again (DSTS %#x)\n", said, watch, rd(usb + DSTS));
		wr(usb + DCTL, rd(usb + DCTL) & ~DCTL_RUN_STOP);
	}
	step++;
out:
	printf("usb: stopping after step %u (usb=%u)\n", step < steps ? step : steps, steps);
	thread_terminate(current_thread());
}

void
redwood_usb_init(void)
{
	uint32_t steps = 0;
	char device[16] = "";
	thread_t thread;

	if (!PE_parse_boot_argn("usb", &steps, sizeof(steps)) || steps == 0 ||
	    !PE_parse_boot_arg_str("device", device, sizeof(device)) || strcmp(device, "willow") != 0) {
		return;
	}
	/*
	 * Step 9's en0 now, as the kernel starts, and not when the controller is on the cable some
	 * seconds later: iOS's configd is to find it among the interfaces there are when it starts
	 * (as with QEMU's virtio_net, attached here too), the cable out until the host picks setting 1.
	 */
	if (steps >= 9) {
		ncm_queue = kalloc_data(NCM_TX_QUEUE * sizeof(*ncm_queue), Z_WAITOK | Z_ZERO);
		ncm_attached = ncm_queue != NULL && redwood_net_attach(&ncm_link);
	}
	if (kernel_thread_start(usb_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
