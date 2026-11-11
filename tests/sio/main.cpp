/* SIO0, SIO2, the memory card protocol and the multitap (pcsx2/Sio.c,
 * MemoryCardProtocol.c, MultitapProtocol.c, MemoryCardFile.c), driven as
 * the IOP drives them, every byte that comes back written out.
 *
 * The transcript is compared with expected.txt, which the same harness
 * made from the C++ these units were converted from (built with
 * -DSIO_CXX_API against that tree): the conversion changes no byte a
 * game sees, no byte a card file holds, and no byte of a savestate's SIO
 * blocks. The pads, the IOP's interrupt lines and MagicGate are stubs that
 * answer the same way every time. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include <libretro.h>
#include "Config.h"
#include "Sio.h"
#include "MemoryCardFile.h"
#include "SaveState.h"
#include "IopHw.h"
#include "R3000A.h"

#ifdef SIO_CXX_API
#define SIO2_WRITE(b) sio2.Write(b)
#define SIO2_READ() sio2.Read()
#define SIO2_SEND3(p, v) sio2.SetSend3(p, v)
#define SIO2_RESET() sio2.FullReset()
#define SIO0_RESET() sio0.FullReset()
#define SIO0_CTRL(v) sio0.SetCtrl(v)
#define SIO0_TX(v) sio0.SetTxData(v)
#define SIO0_RX() sio0.GetRxData()
#define SIO0_STAT() sio0.GetStat()
#define EJECT_SET(p, s) AutoEject::Set(p, s)
#define FIFO_OUT_EMPTY() fifoOut.empty()
#else
#define SIO2_WRITE(b) sio2_write(b)
#define SIO2_READ() sio2_read()
#define SIO2_SEND3(p, v) sio2_set_send3(p, v)
#define SIO2_RESET() sio2_full_reset()
#define SIO0_RESET() sio0_full_reset()
#define SIO0_CTRL(v) sio0_set_ctrl(v)
#define SIO0_TX(v) sio0_set_tx_data(v)
#define SIO0_RX() sio0_get_rx_data()
#define SIO0_STAT() sio0_get_stat()
#define EJECT_SET(p, s) AutoEject_Set(p, s)
#define FIFO_OUT_EMPTY() sio_fifo_empty(&fifoOut)
#endif

/* ---- what the core provides around them -------------------------- */

static void quiet(enum retro_log_level, const char*, ...) {}
extern "C" {
retro_log_printf_t log_cb = quiet;
psxRegisters psxRegs;
s64 PSXCLK = 36864000;
static int s_irqs;
void iopIntcIrq(uint irq) { s_irqs += (int)irq; }
void PSX_INT(IopEventId n, s32 c) { psxRegs.interrupt |= 1u << n; (void)c; }


static int s_p1io_slot = -1;
int p1io_memcard_slot(void) { return s_p1io_slot; }

/* MagicGate: keyed or not as the script says; a card's values a pattern. */
static bool s_keyed;
static u8 s_put[8];
bool mecha_mg_keyed(void) { return s_keyed; }
void mecha_mg_card_set_id(int, const u8*) {}
void mecha_mg_card_auth_reset(int) {}
bool mecha_mg_card_get(int slot, u8 mode, u8 out[8])
{
	int i;
	if (mode != 0x01 && mode != 0x02 && mode != 0x04 && mode != 0x0f && mode != 0x11 && mode != 0x13)
		return false;
	for (i = 0; i < 8; i++)
		out[i] = (u8)(mode * 16 + slot + i);
	return true;
}
bool mecha_mg_card_put(int, u8, const u8 in[8]) { memcpy(s_put, in, 8); return true; }
}

/* A pad that answers its poll with a running count. The C++ tree's are
 * C++ functions, the C tree's C ones. */
#ifndef SIO_CXX_API
extern "C" {
#endif
static u8 s_pad;
u8 PADstartPoll(int port, int slot) { s_pad = (u8)(0x10 * port + slot); return 0xff; }
u8 PADpoll(u8 v) { return (u8)(s_pad++ ^ v); }
bool PADcomplete(void) { return (s_pad & 7) == 0; }
#ifndef SIO_CXX_API
}
#endif

