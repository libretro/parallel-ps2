/* The i.LINK link (pcsx2/FW.c), the Konami Python 1 I/O board on it
 * (pcsx2/FireWire/p1io.c), the CF card's FAT reader (fatimg.c) and the
 * .py1 descriptor (py1desc.c).
 *
 * The link is driven through its registers as the IOP's driver drives
 * it: packets go out through the user buffer, answers come back through
 * the user and DBUF receive buffers, and the board's writes to IOP RAM
 * land in a RAM of the test's own. The IOP around it is stubbed: its
 * cycle counter is a variable the test moves, its event is recorded.
 *
 * The board's sound is checked against a decoder written the way
 * Reliquary's is (the whole sound decoded ahead into a table), sample for
 * sample through a loop and a savestate. */
#include "FW.h"
#include "IopHw.h"
#include "IopDma.h"
#include "R3000A.h"
#include "MemoryTypes.h"
#include "FireWire/fatimg.h"
#include "FireWire/p1io.h"
#include "FireWire/py1desc.h"
#include "DEV9/ATA/HddImage.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_bad;

static void check(int ok, const char* what)
{
	printf("  %-66s %s\n", what, ok ? "ok" : "WRONG");
	if (!ok)
		s_bad++;
}

/* ---- the IOP, stubbed -------------------------------------------- */

psxRegisters psxRegs;
static IopVM_MemoryAllocMess s_iop;
IopVM_MemoryAllocMess* iopMem = &s_iop;
s64 PSXCLK = 36864000;
static int s_irqs;
static int s_event_set;
static s32 s_event_cycles;
static u32 s_cleared;

void iopIntcIrq(uint irq)
{
	if (irq == 24)
		s_irqs++;
}

void PSX_INT(IopEventId n, s32 ecycle)
{
	if (n == IopEvt_FW)
	{
		psxRegs.interrupt |= 1u << n;
		s_event_set = 1;
		s_event_cycles = ecycle;
	}
}

static void clear_rec(u32 addr, u32 size)
{
	(void)addr;
	s_cleared += size;
}

static R3000Acpu s_cpu = { NULL, NULL, NULL, clear_rec, NULL };
R3000Acpu* psxCpu = &s_cpu;

static void log_stub(enum retro_log_level level, const char* fmt, ...)
{
	(void)level;
	(void)fmt;
}
retro_log_printf_t log_cb = log_stub;

/* Raw images only, here. */
int hdd_image_kind(const char* path)
{
	(void)path;
	return HDD_IMAGE_RAW;
}
hdd_image_t* hdd_image_open(const char* path)
{
	(void)path;
	return NULL;
}
void hdd_image_close(hdd_image_t* img)
{
	(void)img;
}
uint64_t hdd_image_size(const hdd_image_t* img)
{
	(void)img;
	return 0;
}
bool hdd_image_read(hdd_image_t* img, uint64_t o, void* d, uint32_t l)
{
	(void)img; (void)o; (void)d; (void)l;
	return 0;
}
bool hdd_image_write(hdd_image_t* img, uint64_t o, const void* s, uint32_t l)
{
	(void)img; (void)o; (void)s; (void)l;
	return 0;
}
bool hdd_image_flush(hdd_image_t* img)
{
	(void)img;
	return 0;
}

/* ---- the link's registers ---------------------------------------- */

#define R(off) (0x1f808400u + (off))
#define NODE_ID   R(0x000)
#define CTRL2     R(0x010)
#define PHY_ACC   R(0x014)
#define INTR0     R(0x020)
#define INTR0_M   R(0x024)
#define INTR1     R(0x028)
#define ACK_STAT  R(0x03c)
#define TX_NEXT   R(0x040)
#define TX_LAST   R(0x044)
#define RX_DATA   R(0x050)
#define RX_LVL    R(0x054)
#define REG_7C    R(0x07c)
#define PHT0      R(0x080)
#define DBUF_LV0  R(0x0c0)
#define DBUF_TX0  R(0x0c4)
#define DBUF_RX   R(0x0c8)

static u32 bswap(u32 v)
{
	return (v >> 24) | ((v >> 8) & 0xff00u) | ((v & 0xff00u) << 8) | (v << 24);
}

/* A block write from the IOP: the payload as the device should see it. */
static void send_block(u32 hi, u32 lo, const u32* payload, u32 quads)
{
	u32 i;
	FWwrite32(TX_NEXT, (5u << 10) | (1u << 4));
	FWwrite32(TX_NEXT, (0xffc0u << 16) | hi);
	FWwrite32(TX_NEXT, lo);
	FWwrite32(quads ? TX_NEXT : TX_LAST, (quads * 4) << 16);
	for (i = 0; i < quads; i++)
		FWwrite32(i + 1 == quads ? TX_LAST : TX_NEXT, bswap(payload[i]));
}

static u32 read_quadlet(u32 hi, u32 lo, u32* rx)
{
	u32 i, n;
	FWwrite32(TX_NEXT, (7u << 10) | (4u << 4));
	FWwrite32(TX_NEXT, (0xffc0u << 16) | hi);
	FWwrite32(TX_LAST, lo);
	n = FWread32(RX_LVL);
	for (i = 0; i < n && i < 8; i++)
		rx[i] = FWread32(RX_DATA);
	return n;
}

