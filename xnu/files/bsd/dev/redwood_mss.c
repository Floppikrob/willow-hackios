/*
 * The modem of the Redmi Note 8T (willow, SM6125), for the sake of its Wi-Fi. The radio is a
 * WCN3990, and its firmware does not run on the application processor, nor on a processor of the
 * radio's own as redwood's WCN6750 does (bsd/dev/redwood_wifi.c): it runs inside the modem's
 * Hexagon, the "MPSS", as one of the modem's protection domains. So the Wi-Fi begins with the modem:
 * its signed image -- the phone's own, modem.mdt and modem.b00 to .b29 in the FAT partition "modem"
 * (mmcblk0p80, /vendor/firmware_mnt on Android) -- goes into the memory reserved for it
 * (0x4b000000, 126 MB; dt: modem_region) and TrustZone authenticates and starts it (PAS id 4; dt:
 * qcom,mss@6080000). What the modem then wants from us -- its file system in modemst1/2 and fsg,
 * the WLAN firmware wlanmdsp.mbn, the service that starts its protection domains -- and the Wi-Fi's
 * own conversation (QMI, "wlfw") come over GLINK and QRTR, as on redwood.
 *
 * Boot-arg mss_step=N performs steps 1 to N and stops, as wifi_step does on redwood: a step says
 * what it is about to touch, and a step that does not find what it needs ends the sequence.
 *
 *   1  reads   the shared memory area: how the boot loader laid it out, the partition of the
 *              modem and us, and whether the modem has left anything there (it has if it has run
 *              since the phone was started)
 *   2  reads   the partition "modem" through the storage driver (FAT16): the directory IMAGE, and
 *              in it the modem's image, the WLAN firmware and the board data
 *   3  reads   the modem image's header (an ELF of 32 bits, Qualcomm's "split" form: the header in
 *              modem.mdt, each segment in a file of its own): its segments, where they go, whether
 *              they fit the reserved memory, whether every segment's file is there and of the size
 *              the header says
 * Steps 1 to 3 are the survey; nothing is written.
 *   4  asks    TrustZone to take the image's header (modem.mdt: the ELF header, the table and the
 *              hash segment, 9100 bytes, in one page of ours) and check its signature (PAS id 4),
 *              and tells it where the image will be (0x4b000000, 126 MB)
 *   5  writes  the image's segments into that memory, from the FAT partition, zeroes behind each
 *              file up to the segment's length; and reads a sample of it back
 *   6  writes  our items into the partition of the modem and us, under the SoC's lock for that
 *              (TCSR mutex 3, 0x343000): SMP2P to the modem (435), with the entry "master-kernel"
 *              at 0; GLINK's descriptor (478, both our marks at 0) and our GLINK queue (479, 16 KB)
 *              -- the boot chain makes none of them on this phone, Linux makes them before the start
 *              -- and rings the modem's SMP2P doorbell
 *   7  asks    TrustZone to authenticate the image and let the modem out of reset, and watches the
 *              shared memory (mss_wait=S seconds, 10) for the modem's item (428) and its entry
 *              "slave-kernel": bit 1 "ready", bit 2 "it has asked for its own power", bit 0 "fatal"
 *   11 tells   the modem of services of ours (the service locator, RMTFS, TFTP: step_serve()), answers
 *              the locator and writes down what the other two are asked (mss_serve=S seconds, 60)
 * The modem is not stopped afterwards: a run ends with the phone restarted, which stops it.
 *
 * Addresses (dt of the phone's Android, ~/willow/live.dts): SMEM 0x46000000, 2 MB; the modem's
 * SMP2P items 435 (ours to it) and 428 (its to us); GLINK's 478-480; the doorbells: the APCS IPC
 * register of qcom,trinket-apcs-hmss-global at 0x0f111000, bit 12 GLINK, bit 14 SMP2P; the modem's
 * interrupts to us: SPI 68 GLINK, SPI 70 SMP2P.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/redwood_input.h>
#include <sys/redwood_qrtr.h>
#include <kern/locks.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void IOSleep(unsigned int milliseconds);
extern kern_return_t thread_terminate(thread_t thread);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern void delay(int usec);
extern volatile int redwood_rpm_done;
extern bool redwood_rpm_ldo(const char *what, uint32_t id, uint32_t microvolts);    /* redwood_rpm.c */

static uint32_t mss_step, step;
static bool modem_started;             /* TrustZone has let it out of reset (step 7) */

static uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__builtin_arm_dsb(0xf);
}

static void
settle(void)
{
	__builtin_arm_dsb(0xf);
	__builtin_arm_isb(0xf);
}

static void
announce(const char *what)
{
	printf("mss: %u: next: %s\n", step, what);
}

/* ---- step 1: the shared memory area ---------------------------------------------------------- */

#define SMEM_PHYS               0x46000000UL
#define SMEM_SIZE               0x00200000u
#define SMEM_US                 0
#define SMEM_MODEM              1
#define SMEM_GLOBAL             0xfffe
#define SMEM_VERSION            (0x40 + 4 * 7)
#define SMEM_VERSION_PARTITIONS 12
#define SMEM_CANARY             0xa5a5

#define ITEM_SMP2P_OURS         435             /* dt: qcom,smp2p-modem, qcom,smem = <0x1b3 0x1ac> */
#define ITEM_SMP2P_THEIRS       428
#define ITEM_CRASH_REASON       421             /* SMEM_SSR_REASON_MSS0: the modem's last words */
#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_OUR_QUEUE    479
#define ITEM_GLINK_THEIR_QUEUE  480

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};
struct smem_item {
	uint32_t item, offset, size;
};

static const volatile uint8_t *smem;

static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
		smem = (const volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	if (smem == NULL || offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem[offset + i];
	}
	return true;
}

static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;

	if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
	    table.count > 64) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			return 0;
		}
		*cacheline = entry.cacheline;
		*size = entry.size;
		return entry.offset;
	}
	return 0;
}

static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned count = 0;

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		return 0;
	}
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return count;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at - entry.size;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return count;
}

/* An item the modem and we share: in the partition of the two, or else in the global one. 0: nowhere. */
static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
	static struct smem_item found[256];
	static const uint16_t hosts[2][2] = { { SMEM_US, SMEM_MODEM }, { SMEM_GLOBAL, SMEM_GLOBAL } };

	for (unsigned h = 0; h < 2; h++) {
		uint32_t cacheline = 0, partition_size = 0;
		uint32_t partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline, &partition_size);
		unsigned count = smem_items(partition, cacheline, found, 256);

		for (unsigned i = 0; i < count && i < 256; i++) {
			if (found[i].item == item) {
				*size = found[i].size;
				return found[i].offset;
			}
		}
	}
	return 0;
}

struct smp2p {
	uint32_t magic, version;
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324

static void
show_partition(const char *name, uint16_t one, uint16_t other)
{
	static struct smem_item found[256];
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0, partition = smem_partition(one, other, &cacheline, &size);
	unsigned count;
	char line[320];

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("mss: %u: there is no partition for %s\n", step, name);
		return;
	}
	count = smem_items(partition, cacheline, found, 256);
	printf("mss: %u: the partition for %s: at %#lx, %#x bytes, of which %#x free; %u items\n", step, name, SMEM_PHYS + partition,
	    size, header.free_cached - header.free_uncached, count);
	for (unsigned i = 0; i < count && i < 256;) {
		int at = 0;

		line[0] = 0;
		for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		printf("mss: %u:   items (bytes):%s\n", step, line);
	}
}

static void
show_item(uint32_t item, const char *what)
{
	uint32_t size = 0, at = smem_item(item, &size);

	printf("mss: %u: item %u, %s: %s", step, item, what, at ? "THERE" : "not there\n");
	if (at) {
		printf(" (%u bytes)\n", size);
	}
}

static bool
step_smem(void)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smp2p words;
	uint32_t version = 0, size = 0, at;
	char text[81];

	announce("map the shared memory area (0x46000000, 2 MB: memory, not registers) and read how the boot loader laid it out");
	if (!smem_peek(SMEM_VERSION, &version, sizeof(version)) || !smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table))) {
		printf("mss: %u: the area cannot be mapped\n", step);
		return false;
	}
	printf("mss: %u: the boot loader's version word is %#x; the table of partitions begins %02x %02x %02x %02x, version %u, "
	    "%u entries\n", step, version, table.magic[0], table.magic[1], table.magic[2], table.magic[3], table.version, table.count);
	if (version >> 16 != SMEM_VERSION_PARTITIONS || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 || table.count > 64) {
		printf("mss: %u: that is not the layout this knows (version 12, everything in partitions)\n", step);
		return false;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry))) {
			printf("mss: %u: partition %u: processors %#x and %#x, at %#lx, %#x bytes, flags %#x, alignment %u\n", step, i,
			    entry.host0, entry.host1, SMEM_PHYS + entry.offset, entry.size, entry.flags, entry.cacheline);
		}
	}
	show_partition("us and the modem (0 and 1)", SMEM_US, SMEM_MODEM);
	show_partition("everybody (the global one)", SMEM_GLOBAL, SMEM_GLOBAL);
	show_item(ITEM_SMP2P_OURS, "SMP2P from us to the modem");
	show_item(ITEM_GLINK_DESCRIPTOR, "GLINK's descriptor with the modem");
	show_item(ITEM_GLINK_OUR_QUEUE, "GLINK's queue from us");
	show_item(ITEM_GLINK_THEIR_QUEUE, "GLINK's queue from the modem");
	at = smem_item(ITEM_CRASH_REASON, &size);
	if (at != 0 && size != 0) {
		bzero(text, sizeof(text));
		smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
		printf("mss: %u: item %u, the modem's last words (%u bytes): %s%s%s\n", step, ITEM_CRASH_REASON, size, text[0] ? "\"" : "",
		    text[0] ? text : "none", text[0] ? "\"" : "");
	} else {
		printf("mss: %u: item %u, the modem's last words: not there\n", step, ITEM_CRASH_REASON);
	}
	at = smem_item(ITEM_SMP2P_THEIRS, &size);
	if (at != 0 && size >= sizeof(words) && smem_peek(at, &words, sizeof(words)) && words.magic == SMP2P_MAGIC) {
		char line[320];
		int n = 0;

		line[0] = 0;
		for (unsigned i = 0; i < words.valid && i < 16 && n < (int)sizeof(line) - 40; i++) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %.16s = %#x;", words.entry[i].name, words.entry[i].value);
		}
		printf("mss: %u: item %u, SMP2P from the modem to us, IS THERE (the modem has run since the phone was started): "
		    "version %u, from %u to %u, flags %#x, %u of %u entries:%s\n", step, ITEM_SMP2P_THEIRS, words.version & 0xff,
		    words.local, words.remote, words.flags, words.valid, words.total, line);
	} else {
		printf("mss: %u: item %u, SMP2P from the modem to us: not there (the modem has not run since the phone was started)\n",
		    step, ITEM_SMP2P_THEIRS);
	}
	return true;
}

/* ---- step 2: the partition "modem", FAT16 ----------------------------------------------------- */

static struct {
	uint32_t sector, cluster;               /* bytes */
	uint64_t fat, root, data;               /* where each begins, in bytes from the partition's start */
	uint32_t root_entries, clusters;
	uint16_t *table;                        /* the first FAT, whole */
	uint32_t table_entries;
	uint32_t image_cluster;                 /* the directory IMAGE */
} fat;

struct fat_dirent {
	char name[11];
	uint8_t attributes, reserved, ctime_tenths;
	uint16_t ctime, cdate, adate, cluster_high, mtime, mdate, cluster;
	uint32_t size;
} __attribute__((packed));

static int
part_read(uint64_t offset, void *to, uint64_t length)
{
	return redwood_ufs_read_named("modem", offset, to, length, NULL);
}

static bool
fat_open(void)
{
	uint8_t boot[512];
	uint32_t reserved, fats, fat_sectors, total, root_sectors;
	uint64_t part_size = 0;
	int error;

	if ((error = redwood_ufs_read_named("modem", 0, NULL, 0, &part_size)) != 0) {
		printf("mss: %u: the partition \"modem\" is not there (%d): the storage has to be up (ufs=2)\n", step, error);
		return false;
	}
	if ((error = part_read(0, boot, sizeof(boot))) != 0) {
		printf("mss: %u: its first sector cannot be read (%d)\n", step, error);
		return false;
	}
	fat.sector = boot[11] | boot[12] << 8;
	fat.cluster = fat.sector * boot[13];
	reserved = boot[14] | boot[15] << 8;
	fats = boot[16];
	fat.root_entries = boot[17] | boot[18] << 8;
	total = boot[19] | boot[20] << 8;
	if (total == 0) {
		total = boot[32] | boot[33] << 8 | boot[34] << 16 | (uint32_t)boot[35] << 24;
	}
	fat_sectors = boot[22] | boot[23] << 8;
	if (boot[510] != 0x55 || boot[511] != 0xaa || fat.sector != 512 || fat.cluster == 0 || fats == 0 || fat_sectors == 0 ||
	    memcmp(boot + 54, "FAT16", 5) != 0) {
		printf("mss: %u: not the FAT16 this reads: %u-byte sectors, %u-byte clusters, %u FATs of %u sectors, \"%.8s\"\n", step,
		    fat.sector, fat.cluster, fats, fat_sectors, (const char *)boot + 54);
		return false;
	}
	root_sectors = (fat.root_entries * 32 + fat.sector - 1) / fat.sector;
	fat.fat = (uint64_t)reserved * fat.sector;
	fat.root = fat.fat + (uint64_t)fats * fat_sectors * fat.sector;
	fat.data = fat.root + (uint64_t)root_sectors * fat.sector;
	fat.clusters = (uint32_t)(((uint64_t)total * fat.sector - fat.data) / fat.cluster);
	fat.table_entries = fat_sectors * fat.sector / 2;
	fat.table = kalloc_data(fat_sectors * fat.sector, Z_WAITOK);
	if (fat.table == NULL || part_read(fat.fat, fat.table, (uint64_t)fat_sectors * fat.sector) != 0) {
		printf("mss: %u: the FAT cannot be read\n", step);
		return false;
	}
	printf("mss: %u: FAT16 of %llu MB: %u-byte clusters, %u of them; the FAT at %#llx, the root (%u entries) at %#llx, data at "
	    "%#llx\n", step, part_size >> 20, fat.cluster, fat.clusters, fat.fat, fat.root_entries, fat.root, fat.data);
	return true;
}

/* The entry of a directory (cluster 0: the root) with that 8.3 name ("MODEM   MDT"); false if there is none. */
static bool
fat_find(uint32_t cluster, const char name[11], struct fat_dirent *found)
{
	uint8_t *buffer;
	uint32_t length = cluster == 0 ? fat.root_entries * 32 : fat.cluster;
	bool done = false, there = false;

	buffer = kalloc_data(length, Z_WAITOK);
	if (buffer == NULL) {
		return false;
	}
	for (uint32_t hops = 0; !done && hops < 4096; hops++) {
		uint64_t at = cluster == 0 ? fat.root : fat.data + (uint64_t)(cluster - 2) * fat.cluster;

		if (part_read(at, buffer, length) != 0) {
			break;
		}
		for (uint32_t i = 0; i < length / 32; i++) {
			const struct fat_dirent *e = (const struct fat_dirent *)(buffer + i * 32);

			if (e->name[0] == 0) {
				done = true;
				break;
			}
			if ((uint8_t)e->name[0] == 0xe5 || e->attributes == 0x0f || memcmp(e->name, name, 11) != 0) {
				continue;
			}
			*found = *e;
			there = done = true;
			break;
		}
		if (cluster == 0 || done) {
			break;
		}
		cluster = cluster < fat.table_entries ? fat.table[cluster] : 0xffff;
		if (cluster < 2 || cluster >= 0xfff8) {
			break;
		}
	}
	kfree_data(buffer, length);
	return there;
}

/* "modem.b07" as FAT's 8.3 name, "MODEM   B07" */
static void
short_name(const char *file, char name[11])
{
	unsigned i = 0, j = 0;

	memset(name, ' ', 11);
	for (; file[i] && file[i] != '.' && j < 8; i++, j++) {
		name[j] = (char)((file[i] >= 'a' && file[i] <= 'z') ? file[i] - 32 : file[i]);
	}
	while (file[i] && file[i] != '.') {
		i++;
	}
	if (file[i] == '.') {
		i++;
		for (j = 8; file[i] && j < 11; i++, j++) {
			name[j] = (char)((file[i] >= 'a' && file[i] <= 'z') ? file[i] - 32 : file[i]);
		}
	}
}