static FILE* s_out;

static void line(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(s_out, fmt, ap);
	va_end(ap);
	fputc('\n', s_out);
}

/* One command: its SEND3 descriptor at position 0, its bytes written,
 * the answer read out. */
static void cmd(const char* what, u32 port, const u8* b, u32 n, u32 dma_block)
{
	char text[4096];
	size_t k = 0;
	u32 i;
	SIO2_SEND3(1, 0);
	SIO2_SEND3(0, port | (n << 8));
	sio2.dmaBlockSize = dma_block;
	for (i = 0; i < n; i++)
		SIO2_WRITE(b[i]);
	for (i = 0; i < 1000 && !FIFO_OUT_EMPTY(); i++)
		k += (size_t)snprintf(text + k, sizeof(text) - k, "%02x", SIO2_READ());
	text[k] = '\0';
	line("%s: recv1 %x slot %u: %s", what, sio2.recv1, sio2.slot, k ? text : "-");
}

static void cmdv(const char* what, u32 port, u32 n, ...)
{
	u8 b[512];
	va_list ap;
	u32 i;
	memset(b, 0, sizeof(b));
	va_start(ap, n);
	for (i = 0; i < n && i < 16; i++)
		b[i] = (u8)va_arg(ap, int);
	va_end(ap);
	cmd(what, port, b, n, 0);
}

static unsigned long fnv(const void* p, size_t n)
{
	const u8* b = (const u8*)p;
	unsigned long h = 2166136261u;
	size_t i;
	for (i = 0; i < n; i++)
		h = ((h ^ b[i]) * 16777619u) & 0xffffffffu;
	return h;
}

static unsigned long file_hash(const char* path)
{
	FILE* f = fopen(path, "rb");
	static u8 buf[1 << 16];
	unsigned long h = 2166136261u;
	size_t n, i;
	if (!f)
		return 0;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		for (i = 0; i < n; i++)
			h = ((h ^ buf[i]) * 16777619u) & 0xffffffffu;
	fclose(f);
	return h;
}