/* One packet out of the DBUF receive buffer: its quads into p. */
static u32 dbuf_packet(u32* p, u32 max)
{
	u32 n = FWread32(DBUF_LV0) >> 18, i;
	for (i = 0; i < n; i++)
	{
		const u32 v = FWread32(DBUF_RX);
		if (i < max)
			p[i] = v;
	}
	return n;
}

/* The board's status writes, drained: the last one to off in *value. */
static int drain_status(u32 off, u32* value)
{
	u32 p[600];
	int found = 0, guard = 0;
	u32 n;
	while ((n = dbuf_packet(p, 600)) && guard++ < 100000)
	{
		if (n == 5 && (p[0] & 0xf0) == 0 && (p[1] & 0xffff) == 0xfffe && p[2] == off)
		{
			*value = p[3];
			found = 1;
		}
	}
	return found;
}

static void drain_all(void)
{
	u32 p[600];
	int guard = 0;
	while (dbuf_packet(p, 600) && guard++ < 100000)
		;
}

/* ---- a device of the test's, for the link alone ------------------ */

static u64 s_w_off;
static u32 s_w_payload[64];
static u32 s_w_quads;
static int s_w_count;

static int dev_read(fw_device* d, u64 off, u32* v)
{
	(void)d;
	*v = (u32)off ^ 0x5a5a5a5au;
	return 1;
}

static int dev_write(fw_device* d, u64 off, const u32* p, u32 q)
{
	(void)d;
	s_w_off = off;
	s_w_quads = q;
	memcpy(s_w_payload, p, (q > 64 ? 64 : q) * 4);
	s_w_count++;
	return 1;
}

static void test_stub(void)
{
	printf("the link with nothing on the bus\n");
	FWsetDevice(NULL);
	FWopen();
	check(FWread32(NODE_ID) == 0xffc00001u, "node ID reads 0xffc00001");
	check(FWread32(REG_7C) == 0x10000001u, "0x7c reads 0x10000001");
	FWwrite32(CTRL2, 0x1234);
	check(FWread32(CTRL2) == 0x8, "control 2 reads 8 whatever is written");
	FWwrite32(INTR0_M, 0x40000000u);
	s_irqs = 0;
	FWwrite32(PHY_ACC, 0x80000000u | (2u << 24));
	check(s_irqs == 1 && (FWread32(INTR0) & 0x40000000u), "a PHY read raises its interrupt when unmasked");
	FWwrite32(INTR0, 0x40000000u);
	check(!(FWread32(INTR0) & 0x40000000u), "interrupt bits clear by writing ones");
	check(FWStateSize() == 0, "no savestate block without a device");
	FWreset();
	check(FWread32(NODE_ID) == 0xffc00001u, "a machine reset leaves it as it was");
	FWclose();
}

static void test_link(void)
{
	static fw_device dev;
	u32 p[16], rx[8], n;
	u32 payload[3] = { 0x11223344u, 0x55667788u, 0x99aabbccu };

	printf("the link with a device\n");
	memset(&dev, 0, sizeof(dev));
	dev.read_quadlet = dev_read;
	dev.write = dev_write;
	FWsetDevice(&dev);
	FWopen();
	FWreset();
	check(FWread32(NODE_ID) == ((0x3ffu << 22) | (1u << 16) | 1u), "a bus reset makes the console node 1");
	check((FWread32(INTR0) & 0x20000001u) == 0x20000001u, "and raises PhyRst and DRFR");
	s_irqs = 0;
	FWwrite32(INTR0_M, 1u);
	check(s_irqs == 1 && (FWread32(INTR0_M) & (1u << 29)), "which interrupt once unmasked; PhyRst is kept unmasked");
	n = dbuf_packet(p, 16);
	check(n == 4 && p[0] == 0xe1 && p[3] == 1, "self-ID: the marker, two IDs and the end");
	check(((p[1] >> 24) & 0x3f) == 0 && ((p[2] >> 24) & 0x3f) == 1, "the device is node 0, the console node 1");

	FWwrite32(PHY_ACC, 0x80000000u | (2u << 24));
	check((FWread32(PHY_ACC) & 0xfff) == ((2u << 8) | 3), "PHY register 2 reads three ports");

	n = read_quadlet(0xffff, 0xf0000404u, rx);
	check(n == 5 && rx[3] == (0xf0000404u ^ 0x5a5a5a5au), "a quadlet read is answered in the user buffer");
	check((rx[0] >> 4 & 0xf) == 6 && ((rx[0] >> 10) & 0x3f) == 7, "as a read response with the request's label");
	check(FWread32(ACK_STAT) >> 28 == 2, "acknowledged pending");

	s_w_count = 0;
	send_block(0xfffe, 0x1234, payload, 3);
	check(s_w_count == 1 && s_w_off == 0xfffe00001234ULL && s_w_quads == 3, "a block write reaches the device");
	check(!memcmp(s_w_payload, payload, sizeof(payload)), "its payload byte-swapped to the device's order");
	check(FWread32(ACK_STAT) >> 28 == 1 && (FWread32(INTR0) & (1u << 9)), "acknowledged complete, PBCntR raised");

	/* A PHT channel: control, length, then the data. */
	s_w_count = 0;
	FWwrite32(PHT0 + 0x08, 0xfffe);
	FWwrite32(PHT0 + 0x0c, 0x0100);
	FWwrite32(PHT0 + 0x10, 8);
	FWwrite32(PHT0, 1u << 16);
	FWwrite32(DBUF_TX0, 0xa1);
	check(s_w_count == 0, "a PHT write waits for its data");
	FWwrite32(DBUF_TX0, 0xa2);
	check(s_w_count == 1 && s_w_off == 0xfffe00000100ULL && s_w_quads == 2 && s_w_payload[1] == 0xa2,
		"and goes once it is all in");

	/* The device's own writes, released one at a time. */
	fw_host_queue_quad(0xfffe, 0x10000, 0xcafef00du);
	{
		const u8 bytes[6] = { 1, 2, 3, 4, 5, 6 };
		fw_host_queue_bytes(0x1000, 0x2000, bytes, 6);
	}
	fw_host_flush();
	check(iopMem->Main[0x2000] == 0, "the second waits for the first to be read");
	n = dbuf_packet(p, 16);
	check(n == 5 && p[2] == 0x10000 && p[3] == 0xcafef00du && p[4] == 2, "a quad write, as a packet");
	n = dbuf_packet(p, 16);
	check(n == 7 && p[3] == (6u << 16) && p[4] == 0x01020304u && p[5] == 0x05060000u, "then the block, big-endian");
	check(!memcmp(iopMem->Main + 0x2000, "\1\2\3\4\5\6", 6), "and to 0x1000 it lands in IOP RAM");
	check(dbuf_packet(p, 16) == 0, "then nothing");
	FWclose();
	FWsetDevice(NULL);
}