/* A file of IMAGE: its size and first cluster; false if it is not there. */
static bool
image_file(const char *file, uint32_t *size, uint32_t *first)
{
	struct fat_dirent e;
	char name[11];

	short_name(file, name);
	if (fat.image_cluster == 0 || !fat_find(fat.image_cluster, name, &e) || (e.attributes & 0x10)) {
		return false;
	}
	*size = e.size;
	*first = e.cluster | (uint32_t)e.cluster_high << 16;
	return true;
}

/* Bytes [offset, offset + length) of a file whose chain begins at first: cluster by cluster. */
static int
file_read(uint32_t first, uint32_t offset, void *to, uint32_t length)
{
	uint32_t cluster = first, skip = offset / fat.cluster, within = offset % fat.cluster;
	uint8_t *dst = to;

	for (uint32_t i = 0; i < skip; i++) {
		cluster = cluster < fat.table_entries ? fat.table[cluster] : 0xffff;
		if (cluster < 2 || cluster >= 0xfff8) {
			return EIO;
		}
	}
	while (length > 0) {
		uint32_t take = fat.cluster - within;
		int error;

		if (cluster < 2 || cluster >= 0xfff8) {
			return EIO;
		}
		take = take < length ? take : length;
		error = part_read(fat.data + (uint64_t)(cluster - 2) * fat.cluster + within, dst, take);
		if (error != 0) {
			return error;
		}
		dst += take;
		length -= take;
		within = 0;
		cluster = cluster < fat.table_entries ? fat.table[cluster] : 0xffff;
	}
	return 0;
}

static bool
step_partition(void)
{
	struct fat_dirent e;
	static const char *const files[] = { "modem.mdt", "modem.b00", "wlanmdsp.mbn", "bdwlan.bin", "modemr.jsn", "modemuw.jsn" };

	announce("read the partition \"modem\" through the storage driver: its FAT, and the directory IMAGE");
	if (!fat_open()) {
		return false;
	}
	if (!fat_find(0, "IMAGE      ", &e) || !(e.attributes & 0x10)) {
		printf("mss: %u: there is no directory IMAGE in its root\n", step);
		return false;
	}
	fat.image_cluster = e.cluster | (uint32_t)e.cluster_high << 16;
	printf("mss: %u: IMAGE is a directory, from cluster %u\n", step, fat.image_cluster);
	for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		uint32_t size = 0, first = 0;

		if (image_file(files[i], &size, &first)) {
			printf("mss: %u:   %s: %u bytes, from cluster %u\n", step, files[i], size, first);
		} else {
			printf("mss: %u:   %s: NOT THERE\n", step, files[i]);
			if (i == 0 || i == 2) {
				return false;
			}
		}
	}
	return true;
}

/* ---- step 3: the modem image's header ----------------------------------------------------------- */

#define MODEM_MEMORY_PHYS       0x4b000000UL    /* dt: modem_region */
#define MODEM_MEMORY_SIZE       0x07e00000u

