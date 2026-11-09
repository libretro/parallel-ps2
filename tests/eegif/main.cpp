/* The GIF DMA channel (pcsx2/Gif.c), driven with seeded chains, normal
 * transfers, MFIFO transfers, FIFO traffic and register accesses.
 *
 * The GIF unit it hands PATH3 data to is a recorder that hashes what it
 * was handed and takes some or all of it, and answers the path and mask
 * questions from a sequence. Main memory is filled with quadwords that
 * all read as sane source tags. Every register, the GIF state and FIFO,
 * the GS CSR, every call made and every value read go into a running
 * hash, written out every so many scenarios. The transcript is compared
 * with expected.txt, which this harness made from the C++ the unit was
 * converted from (EEGIF_TREE=<that checkout>): the conversion changes
 * none of it. Transfers stay clear of the end of main RAM and of tag
 * addresses with nothing behind them, where the C++ read past the array
 * or through NULL; the C build checks those in fixes(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "Config.h"
#include "R5900.h"
#include "Dmac.h"
#include "Hw.h"
#include "Vif_Dma.h"
#include "vtlb.h"
#include "SaveState.h"
#ifdef EEGIF_CXX_TREE
#include "Gif_Unit.h"
#include "GS.h"
#define FIFO_WRITE(p, n) gif_fifo.write_fifo((p), (n))
#define FIFO_READ() gif_fifo.read_fifo()
#else
#include "Gif.h"
#include "GSmmio.h"
#define FIFO_WRITE(p, n) gif_fifo_write((p), (n))
#define FIFO_READ() gif_fifo_read()
#endif
#include "ps2/HwInternal.h"

/* ---- the record ---------------------------------------------------- */

static unsigned long long s_hash = 1469598103934665603ULL;

static void mix(unsigned long long v)
{
	int i;
	for (i = 0; i < 8; i++)
	{
		s_hash ^= (v >> (i * 8)) & 0xff;
		s_hash *= 1099511628211ULL;
	}
}

static void mix_bytes(const void* p, size_t n)
{
	const u8* b = (const u8*)p;
	size_t i;
	for (i = 0; i < n; i++)
	{
		s_hash ^= b[i];
		s_hash *= 1099511628211ULL;
	}
}

static u32 s_rng = 0x6a09e667u;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static u32 rnd_n(u32 n) { return rnd() % n; }

/* fixes() answers the GIF unit's questions itself instead of from the
 * sequence. */
static bool s_fixed;
static u32 s_tick = 0xbb67ae85u;
static u32 answer(u32 tag, u32 a, u32 b)
{
	mix(tag); mix(a); mix(b);
	s_tick = s_tick * 1664525u + 1013904223u;
	return s_tick >> 8;
}

/* ---- what the unit reaches ----------------------------------------- */

alignas(16) cpuRegisters cpuRegs;
alignas(4096) u8 eeHw[PS2MEM_HARDWARE];
EEVM_MemoryAllocMess* eeMem;
alignas(16) vifStruct vif0, vif1;
alignas(16) u8 g_RealGSMem[PS2MEM_GS_REGS];

static bool s_giffifo, s_ophflag;
alignas(16) u8 s_emuconfig[sizeof(Pcsx2Config)];
asm(".globl EmuConfig\n.set EmuConfig, s_emuconfig");
static void set_config(void)
{
	Pcsx2Config* c = (Pcsx2Config*)s_emuconfig;
	s_giffifo = rnd_n(2) == 0;
	s_ophflag = rnd_n(4) == 0;
	c->Gamefixes.GIFFIFOHack = s_giffifo;
	c->Gamefixes.OPHFlagHack = s_ophflag;
}

void CPU_INT(EE_EventType n, s32 ecycle) { answer(0x01, (u32)n, (u32)ecycle); }
void hwDmacIrq(int n) { answer(0x02, (u32)n, 0); }
void hwIntcIrq(int n) { answer(0x03, (u32)n, 0); }
void* vtlb_GetPhyPtr(u32 paddr) { return paddr < PS2MEM_MAIN_RAM ? eeMem->Main + paddr : NULL; }
bool SaveState_FreezeTag(SaveStateBase* s, const char* src) { (void)s; (void)src; return true; }
void SaveState_FreezeMem(SaveStateBase* s, void* data, int size) { (void)s; (void)data; (void)size; }