/* ---- images ------------------------------------------------------ */

static void put16(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8* p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }

static void file_write(FILE* f, long off, const void* d, size_t n)
{
	fseek(f, off, SEEK_SET);
	fwrite(d, 1, n, f);
}

/* A FAT volume at base (a partition when nonzero) holding a deleted
 * entry, a directory, an empty file and then the file; its clusters
 * scattered. Returns the file's bytes. */
static u8* make_fat(const char* path, int type, u32 base_lba, u32 size)
{
	FILE* f = fopen(path, "wb+");
	u8 bs[512], e[32];
	const u32 spc = type == 32 ? 1 : 2;
	const u32 total = type == 12 ? 2048 : type == 16 ? 16384 : 70000;
	const u32 reserved = type == 32 ? 32 : 1;
	const u32 root_ents = type == 32 ? 0 : 64;
	const u32 fat_sz = type == 12 ? 6 : type == 16 ? 32 : 548;
	const long base = (long)base_lba * 512;
	const long fat = base + (long)reserved * 512;
	const long root = fat + 2L * fat_sz * 512;
	const long data = root + (long)(root_ents * 32 + 511) / 512 * 512;
	const u32 cbytes = spc * 512;
	const u32 nclus = (size + cbytes - 1) / cbytes;
	u8* content = (u8*)malloc(size);
	u32* chain = (u32*)malloc(nclus * sizeof(u32));
	u32 i, root_clus = 2;

	for (i = 0; i < size; i++)
		content[i] = (u8)(i * 7 + (i >> 9));
	/* Runs of three clusters, the runs out of order. */
	for (i = 0; i < nclus; i++)
		chain[i] = 10 + (i % 3) + ((nclus / 3 + 1) - i / 3) * 3;
	memset(bs, 0, sizeof(bs));
	bs[0] = 0xeb;
	put16(bs + 11, 512);
	bs[13] = (u8)spc;
	put16(bs + 14, reserved);
	bs[16] = 2;
	put16(bs + 17, root_ents);
	if (total < 65536 && type != 32)
		put16(bs + 19, total);
	else
		put32(bs + 32, total);
	if (type == 32)
	{
		put32(bs + 36, fat_sz);
		put32(bs + 44, root_clus);
	}
	else
		put16(bs + 22, fat_sz);
	bs[510] = 0x55;
	bs[511] = 0xaa;
	file_write(f, base, bs, 512);
	if (base_lba)
	{
		u8 mbr[512];
		memset(mbr, 0, sizeof(mbr));
		mbr[446 + 4] = type == 32 ? 0x0c : 0x06;
		put32(mbr + 446 + 8, base_lba);
		put32(mbr + 446 + 12, total);
		mbr[510] = 0x55;
		mbr[511] = 0xaa;
		file_write(f, 0, mbr, 512);
	}
	/* The FAT's entries. */
	for (i = 0; i < nclus; i++)
	{
		const u32 c = chain[i], next = i + 1 < nclus ? chain[i + 1] : 0x0fffffff;
		u8 b[4];
		if (type == 12)
		{
			const long at = fat + c + c / 2;
			u8 cur[2];
			fseek(f, at, SEEK_SET);
			if (fread(cur, 1, 2, f) != 2)
				memset(cur, 0, 2);
			if (c & 1)
			{
				cur[0] = (u8)((cur[0] & 0x0f) | ((next & 0xf) << 4));
				cur[1] = (u8)(next >> 4);
			}
			else
			{
				cur[0] = (u8)next;
				cur[1] = (u8)((cur[1] & 0xf0) | ((next >> 8) & 0xf));
			}
			file_write(f, at, cur, 2);
		}
		else if (type == 16)
		{
			put16(b, next);
			file_write(f, fat + c * 2, b, 2);
		}
		else
		{
			put32(b, next);
			file_write(f, fat + c * 4, b, 4);
		}
		file_write(f, data + (long)(c - 2) * cbytes, content + i * cbytes,
			i + 1 < nclus ? cbytes : size - i * cbytes);
	}
	/* The root: deleted, a directory, an LFN, an empty file, the file. */
	{
		const long r = type == 32 ? data : root;
		if (type == 32)
		{
			u8 b[4];
			put32(b, 0x0fffffff);
			file_write(f, fat + root_clus * 4, b, 4);
		}
		memset(e, ' ', 11);
		memcpy(e, "\xe5OLD    BIN", 11);
		memset(e + 11, 0, 21);
		put32(e + 28, 100);
		put16(e + 26, 3);
		file_write(f, r, e, 32);
		memcpy(e, "SUBDIR     ", 11);
		e[11] = 0x10;
		file_write(f, r + 32, e, 32);
		memcpy(e, "LONGNAMEXXX", 11);
		e[11] = 0x0f;
		file_write(f, r + 64, e, 32);
		memcpy(e, "EMPTY   TXT", 11);
		e[11] = 0x20;
		put32(e + 28, 0);
		file_write(f, r + 96, e, 32);
		memcpy(e, "PYTHONFSBIN", 11);
		e[11] = 0x20;
		put32(e + 28, size);
		put16(e + 26, chain[0] & 0xffff);
		put16(e + 20, chain[0] >> 16);
		file_write(f, r + 128, e, 32);
	}
	/* The volume's last byte, so the image is its whole size. */
	file_write(f, base + (long)total * 512 - 1, "", 1);
	fclose(f);
	free(chain);
	return content;
}