struct elf32_header {
	uint8_t ident[16];
	uint16_t type, machine;
	uint32_t version, entry, phoff, shoff, flags;
	uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct elf32_phdr {
	uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};
#define QCOM_MDT_TYPE(flags)    (((flags) >> 24) & 7)   /* 2: the hash segment */
#define QCOM_MDT_RELOCATABLE    (1u << 27)

static bool
step_header(void)
{
	static uint8_t mdt[16384];
	const struct elf32_header *h = (const struct elf32_header *)mdt;
	uint32_t size = 0, first = 0, low = ~0u, high = 0, missing = 0, wrong = 0, relocatable = 0, total = 0;

	announce("read modem.mdt, the modem image's header, and every segment file's directory entry");
	if (!image_file("modem.mdt", &size, &first) || size > sizeof(mdt) || file_read(first, 0, mdt, size) != 0) {
		printf("mss: %u: modem.mdt cannot be read (%u bytes)\n", step, size);
		return false;
	}
	if (memcmp(h->ident, "\177ELF", 4) != 0 || h->ident[4] != 1 || h->phentsize != sizeof(struct elf32_phdr) ||
	    h->phoff + (uint32_t)h->phnum * sizeof(struct elf32_phdr) > size) {
		printf("mss: %u: modem.mdt is not the ELF of 32 bits this reads\n", step);
		return false;
	}
	printf("mss: %u: modem.mdt: ELF32, machine %u, entry %#x, %u segments\n", step, h->machine, h->entry, h->phnum);
	for (unsigned i = 0; i < h->phnum; i++) {
		const struct elf32_phdr *p = (const struct elf32_phdr *)(mdt + h->phoff + i * sizeof(struct elf32_phdr));
		char file[16];
		uint32_t fsize = 0, ffirst = 0;
		bool there;

		snprintf(file, sizeof(file), "modem.b%02u", i);
		there = image_file(file, &fsize, &ffirst);
		if (p->type == 1 && QCOM_MDT_TYPE(p->flags) != 2 && p->memsz != 0) {
			low = p->paddr < low ? p->paddr : low;
			high = p->paddr + p->memsz > high ? p->paddr + p->memsz : high;
			total += p->filesz;
		}
		relocatable |= p->flags & QCOM_MDT_RELOCATABLE;
		if (p->filesz != 0 && !there) {
			missing++;
		}
		if (p->filesz != 0 && there && fsize != p->filesz) {
			wrong++;
		}
		printf("mss: %u:   segment %2u: type %u flags %#010x paddr %#010x file %#9x memory %#9x%s%s%s\n", step, i, p->type,
		    p->flags, p->paddr, p->filesz, p->memsz, QCOM_MDT_TYPE(p->flags) == 2 ? " (hash)" : "",
		    p->filesz == 0 ? "" : there ? (fsize == p->filesz ? " file ok" : " FILE OF ANOTHER SIZE") : " FILE MISSING",
		    (p->flags & QCOM_MDT_RELOCATABLE) ? " relocatable" : "");
	}
	printf("mss: %u: the loaded segments span %#x..%#x (%u MB) with %u MB of file; %s; reserved memory %#lx, %u MB: %s; "
	    "%u segment files missing, %u of another size\n", step, low, high, (high - low) >> 20, total >> 20,
	    relocatable ? "relocatable (goes where the memory is)" : "at fixed addresses", MODEM_MEMORY_PHYS, MODEM_MEMORY_SIZE >> 20,
	    high - low <= MODEM_MEMORY_SIZE ? "IT FITS" : "IT DOES NOT FIT", missing, wrong);
	return missing == 0 && wrong == 0 && high - low <= MODEM_MEMORY_SIZE;
}

/* ---- step 4: the header, to TrustZone -------------------------------------------------------- */

#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_ONE_VALUE           1
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1
#define PAS_MODEM               4               /* dt: qcom,pas-id of qcom,mss@6080000 */

/*
 * One call that may be preempted, as Linux makes it (and redwood_wifi.c). TrustZone's word: 0 or an
 * error; *answer its answer. A call of more than three arguments has the fourth and those after it
 * in memory, whose physical address is `rest` (x5: Linux's scm_smc_call).
 */
static int32_t
scm_ext(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t rest,
    uint64_t *answer)
{
	uint64_t session = 0;

	function |= SCM_64 | SCM_SIP;
	printf("mss: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx%s\n", step, what, function, first, second,
	    third, rest != 0 ? " and more in memory" : "");
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = rest;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("mss: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("mss: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	return scm_ext(what, function, arguments, first, second, third, 0, answer);
}

static bool
step_tz_header(void)
{
	uint32_t size = 0, first = 0;
	vm_offset_t page = 0;
	uint64_t answer = ~0ull;
	int32_t result;

	announce("hand modem.mdt (header, table, hashes) to TrustZone to check its signature (image 4), in a page of ours; then "
	    "tell it where the image will be");
	if (!image_file("modem.mdt", &size, &first) || size > PAGE_SIZE) {
		printf("mss: %u: modem.mdt is not there, or longer than a page (%u bytes)\n", step, size);
		return false;
	}
	/* in memory of ours, one page and so in one piece, not touched again and never freed (see redwood_adreno.c) */
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    file_read(first, 0, (void *)page, size) != 0) {
		printf("mss: %u: no memory, or modem.mdt cannot be read\n", step);
		return false;
	}
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));       /* TrustZone reads memory, not this processor's cache */
	}
	settle();
	result = scm("to take the modem's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_MODEM, kvtophys(page), 0, &answer);
	if (result != 0 || answer != 0) {
		printf("mss: %u: TRUSTZONE DOES NOT TAKE THE HEADER: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("mss: %u: TRUSTZONE ACCEPTS THE MODEM'S SIGNATURE\n", step);
	result = scm("where the modem's image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_MODEM, MODEM_MEMORY_PHYS,
	    MODEM_MEMORY_SIZE, &answer);
	if (result != 0 || answer != 0) {
		printf("mss: %u: TRUSTZONE DOES NOT TAKE THE PLACE: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("mss: %u: TrustZone takes the place: %#x bytes at %#lx\n", step, MODEM_MEMORY_SIZE, MODEM_MEMORY_PHYS);
	return true;
}

/* ---- step 5: the image, in the modem's memory --------------------------------------------------- */

static bool image_written;

static bool
step_image(void)
{
	static uint8_t mdt[16384];
	const struct elf32_header *h = (const struct elf32_header *)mdt;
	uint32_t size = 0, first = 0, chunk = 1u << 20;
	uint64_t written = 0, cleared = 0, wrong = 0, started = mach_absolute_time(), ns;
	uint8_t *buffer;

	announce("put the image's segments into the modem's memory (0x4b000000, 126 MB: nobody's until TrustZone starts the "
	    "modem), each from its file in the FAT partition, zeroes behind it; then read the first and last bytes of each back");
	if (!image_file("modem.mdt", &size, &first) || size > sizeof(mdt) || file_read(first, 0, mdt, size) != 0) {
		return false;
	}
	buffer = kalloc_data(chunk, Z_WAITOK);
	if (buffer == NULL) {
		return false;
	}
	for (unsigned i = 0; i < h->phnum; i++) {
		const struct elf32_phdr *p = (const struct elf32_phdr *)(mdt + h->phoff + i * sizeof(struct elf32_phdr));
		uint32_t at, fsize = 0, ffirst = 0, length;
		volatile uint8_t *to;
		char file[16];

		if (p->type != 1 || QCOM_MDT_TYPE(p->flags) == 2 || p->memsz == 0) {
			continue;
		}
		at = p->paddr - (uint32_t)MODEM_MEMORY_PHYS;
		if (p->paddr < MODEM_MEMORY_PHYS || at + p->memsz > MODEM_MEMORY_SIZE || p->filesz > p->memsz) {
			printf("mss: %u: segment %u does not fit the memory\n", step, i);
			kfree_data(buffer, chunk);
			return false;
		}
		snprintf(file, sizeof(file), "modem.b%02u", i);
		if (p->filesz != 0 && (!image_file(file, &fsize, &ffirst) || fsize != p->filesz)) {
			printf("mss: %u: %s is not there or not %u bytes\n", step, file, p->filesz);
			kfree_data(buffer, chunk);
			return false;
		}
		length = (p->memsz + PAGE_MASK) & ~PAGE_MASK;
		to = (volatile uint8_t *)ml_io_map_unmappable(MODEM_MEMORY_PHYS + (at & ~PAGE_MASK), length + PAGE_SIZE, VM_WIMG_WCOMB);
		if (to == NULL) {
			printf("mss: %u: the memory of segment %u cannot be mapped\n", step, i);
			kfree_data(buffer, chunk);
			return false;
		}
		to += at & PAGE_MASK;
		for (uint32_t done = 0; done < p->filesz;) {
			uint32_t take = p->filesz - done < chunk ? p->filesz - done : chunk;

			if (file_read(ffirst, done, buffer, take) != 0) {
				printf("mss: %u: %s cannot be read at %u\n", step, file, done);
				kfree_data(buffer, chunk);
				return false;
			}
			for (uint32_t b = 0; b < take; b++) {
				to[done + b] = buffer[b];
			}
			done += take;
		}
		for (uint32_t b = p->filesz; b < p->memsz; b++) {
			to[b] = 0;
		}
		settle();
		/* a sample back: the first and the last 64 bytes of the file's part */
		if (p->filesz != 0) {
			uint32_t n = p->filesz < 64 ? p->filesz : 64;

			if (file_read(ffirst, 0, buffer, n) == 0) {
				for (uint32_t b = 0; b < n; b++) {
					wrong += to[b] != buffer[b];
				}
			}
			if (file_read(ffirst, p->filesz - n, buffer, n) == 0) {
				for (uint32_t b = 0; b < n; b++) {
					wrong += to[p->filesz - n + b] != buffer[b];
				}
			}
		}
		written += p->filesz;
		cleared += p->memsz - p->filesz;
		ml_io_unmap((vm_offset_t)to - (at & PAGE_MASK), length + PAGE_SIZE);
	}
	kfree_data(buffer, chunk);
	absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
	printf("mss: %u: %llu MB of the image written and %llu MB cleared in %llu ms; of the sample read back, %llu bytes differ\n",
	    step, written >> 20, cleared >> 20, ns / 1000000, wrong);
	image_written = wrong == 0;
	return image_written;
}

/* ---- step 6: our items in the shared memory ----------------------------------------------------- */

#define SMP2P_VERSION           1
#define SMP2P_RESTART_ACKS      1u
#define SMP2P_AT_VERSION        4
#define SMP2P_AT_PROCESSORS     8
#define SMP2P_AT_COUNTS         12
#define SMP2P_AT_NAME(i)        (20 + 20 * (i))
#define SMP2P_AT_VALUE(i)       (36 + 20 * (i))
#define GLINK_QUEUE_SIZE        16384
#define LOCK_PHYS               0x00343000UL    /* dt: hwlock (qcom,tcsr-mutex on syscon 0x340000, 0x1000 a lock), qcom,smem has lock 3 */
#define LOCK_US                 1u
#define IPC_PHYS                0x0f111000UL    /* dt: mailbox@0F111000, qcom,trinket-apcs-hmss-global */
#define IPC_RING                0x8             /* (Linux: msm8994_apcs_data, offset 8, for the "hmss-global" kind) */
#define IPC_MODEM_GLINK         12
#define IPC_MODEM_SMP2P         14

static vm_offset_t smem_rw, ipc;
static uint32_t our_smp2p, our_glink_descriptor, our_glink_queue;

static void
smem_poke(uint32_t offset, uint32_t value)
{
	if (smem_rw == 0) {
		smem_rw = ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	wr(smem_rw + offset, value);
}

static void
ring(unsigned bit, const char *whose)
{
	if (ipc == 0) {
		ipc = ml_io_map(IPC_PHYS, 0x1000);
	}
	printf("mss: %u: about to ring %s (write %#x at %#lx)\n", step, whose, 1u << bit, IPC_PHYS + IPC_RING);
	wr(ipc + IPC_RING, 1u << bit);
}

/* Our item in the partition, as Linux's qcom_smem_alloc_private() makes one; under the lock, which the caller holds. */
static bool
item_make(uint32_t partition, uint16_t item, uint32_t size)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (size + 7) & ~7u, need = sizeof(entry) + item_size;
	uint32_t at;

	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		printf("mss: %u: the partition's marks make no sense: nothing is written\n", step);
		return false;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (entry.item == item) {
			printf("mss: %u: item %u is there already: used as it is\n", step, item);
			return true;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (at != header.free_uncached || at + need > header.free_cached) {
		printf("mss: %u: the entries do not end at the mark (+%#x, the mark +%#x), or no room: nothing is written\n", step, at,
		    header.free_uncached);
		return false;
	}
	smem_poke(partition + at, SMEM_CANARY | (uint32_t)item << 16);
	smem_poke(partition + at + 4, item_size);
	smem_poke(partition + at + 8, item_size - size);       /* padding behind the item; none in front */
	smem_poke(partition + at + 12, 0);
	for (uint32_t b = 0; b < item_size; b += 4) {
		smem_poke(partition + at + (uint32_t)sizeof(entry) + b, 0);
	}
	settle();
	smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need);
	settle();
	printf("mss: %u: item %u made: %u bytes at %#lx\n", step, item, size, SMEM_PHYS + partition + at + sizeof(entry));
	return true;
}

static bool
step_items(void)
{
	static const struct {
		uint16_t item;
		uint32_t size;
	} items[] = { { ITEM_SMP2P_OURS, sizeof(struct smp2p) }, { ITEM_GLINK_DESCRIPTOR, 32 }, { ITEM_GLINK_OUR_QUEUE, GLINK_QUEUE_SIZE } };
	vm_offset_t lock;
	uint32_t cacheline = 0, size = 0, partition, owner = ~0u;
	bool made = true;
	uint32_t name[4] = { 0 };

	announce("make our items in the partition of the modem and us (SMP2P 435, GLINK 478 and 479), under the SoC's lock "
	    "(0x343000: the first access to it), fill the SMP2P one in, and ring the modem's SMP2P doorbell");
	partition = smem_partition(SMEM_US, SMEM_MODEM, &cacheline, &size);
	if (partition == 0) {
		printf("mss: %u: there is no partition for the modem and us\n", step);
		return false;
	}
	lock = ml_io_map(LOCK_PHYS, 0x1000);
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(lock, LOCK_US);
		owner = rd(lock);
		if (owner == LOCK_US) {
			break;
		}
		IOSleep(1);
	}
	if (owner != LOCK_US) {
		printf("mss: %u: THE LOCK IS HELD BY PROCESSOR %u FOR A SECOND: nothing is written\n", step, owner);
		return false;
	}
	for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]) && made; i++) {
		made = item_make(partition, items[i].item, items[i].size);
	}
	wr(lock, 0);
	printf("mss: %u: the lock is released (it reads %u)\n", step, rd(lock));
	our_smp2p = smem_item(ITEM_SMP2P_OURS, &size);
	our_glink_descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);
	our_glink_queue = smem_item(ITEM_GLINK_OUR_QUEUE, &size);
	if (!made || our_smp2p == 0 || our_glink_descriptor == 0 || our_glink_queue == 0) {
		printf("mss: %u: THE ITEMS ARE NOT ALL THERE after making them (SMP2P %#x, GLINK %#x %#x)\n", step, our_smp2p,
		    our_glink_descriptor, our_glink_queue);
		return false;
	}
	show_partition("us and the modem (0 and 1)", SMEM_US, SMEM_MODEM);
	/* SMP2P: zeroes, from 0 to 1, room for 16, "master-kernel" at 0 (its bit 0 is "stop": dt qcom,force-stop), then the version */
	for (uint32_t at = 0; at < sizeof(struct smp2p); at += 4) {
		smem_poke(our_smp2p + at, 0);
	}
	settle();
	smem_poke(our_smp2p, SMP2P_MAGIC);
	smem_poke(our_smp2p + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_MODEM << 16);
	strlcpy((char *)name, "master-kernel", sizeof(name));
	for (uint32_t word = 0; word < 4; word++) {
		smem_poke(our_smp2p + SMP2P_AT_NAME(0) + 4 * word, name[word]);
	}
	smem_poke(our_smp2p + SMP2P_AT_COUNTS, 16 | 1u << 16);
	smem_poke(our_smp2p + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	settle();
	smem_poke(our_smp2p + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	settle();
	printf("mss: %u: our SMP2P item is filled in, at %#lx; GLINK's descriptor at %#lx (our marks at 0), our queue at %#lx\n",
	    step, SMEM_PHYS + our_smp2p, SMEM_PHYS + our_glink_descriptor, SMEM_PHYS + our_glink_queue);
	ring(IPC_MODEM_SMP2P, "the modem's SMP2P doorbell");
	return true;
}

/* ---- step 7: the modem, started ------------------------------------------------------------------- */

#define MODEM_FATAL             (1u << 0)
#define MODEM_READY             (1u << 1)
#define MODEM_HANDOVER          (1u << 2)

static void
show_smp2p(const char *prefix, const struct smp2p *words)
{
	char line[320];
	int n = 0;

	line[0] = 0;
	for (unsigned i = 0; i < words->valid && i < 16 && n < (int)sizeof(line) - 40; i++) {
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %.16s = %#x;", words->entry[i].name, words->entry[i].value);
	}
	printf("mss: %u: %s: SMP2P version %u, features %#x, from %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
	    words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}

static uint32_t
slave_kernel(const struct smp2p *words)
{
	for (unsigned i = 0; i < words->valid && i < 16; i++) {
		if (strncmp(words->entry[i].name, "slave-kernel", 16) == 0) {
			return words->entry[i].value;
		}
	}
	return 0;
}

static bool
step_start(void)
{
	struct smp2p words, last;
	uint64_t answer = ~0ull, started, now, ns;
	uint32_t wait_seconds = 10, size = 0, at = 0, state = 0, changes = 0;
	bool have = false, ready = false;
	int32_t result;

	if (!image_written || our_smp2p == 0) {
		printf("mss: %u: the image is not in place, or our items are not made\n", step);
		return false;
	}
	wait_seconds = 2;               /* (mss_wait=S: longer) */
	PE_parse_boot_argn("mss_wait", &wait_seconds, sizeof(wait_seconds));
	announce("ask TrustZone to authenticate the modem's image and let the modem out of reset; then watch the shared memory");
	result = scm("to authenticate the modem's image and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_MODEM, 0, 0,
	    &answer);
	started = mach_absolute_time();
	image_written = false;
	if (result != 0 || answer != 0) {
		printf("mss: %u: TRUSTZONE DOES NOT START THE MODEM: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	modem_started = true;
	printf("mss: %u: TRUSTZONE HAS AUTHENTICATED THE IMAGE AND LET THE MODEM OUT OF RESET; watching for %u s\n", step,
	    wait_seconds);
	bzero(&last, sizeof(last));
	for (;;) {
		absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
		now = ns / 1000;
		if (now >= (uint64_t)wait_seconds * 1000000) {
			break;
		}
		if (at == 0) {
			at = smem_item(ITEM_SMP2P_THEIRS, &size);
		}
		if (at != 0 && size >= sizeof(words) && smem_peek(at, &words, sizeof(words)) && words.magic == SMP2P_MAGIC &&
		    (!have || memcmp(&words, &last, sizeof(words)) != 0)) {
			char when[80];

			snprintf(when, sizeof(when), "+%llu.%03llu ms: the modem's item %s", now / 1000, now % 1000, have ? "changes" :
			    "APPEARS");
			if (changes++ < 40) {
				show_smp2p(when, &words);
			}
			last = words;
			have = true;
			state = slave_kernel(&words);
			if ((state & MODEM_READY) && !ready) {
				ready = true;
				printf("mss: %u: THE MODEM SAYS IT IS UP: \"slave-kernel\" says %#x, %llu.%03llu ms after the start\n", step,
				    state, now / 1000, now % 1000);
			}
			if (state & MODEM_FATAL) {
				uint32_t csize = 0, cat = smem_item(ITEM_CRASH_REASON, &csize);
				char text[81];

				bzero(text, sizeof(text));
				if (cat != 0) {
					smem_peek(cat, text, csize < sizeof(text) - 1 ? csize : sizeof(text) - 1);
				}
				printf("mss: %u: THE MODEM SAYS IT HAS GIVEN UP (bit 0); its last words: \"%s\"\n", step, text);
				break;
			}
		}
		delay(200);
	}
	show_partition("us and the modem, after the watch", SMEM_US, SMEM_MODEM);
	show_partition("everybody, after the watch", SMEM_GLOBAL, SMEM_GLOBAL);
	{
		static const uint32_t looked[] = { ITEM_GLINK_DESCRIPTOR, ITEM_GLINK_THEIR_QUEUE, 605 };

		for (unsigned k = 0; k < sizeof(looked) / sizeof(looked[0]); k++) {
			uint32_t isize = 0, iat = smem_item(looked[k], &isize);
			uint8_t bytes[64];
			char line[200];
			int n = 0;

			if (iat == 0) {
				printf("mss: %u: item %u: not there\n", step, looked[k]);
				continue;
			}
			bzero(bytes, sizeof(bytes));
			smem_peek(iat, bytes, isize < sizeof(bytes) ? isize : sizeof(bytes));
			line[0] = 0;
			for (unsigned b = 0; b < sizeof(bytes) && b < isize; b += 4) {
				n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x", bytes[b] | bytes[b + 1] << 8 | bytes[b + 2] << 16 |
				    (uint32_t)bytes[b + 3] << 24);
			}
			printf("mss: %u: item %u (%u bytes) begins:%s\n", step, looked[k], isize, line);
		}
	}
	if (have) {
		show_smp2p("at the end of the watch the modem's item says", &last);
	} else {
		printf("mss: %u: NO ITEM OF THE MODEM'S in %u s\n", step, wait_seconds);
	}
	/* (GLINK comes first with this modem: it says its version and waits; its SMP2P item comes later, if at all) */
	return ready || smem_item(ITEM_GLINK_THEIR_QUEUE, &size) != 0;
}

/* ---- what the link (below, from redwood_wifi.c) needs of this file ----------------------------- */

static LCK_GRP_DECLARE(mss_lock_group, "redwood-mss");

/* The doorbells of this phone are rung, not heard: the link's thread looks every millisecond instead. */
static struct {
	uint32_t glink, smp2p, mailbox, other;
	uint64_t since;
} doorbell;

static void
doorbells(void)
{
}

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

static void
ring_quietly(unsigned bit)
{
	if (ipc == 0) {
		ipc = ml_io_map(IPC_PHYS, 0x1000);
	}
	wr(ipc + IPC_RING, 1u << bit);
}

/* The modem's SMP2P item as it is now, if there is one. */
static bool
modem_words(struct smp2p *words)
{
	static uint32_t at;
	uint32_t size = 0;

	if (at == 0) {
		at = smem_item(ITEM_SMP2P_THEIRS, &size);
		if (at == 0 || size < sizeof(*words)) {
			at = 0;
			return false;
		}
	}
	return smem_peek(at, words, sizeof(*words)) && words->magic == SMP2P_MAGIC;
}

/* A change in the modem's item: said (SMP2P's restart handshake is not needed on a first start). */
static void
modem_listen(const struct smp2p *words)
{
	show_smp2p("the link: the modem's item changes", words);
}

static void
modem_last_words(void)
{
	uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);
	char text[81];

	bzero(text, sizeof(text));
	if (at != 0) {
		smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
	}
	printf("mss: the modem's last words: \"%s\"\n", text);
}

/* ---- GLINK and QRTR, from redwood_wifi.c (steps 12, 13, 14 there: 8, 9, 10 here) ------------------ */

#define GLINK_OUR_TAIL          0               /* the descriptor's words: how far the modem has read our queue (its to move) */
#define GLINK_OUR_HEAD          4               /* how far we have written it */
#define GLINK_THEIR_TAIL        8               /* how far we have read the modem's queue */
#define GLINK_THEIR_HEAD        12              /* how far it has written it (its to move) */
#define GLINK_RESERVE           16              /* of our queue, never filled (Linux's reserve, and room for one short message) */

enum {
	GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
	GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
	GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_VERSION_1         1
#define GLINK_INTENT_REUSE      1u              /* the feature Linux offers over this transport */
#define GLINK_CHANNEL           "IPCRTR"
#define GLINK_US                1               /* our number for that channel */

struct glink_message {
	uint16_t command, param1;
	uint32_t param2;
};
struct glink_packet {                           /* a whole message from the channel, waiting for the link's thread */
	struct glink_packet *next;
	uint32_t length;
	uint8_t data[];
};

static LCK_MTX_DECLARE(glink_lock, &mss_lock_group);   /* the queues' marks and everything below */
static struct {
	uint32_t descriptor, ours, ours_size, theirs, theirs_size;      /* where in the shared memory */
	bool up;                                /* the queues are found and ours to use */
	bool broken;                            /* something in the modem's queue was not understood: nothing more is read */
	uint32_t features;
	bool agreed;                            /* the versions are settled */
	uint32_t their_channel;                 /* the modem's number for IPCRTR; 0: it has not opened it */
	bool asked, acknowledged, open;         /* our open sent; acknowledged; both ways done and our intents told */
	bool request_answered, request_granted; /* our last request for an intent */
	struct {
		uint32_t id, size;
		bool there, in_use;
	} their_intents[64];                    /* what we may send into */
	struct {
		uint32_t size, fill;
		uint8_t *data;
	} our_intents[24];                      /* what the modem may send into; an intent's number is its place here + 1 */
	uint32_t our_intent_count;
	struct glink_packet *first, *last;      /* received, in order */
	uint32_t said, heard, trace;            /* messages each way; how many more are written to the log */
} glink;

static uint32_t
smem_word(uint32_t offset)
{
	return rd((vm_offset_t)smem + offset);
}

/* The three items; false if one is missing or not of a shape this knows. */
static bool
glink_find(void)
{
	uint32_t size = 0;

	glink.descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);
	if (glink.descriptor == 0 || size != 32) {
		return false;
	}
	glink.ours = smem_item(ITEM_GLINK_OUR_QUEUE, &glink.ours_size);
	glink.theirs = smem_item(ITEM_GLINK_THEIR_QUEUE, &glink.theirs_size);
	return glink.ours != 0 && glink.theirs != 0 && glink.ours_size >= 1024 && glink.theirs_size >= 1024 &&
	       !(glink.ours_size & 7) && !(glink.theirs_size & 7);
}

/* Our two marks to zero, if they are not: before the modem starts (see above). */
static void
glink_reset(void)
{
	uint32_t size = 0, descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);

	if (descriptor == 0 || size != 32) {
		printf("mss: %u: GLINK's descriptor (item %u) is not there: nothing to reset\n", step, ITEM_GLINK_DESCRIPTOR);
		return;
	}
	printf("mss: %u: GLINK's descriptor at %#lx: our queue read to %#x and written to %#x, the modem's read to %#x and written "
	    "to %#x\n", step, SMEM_PHYS + descriptor, smem_word(descriptor + GLINK_OUR_TAIL), smem_word(descriptor + GLINK_OUR_HEAD),
	    smem_word(descriptor + GLINK_THEIR_TAIL), smem_word(descriptor + GLINK_THEIR_HEAD));
	if (smem_word(descriptor + GLINK_OUR_HEAD) != 0 || smem_word(descriptor + GLINK_THEIR_TAIL) != 0) {
		printf("mss: %u: about to write our two marks there (+4, +8): 0\n", step);
		smem_poke(descriptor + GLINK_OUR_HEAD, 0);
		smem_poke(descriptor + GLINK_THEIR_TAIL, 0);
		settle();
	}
}

/* How much of the modem's queue is unread. */
static uint32_t
glink_waiting(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_THEIR_HEAD), tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);

	if (head >= glink.theirs_size || tail >= glink.theirs_size) {
		return 0;
	}
	return head >= tail ? head - tail : glink.theirs_size - tail + head;
}

static void
glink_peek(void *to, uint32_t offset, uint32_t count)
{
	uint32_t at = (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + offset) % glink.theirs_size;

	for (uint32_t i = 0; i < count; i++) {
		((uint8_t *)to)[i] = smem[glink.theirs + (at + i) % glink.theirs_size];
	}
}

static void
glink_advance(uint32_t count)
{
	smem_poke(glink.descriptor + GLINK_THEIR_TAIL, (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + count) % glink.theirs_size);
}

/* How much we may write into our queue now. */
static uint32_t
glink_room(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), tail = smem_word(glink.descriptor + GLINK_OUR_TAIL), room;

	if (head >= glink.ours_size || tail >= glink.ours_size) {
		return 0;
	}
	room = tail <= head ? glink.ours_size - head + tail : tail - head;
	return room < GLINK_RESERVE ? 0 : room - GLINK_RESERVE;
}