/* The GIF unit. */
static u32 s_last_size;
static int rec_check_paths(bool a, bool b, bool c) { return (int)(answer(0x10, a | (b << 1) | (c << 2), 0) & 1); }
static int rec_execute(bool a, bool b) { return (int)answer(0x11, a, b); }
static bool rec_can2(void) { return (answer(0x12, 0, 0) & 3) != 0; }
static bool rec_can2hl(void) { return (answer(0x13, 0, 0) & 3) != 0; }
static bool rec_can3(void) { return s_fixed ? true : (answer(0x14, 0, 0) % 3) != 0; }
static bool rec_masked(void) { return s_fixed ? false : (answer(0x15, 0, 0) % 3) == 0; }
static bool rec_queued(void) { return s_fixed ? false : (answer(0x16, 0, 0) % 5) == 0; }
static u32 rec_transfer(u8* mem, u32 size)
{
	const u32 a = answer(0x17, size, 0);
	s_last_size = size;
	if (s_fixed)
		return size;
	mix_bytes(mem, size < 256 ? size : 256);
	return (a & 3) ? size : (a % (size / 16 + 1)) * 16;
}
static void rec_reset(bool soft) { answer(0x18, soft, 0); }
static bool rec_done(int p) { return (answer(0x19, (u32)p, 0) & 1) != 0; }
static int rec_state(int p) { return s_fixed ? GIF_PATH_IDLE : (int)(answer(0x1a, (u32)p, 0) % 5); }
static void rec_set_state(int p, int v) { answer(0x1b, (u32)p, (u32)v); }
#ifdef EEGIF_CXX_TREE
int  eegif_check_paths(bool p1, bool p2, bool p3) { return rec_check_paths(p1, p2, p3); }
int  eegif_execute(bool a, bool b) { return rec_execute(a, b); }
bool eegif_can_do_path2(void) { return rec_can2(); }
bool eegif_can_do_path2hl(void) { return rec_can2hl(); }
bool eegif_can_do_path3(void) { return rec_can3(); }
bool eegif_path3_masked(void) { return rec_masked(); }
bool eegif_signal_queued(void) { return rec_queued(); }
u32  eegif_transfer(u8* mem, u32 size) { return rec_transfer(mem, size); }
void eegif_reset(bool soft) { rec_reset(soft); }
bool eegif_path_is_done(int p) { return rec_done(p); }
int  eegif_path_state(int p) { return rec_state(p); }
void eegif_path_set_state(int p, int v) { rec_set_state(p, v); }
EegifUnit gifUnit;
#else
int  gif_unit_check_paths(bool p1, bool p2, bool p3) { return rec_check_paths(p1, p2, p3); }
int  gif_unit_execute(bool a, bool b) { return rec_execute(a, b); }
bool gif_unit_can_do_path2(void) { return rec_can2(); }
bool gif_unit_can_do_path2hl(void) { return rec_can2hl(); }
bool gif_unit_can_do_path3(void) { return rec_can3(); }
bool gif_unit_path3_masked(void) { return rec_masked(); }
bool gif_unit_signal_queued(void) { return rec_queued(); }
u32  gif_unit_transfer_dma(u8* mem, u32 size) { return rec_transfer(mem, size); }
void gif_unit_reset(bool soft) { rec_reset(soft); }
bool gif_path_is_done(int p) { return rec_done(p); }
bool gif_path_is_idle(int p) { return rec_state(p) == GIF_PATH_IDLE; }
int  gif_path_state(int p) { return rec_state(p); }
void gif_path_set_state(int p, int v) { rec_set_state(p, v); }
bool emu_config_giffifo_hack(void) { return s_giffifo; }
bool emu_config_ophflag_hack(void) { return s_ophflag; }
#endif