static FILE* s_fat_file;

static int file_io(void* ctx, u64 off, void* buf, u32 len, int write)
{
	(void)ctx;
	if (fseek(s_fat_file, (long)off, SEEK_SET))
		return 0;
	return write ? fwrite(buf, 1, len, s_fat_file) == len : fread(buf, 1, len, s_fat_file) == len;
}

static void test_fat_one(int type, u32 base, const char* what)
{
	char name[256];
	const u32 size = 40000;
	u8* want;
	u8 got[1500];
	fat_file f;
	long end;
	int ok;

	sprintf(name, "%s/p1io_fat%d.img", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", type);
	want = make_fat(name, type, base, size);
	s_fat_file = fopen(name, "rb+");
	fseek(s_fat_file, 0, SEEK_END);
	end = ftell(s_fat_file);
	ok = fat_open_first_file(&f, file_io, NULL, (u64)end);
	check(ok && !strcmp(f.name, "PYTHONFS.BIN") && f.size == size, what);
	check(ok && fat_rw(&f, 0, got, 512, 0) && !memcmp(got, want, 512), "  its first sector");
	check(ok && fat_rw(&f, 1000, got, 1500, 0) && !memcmp(got, want + 1000, 1500), "  bytes across clusters out of order");
	check(ok && !fat_rw(&f, size - 10, got, 11, 0) && fat_rw(&f, size - 10, got, 10, 0), "  and not past its end");
	memset(got, 0x3c, 600);
	check(ok && fat_rw(&f, 3000, got, 600, 1), "  written in place");
	memset(want + 3000, 0x3c, 600);
	check(ok && fat_rw(&f, 2900, got, 800, 0) && !memcmp(got, want + 2900, 800), "  and read back");
	fat_close(&f);
	fclose(s_fat_file);
	remove(name);
	free(want);
}

static void test_fat(void)
{
	printf("the CF card's file\n");
	test_fat_one(12, 0, "FAT12, a bare volume");
	test_fat_one(16, 63, "FAT16, the first partition");
	test_fat_one(32, 0, "FAT32");
}

/* ---- the descriptor ---------------------------------------------- */

static void test_desc(void)
{
	static const char text[] =
		"; a Python 1 game\r\n"
		"[Other]\nHddImagePath=nope.img\n"
		"[Game]\n"
		"Name = Keyboardmania 3rd Mix\n"
		"CfImagePath=cf/game.img\n"
		"BbsRamPath = \"saves/bbsram.bin\"\n"
		"IoBootRomPath=/abs/boot.bin\n"
		"InternalDonglePath=d1.bin\nExternalDonglePath=d2.bin\n"
		"MemoryCardDonglePath=card.ps2\nMemoryCardIdPath=card.id\n"
		"IoConfigRomPath=crom.bin\n"
		"IoMode=b22\n";
	py1_desc d;
	printf("the descriptor\n");
	check(py1_parse(text, strlen(text), "/games/km", &d), "a [Game] with a CF image is a game");
	check(!strcmp(d.name, "Keyboardmania 3rd Mix") && !d.hdd_image[0], "its name; the other section's HDD left out");
	check(!strcmp(d.cf_image, "/games/km/cf/game.img") && !strcmp(d.bbsram, "/games/km/saves/bbsram.bin"),
		"paths from its directory, quotes off");
	check(!strcmp(d.bootrom, "/abs/boot.bin") && !strcmp(d.memcard_id, "/games/km/card.id"), "absolute ones as they are");
	check(d.io_mode == P1IO_MODE_B22 && !strcmp(py1_io_mode_name(d.io_mode), "B22"), "the I/O mode, case aside");
	check(!py1_parse("[Game]\nName=x\n", 14, "/", &d), "no storage, no game");
}

/* ---- the board --------------------------------------------------- */

static s16 s_input[2][16];
static s16 s_keys[512];

static int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)index;
	if (device == RETRO_DEVICE_JOYPAD && port < 2 && id < 16)
		return s_input[port][id];
	if (device == RETRO_DEVICE_KEYBOARD && id < 512)
		return s_keys[id];
	return 0;
}