/* A message into our queue, in two pieces, padded to eight bytes; then our mark, and the modem's doorbell. */
static bool
glink_put(const void *first, uint32_t first_length, const void *second, uint32_t second_length)
{
	volatile uint8_t *queue = (volatile uint8_t *)(uintptr_t)smem + glink.ours;
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), length = first_length + second_length;
	uint32_t padded = (length + 7) & ~7u;

	if (!glink.up || glink_room() < padded) {
		return false;
	}
	for (uint32_t i = 0; i < padded; i++) {
		queue[(head + i) % glink.ours_size] = i < first_length ? ((const uint8_t *)first)[i] :
		    i < length ? ((const uint8_t *)second)[i - first_length] : 0;
	}
	settle();
	smem_poke(glink.descriptor + GLINK_OUR_HEAD, (head + padded) % glink.ours_size);
	settle();
	ring_quietly(IPC_MODEM_GLINK);
	glink.said++;
	return true;
}

static const char *
glink_name(unsigned command)
{
	static const char *const names[] = {
		"version", "version-ack", "open", "close", "open-ack", "intent", "done", "intent-request", "intent-request-ack",
		"data", "10", "close-ack", "data-more", "read-notify", "done-reuse", "signals"
	};

	return command < sizeof(names) / sizeof(names[0]) ? names[command] : "?";
}

/* A short message of ours. */
static void
glink_say(unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
	bool sent = glink_put(&message, sizeof(message), more, more_length);

	if (glink.trace > 0 || !sent) {
		glink.trace -= glink.trace > 0;
		printf("mss: glink: > %s %u %#x%s\n", glink_name(command), param1, param2, sent ? "" : ": NO ROOM IN OUR QUEUE, not sent");
	}
}

/* An intent of ours of that size: a buffer, and the modem told of it. False: there are no more. */
static bool
glink_offer(uint32_t size)
{
	uint32_t words[2] = { size, glink.our_intent_count + 1 }, i = glink.our_intent_count;

	if (i >= sizeof(glink.our_intents) / sizeof(glink.our_intents[0]) || size == 0 || size > 0x10000) {
		return false;
	}
	if (glink.our_intents[i].data == NULL || glink.our_intents[i].size != size) {
		glink.our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);       /* (kept for good: a later link uses it again) */
	}
	if (glink.our_intents[i].data == NULL) {
		return false;
	}
	glink.our_intents[i].size = size;
	glink.our_intents[i].fill = 0;
	glink.our_intent_count++;
	glink_say(GLINK_INTENT, GLINK_US, 1, words, sizeof(words));
	return true;
}

/*
 * Everything the modem has put into its queue, taken out and acted on: Linux's
 * qcom_glink_native_rx(). With glink_lock held; whoever waits for something from the modem calls
 * this, so it must not wait itself. Whole messages for the channel are left in a list for the
 * link's thread.
 */
static void
glink_pump(void)
{
	static const struct {
		uint32_t size, count;
	} intents[] = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } };           /* dt: qcom,intents of qcom,wpss_qrtr */
	struct glink_message message;
	uint32_t words[4];
	char name[33];

	while (glink.up && !glink.broken) {
		uint32_t waiting = glink_waiting(), length = sizeof(message);

		if (waiting < sizeof(message)) {
			return;
		}
		glink_peek(&message, 0, sizeof(message));
		if (message.command == GLINK_OPEN) {
			length += message.param2;
		} else if (message.command == GLINK_INTENT) {
			length += 8 * message.param2;
		} else if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
			if (waiting < 16) {
				return;
			}
			glink_peek(words, 8, 8);                /* the size of this piece, and what is still to come */
			length = 16 + words[0];
		}
		length = (length + 7) & ~7u;
		if (length > glink.theirs_size - 8) {
			printf("mss: glink: A MESSAGE OF %u BYTES (%s %u %#x): not believed, and nothing more is read\n", length,
			    glink_name(message.command), message.param1, message.param2);
			glink.broken = true;
			return;
		}
		if (waiting < length) {
			return;                                 /* (the rest of it is on its way) */
		}
		glink.heard++;
		if (glink.trace > 0 && message.command != GLINK_DATA && message.command != GLINK_DATA_MORE) {
			glink.trace--;
			printf("mss: glink: < %s %u %#x\n", glink_name(message.command), message.param1, message.param2);
		}
		switch (message.command) {
		case GLINK_VERSION:
			if (message.param1 == GLINK_VERSION_1) {
				glink.features &= message.param2;
			}
			glink_advance(length);
			glink_say(GLINK_VERSION_ACK, GLINK_VERSION_1, glink.features, NULL, 0);
			break;
		case GLINK_VERSION_ACK:
			glink_advance(length);
			if (message.param1 == GLINK_VERSION_1 && message.param2 == glink.features) {
				glink.agreed = true;
			} else if (message.param1 != 0) {
				glink.features &= message.param2;
				glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
			}
			break;
		case GLINK_OPEN:
			bzero(name, sizeof(name));
			glink_peek(name, sizeof(message), message.param2 < sizeof(name) - 1 ? message.param2 : sizeof(name) - 1);
			glink_advance(length);
			if (strcmp(name, GLINK_CHANNEL) != 0) {
				printf("mss: glink: the modem opens \"%s\" as its channel %u: left unanswered, nobody here speaks it\n", name,
				    message.param1);
				break;
			}
			printf("mss: glink: the modem opens \"%s\" as its channel %u: acknowledged, and opened from our side as channel %u\n",
			    name, message.param1, GLINK_US);
			glink.their_channel = message.param1;
			glink_say(GLINK_OPEN_ACK, glink.their_channel, 0, NULL, 0);
			if (!glink.asked) {
				bzero(name, sizeof(name));
				strlcpy(name, GLINK_CHANNEL, sizeof(name));
				glink_say(GLINK_OPEN, GLINK_US, sizeof(GLINK_CHANNEL), name, (sizeof(GLINK_CHANNEL) + 7) & ~7u);
				glink.asked = true;
			}
			break;
		case GLINK_OPEN_ACK:
			glink_advance(length);
			if (message.param1 == GLINK_US) {
				glink.acknowledged = true;
			}
			break;
		case GLINK_INTENT:
			for (uint32_t i = 0; i < message.param2 && message.param1 == glink.their_channel; i++) {
				glink_peek(words, 8 + 8 * i, 8);        /* a size and a number */
				for (unsigned n = 0; n < sizeof(glink.their_intents) / sizeof(glink.their_intents[0]); n++) {
					if (!glink.their_intents[n].there || glink.their_intents[n].id == words[1]) {
						glink.their_intents[n].there = true;
						glink.their_intents[n].in_use = false;
						glink.their_intents[n].size = words[0];
						glink.their_intents[n].id = words[1];
						break;
					}
				}
				if (glink.trace > 0) {
					printf("mss: glink:   the modem will take %u bytes into its intent %u\n", words[0], words[1]);
				}
			}
			glink_advance(length);
			break;
		case GLINK_DONE:
		case GLINK_DONE_REUSE:
			for (unsigned n = 0; n < sizeof(glink.their_intents) / sizeof(glink.their_intents[0]); n++) {
				if (glink.their_intents[n].there && glink.their_intents[n].id == message.param2) {
					glink.their_intents[n].in_use = false;
					glink.their_intents[n].there = message.command == GLINK_DONE_REUSE;
				}
			}
			glink_advance(length);
			break;
		case GLINK_INTENT_REQUEST:
			glink_advance(length);
			if (message.param1 == glink.their_channel) {
				bool granted = glink_offer(message.param2);

				glink_say(GLINK_INTENT_REQUEST_ACK, GLINK_US, granted, NULL, 0);
			}
			break;
		case GLINK_INTENT_REQUEST_ACK:
			glink_advance(length);
			glink.request_answered = true;
			glink.request_granted = message.param2 != 0;
			break;
		case GLINK_DATA:
		case GLINK_DATA_MORE: {
			uint32_t intent = message.param2 - 1;

			if (message.param1 != glink.their_channel || intent >= glink.our_intent_count ||
			    words[0] > glink.our_intents[intent].size - glink.our_intents[intent].fill) {
				printf("mss: glink: < data of %u bytes on channel %u for intent %u: NOT OURS OR TOO MUCH, dropped\n", words[0],
				    message.param1, message.param2);
				glink_advance(length);
				break;
			}
			glink_peek(glink.our_intents[intent].data + glink.our_intents[intent].fill, 16, words[0]);
			glink.our_intents[intent].fill += words[0];
			glink_advance(length);
			if (words[1] == 0) {
				/* whole: into the list, and the intent is the modem's to use again */
				struct glink_packet *packet = kalloc_data(sizeof(*packet) + glink.our_intents[intent].fill, Z_WAITOK);

				if (packet != NULL) {
					packet->next = NULL;
					packet->length = glink.our_intents[intent].fill;
					memcpy(packet->data, glink.our_intents[intent].data, packet->length);
					if (glink.last != NULL) {
						glink.last->next = packet;
					} else {
						glink.first = packet;
					}
					glink.last = packet;
				}
				glink.our_intents[intent].fill = 0;
				glink_say(GLINK_DONE_REUSE, GLINK_US, message.param2, NULL, 0);
			}
			break;
		}
		case GLINK_READ_NOTIFY:
			glink_advance(length);
			ring_quietly(IPC_MODEM_GLINK);
			break;
		case GLINK_CLOSE:
			glink_advance(length);
			glink_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
			if (message.param1 == glink.their_channel) {
				printf("mss: glink: THE DSP CLOSES \"%s\"\n", GLINK_CHANNEL);
				glink.open = false;
				glink.their_channel = 0;
			}
			break;
		case GLINK_CLOSE_ACK:
		case GLINK_SIGNALS:
			glink_advance(length);
			break;
		default:
			printf("mss: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", message.command,
			    message.param1, message.param2);
			glink.broken = true;
			return;
		}
		if (glink.their_channel != 0 && glink.acknowledged && !glink.open) {
			glink.open = true;
			printf("mss: glink: THE CHANNEL \"%s\" IS OPEN both ways (ours %u, the modem's %u); telling the modem our intents\n",
			    GLINK_CHANNEL, GLINK_US, glink.their_channel);
			for (unsigned i = 0; i < sizeof(intents) / sizeof(intents[0]); i++) {
				for (unsigned n = 0; n < intents[i].count; n++) {
					glink_offer(intents[i].size);
				}
			}
		}
	}
}

/*
 * One whole message to the modem on the channel (Linux's __qcom_glink_send(), for messages that
 * go in one piece). Waits, up to five seconds, for an intent of the modem's that it fits and
 * for room in our queue, asking for the intent if there is none.
 */
static int
glink_send(const void *data, uint32_t length)
{
	struct {
		struct glink_message message;
		uint32_t size, left;
	} header = { { GLINK_DATA, GLINK_US, 0 }, length, 0 };
	bool asked = false, fits;
	int found = -1;

	lck_mtx_lock(&glink_lock);
	if (glink.up && length + sizeof(header) + 8 > glink.ours_size - GLINK_RESERVE) {
		lck_mtx_unlock(&glink_lock);
		return EMSGSIZE;
	}
	for (unsigned waited = 0;; waited++) {
		glink_pump();
		if (!glink.up || !glink.open || glink.broken) {
			lck_mtx_unlock(&glink_lock);
			return ENOTCONN;
		}
		found = -1;
		fits = false;
		for (unsigned n = 0; n < sizeof(glink.their_intents) / sizeof(glink.their_intents[0]); n++) {
			if (!glink.their_intents[n].there || glink.their_intents[n].size < length) {
				continue;
			}
			fits = true;                            /* (one that is in use comes back: that is waited for) */
			if (!glink.their_intents[n].in_use && (found < 0 || glink.their_intents[n].size < glink.their_intents[found].size)) {
				found = (int)n;
			}
		}
		if (found >= 0 && glink_room() >= ((length + sizeof(header) + 7) & ~7u)) {
			break;
		}
		if (!fits && !asked) {
			glink.request_answered = false;
			glink_say(GLINK_INTENT_REQUEST, GLINK_US, length, NULL, 0);
			asked = true;
		}
		if (waited >= 5000 || (!fits && asked && glink.request_answered && !glink.request_granted)) {
			lck_mtx_unlock(&glink_lock);
			return found < 0 ? ENOBUFS : EAGAIN;
		}
		lck_mtx_unlock(&glink_lock);
		IOSleep(1);
		lck_mtx_lock(&glink_lock);
	}
	glink.their_intents[found].in_use = true;
	header.message.param2 = glink.their_intents[found].id;
	glink_put(&header, sizeof(header), data, length);
	lck_mtx_unlock(&glink_lock);
	return 0;
}

/* ---- QRTR -------------------------------------------------------------------------------------------- */

/*
 * What the channel carries (Linux: net/qrtr/af_qrtr.c, ns.c, smd.c): datagrams from a port of a
 * node to a port of a node, one in each GLINK message, behind a header of eight words. A node's
 * services announce themselves to the other nodes through a port that all have, the control
 * port: when a link comes up each side says hello, and is told the other's services one by one
 * ("new server": a service, an instance, a node, a port), and later their going. We are node 1
 * (Linux's default, and what this phone's Android is); the modem says who it is in what it sends.
 * The interface for the rest of the kernel is sys/redwood_qrtr.h: the four calls at the end of
 * this section, handed to the QMI helpers (bsd/dev/redwood_qmi.c), which is what speaks to the
 * services.
 */
#define REDWOOD_QRTR_MAX        8192            /* the most a datagram holds, either way */
#define QRTR_US                 1u
#define QRTR_CONTROL            0xfffffffeu     /* the control port */
#define QRTR_FIRST_PORT         0x4000u         /* where Linux begins numbering ports that ask for no number */
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7 };

struct qrtr_header {                            /* version 1, which Linux sends; version 2 is read as well */
	uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};
struct qrtr_control {
	uint32_t command;
	uint32_t words[4];                      /* a server: service, instance, node, port; a client: node, port */
};

struct redwood_qrtr_port {
	uint32_t number;                        /* 0: free */
	redwood_qrtr_receive_t receive;
	redwood_qrtr_server_t server;
	void *context;
	struct {
		uint32_t service, instance;     /* the instance as on the wire: version | instance << 8; 0: any */
		bool asked;
		bool fresh;                     /* not yet told of the services that were there before it */
	} lookups[4];
};

static LCK_MTX_DECLARE(qrtr_lock, &mss_lock_group);    /* the tables below */
static LCK_MTX_DECLARE(qrtr_calling, &mss_lock_group); /* held by the link's thread around a callback */
static struct redwood_qrtr_port qrtr_ports[16];
static struct {
	uint32_t service, instance, node, port;
	bool there;
} qrtr_servers[64];
static uint32_t qrtr_peer;                      /* the modem's node (it is node 0: so this flag says whether it has spoken) */
static bool qrtr_peer_known;
static uint32_t qrtr_next_port = QRTR_FIRST_PORT, qrtr_in, qrtr_out, qrtr_hellos;
static thread_t link_thread_is;

/* One datagram out, of any type. */
static int
qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	uint32_t padded = (length + 3) & ~3u;
	struct qrtr_header *packet;
	int error;

	if (length == 0 || length > REDWOOD_QRTR_MAX) {
		return EMSGSIZE;
	}
	packet = kalloc_data(sizeof(*packet) + padded, Z_WAITOK | Z_ZERO);
	if (packet == NULL) {
		return ENOMEM;
	}
	packet->version = 1;
	packet->type = type;
	packet->from_node = QRTR_US;
	packet->from_port = from_port;
	packet->size = length;
	packet->to_node = to_node;
	packet->to_port = to_port;
	memcpy(packet + 1, data, length);
	error = glink_send(packet, (uint32_t)sizeof(*packet) + padded);
	kfree_data(packet, sizeof(*packet) + padded);
	qrtr_out += error == 0;
	return error;
}

static bool
qrtr_matches(uint32_t service, uint32_t instance, unsigned server)
{
	return qrtr_servers[server].there && qrtr_servers[server].service == service &&
	       (instance == 0 || qrtr_servers[server].instance == instance);
}

/* Everybody who looked a service up is told that it has come, or gone. From the link's thread. */
static void
qrtr_tell(uint32_t service, uint32_t instance, uint32_t node, uint32_t port, bool arrived)
{
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		for (unsigned l = 0; l < 4; l++) {
			redwood_qrtr_server_t server = NULL;
			void *context;

			lck_mtx_lock(&qrtr_lock);
			if (qrtr_ports[p].number != 0 && qrtr_ports[p].lookups[l].asked && !qrtr_ports[p].lookups[l].fresh &&
			    qrtr_ports[p].lookups[l].service == service &&
			    (qrtr_ports[p].lookups[l].instance == 0 || qrtr_ports[p].lookups[l].instance == instance)) {
				server = qrtr_ports[p].server;
			}
			context = qrtr_ports[p].context;
			lck_mtx_unlock(&qrtr_lock);
			if (server != NULL) {
				server(context, arrived, service, instance & 0xff, instance >> 8, node, port);
			}
		}
	}
}