/* ---- the layout ---------------------------------------------------- */

static const u32 kMainA  = 0x00400000;
static const u32 kMainB  = 0x01b00000;
static const u32 kWindow = 0x20000;
static const u32 kMargin = 0x10000;

static u32 main_addr(void)
{
	return (rnd() & 1 ? kMainA : kMainB) + (rnd_n((kWindow - kMargin) >> 4) << 4);
}

static void fill_tags(u8* p, u32 bytes)
{
	u32 i, w[4];
	for (i = 0; i < bytes; i += 16)
	{
		w[0] = rnd_n(12) | (rnd_n(8) << 28) | (rnd_n(6) == 0 ? 0x80000000u : 0);
		w[1] = main_addr();
		w[2] = rnd();
		w[3] = rnd();
		memcpy(p + i, w, 16);
	}
}

/* A normal-mode channel is always done with its chain: GIFdma would spin
 * on a chain it never reads otherwise. */
static void keep_normal_done(void)
{
	if (gifch.chcr.MOD != CHAIN_MODE)
		gif.gspath3done = true;
}

static void random_channel(void)
{
	gifch.chcr._u32 = 0x100 | (rnd_n(2) << 2) | (rnd() & 0xc1);
	if (rnd_n(4) == 0)
		gifch.chcr._u32 |= (rnd_n(8) << 28) | (rnd() & 0x80000000u);
	gifch.qwc  = rnd_n(3) == 0 ? 0 : rnd_n(80);
	gifch.madr = main_addr();
	gifch.tadr = main_addr();
}

static void random_gif(void)
{
	u32 i;
	gif.gifstate    = rnd_n(3) == 0 ? GIF_STATE_EMPTY : GIF_STATE_READY;
	gif.gspath3done = rnd() & 1;
	gif.gscycles    = rnd_n(64);
	gif.prevcycles  = rnd_n(3) == 0 ? rnd_n(64) : 0;
	gif.mfifocycles = rnd_n(64);
	gif_fifo.fifoSize = rnd_n(3) == 0 ? rnd_n(17) : 0;
	for (i = 0; i < 64; i++)
		gif_fifo.data[i] = rnd();
	keep_normal_done();
}

/* The MFIFO cases worth having: the GIF draining just behind SPR0, near
 * the ring's end, a tag at its very top, the drain caught up. */
static void mfifo_shape(void)
{
	const u32 top = dmacRegs.rbor._u32 + dmacRegs.rbsr._u32;
	switch (rnd_n(6))
	{
		case 0: spr0ch.madr = gifch.madr + (rnd_n(16) << 4); break;
		case 1: gifch.madr = top - (rnd_n(8) << 4); break;
		case 2: gifch.tadr = top - (rnd_n(2) << 4); break;
		case 3: spr0ch.madr = gifch.tadr; break;
		case 4:
		{
			/* A REFS tag whose data runs past the stall address. */
			u32 w[2];
			const u32 addr = main_addr();
			w[0] = 8 | (TAG_REFS << 28);
			w[1] = addr;
			memcpy(&eeMem->Main[gifch.tadr & ~15u], w, 8);
			dmacRegs.stadr._u32 = addr + (rnd_n(16) << 4);
			dmacRegs.ctrl._u32  = (dmacRegs.ctrl._u32 & ~0xc0u) | (STD_GIF << 6);
			break;
		}
		default: break;
	}
}