static retro_input_state_t input_source(void)
{
	return input_cb;
}

static char s_dir[512];
static char* tmp_path(const char* name)
{
	static char paths[8][600];
	static int k;
	k = (k + 1) & 7;
	sprintf(paths[k], "%s/%s", s_dir, name);
	return paths[k];
}

/* An HDD image: sector 0x100 an ADPCM sound with a loop, the rest a
 * pattern. */
#define SOUND_SECTOR 0x100
#define SOUND_BYTES  3000
#define LOOP_START   1200
#define LOOP_END     2600

static void make_hdd(const char* path, u8** pattern)
{
	FILE* f = fopen(path, "wb");
	u8* d = (u8*)malloc(0x40000);
	u32 i;
	for (i = 0; i < 0x40000; i++)
		d[i] = (u8)(i ^ (i >> 8) ^ (i >> 13));
	memcpy(d + SOUND_SECTOR * 512, "ADP\2", 4);
	d[SOUND_SECTOR * 512 + 4] = 0;
	d[SOUND_SECTOR * 512 + 5] = 0;
	d[SOUND_SECTOR * 512 + 6] = SOUND_BYTES >> 8;
	d[SOUND_SECTOR * 512 + 7] = SOUND_BYTES & 0xff;
	put32(d + SOUND_SECTOR * 512 + 8, bswap(LOOP_START + 16));
	put32(d + SOUND_SECTOR * 512 + 12, bswap(LOOP_END + 16));
	for (i = 0; i < SOUND_BYTES; i++)
		d[SOUND_SECTOR * 512 + 16 + i] = (u8)((i * 37) ^ (i >> 3));
	fwrite(d, 1, 0x40000, f);
	fclose(f);
	*pattern = d;
}

static p1io* s_board;

static void board_open(int mode)
{
	py1_desc d;
	memset(&d, 0, sizeof(d));
	strcpy(d.hdd_image, tmp_path("hdd.img"));
	strcpy(d.bbsram, tmp_path("bbsram.bin"));
	strcpy(d.bootrom, tmp_path("bootrom.bin"));
	d.io_mode = mode;
	s_board = p1io_create(&d, input_source);
	FWsetDevice(p1io_device(s_board));
	FWopen();
	FWreset();
	drain_all();
}

static void board_close(void)
{
	FWclose();
	FWsetDevice(NULL);
	p1io_destroy(s_board);
	s_board = NULL;
}