int main(int argc, char** argv)
{
	const char* dir = argc > 1 ? argv[1] : ".";
	char path[1024];
	u8 b[512];
	u32 i;

	/* EmuConfig is C: its defaults are set, as retro_init sets them. */
	emu_config_init(&EmuConfig);
	s_out = argc > 2 ? fopen(argv[2], "w") : stdout;
	if (!s_out)
		return 1;

	/* A PS2 card on port 0 (made blank), a PS1 card on port 1, multitap
	 * on port 0 with no cards behind it. */
	strcpy(EmuFolders.MemoryCards, dir);
	snprintf(path, sizeof(path), "%s/ps1.mcr", dir);
	{
		FILE* f = fopen(path, "wb");
		for (i = 0; i < 0x20000; i++)
			fputc((int)((i * 13) ^ (i >> 7)) & 0xff, f);
		fclose(f);
	}
	strcpy(EmuConfig.Mcd[0].Filename, "ps2.ps2");
	strcpy(EmuConfig.Mcd[1].Filename, "ps1.mcr");
	EmuConfig.Mcd[0].Enabled = EmuConfig.Mcd[1].Enabled = true;
	EmuConfig.MultitapPort0_Enabled = true;
	EmuConfig.McdEnableEjection = true;
	FileMcd_EmuOpen();
	SIO0_RESET();
	SIO2_RESET();

	cmdv("probe", 0, 4, 0x81, 0x11, 0, 0);
	cmdv("specs", 0, 12, 0x81, 0x26);
	cmdv("set term", 0, 5, 0x81, 0x27, 0x5a, 0, 0);
	cmdv("get term", 0, 5, 0x81, 0x28, 0, 0, 0);
	cmdv("set read sector", 0, 9, 0x81, 0x23, 0x12, 0x00, 0x00, 0x00, 0x12, 0, 0);
	/* Write 128 bytes, then read them back. */
	memset(b, 0, sizeof(b));
	b[0] = 0x81; b[1] = 0x42; b[2] = 0x80;
	for (i = 0; i < 128; i++)
		b[3 + i] = (u8)(i * 3 + 1);
	cmdv("set write sector", 0, 9, 0x81, 0x22, 0x12, 0x00, 0x00, 0x00, 0x12, 0, 0);
	cmd("write data", 0, b, 3 + 128 + 2, 0);
	cmdv("read write end", 0, 4, 0x81, 0x81, 0, 0);
	cmdv("set read sector", 0, 9, 0x81, 0x23, 0x12, 0x00, 0x00, 0x00, 0x12, 0, 0);
	memset(b, 0, sizeof(b));
	b[0] = 0x81; b[1] = 0x43; b[2] = 0x80;
	cmd("read data", 0, b, 3 + 128 + 2, 0);
	cmdv("bad checksum sector", 0, 9, 0x81, 0x21, 0x20, 0x00, 0x00, 0x00, 0x99, 0, 0);
	cmdv("erase", 0, 4, 0x81, 0x82, 0, 0);
	cmdv("boot", 0, 5, 0x81, 0xbf, 0, 0, 0);
	cmdv("auth f3", 0, 5, 0x81, 0xf3, 0, 0, 0);
	cmdv("auth f7", 0, 5, 0x81, 0xf7, 0, 0, 0);
	cmdv("auth xor 01", 0, 14, 0x81, 0xf0, 0x01, 1, 2, 3, 4, 5, 6, 7, 8);
	cmdv("auth xor 03", 0, 5, 0x81, 0xf0, 0x03);
	cmdv("auth xor 06", 0, 14, 0x81, 0xf0, 0x06, 9, 8, 7, 6, 5, 4, 3, 2);
	cmdv("auth xor 99", 0, 5, 0x81, 0xf0, 0x99);
	s_keyed = true;
	cmdv("keyed xor 02", 0, 14, 0x81, 0xf0, 0x02, 1, 2, 3, 4, 5, 6, 7, 8);
	cmdv("keyed xor 07", 0, 14, 0x81, 0xf0, 0x07, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);
	line("keyed put %02x%02x%02x%02x%02x%02x%02x%02x", s_put[0], s_put[1], s_put[2], s_put[3], s_put[4], s_put[5], s_put[6], s_put[7]);
	cmdv("keyed xor 03", 0, 5, 0x81, 0xf0, 0x03);
	s_keyed = false;

	/* The PS1 card over SIO2: dead air for PS2 commands, then a read. */
	cmdv("ps1 probe", 1, 4, 0x81, 0x11, 0, 0);
	memset(b, 0, sizeof(b));
	b[0] = 0x81; b[1] = 0x52; b[3] = 0x00; b[4] = 0x02;
	cmd("ps1 read", 1, b, 140, 0);
	memset(b, 0, sizeof(b));
	b[0] = 0x81; b[1] = 0x57; b[3] = 0x00; b[4] = 0x03;
	{
		u8 x = 0x00 ^ 0x03;
		for (i = 0; i < 128; i++)
		{
			b[5 + i] = (u8)(i + 0x40);
			x ^= b[5 + i];
		}
		b[133] = x;
	}
	cmd("ps1 write", 1, b, 138, 0);
	cmdv("ps1 state", 1, 4, 0x81, 0x53, 0, 0);
	cmdv("pocketstation", 1, 4, 0x81, 0x58, 0, 0);

	/* The PS1 card over SIO0, a byte at a time. */
	SIO0_CTRL(0x0003 | 0x2000);
	{
		static const u8 seq[] = { 0x81, 0x52, 0x00, 0x00, 0x05 };
		char text[2048];
		size_t k = 0;
		for (i = 0; i < 140; i++)
		{
			SIO0_TX(i < sizeof(seq) ? seq[i] : 0);
			k += (size_t)snprintf(text + k, sizeof(text) - k, "%02x", SIO0_RX());
		}
		text[k] = '\0';
		line("sio0 ps1 read: stat %x: %s", SIO0_STAT(), text);
	}
	SIO0_CTRL(0);
	SIO0_CTRL(0x0003);
	{
		char text[256];
		size_t k = 0;
		for (i = 0; i < 9; i++)
		{
			SIO0_TX(i == 0 ? 0x01 : i == 1 ? 0x42 : 0);
			k += (size_t)snprintf(text + k, sizeof(text) - k, "%02x", SIO0_RX());
		}
		text[k] = '\0';
		line("sio0 pad: %s", text);
	}
	SIO0_CTRL(0x40);

	cmdv("pad", 0, 9, 0x01, 0x42, 0, 0xff, 0xff, 0, 0, 0, 0);
	cmdv("multitap check", 0, 7, 0x21, 0x12);
	cmdv("multitap select 2", 0, 7, 0x21, 0x21, 2);
	cmdv("card on mtap slot 2", 0, 4, 0x81, 0x11, 0, 0);
	cmdv("multitap select 9", 0, 7, 0x21, 0x22, 9);
	cmdv("multitap select 0", 0, 7, 0x21, 0x21, 0);
	cmdv("multitap port 1", 1, 7, 0x21, 0x12);
	cmdv("infrared", 0, 6, 0x61, 1, 2);
	cmdv("unknown", 0, 3, 0x99, 1);
	memset(b, 0, sizeof(b));
	b[0] = 0x81; b[1] = 0x11;
	cmd("dma probe", 0, b, 0x24, 0x24);
	cmdv("zero length", 0, 0);

	s_p1io_slot = 1;
	cmdv("p1io slot 1", 1, 4, 0x81, 0x11, 0, 0);
	s_p1io_slot = 0;
	cmdv("p1io slot 0", 1, 4, 0x81, 0x11, 0, 0);
	s_p1io_slot = -1;

	EJECT_SET(0, 0);
	cmdv("ejected", 0, 4, 0x81, 0x11, 0, 0);
	cmdv("ejected again", 0, 4, 0x81, 0x11, 0, 0);

	/* The SIO blocks of a state, mid-transfer. */
	SIO2_SEND3(0, 0 | (9u << 8));
	SIO2_WRITE(0x81);
	SIO2_WRITE(0x23);
	{
		SaveStateBase s;
		SaveState_Init(&s, NULL, 0, 0, true);
		sioFreeze(&s);
		sio2Freeze(&s);
		line("state: %d bytes, %08lx", s.idx, fnv(s.memory, (size_t)s.idx));
		{
			SaveStateBase r;
			int idx = s.idx;
			int ok0, ok2;
			/* Whatever happened since, the load puts back. */
			SIO2_RESET();
			SaveState_Init(&r, s.memory, s.memory_size, s.memory_cap, false);
			ok0 = (int)sioFreeze(&r);
			ok2 = (int)sio2Freeze(&r);
			line("state loads: %d %d, to %d of %d", ok0, ok2, r.idx, idx);
		}
		free(s.memory);
	}
	/* The rest of the command, after the load: sector 0x10, its check. */
	for (i = 2; i < 9; i++)
		SIO2_WRITE(i == 2 || i == 6 ? 0x10 : 0x00);
	{
		char text[256];
		size_t k = 0;
		while (!FIFO_OUT_EMPTY() && k < 200)
			k += (size_t)snprintf(text + k, sizeof(text) - k, "%02x", SIO2_READ());
		text[k] = '\0';
		line("after load: %s", k ? text : "-");
	}

	line("crc %08lx %08lx", (unsigned long)(FileMcd_GetCRC(0, 0) & 0xffffffffu),
		(unsigned long)(FileMcd_GetCRC(1, 0) & 0xffffffffu));
	line("irqs %d", s_irqs);
	FileMcd_EmuClose();
	snprintf(path, sizeof(path), "%s/ps2.ps2", dir);
	line("ps2 card %08lx", file_hash(path));
	snprintf(path, sizeof(path), "%s/ps1.mcr", dir);
	line("ps1 card %08lx", file_hash(path));
	if (s_out != stdout)
		fclose(s_out);
	return 0;
}