static void reset_world(void)
{
	memset(eeHw, 0, sizeof(eeHw));
	memset(&cpuRegs, 0, sizeof(cpuRegs));
	memset(&vif1, 0, sizeof(vif1));
	memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
	fill_tags(&eeMem->Main[kMainA], kWindow);
	fill_tags(&eeMem->Main[kMainB], kWindow);
	set_config();
	random_channel();
	dmacRegs.ctrl._u32  = rnd() & 0x3ff;
	dmacRegs.stadr._u32 = main_addr();
	/* The MFIFO ring in main memory, SPR0 somewhere in it. */
	dmacRegs.rbor._u32  = kMainA + 0x8000;
	dmacRegs.rbsr._u32  = 0x7ff0;
	spr0ch.madr         = dmacRegs.rbor._u32 + (rnd_n(0x800) << 4);
	if (rnd_n(2))
	{
		gifch.tadr = dmacRegs.rbor._u32 + (rnd_n(0x800) << 4);
		gifch.madr = dmacRegs.rbor._u32 + (rnd_n(0x700) << 4);
	}
	random_gif();
	mfifo_shape();
	gifRegs.ctrl._u32 = rnd_n(4) == 0 ? 8 : 0;
	gifRegs.mode._u32 = rnd() & 5;
	gifRegs.stat._u32 = rnd() & 0x1f001fefu;
	cpuRegs.interrupt = rnd() & ((1 << DMAC_VIF1) | (1 << DMAC_GIF) | (1 << DMAC_MFIFO_GIF));
	cpuRegs.eCycle[DMAC_GIF]       = rnd_n(256);
	cpuRegs.eCycle[DMAC_MFIFO_GIF] = rnd_n(256);
	cpuRegs.dmastall = rnd() & 0x3ff;
	vif1Regs.stat._u32 = rnd() & 0x3f0f;
	vif1.inprogress = (u8)(rnd() & 0x11);
	vif1ch.chcr._u32 = 0x100 | (rnd() & 1);
}

static void mix_state(void)
{
	mix_bytes(&eeHw[0x3000], 0x100);
	mix_bytes(&eeHw[0x3c00], 0x100);
	mix_bytes(&eeHw[0x9000], 0x100);
	mix_bytes(&eeHw[0xa000], 0x100);
	mix_bytes(&eeHw[0xd000], 0x100);
	mix_bytes(&eeHw[0xe000], 0x100);
	mix_bytes(&gif, sizeof(gif));
	mix_bytes(&gif_fifo, sizeof(gif_fifo));
	mix_bytes(&g_RealGSMem[0x1000], 4);
	mix(vif1.inprogress);
	mix(cpuRegs.dmastall);
}

static const u32 kRegs[] = { GIF_CTRL, GIF_MODE, GIF_STAT, 0x10003030, 0x10003040, 0x100030a0 };

static void step(void)
{
	switch (rnd_n(16))
	{
		case 0: dmaGIF(); break;
		case 1:
		case 2: gifInterrupt(); break;
		case 3:
			if (gifch.chcr.MOD == CHAIN_MODE)
				gifMFIFOInterrupt();
			break;
		case 4:
			if (gifch.chcr.MOD == CHAIN_MODE)
				mfifoGIFtransfer();
			break;
		case 5: GIFdma(); break;
		case 6: gifCheckPathStatus(rnd() & 1); break;
		case 7: mix(gifRead32(kRegs[rnd_n(6)])); break;
		case 8:
		{
			const u32 reg = kRegs[rnd_n(6)];
			mix(gifWrite32(reg, rnd() & (reg == GIF_CTRL ? 0x9 : 0xf)));
			break;
		}
		case 9:
		{
			u32 n = FIFO_WRITE((u32*)&eeMem->Main[main_addr()], (int)rnd_n(5));
			mix(n);
			mix((u32)FIFO_READ());
			break;
		}
		case 10:
			hwMFIFOResume(rnd_n(4));
			if (rnd_n(2))
				FireMFIFOEmpty();
			break;
		case 11:
			if (rnd_n(2)) gifHwReset();
			else          gifFifoReset();
			break;
		case 12:
			gifRegs.stat._u32 = rnd() & 0x1f001fefu;
			gifRegs.mode._u32 = rnd() & 5;
			cpuRegs.interrupt = rnd() & ((1 << DMAC_VIF1) | (1 << DMAC_GIF) | (1 << DMAC_MFIFO_GIF));
			break;
		case 13:
			if (!gifch.chcr.STR && rnd_n(2))
			{
				gifch.chcr.STR = 1;
				gifch.madr = main_addr();
				dmaGIF();
			}
			break;
		default:
			dmacRegs.ctrl._u32 = rnd() & 0x3ff;
			spr0ch.madr = dmacRegs.rbor._u32 + (rnd_n(0x800) << 4);
			mfifo_shape();
			break;
	}
	keep_normal_done();
}