/* The control port's part: Linux's name service, without services of our own to tell of. */
static void
qrtr_control(const struct qrtr_header *header, const struct qrtr_control *control)
{
	struct qrtr_control hello = { .command = QRTR_HELLO };
	int found = -1, error;

	switch (control->command) {
	case QRTR_HELLO:
		if (qrtr_hellos++ != 0) {
			break;                                  /* (answered once: two that answer every hello never end) */
		}
		error = qrtr_out_packet(QRTR_HELLO, QRTR_CONTROL, header->from_node, QRTR_CONTROL, &hello, sizeof(hello));
		printf("mss: qrtr: HELLO FROM NODE %u; ours to it: %s\n", header->from_node, error == 0 ? "SENT" : "NOT SENT");
		break;
	case QRTR_NEW_SERVER:
		lck_mtx_lock(&qrtr_lock);
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				found = (int)i;
				break;
			}
			if (!qrtr_servers[i].there && found < 0) {
				found = (int)i;
			}
		}
		if (found >= 0) {
			qrtr_servers[found].there = true;
			qrtr_servers[found].service = control->words[0];
			qrtr_servers[found].instance = control->words[1];
			qrtr_servers[found].node = control->words[2];
			qrtr_servers[found].port = control->words[3];
		}
		lck_mtx_unlock(&qrtr_lock);
		printf("mss: qrtr: a service: %u (%#x), version %u, instance %u, at node %u port %u\n", control->words[0],
		    control->words[0], control->words[1] & 0xff, control->words[1] >> 8, control->words[2], control->words[3]);
		qrtr_tell(control->words[0], control->words[1], control->words[2], control->words[3], true);
		break;
	case QRTR_DEL_SERVER:
	case QRTR_BYE:
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			bool gone;

			lck_mtx_lock(&qrtr_lock);
			gone = qrtr_servers[i].there && (control->command == QRTR_BYE ? qrtr_servers[i].node == header->from_node :
			    qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]);
			qrtr_servers[i].there = qrtr_servers[i].there && !gone;
			lck_mtx_unlock(&qrtr_lock);
			if (gone) {
				printf("mss: qrtr: service %u at node %u port %u has gone\n", qrtr_servers[i].service, qrtr_servers[i].node,
				    qrtr_servers[i].port);
				qrtr_tell(qrtr_servers[i].service, qrtr_servers[i].instance, qrtr_servers[i].node, qrtr_servers[i].port, false);
			}
		}
		break;
	default:
		break;                                  /* (a client's going, and what else there is: nothing here wants it) */
	}
}

/* A message from the channel: one datagram. From the link's thread. */
static void
qrtr_input(const uint8_t *data, uint32_t length)
{
	struct qrtr_header header;
	redwood_qrtr_receive_t receive = NULL;
	void *context = NULL;
	uint32_t at;

	if (length >= sizeof(header) && data[0] == 1) {
		memcpy(&header, data, sizeof(header));
		at = sizeof(header);
	} else if (length >= 16 && data[0] == 3) {
		/* version 2: version, type, flags, a length of options; the size; then four numbers of 16 bits */
		bzero(&header, sizeof(header));
		header.type = data[1];
		header.confirm = data[2] & 1;
		memcpy(&header.size, data + 4, 4);
		header.from_node = (uint32_t)(data[8] | data[9] << 8);
		header.from_port = (uint32_t)(data[10] | data[11] << 8);
		header.to_node = (uint32_t)(data[12] | data[13] << 8);
		header.to_port = (uint32_t)(data[14] | data[15] << 8);
		header.from_port = header.from_port == 0xfffe ? QRTR_CONTROL : header.from_port;
		header.to_port = header.to_port == 0xfffe ? QRTR_CONTROL : header.to_port;
		at = 16 + data[3];
	} else {
		printf("mss: qrtr: %u bytes that are no datagram (they begin %#x)\n", length, length ? data[0] : 0);
		return;
	}
	if (header.size == 0 || at + header.size > length) {
		printf("mss: qrtr: a datagram of %u bytes in a message of %u: dropped\n", header.size, length);
		return;
	}
	qrtr_in++;
	if (!qrtr_peer_known) {
		qrtr_peer = header.from_node;
		qrtr_peer_known = true;
	}
	if (header.type == QRTR_RESUME_TX) {
		return;                                 /* (we never ask for one: GLINK's intents hold us back already) */
	}
	if (header.confirm) {
		/* the sender counts what it has sent to this port, and waits for this before it sends many more */
		struct qrtr_control resume = { .command = QRTR_RESUME_TX, .words = { header.to_node, header.to_port } };

		qrtr_out_packet(QRTR_RESUME_TX, header.to_port, header.from_node, header.from_port, &resume, sizeof(resume));
	}
	if (header.to_port == QRTR_CONTROL) {
		struct qrtr_control control = { 0 };

		memcpy(&control, data + at, header.size < sizeof(control) ? header.size : sizeof(control));
		qrtr_control(&header, &control);
		return;
	}
	lck_mtx_lock(&qrtr_lock);
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		if (qrtr_ports[p].number == header.to_port && header.type == QRTR_DATA) {
			receive = qrtr_ports[p].receive;
			context = qrtr_ports[p].context;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	if (receive != NULL) {
		receive(context, header.from_node, header.from_port, data + at, header.size);
	}
}

/* Lookups made since the last look are told of the services that were there before them. From the link's thread. */
static void
qrtr_fresh(void)
{
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		for (unsigned l = 0; l < 4; l++) {
			redwood_qrtr_server_t server;
			uint32_t service, instance;
			void *context;

			lck_mtx_lock(&qrtr_lock);
			if (qrtr_ports[p].number == 0 || !qrtr_ports[p].lookups[l].fresh) {
				lck_mtx_unlock(&qrtr_lock);
				continue;
			}
			qrtr_ports[p].lookups[l].fresh = false;
			server = qrtr_ports[p].server;
			service = qrtr_ports[p].lookups[l].service;
			instance = qrtr_ports[p].lookups[l].instance;
			context = qrtr_ports[p].context;
			lck_mtx_unlock(&qrtr_lock);
			for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]) && server != NULL; i++) {
				if (qrtr_matches(service, instance, i)) {
					server(context, true, qrtr_servers[i].service, qrtr_servers[i].instance & 0xff,
					    qrtr_servers[i].instance >> 8, qrtr_servers[i].node, qrtr_servers[i].port);
				}
			}
		}
	}
}

static struct redwood_qrtr_port *
qrtr_open(redwood_qrtr_receive_t receive, redwood_qrtr_server_t server, void *context)
{
	struct redwood_qrtr_port *port = NULL;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]) && port == NULL; p++) {
		if (qrtr_ports[p].number == 0) {
			port = &qrtr_ports[p];
			bzero(port, sizeof(*port));
			port->number = qrtr_next_port++;
			port->receive = receive;
			port->server = server;
			port->context = context;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return port;
}

static int
qrtr_send(struct redwood_qrtr_port *port, uint32_t node, uint32_t to, const void *data, size_t length)
{
	if (port == NULL || port->number == 0 || length > REDWOOD_QRTR_MAX) {
		return EINVAL;
	}
	return qrtr_out_packet(QRTR_DATA, port->number, node, to, data, (uint32_t)length);
}

static int
qrtr_lookup(struct redwood_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	int error = ENOSPC;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned l = 0; l < 4 && error != 0 && port != NULL && port->number != 0; l++) {
		if (!port->lookups[l].asked) {
			port->lookups[l].service = service;
			port->lookups[l].instance = instance != 0 ? (version & 0xff) | instance << 8 : 0;
			port->lookups[l].asked = true;
			port->lookups[l].fresh = true;
			error = 0;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return error;
}

static void
qrtr_close(struct redwood_qrtr_port *port)
{
	struct qrtr_control gone = { .command = QRTR_DEL_CLIENT, .words = { QRTR_US, port ? port->number : 0 } };
	bool outside = current_thread() != link_thread_is;

	if (port == NULL || port->number == 0) {
		return;
	}
	if (outside) {
		lck_mtx_lock(&qrtr_calling);            /* (a callback of the port's that is running ends first) */
	}
	lck_mtx_lock(&qrtr_lock);
	port->number = 0;
	lck_mtx_unlock(&qrtr_lock);
	if (outside) {
		lck_mtx_unlock(&qrtr_calling);
	}
	if (qrtr_peer_known) {
		qrtr_out_packet(QRTR_DEL_CLIENT, gone.words[1], qrtr_peer, QRTR_CONTROL, &gone, sizeof(gone));
	}
}

/*
 * A service of ours at this port, told to the modem's name service ("new server" to its control
 * port, as Linux's ns.c tells a node that has said hello). 0, or an errno: ENOTCONN before the
 * modem has spoken.
 */
static int
qrtr_serve(struct redwood_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	struct qrtr_control server = {
		.command = QRTR_NEW_SERVER, .words = { service, (version & 0xff) | instance << 8, QRTR_US, port ? port->number : 0 },
	};

	if (port == NULL || port->number == 0) {
		return EINVAL;
	}
	if (!qrtr_peer_known) {
		return ENOTCONN;
	}
	return qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, qrtr_peer, QRTR_CONTROL, &server, sizeof(server));
}

static const struct redwood_qrtr_ops qrtr_ops = { .open = qrtr_open, .send = qrtr_send, .lookup = qrtr_lookup, .close = qrtr_close };

/* ---- the link's thread -------------------------------------------------------------------------------- */

static volatile bool link_wanted, link_runs;
/*
 * Until the modem may have said something. Nothing tells us today, so: a millisecond. This is the
 * one function to change when the doorbells' interrupt can be taken (the end of this file): it
 * then blocks until the handler has seen a ring of the modem's.
 */
static void
link_wait(void)
{
	delay(1000);
}

/* Everything the modem says once it runs: its SMP2P item, its queue, and from that the datagrams, handed on from here. */
static void
link_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct smp2p words, last;
	struct glink_packet *packet;
	bool gave_up = false;

	bzero(&last, sizeof(last));
	modem_words(&last);
	while (link_wanted) {
		link_wait();
		doorbells();
		if (modem_words(&words) && memcmp(&words, &last, sizeof(words)) != 0) {
			last = words;
			modem_listen(&words);
			if ((slave_kernel(&words) & MODEM_FATAL) && !gave_up) {
				gave_up = true;
				printf("mss: link: THE MODEM SAYS IT HAS GIVEN UP (\"slave-kernel\" = %#x)\n", slave_kernel(&words));
				modem_last_words();
			}
		}
		for (;;) {
			lck_mtx_lock(&glink_lock);
			glink_pump();
			packet = glink.first;
			if (packet != NULL) {
				glink.first = packet->next;
				glink.last = glink.first == NULL ? NULL : glink.last;
			}
			lck_mtx_unlock(&glink_lock);
			if (packet == NULL) {
				break;
			}
			lck_mtx_lock(&qrtr_calling);
			qrtr_input(packet->data, packet->length);
			lck_mtx_unlock(&qrtr_calling);
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
		lck_mtx_lock(&qrtr_calling);
		qrtr_fresh();
		lck_mtx_unlock(&qrtr_calling);
	}
	link_runs = false;
	thread_terminate(current_thread());
}

/* The link given up, before the modem is stopped: Linux too takes GLINK down first and tells the modem nothing of it. */
static void
link_stop(void)
{
	struct glink_packet *packet;

	if (link_runs) {
		link_wanted = false;
		for (unsigned ms = 0; ms < 2000 && link_runs; ms++) {
			IOSleep(1);
		}
		printf("mss: link: %s; GLINK said %u and heard %u messages, QRTR sent %u and received %u datagrams; doorbells heard: "
		    "GLINK %u, SMP2P %u, the mailbox %u, others %u\n", link_runs ? "THE THREAD DOES NOT END" : "down", glink.said,
		    glink.heard, qrtr_out, qrtr_in, doorbell.glink, doorbell.smp2p, doorbell.mailbox, doorbell.other);
	}
	lck_mtx_lock(&glink_lock);
	glink.up = glink.open = false;
	while ((packet = glink.first) != NULL) {
		glink.first = packet->next;
		kfree_data(packet, sizeof(*packet) + packet->length);
	}
	glink.last = NULL;
	lck_mtx_unlock(&glink_lock);
	lck_mtx_lock(&qrtr_lock);
	bzero(qrtr_servers, sizeof(qrtr_servers));
	qrtr_peer = 0;
	qrtr_peer_known = false;
	lck_mtx_unlock(&qrtr_lock);
}

/* ---- step 12: GLINK's queues, looked at --------------------------------------------------------------- */

static bool
step_queues(void)
{
	struct glink_message message;
	uint32_t head, tail;

	announce("find GLINK's descriptor and the two queues in the shared memory, and read what the modem has put into its own "
	    "(nothing is taken out)");
	if (!glink_find()) {
		printf("mss: %u: GLINK's items are not all there, or not of a shape this knows (descriptor %#x, ours %#x of %u bytes, "
		    "the modem's %#x of %u)\n", step, glink.descriptor, glink.ours, glink.ours_size, glink.theirs, glink.theirs_size);
		return false;
	}
	head = smem_word(glink.descriptor + GLINK_THEIR_HEAD);
	tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);
	printf("mss: %u: our queue: %u bytes at %#lx, written to %#x, read by the modem to %#x; the modem's: %u bytes at %#lx, written "
	    "to %#x, read by us to %#x\n", step, glink.ours_size, SMEM_PHYS + glink.ours, smem_word(glink.descriptor + GLINK_OUR_HEAD),
	    smem_word(glink.descriptor + GLINK_OUR_TAIL), glink.theirs_size, SMEM_PHYS + glink.theirs, head, tail);
	for (uint32_t at = 0, shown = 0; at + sizeof(message) <= head && head < glink.theirs_size && tail == 0 && shown < 8; shown++) {
		char name[33] = "";
		uint32_t length = sizeof(message);

		glink_peek(&message, at, sizeof(message));
		if (message.command == GLINK_OPEN) {
			glink_peek(name, at + 8, message.param2 < 32 ? message.param2 : 32);
			length += message.param2;
		}
		printf("mss: %u:   in the modem's queue at %#x: %s %u %#x %s\n", step, at, glink_name(message.command), message.param1,
		    message.param2, name);
		if (message.command != GLINK_VERSION && message.command != GLINK_OPEN) {
			break;                                  /* (how long the others are, the reading below knows) */
		}
		at += (length + 7) & ~7u;
	}
	return true;
}

/* ---- step 13: GLINK, spoken ---------------------------------------------------------------------------- */

static bool
step_link(void)
{
	thread_t thread;
	uint64_t began;

	if (!modem_started || glink.descriptor == 0) {
		return false;
	}
	announce("take up GLINK with the modem: our version into our queue and its doorbell, then whatever its messages call for "
	    "(writes: our queue, our two marks in the descriptor, the doorbell); a thread of ours listens from here on, "
	    "every millisecond");
	lck_mtx_lock(&glink_lock);
	glink.features = GLINK_INTENT_REUSE;
	glink.agreed = glink.asked = glink.acknowledged = glink.open = glink.broken = false;
	glink.their_channel = glink.our_intent_count = glink.said = glink.heard = 0;
	bzero(glink.their_intents, sizeof(glink.their_intents));
	glink.trace = 60;
	glink.up = true;
	qrtr_hellos = qrtr_in = qrtr_out = 0;
	doorbell.since = began = now_us();
	glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
	lck_mtx_unlock(&glink_lock);
	link_wanted = link_runs = true;
	if (kernel_thread_start(link_thread, NULL, &thread) != KERN_SUCCESS) {
		link_wanted = link_runs = false;
		printf("mss: %u: no thread\n", step);
		return false;
	}
	link_thread_is = thread;
	thread_deallocate(thread);
	for (unsigned ms = 0; ms < 5000 && !glink.open && !glink.broken; ms++) {
		IOSleep(1);
	}
	IOSleep(50);                                   /* (for the intents each side tells the other of, to be in the log) */
	printf("mss: %u: %llu ms after our version: versions %s (features %#x); the channel \"%s\" is %s\n", step,
	    (now_us() - began) / 1000, glink.agreed ? "agreed" : "NOT AGREED", glink.features, GLINK_CHANNEL, glink.open ?
	    "OPEN" : "NOT OPEN");
	return glink.open;
}