/* Reliquary's decoder, the whole sound ahead. */
static const s32 steps[49] = {
	256, 272, 304, 336, 368, 400, 448, 496, 544, 592, 656, 720,
	800, 880, 960, 1056, 1168, 1280, 1408, 1552, 1712, 1888, 2080, 2288,
	2512, 2768, 3040, 3344, 3680, 4048, 4464, 4912, 5392, 5936, 6528, 7184,
	7904, 8704, 9568, 10528, 11584, 12736, 14016, 15408, 16960, 18656, 20512, 22576,
	24832
};
static const s32 changes[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static s16 ref_nibble(u8 n, s32* si, s32* pcm)
{
	const s32 step = steps[*si];
	s32 delta = (step >> 3) + ((step >> 2) & -(n & 1)) + ((step >> 1) & -((n >> 1) & 1)) + (step & -((n >> 2) & 1));
	if (n & 8)
		delta = -delta;
	*si += changes[n];
	*si = *si < 0 ? 0 : *si > 48 ? 48 : *si;
	*pcm += delta;
	*pcm = *pcm < -32768 ? -32768 : *pcm > 32767 ? 32767 : *pcm;
	return (s16)*pcm;
}

typedef struct ref_player
{
	s16 (*samples)[2];
	u32 n, pos, loop_start, loop_end, acc, fade_in, vol;
	float filt[2];
	int filt_init, playing;
} ref_player;

static void ref_start(ref_player* r, const u8* enc, u32 n, u32 vol)
{
	s32 si[2] = { 0, 0 }, pcm[2] = { 0, 0 };
	u32 i;
	memset(r, 0, sizeof(*r));
	r->samples = (s16(*)[2])malloc(n * sizeof(*r->samples));
	for (i = 0; i < n; i++)
	{
		r->samples[i][0] = ref_nibble((u8)(enc[i] >> 4), &si[0], &pcm[0]);
		r->samples[i][1] = ref_nibble((u8)(enc[i] & 15), &si[1], &pcm[1]);
	}
	r->n = n;
	r->loop_start = LOOP_START;
	r->loop_end = LOOP_END;
	r->fade_in = 240;
	r->vol = vol;
	r->playing = 1;
}

static void ref_mix(ref_player* r, s32* l, s32* rr)
{
	float env = 1.0f, frac, s[2];
	u32 next = r->pos + 1 < r->loop_end ? r->pos + 1 : r->loop_start;
	int ch;
	if (r->fade_in)
	{
		env *= (float)(240 - r->fade_in) / 240.0f;
		r->fade_in--;
	}
	frac = (float)r->acc / 48000.0f;
	for (ch = 0; ch < 2; ch++)
	{
		s[ch] = (float)r->samples[r->pos][ch] + ((float)r->samples[next][ch] - (float)r->samples[r->pos][ch]) * frac;
		if (r->filt_init)
			r->filt[ch] += (s[ch] - r->filt[ch]) * 0.60f;
		else
			r->filt[ch] = s[ch];
	}
	r->filt_init = 1;
	*l += (s32)((r->filt[0] * (float)r->vol * env) / 65536.0f);
	*rr += (s32)((r->filt[1] * (float)r->vol * env) / 65536.0f);
	r->acc += 22050;
	while (r->acc >= 48000)
	{
		r->acc -= 48000;
		if (++r->pos >= r->loop_end)
			r->pos = r->loop_start;
	}
}

static void test_board_storage(const u8* hdd)
{
	u32 cmd[5], st = 0, i;
	int ok;

	printf("the board's disk\n");
	board_open(P1IO_MODE_JVS);
	memset(iopMem->Main + 0x10000, 0, 0x4000);
	/* ATA read: subop 0, device, sector 4, 16 sectors, to 0x10000. */
	cmd[0] = 0; cmd[1] = 0; cmd[2] = 4; cmd[3] = 16; cmd[4] = 0x10000;
	psxRegs.cycle = 1000;
	s_event_set = 0;
	s_cleared = 0;
	send_block(0xfffe, 0x3a0, cmd, 5);
	check(!memcmp(iopMem->Main + 0x10000, hdd + 4 * 512, 16 * 512), "sectors land in IOP RAM as the command goes");
	check(s_cleared == 16 * 512 / 4, "and the IOP's compiled code under them is let go");
	check(s_event_set && s_event_cycles > 36864 / 2, "the status waits for the disk: half a millisecond and the transfer");
	check(!drain_status(0x60000, &st), "none yet");
	psxRegs.cycle += (u32)s_event_cycles;
	FWinterrupt();
	ok = drain_status(0x60000, &st);
	{
		u32 sum = 0;
		for (i = 0; i < 16 * 512; i += 2)
			sum = (sum + ((u32)hdd[4 * 512 + i] << 8 | hdd[4 * 512 + i + 1])) & 0x7fffffffu;
		check(ok && st == bswap(sum), "then its checksum");
	}

	/* A write, read back. */
	memset(iopMem->Main + 0x20000, 0x5e, 1024);
	cmd[0] = 2; cmd[1] = 0; cmd[2] = 8; cmd[3] = 2; cmd[4] = 0x20000;
	send_block(0xfffe, 0x3a0, cmd, 5);
	check(drain_status(0x60000, &st) && st == 0, "a write answers at once");
	cmd[0] = 0; cmd[4] = 0x30000;
	send_block(0xfffe, 0x3a0, cmd, 5);
	check(iopMem->Main[0x30000] == 0x5e && iopMem->Main[0x303ff] == 0x5e, "and reads back");
	psxRegs.cycle += 10000000;
	FWinterrupt();
	drain_all();

	/* The format query: not formatted until device 0x14 asks. */
	cmd[0] = 7; cmd[1] = 0x0a; cmd[2] = 0; cmd[3] = 0; cmd[4] = 0;
	send_block(0xfffe, 0x3a0, cmd, 5);
	check(drain_status(0x60000, &st) && st == bswap(0xffffffd1u), "the game's file system: not there");
	cmd[1] = 0x14;
	send_block(0xfffe, 0x3a0, cmd, 5);
	drain_all();
	cmd[1] = 0x0a;
	send_block(0xfffe, 0x3a0, cmd, 5);
	check(drain_status(0x60000, &st) && st == 0, "until it is made");
	board_close();
}

static void test_board_memories(void)
{
	u32 cmd[8], st = 0, rx[8];
	FILE* f;
	u8 buf[0x2000];

	printf("the board's memories\n");
	remove(tmp_path("bbsram.bin"));
	remove(tmp_path("bootrom.bin"));
	board_open(P1IO_MODE_JVS);
	f = fopen(tmp_path("bootrom.bin"), "rb");
	check(f && fread(buf, 1, 0x2000, f) == 0x2000 && !memcmp(buf, "\x00\x04\x5f\x00\x00\x01", 6),
		"a boot ROM that was not there is made");
	if (f)
		fclose(f);
	check(read_quadlet(0xffff, 0xf0000400u, rx) == 5 && rx[3] == 0x0404a1a4u, "the config ROM, the built-in one");
	check(read_quadlet(0xfffd, 0x05735734u, rx) == 5 && rx[3] == 0x01000000u, "the boot-ready answer");

	/* BBSRAM: write 16 bytes from IOP RAM at 0x100, read them back. */
	memcpy(iopMem->Main + 0x40000, "battery backed!!", 16);
	cmd[0] = 1; cmd[1] = 0x100; cmd[2] = 16; cmd[3] = 0x40000;
	send_block(0xfffe, 0x140, cmd, 4);
	check(drain_status(0x80000, &st) && st == 0, "a BBSRAM write");
	f = fopen(tmp_path("bbsram.bin"), "rb");
	check(f && fread(buf, 1, 0x2000, f) == 0x2000 && !memcmp(buf + 0x100, "battery backed!!", 16), "kept in its file");
	if (f)
		fclose(f);
	cmd[0] = 0; cmd[3] = 0x41000;
	send_block(0xfffe, 0x140, cmd, 4);
	check(drain_status(0x80000, &st) && !memcmp(iopMem->Main + 0x41000, "battery backed!!", 16),
		"read back through the board's writes");

	/* The boot ROM: 0x600 bytes read in chunks of 0x200. */
	memset(iopMem->Main + 0x50000, 0, 0x600);
	cmd[0] = 0; cmd[1] = 0; cmd[2] = 0x600; cmd[3] = 0x50000;
	send_block(0xfffe, 0x150, cmd, 4);
	check(drain_status(0x90000, &st) && st == 0x41000000u, "a boot ROM read");
	check(!memcmp(iopMem->Main + 0x50000, "\x00\x04\x5f\x00\x00\x01", 6), "its bytes in IOP RAM");

	/* Dallas: the internal key's serial. */
	cmd[0] = 0; cmd[1] = 0;
	send_block(0xfffe, 0x120, cmd, 2);
	{
		u32 p[16], n, found = 0;
		while ((n = dbuf_packet(p, 16)) != 0)
			if (n == 8 && p[2] == 0x10000)
				found = (p[5] & 0xff) == 0x14;
		check(found, "the internal Dallas key: family 0x14");
	}
	board_close();
}

static void test_board_panel(void)
{
	u32 cmd[8], st = 0, i;
	printf("the board's panel\n");
	board_open(P1IO_MODE_JVS);
	memset(cmd, 0, sizeof(cmd));
	cmd[0] = 0x60000;
	send_block(0xfffe, 0x0e0, cmd, 8);
	drain_all();
	{
		u32 r0;
		memcpy(&r0, iopMem->Main + 0x60000, 4);
		check(r0 == 0x98062caau, "the panel's attach report, last of nine");
	}
	memset(s_input, 0, sizeof(s_input));
	s_input[0][RETRO_DEVICE_ID_JOYPAD_START] = 1;
	s_input[0][RETRO_DEVICE_ID_JOYPAD_B] = 1;
	s_input[1][RETRO_DEVICE_ID_JOYPAD_UP] = 1;
	s_input[0][RETRO_DEVICE_ID_JOYPAD_SELECT] = 1;
	send_block(0xfffe, 0xf80, cmd, 8);
	{
		u32 r[9];
		memcpy(r, iopMem->Main + 0x60000, sizeof(r));
		check(r[0] == bswap(1), "a coin counted");
		check(bswap(r[4] & ~0x00010000u) == ((1u << 11) | (1u << 4) | (1u << 18)), "P1 start and button 1, P2 up");
		check((r[8] & 0xffff) == (0xffff & ~(0x0004 | 0x0010 | 0x0200)), "the active-low status: coin, start, button 1");
	}
	send_block(0xfffe, 0xf80, cmd, 8);
	{
		u32 r0;
		memcpy(&r0, iopMem->Main + 0x60000, 4);
		check(r0 == bswap(1), "a held coin is one coin");
	}
	(void)st;
	(void)i;
	board_close();

	printf("the serial boards\n");
	memset(s_input, 0, sizeof(s_input));
	board_open(P1IO_MODE_DOGSTATIONDX);
	check(p1io_memcard_slot() == 1, "Dog Station DX: the card slot starts at 1");
	cmd[4] = 0x05;
	send_block(0xfffe, 0xf80, cmd, 8);
	cmd[4] = 0x04;
	send_block(0xfffe, 0xf80, cmd, 8);
	check(p1io_memcard_slot() == 0, "the latch's falling edge selects slot 0");
	board_close();
	check(p1io_memcard_slot() == -1, "no board, no say over the slot");

	board_open(P1IO_MODE_B22);
	check(p1io_memcard_slot() == 1, "B22: always slot 1");
	/* A key down, then the status poll of node 1 (command 0x26). */
	s_keys[RETROK_a] = 1;
	{
		u8 req[6] = { 0xaa, 0x00, 0x01, 0x26, 0x00, 0x00 };
		u32 c[5], p[64], n, got = 0;
		req[5] = (u8)(req[1] + req[2] + req[3] + req[4]);
		c[0] = 1;
		c[1] = 6;
		c[2] = ((u32)req[0] << 24) | ((u32)req[1] << 16) | ((u32)req[2] << 8) | req[3];
		c[3] = ((u32)req[4] << 24) | ((u32)req[5] << 16);
		send_block(0xfffe, 0x100, c, 4);
		drain_all();
		c[0] = 2;
		send_block(0xfffe, 0x100, c, 1);
		while ((n = dbuf_packet(p, 64)) != 0)
			if (p[2] == 0x30000 && n > 8)
			{
				/* The callback block: count and bytes, swapped. */
				u8 bytes[64];
				u32 cnt = bswap(p[5]) >> 16, i2;
				bytes[0] = (u8)bswap(p[5]);
				for (i2 = 1; i2 < cnt && i2 < 38; i2++)
				{
					const u32 w = bswap(p[6 + (i2 - 1) / 2]);
					bytes[i2] = (u8)(((i2 - 1) & 1) ? w : w >> 16);
				}
				/* request echo (6), header (4), status (9): the key at 11. */
				got = cnt >= 19 && bytes[10] == 0x04 && bytes[11] == 0x1c;
			}
		check(got, "the B22 keyboard: A's make code in the status");
	}
	s_keys[RETROK_a] = 0;
	board_close();
}

static void test_board_sound(void)
{
	static s32 got[2][20000], want[2][20000];
	u32 cmd[8], i, st;
	ref_player ref;
	u8* hdd;
	FILE* f;
	u8* block;
	size_t size;
	int same = 1, same2 = 1;

	printf("the board's sound\n");
	f = fopen(tmp_path("hdd.img"), "rb");
	hdd = (u8*)malloc(0x40000);
	if (!f || fread(hdd, 1, 0x40000, f) != 0x40000)
		check(0, "the image");
	if (f)
		fclose(f);
	board_open(P1IO_MODE_JVS);
	/* Volume both sides, then the sector: high, low. */
	cmd[0] = 4; cmd[1] = 0x05008000u; cmd[2] = 0x06008000u; cmd[3] = 0x02000000u; cmd[4] = 0x03000000u | SOUND_SECTOR;
	send_block(0xfffe, 0x0d0, cmd, 5);
	check(drain_status(0x70000, &st), "an ADPCM command");
	ref_start(&ref, hdd + SOUND_SECTOR * 512 + 16, SOUND_BYTES, 0x8000);
	for (i = 0; i < 20000; i++)
	{
		got[0][i] = got[1][i] = 0;
		want[0][i] = want[1][i] = 0;
		FWmixSubboardAudio(&got[0][i], &got[1][i]);
		ref_mix(&ref, &want[0][i], &want[1][i]);
		if (got[0][i] != want[0][i] || got[1][i] != want[1][i])
			same = 0;
		/* A state in the middle, played on from twice over. */
		if (i == 9000)
			break;
	}
	check(same, "sample for sample as the whole sound decoded ahead, through its loop");

	size = FWStateSize();
	block = (u8*)malloc(size);
	FWStateWrite(block);
	for (i = 9001; i < 12000; i++)
	{
		got[0][i] = got[1][i] = 0;
		FWmixSubboardAudio(&got[0][i], &got[1][i]);
	}
	FWStateRead(block, size);
	for (i = 9001; i < 12000; i++)
	{
		s32 l = 0, r = 0;
		FWmixSubboardAudio(&l, &r);
		ref_mix(&ref, &want[0][i], &want[1][i]);
		if (l != got[0][i] || r != got[1][i])
			same2 = 0;
		if (l != want[0][i] || r != want[1][i])
			same = 0;
	}
	check(same2, "a state loaded mid-sound plays on the same");
	check(same, "and still as the reference");
	check(FWStateStrip(block, size) == 0, "its block is found by its footer");
	block[size - 1] ^= 1;
	check(FWStateStrip(block, size) == size, "and not when the footer is wrong");
	free(block);

	cmd[0] = 0; cmd[1] = 0;
	send_block(0xfffe, 0x0d0, cmd, 2);
	for (i = 0; i < 300; i++)
	{
		s32 l = 0, r = 0;
		FWmixSubboardAudio(&l, &r);
	}
	{
		s32 l = 0, r = 0;
		FWmixSubboardAudio(&l, &r);
		check(l == 0 && r == 0, "a stop fades out in 5 ms and is silent");
	}
	free(ref.samples);
	free(hdd);
	board_close();
}

static void test_board_state(void)
{
	u8* block;
	size_t size;
	u32 cmd[5];
	printf("the board's savestate\n");
	board_open(P1IO_MODE_JVS);
	cmd[0] = 0; cmd[1] = 0; cmd[2] = 4; cmd[3] = 1; cmd[4] = 0x10000;
	psxRegs.cycle = 50;
	send_block(0xfffe, 0x3a0, cmd, 5);
	size = FWStateSize();
	block = (u8*)malloc(size);
	FWStateWrite(block);
	psxRegs.cycle += 100000000;
	FWinterrupt();
	drain_all();
	psxRegs.cycle = 50;
	s_event_set = 0;
	FWStateRead(block, size);
	check(s_event_set, "a pending disk status is scheduled again from a state");
	{
		u32 st;
		psxRegs.cycle += 100000000;
		FWinterrupt();
		check(drain_status(0x60000, &st), "and arrives");
	}
	/* A state whose queue claims more than it has. */
	FWStateWrite(block);
	memset(block + 0x200 + 16 + 12, 0xff, 4); /* the link's ubuf_tx.count */
	FWwrite32(INTR0, 0xffffffffu);
	FWStateRead(block, size);
	check((FWread32(INTR0) & (1u << 29)) != 0, "a bad block is refused and the bus reset");
	free(block);
	board_close();
}

int main(void)
{
	u8* hdd;
	const char* t = getenv("TMPDIR");
	sprintf(s_dir, "%s/p1io_test_%d", t ? t : "/tmp", (int)(size_t)&s_bad % 100000);
	{
		char cmd[700];
		sprintf(cmd, "mkdir -p '%s'", s_dir);
		if (system(cmd) != 0)
			return 1;
	}
	test_stub();
	test_link();
	test_fat();
	test_desc();
	make_hdd(tmp_path("hdd.img"), &hdd);
	test_board_storage(hdd);
	test_board_memories();
	test_board_panel();
	test_board_sound();
	test_board_state();
	free(hdd);
	remove(tmp_path("hdd.img"));
	remove(tmp_path("bbsram.bin"));
	remove(tmp_path("bootrom.bin"));
	{
		char cmd[700];
		sprintf(cmd, "rm -rf '%s'", s_dir);
		if (system(cmd) != 0)
			s_bad++;
	}
	printf("%s\n", s_bad ? "FAILED" : "all ok");
	return s_bad ? 1 : 0;
}