static void run(FILE* out)
{
	int scen, i;

	for (scen = 0; scen < 2000; scen++)
	{
		reset_world();
		mix_state();
		for (i = 0; i < 40; i++)
		{
			step();
			mix_state();
		}
		if ((scen + 1) % 50 == 0)
			fprintf(out, "%4d %016llx\n", scen + 1, s_hash);
	}
	fprintf(out, " all %016llx\n", s_hash);
}

/* ---- what the conversion fixed ------------------------------------- */

#ifndef EEGIF_CXX_TREE
static int s_failed;

static void expect(bool ok, const char* what)
{
	if (!ok)
	{
		fprintf(stderr, "eegif: %s\n", what);
		s_failed = 1;
	}
}

static void quiet_channel(void)
{
	reset_world();
	memset(&gif, 0, sizeof(gif));
	memset(&gif_fifo, 0, sizeof(gif_fifo));
	memset(&gifch, 0, sizeof(gifch));
	gifRegs.ctrl._u32 = 0;
	gifRegs.mode._u32 = 0;
	gifRegs.stat._u32 = 0;
	dmacRegs.ctrl._u32 = 1;
	s_giffifo = false;
	s_last_size = 0;
}

static void fixes(void)
{
	s_fixed = true;

	/* A normal transfer from the last quadword of main RAM hands PATH3
	 * that one quadword, not the run past it. */
	quiet_channel();
	gifch.chcr._u32 = 0x101;
	gifch.qwc  = 4;
	gifch.madr = PS2MEM_MAIN_RAM - 16;
	gif.gspath3done = true;
	GIFdma();
	expect(s_last_size == 16, "GIF takes only what is mapped behind MADR");

	/* The same for an MFIFO transfer from outside the ring. */
	quiet_channel();
	gifch.chcr._u32 = 0x105;
	gifch.qwc  = 3;
	gifch.madr = PS2MEM_MAIN_RAM - 32;
	dmacRegs.ctrl._u32 = 1 | (MFD_GIF << 2);
	mfifoGIFtransfer();
	expect(s_last_size == 32, "the MFIFO GIF takes only what is mapped behind MADR");

	/* An MFIFO tag at an address with nothing behind it: a bus error
	 * that stops the channel. The C++ read the tag through NULL. */
	quiet_channel();
	gifch.chcr._u32 = 0x105;
	dmacRegs.ctrl._u32 = 1 | (MFD_GIF << 2);
	dmacRegs.rbor._u32 = 0x12000000;
	dmacRegs.rbsr._u32 = 0xff0;
	spr0ch.madr = 0x12000100;
	gifch.tadr  = 0x12000000;
	mfifoGIFtransfer();
	expect(dmacRegs.stat.BEIS && !gifch.chcr.STR && gif.gspath3done, "an MFIFO tag at a bus error stops the GIF");

	s_fixed = false;
}
#endif

int main(int argc, char** argv)
{
	FILE* out;

	eeMem = (EEVM_MemoryAllocMess*)calloc(1, sizeof(*eeMem));
	if (!eeMem || argc < 2)
		return 2;
#ifdef EEGIF_CXX_TREE
	gifUnit.gifPath[0].idx = 0; gifUnit.gifPath[0].state.idx = 0;
	gifUnit.gifPath[1].idx = 1; gifUnit.gifPath[1].state.idx = 1;
	gifUnit.gifPath[2].idx = 2; gifUnit.gifPath[2].state.idx = 2;
#endif
	out = fopen(argv[1], "w");
	if (!out)
		return 2;
	run(out);
	fclose(out);
#ifndef EEGIF_CXX_TREE
	fixes();
	free(eeMem);
	return s_failed;
#else
	free(eeMem);
	return 0;
#endif
}