/* ---- step 14: QRTR ------------------------------------------------------------------------------------- */

#define SERVICE_WLFW            0x45            /* the Wi-Fi firmware's service, the one the radio's driver talks to */

static uint32_t wlfw_node, wlfw_port;
static volatile uint32_t wlfw_answer;           /* bytes of an answer received; 0: none */
static uint8_t wlfw_answer_bytes[64];

static void
wlfw_service(__unused void *context, bool arrived, __unused uint32_t service, __unused uint32_t version, __unused uint32_t instance,
    uint32_t node, uint32_t port)
{
	printf("mss: qrtr: THE WI-FI FIRMWARE'S SERVICE (%#x) %s node %u port %u\n", SERVICE_WLFW, arrived ? "IS AT" : "has left",
	    node, port);
	wlfw_node = arrived ? node : 0;
	wlfw_port = arrived ? port : 0;
}

static void
wlfw_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	printf("mss: qrtr: a datagram of %lu bytes from node %u port %u\n", (unsigned long)length, node, port);
	memcpy(wlfw_answer_bytes, data, length < sizeof(wlfw_answer_bytes) ? length : sizeof(wlfw_answer_bytes));
	wlfw_answer = (uint32_t)length;
}

static bool
step_qrtr(void)
{
	struct redwood_qrtr_port *port;
	uint32_t ask = 0, seconds = 3, services = 0;

	if (!glink.open) {
		return false;
	}
	PE_parse_boot_argn("mss_listen", &seconds, sizeof(seconds));
	announce("listen for QRTR on the channel: the modem's hello is answered with ours, its services are written down; and a "
	    "port of ours looks the Wi-Fi firmware's service up, as the radio's driver will");
	port = qrtr_open(wlfw_receive, wlfw_service, NULL);
	if (port == NULL || qrtr_lookup(port, SERVICE_WLFW, 1, 0) != 0) {
		printf("mss: %u: no port\n", step);
		return false;
	}
	for (unsigned ms = 0; ms < seconds * 1000 && !(wlfw_port != 0 && ms >= 500); ms++) {
		IOSleep(1);
	}
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	printf("mss: %u: QRTR: the modem is node %u; hellos from it %u; datagrams received %u, sent %u; services it has told of: "
	    "%u; the Wi-Fi firmware's service: %s\n", step, qrtr_peer, qrtr_hellos, qrtr_in, qrtr_out, services, wlfw_port != 0 ?
	    "THERE" : "NOT THERE");
	if (wlfw_port != 0 && PE_parse_boot_argn("mss_ask", &ask, sizeof(ask)) && ask) {
		/*
		 * A question the service answers without having been told anything first? A QMI request is
		 * seven bytes before its content: 0 for "request", a number of ours for the exchange, the
		 * message's number, the content's length. 0x24 is "capabilities" and has no content.
		 */
		static const uint8_t question[7] = { 0, 0x01, 0x00, 0x24, 0x00, 0x00, 0x00 };
		int error;

		announce("ask the Wi-Fi firmware's service for its capabilities (QMI message 0x24, seven bytes) and wait a second "
		    "for an answer");
		wlfw_answer = 0;
		error = qrtr_send(port, wlfw_node, wlfw_port, question, sizeof(question));
		for (unsigned ms = 0; ms < 1000 && error == 0 && wlfw_answer == 0; ms++) {
			IOSleep(1);
		}
		if (error != 0 || wlfw_answer == 0) {
			printf("mss: %u: %s\n", step, error != 0 ? "THE QUESTION COULD NOT BE SENT" : "no answer within a second");
		} else {
			char text[3 * 48 + 1] = "";

			for (uint32_t i = 0; i < wlfw_answer && i < 48; i++) {
				snprintf(text + 3 * i, 4, " %02x", wlfw_answer_bytes[i]);
			}
			printf("mss: %u: THE SERVICE ANSWERS: %u bytes, beginning%s\n", step, wlfw_answer, text);
		}
	}
	qrtr_close(port);
	return qrtr_hellos != 0 && qrtr_out != 0;
}

/* ---- step 11: services of ours for the modem --------------------------------------------------------- */

/*
 * What Linux runs beside the modem on these SoCs in user space, and without which its Wi-Fi part
 * (the "wlan_pd" protection domain, whose service is the wlfw above) does not come up:
 *   pd-mapper   the service locator (0x40): "which domain has service X" -- from the modem's own
 *               tables, modemr.jsn and modemuw.jsn in the partition "modem" (written in below)
 *   rmtfs       the modem's file system on our storage (0x0e): its EFS, modemst1/2 and fsg
 *   tqftpserv   files by TFTP over QRTR (0x1000)
 * This step tells the modem of all three and answers the locator; what is asked of the other two
 * is only written down (the request, the file's name), for the step that will answer them. For
 * mss_serve=S seconds (60), or until the Wi-Fi firmware's service appears and 2 more have passed.
 */
#define SERVICE_RMTFS           0x0e
#define SERVICE_LOCATOR         0x40
#define SERVICE_TFTP            0x1000
#define LOCATOR_DOMAIN_LIST     0x21
#define LOCATOR_PFR             0x24            /* a domain's failure report (pd-mapper: SERVREG_LOC_PFR) */

static struct redwood_qrtr_port *serve_locator, *serve_rmtfs, *serve_tftp;
static uint32_t serve_asked[3];
static unsigned serve_said;

/* this phone's two domains (modemr.jsn, modemuw.jsn) and their services */
static const struct {
	const char *domain;
	uint32_t instance;
	const char *services[3];
} serve_domains[] = {
	{ "msm/modem/root_pd", 180, { "tms/servreg", "gps/gps_service", NULL } },
	{ "msm/modem/wlan_pd", 180, { "kernel/elf_loader", "tms/servreg", "wlan/fw" } },
};

/* The first bytes of a datagram, for the log (up to 48). */
static void
serve_bytes(const char *what, const uint8_t *data, size_t length)
{
	char text[3 * 48 + 1] = "";

	for (size_t i = 0; i < length && i < 48; i++) {
		snprintf(text + 3 * i, 4, " %02x", data[i]);
	}
	printf("mss: 11: %s: %lu bytes:%s\n", what, (unsigned long)length, text);
}

/* A QMI message's TLV of that type: its value and length, or NULL. */
static const uint8_t *
qmi_tlv(const uint8_t *message, size_t length, uint8_t type, uint32_t *size)
{
	for (size_t at = 7; at + 3 <= length;) {
		uint32_t n = message[at + 1] | (uint32_t)message[at + 2] << 8;

		if (at + 3 + n > length) {
			break;
		}
		if (message[at] == type) {
			*size = n;
			return message + at + 3;
		}
		at += 3 + n;
	}
	return NULL;
}

static uint32_t qmi_answer(uint8_t *a, const uint8_t *q, bool success);

/* GET_DOMAIN_LIST, answered as pd-mapper does: the domains that have the service named. */
static void
locator_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *q = data, *value;
	uint8_t a[256];
	char name[65] = "";
	uint32_t size = 0, at, count = 0, txn;
	int error;

	serve_asked[0]++;
	if (length >= 7 && q[0] == 0 && (q[3] | q[4] << 8) == LOCATOR_PFR) {
		/* a protection domain's failure report: its name (TLV 1) and the reason (TLV 2), both text */
		char reason[200] = "";

		if ((value = qmi_tlv(q, length, 0x01, &size)) != NULL) {
			memcpy(name, value, size < 64 ? size : 64);
		}
		if ((value = qmi_tlv(q, length, 0x02, &size)) != NULL) {
			for (uint32_t i = 0; i < size && i + 1 < sizeof(reason); i++) {
				reason[i] = value[i] >= 32 && value[i] < 127 ? (char)value[i] : '?';
			}
		}
		at = qmi_answer(a, q, true);
		a[5] = (uint8_t)(at - 7);
		a[6] = 0;
		error = qrtr_send(serve_locator, node, port, a, at);
		printf("mss: 11: A DOMAIN HAS FAILED: \"%s\": \"%s\" (answer %s)\n", name, reason, error == 0 ? "sent" : "NOT SENT");
		return;
	}
	if (length < 7 || q[0] != 0 || (q[3] | q[4] << 8) != LOCATOR_DOMAIN_LIST) {
		if (serve_said++ < 60) {
			serve_bytes("the locator is sent something else", q, length);
		}
		return;
	}
	txn = q[1] | (uint32_t)q[2] << 8;
	if ((value = qmi_tlv(q, length, 0x01, &size)) != NULL) {
		memcpy(name, value, size < 64 ? size : 64);
	}
	/* the answer: result, total, revision, then the list (a count, and per domain: its name with its length, instance, data) */
	at = 7;
	a[at++] = 0x02; a[at++] = 4; a[at++] = 0;
	a[at++] = 0; a[at++] = 0; a[at++] = 0; a[at++] = 0;
	a[at++] = 0x10; a[at++] = 2; a[at++] = 0;
	uint32_t total_at = at;
	a[at++] = 0; a[at++] = 0;
	a[at++] = 0x11; a[at++] = 2; a[at++] = 0; a[at++] = 1; a[at++] = 0;
	uint32_t list_at = at;
	a[at++] = 0x12; a[at++] = 0; a[at++] = 0; a[at++] = 0;
	for (unsigned d = 0; d < sizeof(serve_domains) / sizeof(serve_domains[0]); d++) {
		bool has = false;

		for (unsigned s = 0; s < 3 && serve_domains[d].services[s] != NULL; s++) {
			has = has || strcmp(serve_domains[d].services[s], name) == 0;
		}
		if (has) {
			uint32_t n = (uint32_t)strlen(serve_domains[d].domain);

			a[at++] = (uint8_t)n;
			memcpy(a + at, serve_domains[d].domain, n);
			at += n;
			a[at++] = (uint8_t)serve_domains[d].instance;
			a[at++] = (uint8_t)(serve_domains[d].instance >> 8);
			a[at++] = 0; a[at++] = 0;
			a[at++] = 0;                    /* service_data_valid */
			a[at++] = 0; a[at++] = 0; a[at++] = 0; a[at++] = 0;
			count++;
		}
	}
	a[total_at] = (uint8_t)count;
	a[list_at + 3] = (uint8_t)count;
	if (count == 0) {
		at = list_at;                           /* (no list at all, as pd-mapper leaves it out) */
	} else {
		a[list_at + 1] = (uint8_t)(at - list_at - 3);
		a[list_at + 2] = (uint8_t)((at - list_at - 3) >> 8);
	}
	a[0] = 2;                                       /* a response */
	a[1] = (uint8_t)txn; a[2] = (uint8_t)(txn >> 8);
	a[3] = LOCATOR_DOMAIN_LIST; a[4] = 0;
	a[5] = (uint8_t)(at - 7); a[6] = (uint8_t)((at - 7) >> 8);
	error = qrtr_send(serve_locator, node, port, a, at);
	if (serve_said++ < 60) {
		printf("mss: 11: THE LOCATOR IS ASKED where \"%s\" is (node %u port %u, transaction %u): %u domain%s, answer %s\n",
		    name, node, port, txn, count, count == 1 ? "" : "s", error == 0 ? "sent" : "NOT SENT");
	}
}

/*
 * RMTFS, as linux-msm's rmtfs does it: the modem opens its file systems by name ("/boot/modem_fs1"
 * and so on, partitions of ours), asks for a buffer -- memory that both of us reach, which TrustZone
 * has been told to let the modem into (the dt's qcom,rmtfs_sharedmem: 2 MB, client 1) -- and then
 * has sectors copied between a partition and places in that buffer (RW_IOVEC).
 * What the modem writes stays in memory: each partition is read whole at its opening, and is never
 * written back -- the phone's EFS (its IMEI, its calibration) is not ours to change. Nothing of
 * what is in them goes to the log.
 */
#define RMTFS_OPEN              1
#define RMTFS_CLOSE             2
#define RMTFS_RW_IOVEC          3
#define RMTFS_ALLOC_BUFF        4
#define RMTFS_GET_DEV_ERROR     5
#define RMTFS_MEMORY_SIZE       0x200000u
#define RMTFS_SECTOR            512
#define SCM_MP_ASSIGN           SCM_CALL(0x0c, 0x16)
#define SCM_ASSIGN_ARGUMENTS    (7 | 1 << 4 | 1 << 8 | 1 << 12)        /* RO, VAL, RO, VAL, RO, VAL, VAL */
#define VMID_HLOS               3
#define VMID_MSS_MSA            15
#define SCM_PERM_RW             6

static struct {
	const char *path, *partition;
	uint8_t *bytes;                         /* the partition, as the modem has it */
	uint64_t size;
	uint32_t reads, writes;
} rmtfs_files[] = {
	{ "/boot/modem_fs1", "modemst1" }, { "/boot/modem_fs2", "modemst2" }, { "/boot/modem_fsg", "fsg" },
	{ "/boot/modem_fsc", "fsc" },
};
static vm_offset_t rmtfs_memory;
static uint64_t rmtfs_memory_phys;
static bool rmtfs_assigned;

static void
to_memory_range(vm_offset_t start, vm_size_t size)
{
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__builtin_arm_dsb(0xf);
}

/*
 * The buffer: 2 MB in one piece, then TrustZone asked to give it to us and the modem both
 * (qcom_scm_assign_mem: from HLOS alone to HLOS and MSS_MSA, read and write), as Linux's rmtfs_mem
 * does for qcom,vmid. The call's tables in a page of their own, out of the cache before it.
 */
static bool
rmtfs_memory_setup(void)
{
	vm_offset_t page = 0;
	uint64_t *ext, answer = 0, base;
	int32_t result;

	if (kmem_alloc_contig(kernel_map, &rmtfs_memory, RMTFS_MEMORY_SIZE, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("mss: %u: RMTFS: no memory for its buffer\n", step);
		return false;
	}
	rmtfs_memory_phys = kvtophys(rmtfs_memory);
	to_memory_range(rmtfs_memory, RMTFS_MEMORY_SIZE);
	base = kvtophys(page);
	{
		uint64_t *region = (uint64_t *)page;                    /* at 0: { address, size } */
		uint32_t *source = (uint32_t *)(page + 64);             /* at 64: the owners now */
		uint32_t *destination = (uint32_t *)(page + 128);       /* at 128: { vmid, perm, ctx (8), ctx size, 0 } each */

		region[0] = rmtfs_memory_phys;
		region[1] = RMTFS_MEMORY_SIZE;
		source[0] = VMID_HLOS;
		destination[0] = VMID_HLOS;
		destination[1] = SCM_PERM_RW;
		destination[6] = VMID_MSS_MSA;
		destination[7] = SCM_PERM_RW;
		ext = (uint64_t *)(page + 256);                         /* at 256: the arguments after the third */
		ext[0] = 4;                                             /* the source list's size */
		ext[1] = base + 128;
		ext[2] = 2 * 24;
		ext[3] = 0;
	}
	to_memory_range(page, PAGE_SIZE);
	result = scm_ext("to give the RMTFS buffer to the modem as well (MP_ASSIGN: HLOS -> HLOS + MSS_MSA)", SCM_MP_ASSIGN,
	    SCM_ASSIGN_ARGUMENTS, base, 16, base + 64, base + 256, &answer);
	rmtfs_assigned = result == 0;
	printf("mss: %u: RMTFS: its buffer at %#llx, 2 MB: %s (%d)\n", step, rmtfs_memory_phys,
	    rmtfs_assigned ? "THE MODEM MAY USE IT" : "NOT GIVEN TO THE MODEM", result);
	return true;
}

/* The start of an answer: QMI's response header and the result TLV. Its length so far. */
static uint32_t
qmi_answer(uint8_t *a, const uint8_t *q, bool success)
{
	a[0] = 2;
	a[1] = q[1]; a[2] = q[2];
	a[3] = q[3]; a[4] = q[4];
	a[7] = 0x02; a[8] = 4; a[9] = 0;
	a[10] = success ? 0 : 1; a[11] = 0;
	a[12] = success ? 0 : 3; a[13] = 0;             /* (QMI_ERR_INTERNAL) */
	return 14;
}

static uint32_t
qmi_u32(const uint8_t *message, size_t length, uint8_t type, bool *there)
{
	uint32_t size = 0;
	const uint8_t *v = qmi_tlv(message, length, type, &size);

	*there = v != NULL && size >= 4;
	return *there ? v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24 : 0;
}

static void
rmtfs_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *q = data, *value;
	uint8_t a[64];
	uint32_t at, message, size = 0, caller;
	bool ok = false, there = false;
	const char *said = "";

	serve_asked[1]++;
	if (length < 7 || q[0] != 0) {
		return;
	}
	message = q[3] | q[4] << 8;
	caller = qmi_u32(q, length, 0x01, &there);
	switch (message) {
	case RMTFS_OPEN: {
		char path[64] = "";

		if ((value = qmi_tlv(q, length, 0x01, &size)) != NULL) {
			memcpy(path, value, size < 63 ? size : 63);
		}
		caller = 0;
		for (unsigned f = 0; f < sizeof(rmtfs_files) / sizeof(rmtfs_files[0]); f++) {
			if (strcmp(path, rmtfs_files[f].path) != 0) {
				continue;
			}
			if (rmtfs_files[f].bytes == NULL && redwood_ufs_read_named(rmtfs_files[f].partition, 0, NULL, 0,
			    &rmtfs_files[f].size) == 0 && rmtfs_files[f].size != 0 && rmtfs_files[f].size <= 16u << 20) {
				rmtfs_files[f].bytes = kalloc_data(rmtfs_files[f].size, Z_WAITOK | Z_ZERO);
				if (rmtfs_files[f].bytes != NULL && redwood_ufs_read_named(rmtfs_files[f].partition, 0,
				    rmtfs_files[f].bytes, rmtfs_files[f].size, NULL) != 0) {
					kfree_data(rmtfs_files[f].bytes, rmtfs_files[f].size);
					rmtfs_files[f].bytes = NULL;
				}
			}
			caller = rmtfs_files[f].bytes != NULL ? f + 1 : 0;
		}
		ok = caller != 0;
		at = qmi_answer(a, q, ok);
		if (ok) {
			a[at++] = 0x10; a[at++] = 4; a[at++] = 0;
			a[at++] = (uint8_t)caller; a[at++] = 0; a[at++] = 0; a[at++] = 0;
		}
		if (serve_said++ < 60) {
			printf("mss: 11: RMTFS: open \"%s\": %s %s (%llu KB), caller %u\n", path, ok ? "the partition" : "NO SUCH FILE:",
			    caller ? rmtfs_files[caller - 1].partition : "", caller ? rmtfs_files[caller - 1].size >> 10 : 0, caller);
		}
		break;
	}
	case RMTFS_ALLOC_BUFF:
		ok = rmtfs_memory != 0;
		at = qmi_answer(a, q, ok);
		if (ok) {
			a[at++] = 0x10; a[at++] = 8; a[at++] = 0;
			for (int i = 0; i < 8; i++) {
				a[at++] = (uint8_t)(rmtfs_memory_phys >> (8 * i));
			}
		}
		said = "a buffer";
		break;
	case RMTFS_RW_IOVEC: {
		uint32_t direction = 0, count = 0, entries_at = 0, done = 0;
		const uint8_t *iovec;

		if ((value = qmi_tlv(q, length, 0x02, &size)) != NULL && size >= 1) {
			direction = value[0];
		}
		iovec = qmi_tlv(q, length, 0x03, &size);
		/* the list: a count of one byte (or four), then { sector, offset in the buffer, sectors } each */
		if (iovec != NULL && size >= 1 && (size - 1) % 12 == 0) {
			count = iovec[0];
			entries_at = 1;
		} else if (iovec != NULL && size >= 4 && (size - 4) % 12 == 0) {
			count = iovec[0] | iovec[1] << 8;
			entries_at = 4;
		}
		ok = there && caller >= 1 && caller <= sizeof(rmtfs_files) / sizeof(rmtfs_files[0]) &&
		    rmtfs_files[caller - 1].bytes != NULL && count != 0;
		for (uint32_t e = 0; ok && e < count; e++) {
			const uint8_t *v = iovec + entries_at + 12 * e;
			uint64_t sector = v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24;
			uint64_t offset = v[4] | v[5] << 8 | v[6] << 16 | (uint32_t)v[7] << 24;
			uint64_t bytes = (uint64_t)(v[8] | v[9] << 8 | v[10] << 16 | (uint32_t)v[11] << 24) * RMTFS_SECTOR;
			uint8_t *file = rmtfs_files[caller - 1].bytes;

			/* (the "offset" is the place's physical address: rmtfs's rmtfs_mem_ptr takes the buffer's start off it) */
			if (offset >= rmtfs_memory_phys) {
				offset -= rmtfs_memory_phys;
			}
			if (sector * RMTFS_SECTOR + bytes > rmtfs_files[caller - 1].size || offset + bytes > RMTFS_MEMORY_SIZE) {
				if (serve_said++ < 60) {
					printf("mss: 11: RMTFS: piece %u out of bounds: sector %llu, place %#llx, %llu bytes (of %llu)\n", e, sector,
					    offset, bytes, rmtfs_files[caller - 1].size);
				}
				ok = false;
				break;
			}
			if (direction == 0) {
				memcpy((void *)(rmtfs_memory + offset), file + sector * RMTFS_SECTOR, bytes);
				to_memory_range(rmtfs_memory + offset, bytes);
			} else {
				to_memory_range(rmtfs_memory + offset, bytes);
				memcpy(file + sector * RMTFS_SECTOR, (const void *)(rmtfs_memory + offset), bytes);
			}
			done += (uint32_t)(bytes / RMTFS_SECTOR);
		}
		if (ok) {
			if (direction == 0) {
				rmtfs_files[caller - 1].reads += done;
			} else {
				rmtfs_files[caller - 1].writes += done;
			}
		}
		at = qmi_answer(a, q, ok);
		if (serve_said++ < 60 && (!ok || serve_asked[1] < 40)) {
			printf("mss: 11: RMTFS: %s %u sectors of caller %u in %u pieces: %s\n", direction ? "write" : "read", done, caller,
			    count, ok ? "done" : "REFUSED");
		}
		break;
	}
	case RMTFS_CLOSE:
		ok = true;
		at = qmi_answer(a, q, ok);
		said = "closed";
		break;
	case RMTFS_GET_DEV_ERROR:
		ok = true;
		at = qmi_answer(a, q, ok);
		a[at++] = 0x10; a[at++] = 1; a[at++] = 0; a[at++] = 0;
		said = "no error";
		break;
	default:
		at = qmi_answer(a, q, false);
		said = "UNKNOWN";
		break;
	}
	a[5] = (uint8_t)(at - 7);
	a[6] = (uint8_t)((at - 7) >> 8);
	int error = qrtr_send(serve_rmtfs, node, port, a, at);
	if (said[0] != 0 && serve_said++ < 60) {
		printf("mss: 11: RMTFS: message %#x (caller %u): %s; answer %s\n", message, caller, said, error == 0 ? "sent" : "NOT SENT");
	}
}

/*
 * TFTP over QRTR, as linux-msm's tqftpserv serves it: a request (RRQ 1, WRQ 2: the name, the mode,
 * options) comes to the service's port; the transfer then goes on between a port of its own here
 * (TFTP's "transfer ID") and the client's, one TFTP packet in each datagram. We answer the options
 * with an OACK; a read then goes out in windows of wsize blocks, each window sent when the client
 * acknowledges the last one (all of it in the callbacks: an ACK in, the next window out); a write
 * is acknowledged block by block.
 * Names: /readonly/firmware/image/X (and any other path's last part, as a fallback) is X in the
 * partition modem's IMAGE; /readwrite/... is a little store in memory, which starts empty with
 * every boot and is never written to the storage.
 */
enum { TFTP_RRQ = 1, TFTP_WRQ = 2, TFTP_DATA = 3, TFTP_ACK = 4, TFTP_ERROR = 5, TFTP_OACK = 6 };
#define TFTP_STORE_FILES        16
#define TFTP_STORE_MAX          (64u << 10)
#define TFTP_TRANSFERS          8

static struct {
	char name[96];
	uint8_t *bytes;
	uint32_t size;
} tftp_store[TFTP_STORE_FILES];

struct tftp_transfer {
	struct redwood_qrtr_port *port;
	uint32_t node, client;                  /* the client's port */
	bool writing, from_store;
	unsigned file;                          /* the store's slot */
	uint32_t first, size;                   /* a file of IMAGE: its first cluster */
	uint32_t seek, total;                   /* where in the file a read begins (option seek), and how much of it (rsize) */
	uint32_t blksize, wsize, acked, last;   /* blocks: acknowledged so far, and the last there is */
	uint32_t written;
	uint8_t *buffer;                        /* blksize + 4 */
};
static struct tftp_transfer tftp_transfers[TFTP_TRANSFERS];
static uint32_t tftp_files_read, tftp_files_written, tftp_blocks_out;

static void tftp_transfer_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length);
static void serve_none(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance, uint32_t node,
    uint32_t port);

static void
tftp_send_error(struct redwood_qrtr_port *from, uint32_t node, uint32_t port, uint16_t code, const char *text)
{
	uint8_t e[64] = { 0, TFTP_ERROR, (uint8_t)(code >> 8), (uint8_t)code };
	size_t n = strlen(text) < sizeof(e) - 5 ? strlen(text) : sizeof(e) - 5;

	memcpy(e + 4, text, n);
	qrtr_send(from, node, port, e, 4 + n + 1);
}

static void
tftp_end(struct tftp_transfer *t)
{
	struct redwood_qrtr_port *port = t->port;

	if (t->buffer != NULL) {
		kfree_data(t->buffer, t->blksize + 4);
	}
	bzero(t, sizeof(*t));
	if (port != NULL) {
		qrtr_close(port);
	}
}

/* Blocks acked + 1 .. acked + wsize of a read (the last one may be short, or empty). */
static void
tftp_send_window(struct tftp_transfer *t)
{
	for (uint32_t block = t->acked + 1; block <= t->last && block <= t->acked + t->wsize; block++) {
		uint32_t within = (block - 1) * t->blksize, at = t->seek + within, n = within < t->total ? t->total - within : 0;

		n = n < t->blksize ? n : t->blksize;
		t->buffer[0] = 0;
		t->buffer[1] = TFTP_DATA;
		t->buffer[2] = (uint8_t)(block >> 8);
		t->buffer[3] = (uint8_t)block;
		if (n != 0) {
			if (t->from_store) {
				memcpy(t->buffer + 4, tftp_store[t->file].bytes + at, n);
			} else if (file_read(t->first, at, t->buffer + 4, n) != 0) {
				tftp_send_error(t->port, t->node, t->client, 0, "read error");
				tftp_end(t);
				return;
			}
		}
		qrtr_send(t->port, t->node, t->client, t->buffer, 4 + n);
		tftp_blocks_out++;
	}
}

static void
tftp_transfer_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct tftp_transfer *t = context;
	const uint8_t *q = data;
	uint32_t opcode, block;

	if (t->port == NULL || length < 4 || node != t->node || port != t->client) {
		return;
	}
	opcode = q[0] << 8 | q[1];
	block = q[2] << 8 | q[3];
	if (opcode == TFTP_ACK && !t->writing) {
		/* (block numbers are 16 bits: a long file's wrap around) */
		uint32_t full = (t->acked & ~0xffffu) | block;

		if (full < t->acked) {
			full += 0x10000;
		}
		/* (as tqftpserv: the client acknowledges a window's last block, and the next window follows from there) */
		if (full >= t->last) {
			tftp_files_read++;
			tftp_end(t);
		} else if (full >= t->acked) {
			t->acked = full;
			tftp_send_window(t);
		}
	} else if (opcode == TFTP_DATA && t->writing) {
		uint32_t n = (uint32_t)length - 4;
		uint8_t ack[4] = { 0, TFTP_ACK, q[2], q[3] };

		if (tftp_store[t->file].bytes != NULL && t->written + n <= TFTP_STORE_MAX) {
			memcpy(tftp_store[t->file].bytes + t->written, q + 4, n);
			t->written += n;
			tftp_store[t->file].size = t->written;
		}
		qrtr_send(t->port, t->node, t->client, ack, sizeof(ack));
		if (n < t->blksize) {
			tftp_files_written++;
			tftp_end(t);
		}
	} else if (opcode == TFTP_ERROR) {
		tftp_end(t);
	}
}

/* One option of an OACK, "name\0value\0", at `at` of a 160-byte packet; where the next goes. */
static uint32_t
tftp_option(uint8_t *packet, uint32_t at, const char *name, uint32_t value)
{
	char number[12];
	size_t n = strlen(name), v;

	snprintf(number, sizeof(number), "%u", value);
	v = strlen(number);
	if (at + n + v + 2 > 160) {
		return at;
	}
	memcpy(packet + at, name, n + 1);
	memcpy(packet + at + n + 1, number, v + 1);
	return at + (uint32_t)(n + v + 2);
}

static void
tftp_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *q = data;
	const char *name, *mode = "", *base;
	uint32_t opcode, blksize = 512, wsize = 1, timeoutms = 0, at, o = 0, seek = 0, rsize = 0;
	bool want_tsize = false, want_blksize = false, want_wsize = false, want_timeout = false, want_seek = false, want_rsize = false;
	char options[120] = "";
	struct tftp_transfer *t = NULL;
	char text[96];
	uint8_t oack[160];

	serve_asked[2]++;
	if (length < 4 || length > 512) {
		return;
	}
	char request[513];
	memcpy(request, q, length);
	request[length] = 0;
	opcode = q[0] << 8 | q[1];
	name = request + 2;
	at = 2 + (uint32_t)strlen(name) + 1;
	if (at < length) {
		mode = request + at;
		at += (uint32_t)strlen(mode) + 1;
	}
	/* the options: name, value, name, value ... */
	while (at < length) {
		const char *option = request + at, *value;

		at += (uint32_t)strlen(option) + 1;
		if (at >= length) {
			break;
		}
		value = request + at;
		at += (uint32_t)strlen(value) + 1;
		if (strlen(options) + strlen(option) + strlen(value) + 3 < sizeof(options)) {
			strlcat(options, " ", sizeof(options));
			strlcat(options, option, sizeof(options));
			strlcat(options, "=", sizeof(options));
			strlcat(options, value, sizeof(options));
		}
		if (strcmp(option, "seek") == 0) {
			seek = (uint32_t)strtoul(value, NULL, 10);
			want_seek = true;
		} else if (strcmp(option, "rsize") == 0) {
			rsize = (uint32_t)strtoul(value, NULL, 10);
			want_rsize = true;
		} else if (strcmp(option, "blksize") == 0) {
			blksize = (uint32_t)strtoul(value, NULL, 10);
			want_blksize = true;
		} else if (strcmp(option, "wsize") == 0) {
			wsize = (uint32_t)strtoul(value, NULL, 10);
			want_wsize = true;
		} else if (strcmp(option, "timeoutms") == 0) {
			timeoutms = (uint32_t)strtoul(value, NULL, 10);
			want_timeout = true;
		} else if (strcmp(option, "tsize") == 0) {
			want_tsize = true;
		}
	}
	blksize = blksize < 8 ? 8 : blksize > 7680 ? 7680 : blksize;
	wsize = wsize < 1 ? 1 : wsize > 32 ? 32 : wsize;
	for (unsigned i = 0; i < TFTP_TRANSFERS && t == NULL; i++) {
		if (tftp_transfers[i].port == NULL) {
			t = &tftp_transfers[i];
		}
	}
	if (t == NULL || (opcode != TFTP_RRQ && opcode != TFTP_WRQ)) {
		tftp_send_error(serve_tftp, node, port, 0, t == NULL ? "busy" : "not a request");
		return;
	}
	bzero(t, sizeof(*t));
	t->node = node;
	t->client = port;
	t->blksize = blksize;
	t->wsize = wsize;
	t->writing = opcode == TFTP_WRQ;
	base = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
	if (strncmp(name, "/readwrite/", 11) == 0) {
		int free_slot = -1, found = -1;

		for (unsigned i = 0; i < TFTP_STORE_FILES; i++) {
			if (tftp_store[i].bytes != NULL && strcmp(tftp_store[i].name, name) == 0) {
				found = (int)i;
			} else if (tftp_store[i].bytes == NULL && free_slot < 0) {
				free_slot = (int)i;
			}
		}
		if (t->writing && found < 0 && free_slot >= 0) {
			tftp_store[free_slot].bytes = kalloc_data(TFTP_STORE_MAX, Z_WAITOK | Z_ZERO);
			strlcpy(tftp_store[free_slot].name, name, sizeof(tftp_store[free_slot].name));
			found = tftp_store[free_slot].bytes != NULL ? free_slot : -1;
		}
		if (found < 0) {
			snprintf(text, sizeof(text), "%s: NOT FOUND", t->writing ? "no room" : "not written yet");
			goto refuse;
		}
		t->from_store = true;
		t->file = (unsigned)found;
		t->size = tftp_store[found].size;
		if (t->writing) {
			tftp_store[found].size = 0;
		}
	} else if (t->writing || !image_file(base, &t->size, &t->first)) {
		snprintf(text, sizeof(text), "%s", t->writing ? "a write outside /readwrite: REFUSED" : "NOT IN IMAGE");
		goto refuse;
	}
	/* (tqftpserv: a read of rsize bytes from seek, or what there is of them) */
	t->seek = seek <= t->size ? seek : t->size;
	t->total = t->size - t->seek;
	if (want_rsize && rsize < t->total) {
		t->total = rsize;
	}
	t->last = t->writing ? 0 : t->total / blksize + 1;
	t->buffer = kalloc_data(blksize + 4, Z_WAITOK | Z_ZERO);
	t->port = qrtr_open(tftp_transfer_receive, serve_none, t);
	if (t->buffer == NULL || t->port == NULL) {
		snprintf(text, sizeof(text), "no port or memory");
		goto refuse;
	}
	/* the OACK: the options asked for, as we take them (tsize: the file's size) */
	oack[0] = 0;
	oack[1] = TFTP_OACK;
	o = 2;
	if (want_blksize) {
		o = tftp_option(oack, o, "blksize", blksize);
	}
	if (want_timeout) {
		o = tftp_option(oack, o, "timeoutms", timeoutms);
	}
	if (want_tsize) {
		o = tftp_option(oack, o, "tsize", t->size);
	}
	if (want_wsize) {
		o = tftp_option(oack, o, "wsize", wsize);
	}
	if (want_rsize) {
		o = tftp_option(oack, o, "rsize", t->total);
	}
	if (want_seek) {
		o = tftp_option(oack, o, "seek", t->seek);
	}
	if (serve_said++ < 60) {
		printf("mss: 11: TFTP: %s \"%s\" (%s;%s)%s%u of %u bytes from %u, from port %u\n", t->writing ? "write" : "READ", name, mode,
		    options, t->writing ? "" : t->from_store ? ": from memory, " : ": from IMAGE, ", t->writing ? 0 : t->total,
		    t->writing ? 0 : t->size, t->seek, t->port->number);
	}
	if (o > 2) {
		qrtr_send(t->port, node, port, oack, o);        /* (a read waits for the ACK of block 0; a write sends block 1) */
	} else if (t->writing) {
		uint8_t ack[4] = { 0, TFTP_ACK, 0, 0 };

		qrtr_send(t->port, node, port, ack, sizeof(ack));
	} else {
		tftp_send_window(t);
	}
	return;
refuse:
	if (serve_said++ < 60) {
		printf("mss: 11: TFTP: %s \"%s\": %s\n", opcode == TFTP_WRQ ? "write" : "read", name, text);
	}
	tftp_send_error(serve_tftp, node, port, 1, "file not found");
	if (t->buffer != NULL) {
		kfree_data(t->buffer, t->blksize + 4);
	}
	bzero(t, sizeof(*t));
}

static void
serve_none(__unused void *context, __unused bool arrived, __unused uint32_t service, __unused uint32_t version,
    __unused uint32_t instance, __unused uint32_t node, __unused uint32_t port)
{
}

/*
 * A listener of the modem's service registry (its notifier, 0x42 instance 180), for the Wi-Fi
 * domain, as icnss's icnss_pdr_register_notifier and Linux's pdr_interface.c are: REGISTER_LISTENER
 * (0x20: enable, the domain's path) answers with its state; every change of it comes as a
 * STATE_UPDATED indication (0x22: state, path, transaction), which waits for our SET_ACK (0x23:
 * path, transaction) before the domain goes on.
 */
#define SERVICE_SERVREG_NOTIFIER        0x42
#define SERVREG_REGISTER_LISTENER       0x20
#define SERVREG_STATE_UPDATED           0x22
#define SERVREG_SET_ACK                 0x23
#define SERVREG_WLAN_PD                 "msm/modem/wlan_pd"

static struct redwood_qrtr_port *servreg_port;
static uint32_t servreg_node, servreg_at, servreg_txn = 1, servreg_updates;

static const char *
servreg_state(uint32_t state)
{
	return state == 0x1fffffff ? "UP" : state == 0x0fffffff ? "DOWN" : state == 0x2fffffff ? "EARLY DOWN" :
	       state == 0x7fffffff ? "UNINIT" : "?";
}

/* A request of ours to the notifier: the message, and TLVs given as type, length, value one after the other. */
static int
servreg_request(uint32_t message, const uint8_t *tlvs, uint32_t length)
{
	uint8_t r[128];
	uint32_t txn = servreg_txn++;

	if (length + 7 > sizeof(r)) {
		return EMSGSIZE;
	}
	r[0] = 0;
	r[1] = (uint8_t)txn; r[2] = (uint8_t)(txn >> 8);
	r[3] = (uint8_t)message; r[4] = (uint8_t)(message >> 8);
	r[5] = (uint8_t)length; r[6] = (uint8_t)(length >> 8);
	memcpy(r + 7, tlvs, length);
	return qrtr_send(servreg_port, servreg_node, servreg_at, r, 7 + length);
}

/* TLVs of the domain's path (type `type`, no length of its own: a string at the top level) and more after it. */
static uint32_t
servreg_path(uint8_t *at, uint8_t type)
{
	uint32_t n = (uint32_t)strlen(SERVREG_WLAN_PD);

	at[0] = type;
	at[1] = (uint8_t)n;
	at[2] = 0;
	memcpy(at + 3, SERVREG_WLAN_PD, n);
	return 3 + n;
}

static void
servreg_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *q = data, *value;
	uint32_t message, size = 0, state;
	bool there = false;

	if (length < 7 || node != servreg_node || port != servreg_at) {
		return;
	}
	message = q[3] | q[4] << 8;
	if (q[0] == 2 && message == SERVREG_REGISTER_LISTENER) {
		uint32_t result = (value = qmi_tlv(q, length, 0x02, &size)) != NULL && size >= 4 ? (uint32_t)(value[0] | value[1] << 8) : ~0u;

		state = qmi_u32(q, length, 0x10, &there);
		printf("mss: 11: SERVREG: listening to %s: result %u, the domain is %s (%#x)\n", SERVREG_WLAN_PD, result,
		    there ? servreg_state(state) : "(not said)", state);
	} else if (q[0] == 4 && message == SERVREG_STATE_UPDATED) {
		uint8_t tlvs[96];
		uint32_t n, txn = 0;

		state = qmi_u32(q, length, 0x01, &there);
		if ((value = qmi_tlv(q, length, 0x03, &size)) != NULL && size >= 2) {
			txn = value[0] | value[1] << 8;
		}
		n = servreg_path(tlvs, 0x01);
		tlvs[n++] = 0x02; tlvs[n++] = 2; tlvs[n++] = 0;
		tlvs[n++] = (uint8_t)txn; tlvs[n++] = (uint8_t)(txn >> 8);
		servreg_updates++;
		printf("mss: 11: SERVREG: THE WI-FI DOMAIN IS NOW %s (%#x, transaction %u): acknowledged: %d\n", servreg_state(state), state,
		    txn, servreg_request(SERVREG_SET_ACK, tlvs, n));
	} else if (q[0] == 2 && message == SERVREG_SET_ACK) {
		/* (the answer to an acknowledgement: nothing to do) */
	} else {
		serve_bytes("SERVREG: something else", q, length);
	}
}

/* The notifier's place, from what the modem has told of its services; then our registration. */
static void
servreg_listen(void)
{
	uint8_t tlvs[96];
	uint32_t n = 0;
	int error;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]) && servreg_at == 0; i++) {
		if (qrtr_servers[i].there && qrtr_servers[i].service == SERVICE_SERVREG_NOTIFIER &&
		    qrtr_servers[i].instance >> 8 == 180) {
			servreg_node = qrtr_servers[i].node;
			servreg_at = qrtr_servers[i].port;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	if (servreg_at == 0 || (servreg_port = qrtr_open(servreg_receive, serve_none, NULL)) == NULL) {
		printf("mss: %u: SERVREG: %s\n", step, servreg_at == 0 ? "the modem has not told of its notifier (0x42, 180)" : "no port");
		return;
	}
	tlvs[n++] = 0x01; tlvs[n++] = 1; tlvs[n++] = 0; tlvs[n++] = 1;         /* enable */
	n += servreg_path(tlvs + n, 0x02);
	error = servreg_request(SERVREG_REGISTER_LISTENER, tlvs, n);
	printf("mss: %u: SERVREG: asked the notifier (node %u port %u) to tell us of %s: %s\n", step, servreg_node, servreg_at,
	    SERVREG_WLAN_PD, error == 0 ? "sent" : "NOT SENT");
}

static bool
step_serve(void)
{
	struct redwood_qrtr_port *wlfw;
	uint32_t seconds = 60, since = 0;
	int e1, e2, e3;

	if (!glink.open || !qrtr_peer_known) {
		return false;
	}
	PE_parse_boot_argn("mss_serve", &seconds, sizeof(seconds));
	announce("tell the modem of three services of ours -- the locator (0x40, answered from the modem's own domain tables), "
	    "RMTFS (0x0e) and TFTP (0x1000), only written down -- and wait for the Wi-Fi firmware's service");
	serve_locator = qrtr_open(locator_receive, serve_none, NULL);
	serve_rmtfs = qrtr_open(rmtfs_receive, serve_none, NULL);
	serve_tftp = qrtr_open(tftp_receive, serve_none, NULL);
	wlfw = qrtr_open(wlfw_receive, wlfw_service, NULL);
	wlfw_port = 0;
	if (serve_locator == NULL || serve_rmtfs == NULL || serve_tftp == NULL || wlfw == NULL ||
	    qrtr_lookup(wlfw, SERVICE_WLFW, 1, 0) != 0) {
		printf("mss: %u: no ports\n", step);
		return false;
	}
	if (rmtfs_memory == 0 && !rmtfs_memory_setup()) {
		return false;
	}
	/*
	 * The radio chip's own supplies, before the modem starts its Wi-Fi domain: that domain's first
	 * thread talks to the WCN3990, and with the chip unpowered it asserted (willow-mss10:
	 * "Asserted in platform_main.c:platform_main_task:230"). As icnss and ath10k_snoc do, in the dt's
	 * order and at its voltages (qcom,icnss@C800000: vdd-cx-mx L8 at its config's 640 mV, vdd-1.8-xo
	 * L16, vdd-1.3-rfa L17, vdd-3.3-ch0 L23). mss_wlan_power=0 leaves them as they are.
	 */
	{
		static const struct {
			const char *what;
			uint32_t ldo, microvolts;
		} supplies[] = {
			/* (L9 is the chip's I/O, which only its Bluetooth half names in the dt: bt_wcn3990's bt-vdd-io) */
			{ "the radio's 1.8 V I/O (vdd-io)", 9, 1800000 },
			{ "the radio's CX/MX (vdd-cx-mx)", 8, 640000 }, { "the radio's 1.8 V XO (vdd-1.8-xo)", 16, 1800000 },
			{ "the radio's 1.3 V RF (vdd-1.3-rfa)", 17, 1248000 }, { "the radio's 3.3 V (vdd-3.3-ch0)", 23, 3000000 },
		};
		uint32_t power = 1;
		unsigned on = 0;

		PE_parse_boot_argn("mss_wlan_power", &power, sizeof(power));
		for (int i = 0; power && i < 500 && !redwood_rpm_done; i++) {
			IOSleep(10);
		}
		for (unsigned s = 0; power && redwood_rpm_done && s < sizeof(supplies) / sizeof(supplies[0]); s++) {
			on += redwood_rpm_ldo(supplies[s].what, supplies[s].ldo, supplies[s].microvolts);
		}
		printf("mss: %u: the radio chip's supplies: %s\n", step, !power ? "left as they are (mss_wlan_power=0)" :
		    !redwood_rpm_done ? "NO CHANNEL TO THE RPM (gpu=1 or rpm=2 brings it up)" : on == 5 ? "ALL FIVE ON" : "NOT ALL ON");
		IOSleep(5);
	}
	/* a listener of the Wi-Fi domain first, so that it is there before the domain starts (mss_servreg=0: none) */
	{
		uint32_t listen = 1;

		PE_parse_boot_argn("mss_servreg", &listen, sizeof(listen));
		if (listen) {
			servreg_listen();
			IOSleep(20);
		}
	}
	/* versions and instances as their Linux servers give them: pd-mapper 257 / 1, rmtfs 1 / 0, tqftpserv 1 / 0 */
	e1 = qrtr_serve(serve_locator, SERVICE_LOCATOR, 257, 1);
	e2 = qrtr_serve(serve_rmtfs, SERVICE_RMTFS, 1, 0);
	e3 = qrtr_serve(serve_tftp, SERVICE_TFTP, 1, 0);
	printf("mss: %u: told the modem (node %u): the locator at port %u (%d), RMTFS at %u (%d), TFTP at %u (%d)\n", step, qrtr_peer,
	    serve_locator->number, e1, serve_rmtfs->number, e2, serve_tftp->number, e3);
	for (unsigned ms = 0; ms < seconds * 1000 && !(wlfw_port != 0 && since >= 2000); ms++) {
		since = wlfw_port != 0 ? since + 1 : 0;
		IOSleep(1);
	}
	printf("mss: %u: asked: the locator %u times, RMTFS %u, TFTP %u (%u files read, %u written, %u blocks sent); the Wi-Fi "
	    "firmware's service: %s\n", step, serve_asked[0], serve_asked[1], serve_asked[2], tftp_files_read, tftp_files_written,
	    tftp_blocks_out, wlfw_port != 0 ? "THERE" : "NOT THERE");
	printf("mss: %u: SERVREG: %u state changes of the Wi-Fi domain told\n", step, servreg_updates);
	for (unsigned f = 0; f < sizeof(rmtfs_files) / sizeof(rmtfs_files[0]); f++) {
		if (rmtfs_files[f].bytes != NULL) {
			printf("mss: %u: RMTFS: %s: %u sectors read, %u written (kept in memory)\n", step, rmtfs_files[f].partition,
			    rmtfs_files[f].reads, rmtfs_files[f].writes);
		}
	}
	qrtr_close(wlfw);
	/* (the three stay: the modem may come back to them, and nothing here is in their way) */
	return true;
}

/* ---- the sequence ------------------------------------------------------------------------------ */

static void
mss_thread(__unused void *arg, __unused wait_result_t wr)
{
	static bool (*const steps[])(void) = { NULL, step_smem, step_partition, step_header, step_tz_header, step_image, step_items,
		                               step_start, step_queues, step_link, step_qrtr, step_serve };

	IOSleep(2000);                          /* (the storage driver's page is up by then) */
	for (step = 1; step <= mss_step && step < sizeof(steps) / sizeof(steps[0]); step++) {
		if (!steps[step]()) {
			printf("mss: %u: STOP: the step did not find what it needs\n", step);
			break;
		}
	}
	printf("mss: done (mss_step=%u)\n", mss_step);
	thread_terminate(current_thread());
}

void
redwood_mss_init(void)
{
	char device[16] = "";
	thread_t thread;

	if (!PE_parse_boot_arg_str("device", device, sizeof(device)) || strcmp(device, "willow") != 0 ||
	    !PE_parse_boot_argn("mss_step", &mss_step, sizeof(mss_step)) || mss_step == 0) {
		return;
	}
	if (kernel_thread_start(mss_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
