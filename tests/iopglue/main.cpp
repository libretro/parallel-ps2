/* The IOP's counters, DMA dispatch, interrupt lines and the hardware
 * reset (pcsx2/IopCounters.c, IopDma.c, IopIrq.c, IopHw.c), driven the
 * way the IOP's event loop and register writes drive them.
 *
 * A hand-written part walks each counter mode, gate and DMA channel and
 * writes the full state after every step; a seeded random part then runs
 * a long mix of the same calls and writes a hash of everything they touch
 * every few hundred steps. The transcript is compared with expected.txt,
 * which this harness made from the C++ these units were converted from
 * (IOPGLUE_TREE=<that checkout>): the conversion changes no counter, no
 * interrupt bit, no event, no DMA and no byte of the counters' state.
 * Everything around them is a stub that records what it was asked. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include "Config.h"
#include "Common.h"
#include "R3000A.h"
#include "R5900.h"
#include "IopMem.h"
#include "IopHw.h"
#include "IopDma.h"
#include "IopCounters.h"
#include "Mdec.h"
#include "Sif.h"
#include "Sio.h"
#include "SaveState.h"
#include "MemoryCardFile.h"
#include "SPU2/spu2.h"
#include "SPU2/defs.h"
#include "USB/USB.h"
#include "USB/usb-host.h"
#include "CDVD/CDVD.h"
#include "CDVD/Ps1CD.h"
#ifdef IOPGLUE_CXX_TREE
#include "DEV9/DEV9.h"
/* The C++ declared these in no header; IopCounters.h does now. */
extern u8 psxhblankgate, psxvblankgate;
#else
#include "DEV9/DEV9irq.h"
#endif

/* ---- the record every stub writes to ----------------------------- */

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

static FILE* s_out;
static bool s_verbose;

static void note(const char* fmt, ...)
{
	va_list ap;
	if (!s_verbose)
		return;
	va_start(ap, fmt);
	vfprintf(s_out, fmt, ap);
	va_end(ap);
	fputc('\n', s_out);
}

/* ---- what the core provides around them -------------------------- */

static void quiet(enum retro_log_level, const char*, ...) {}
extern "C" retro_log_printf_t log_cb;
retro_log_printf_t log_cb = quiet;

psxRegisters psxRegs;
s64 PSXCLK = 36864000;
alignas(4096) u8 iopHw[PS2MEM_IOP_HARDWARE];
static IopVM_MemoryAllocMess s_iopmem;
IopVM_MemoryAllocMess* iopMem = &s_iopmem;
static uptr s_rlut[0x10000];
const uptr* psxMemRLUT = s_rlut;
bool g_SkipBiosHack;
_sif sif0, sif1, sif2;
Sio2 sio2;
V_Core Cores[2];
u32 lClocks;

void PSX_INT(IopEventId n, s32 c)
{
	psxRegs.interrupt |= 1u << n;
	mix(0x100 + n);
	mix((u32)c);
	note("  PSX_INT %d %d", (int)n, c);
}

void psxSetNextBranch(u32 start, s32 delta)
{
	if ((int)(psxRegs.iopNextEventCycle - start) > delta)
		psxRegs.iopNextEventCycle = start + delta;
	mix(0x200);
	mix(start);
	mix((u32)delta);
}

static int s_testintc;
void iopTestIntc(void) { s_testintc++; mix(0x300); }

u8 iopMemRead8_slow(u32 mem) { mix(0x400); mix(mem); return (u8)(mem * 7); }
void iopMemWrite8(u32 mem, u8 value)
{
	mix(0x500);
	mix(mem);
	mix(value);
	s_iopmem.Main[mem & 0x1fffff] = value;
}

void TimeUpdate(u64 c) { mix(0x600); mix(c); }

void V_Core_DoDMAwrite(V_Core* c, u16* p, u32 size)
{
	mix(0x700 + (u32)(c - Cores));
	mix((u64)((u8*)p - s_iopmem.Main));
	mix(size);
	note("  spu2 write core %d at %lx size %u", (int)(c - Cores), (unsigned long)((u8*)p - s_iopmem.Main), size);
}

void V_Core_DoDMAread(V_Core* c, u16* p, u32 size)
{
	mix(0x710 + (u32)(c - Cores));
	mix((u64)((u8*)p - s_iopmem.Main));
	mix(size);
	note("  spu2 read core %d at %lx size %u", (int)(c - Cores), (unsigned long)((u8*)p - s_iopmem.Main), size);
}

static void cpu_clear(u32 addr, u32 size) { mix(0x800); mix(addr); mix(size); note("  clear %x %u", addr, size); }
static R3000Acpu s_cpu = { NULL, NULL, NULL, cpu_clear, NULL };
R3000Acpu* psxCpu = &s_cpu;

static int s_dev9_irq = 1;
int DEV9irqHandler(void) { mix(0x900); return s_dev9_irq; }
void DEV9async(u32 cycles) { mix(0x910); mix(cycles); }
void DEV9writeDMA8Mem(u32* p, int size) { mix(0x920); mix((u64)((u8*)p - s_iopmem.Main)); mix((u32)size); note("  dev9 write %d", size); }
void DEV9readDMA8Mem(u32* p, int size) { mix(0x930); mix((u64)((u8*)p - s_iopmem.Main)); mix((u32)size); note("  dev9 read %d", size); }

void USBasync(u32 cycles) { mix(0xa00); mix(cycles); }

u8 cdvdRead(u8 key) { mix(0xb00); mix(key); return (u8)(key ^ 0x5a); }
void cdvdWrite(u8 key, u8 v) { mix(0xb10); mix(key); mix(v); }
static int s_skip_seen;
void cdvdReset(void) { mix(0xb20); s_skip_seen = g_SkipBiosHack; }
void cdvdVsync(void) { mix(0xb30); }
void cdrReset(void) { mix(0xb40); }
void mdecInit(void) { mix(0xc00); }
void sio0_full_reset(void) { mix(0xd00); }
void sio2_full_reset(void) { mix(0xd10); }
void sio2_write(u8 v) { mix(0xd20); mix(v); }
static u8 s_sio2_byte;
u8 sio2_read(void) { mix(0xd30); return s_sio2_byte++; }
void SIF0Dma(void) { mix(0xe00); }
void SIF1Dma(void) { mix(0xe10); }
/* Pcsx2Config's constructor names the cards. */
bool FileMcd_IsMultitapSlot(uint slot) { return slot > 1; }
void FileMcd_GetDefaultName(char* out, size_t out_size, uint slot) { snprintf(out, out_size, "card%u", slot); }

/* ---- state out --------------------------------------------------- */

static void mix_state(void)
{
	int i;
	for (i = 0; i < NUM_COUNTERS; i++)
	{
		mix(psxCounters[i].count);
		mix(psxCounters[i].target);
		mix(psxCounters[i].mode);
		mix(psxCounters[i].rate);
		mix(psxCounters[i].interrupt);
		mix(psxCounters[i].startCycle);
		mix((u32)psxCounters[i].deltaCycles);
	}
	mix((u32)psxNextDeltaCounter);
	mix(psxNextStartCounter);
	mix(psxhblankgate);
	mix(psxvblankgate);
	mix(psxHu32(0x1070));
	mix(HW_DMA_ICR);
	mix(HW_DMA_ICR2);
	mix(psxRegs.interrupt);
	mix(psxRegs.iopNextEventCycle);
	mix(psxRegs.CP0.n.Cause);
	mix((u32)s_testintc);
}

static void dump(const char* what)
{
	int i;
	if (!s_verbose)
		return;
	fprintf(s_out, "%s: cyc %u delta %d start %u gates %x/%x istat %x icr %x icr2 %x ev %x next %u cause %x intc %d\n",
		what, psxRegs.cycle, psxNextDeltaCounter, psxNextStartCounter, psxhblankgate, psxvblankgate,
		psxHu32(0x1070), HW_DMA_ICR, HW_DMA_ICR2, psxRegs.interrupt, psxRegs.iopNextEventCycle,
		psxRegs.CP0.n.Cause, s_testintc);
	for (i = 0; i < NUM_COUNTERS; i++)
		fprintf(s_out, "  c%d count %llx target %llx mode %x rate %u int %x start %u delta %d\n", i,
			(unsigned long long)psxCounters[i].count, (unsigned long long)psxCounters[i].target,
			psxCounters[i].mode, psxCounters[i].rate, psxCounters[i].interrupt,
			psxCounters[i].startCycle, psxCounters[i].deltaCycles);
}

static void advance(u32 cycles)
{
	psxRegs.cycle += cycles;
}

static void update(void)
{
	/* What iopEventTest does around it. */
	psxRegs.iopNextEventCycle = psxRegs.cycle + 0x7fffffff;
	psxRcntUpdate();
}

/* A save, a scramble and a load: the counters come back as they were,
 * and the bytes saved are the bytes the C++ saved. */
static void roundtrip(const char* what)
{
	SaveStateBase s;
	psxCounter keep[NUM_COUNTERS];
	s32 kd = psxNextDeltaCounter;
	u32 ks = psxNextStartCounter;
	u8 kh = psxhblankgate, kv = psxvblankgate;
	int i;
	memcpy(keep, psxCounters, sizeof(keep));
	SaveState_Init(&s, NULL, 0, 0, true);
	if (!psxRcntFreeze(&s))
		note("%s: save failed", what);
	for (i = 0; i < s.memory_size; i++)
		mix(s.memory[i]);
	note("%s: saved %u bytes", what, (unsigned)s.memory_size);
	memset(psxCounters, 0xa5, sizeof(psxCounters));
	psxNextDeltaCounter = 0;
	psxNextStartCounter = 0;
	psxhblankgate = psxvblankgate = 0;
	{
		SaveStateBase l;
		SaveState_Init(&l, s.memory, s.memory_size, s.memory_cap, false);
		if (!psxRcntFreeze(&l))
			note("%s: load failed", what);
	}
	if (memcmp(keep, psxCounters, sizeof(keep)) || kd != psxNextDeltaCounter || ks != psxNextStartCounter
		|| kh != psxhblankgate || kv != psxvblankgate)
	{
		fprintf(s_out, "%s: state did not come back\n", what);
		mix(0xdead);
	}
	free(s.memory);
}

/* ---- the hand-written walk --------------------------------------- */

static void walk(void)
{
	int i;

	memset(&psxRegs, 0, sizeof(psxRegs));
	psxRegs.cycle = 1000;
	EmuConfig.UseBOOT2Injection = true;
	psxHwReset();
	note("reset: skip bios %d seen by cdvd %d", (int)g_SkipBiosHack, s_skip_seen);
	EmuConfig.UseBOOT2Injection = false;
	psxHwReset();
	note("reset: skip bios %d seen by cdvd %d", (int)g_SkipBiosHack, s_skip_seen);
	dump("init");
	update();
	dump("first update");

	/* Counter 0: sysclock, target interrupt, reset on target, repeat. */
	psxRcntWmode16(0, 0x0058);
	psxRcntWtarget16(0, 0x100);
	dump("c0 mode 58 target 100");
	for (i = 0; i < 4; i++)
	{
		advance(0xc0);
		update();
		note("c0 read %x", psxRcntRcount16(0));
	}
	dump("c0 after 4 updates");

	/* Counter 0: pixel clock, toggle mode, overflow interrupt. */
	psxRcntWmode16(0, 0x01e0);
	advance(1000000);
	update();
	dump("c0 pixel overflow");

	/* Counter 1: hblank source, gated by vblank, every gate mode. */
	for (i = 0; i < 4; i++)
	{
		psxRcntWmode16(1, 0x0101 | (i << 1) | 0x10);
		psxRcntWtarget16(1, 3);
		psxCheckStartGate16(0);
		psxVBlankStart();
		psxCheckStartGate16(0);
		psxCheckEndGate16(0);
		psxVBlankEnd();
		psxCheckStartGate16(0);
		psxCheckStartGate16(0);
		note("c1 gate mode %d read %x", i, psxRcntRcount16(1));
		dump("c1 gated");
	}

	/* Counter 2: sysclock / 8, one shot, target. */
	psxRcntWmode16(2, 0x0210);
	psxRcntWtarget16(2, 0x40);
	psxRcntWcount16(2, 0x50);
	dump("c2 count past target");
	advance(0x80000);
	update();
	dump("c2 after wrap");
	psxRcntWmode16(2, 0x0207);
	dump("c2 stopped by mode 7");

	/* Counter 3: hblank alternate source and the vblank gate. */
	psxRcntWmode32(3, 0x0151);
	psxRcntWtarget32(3, 2);
	for (i = 0; i < 6; i++)
	{
		psxCheckStartGate16(0);
		if (i == 2) psxVBlankStart();
		if (i == 4) psxVBlankEnd();
	}
	note("c3 read %x", psxRcntRcount32(3));
	dump("c3 hblanks");

	/* Counters 4 and 5: every prescaler, targets and wrap. */
	for (i = 0; i < 4; i++)
	{
		psxRcntWmode32(4, 0x0058 | (i << 13));
		psxRcntWtarget32(4, 0x1000 + i);
		psxRcntWmode32(5, 0x0038 | (i << 13));
		psxRcntWcount32(5, 0xfffff000u);
		advance(0x40000);
		update();
		note("c4 %x c5 %x", psxRcntRcount32(4), psxRcntRcount32(5));
		dump("c4/c5");
	}
	psxRcntWmode32(4, 0x0001);
	dump("c4 stopped by gate mode 0");
	roundtrip("state");

	/* The interrupt lines. */
	s_dev9_irq = 1;
	dev9Interrupt();
	s_dev9_irq = 0;
	dev9Interrupt();
	dev9Irq(55);
	usbInterrupt();
	usbIrq(66);
	fwIrq();
	spu2Irq();
	iopIntcIrq(17);
	dump("lines");

	/* DMA ICR: every channel enabled or not, master enable or not. */
	for (i = 0; i < 8; i++)
	{
		HW_DMA_ICR = (i & 1 ? 0x007f0000 : 0) | (i & 2 ? 0x00800000 : 0);
		HW_DMA_ICR2 = (i & 1 ? 0x003f0000 : 0) | (i & 2 ? 0x00800000 : 0) | (i & 4 ? 0x3f000000 : 0);
		psxDmaInterrupt(i % 7);
		psxDmaInterrupt2(i % 6);
		psxDmaInterrupt(33);
		psxDmaInterrupt2(33);
		dump("icr");
	}

	/* The channels. */
	psxDma4(0x1000, 0x00100010, 0x01000201);
	psxDma4(0x1000, 0x00100010, 0x01000200);
	psxDma7(0x2000, 0x00080020, 0x01000201);
	psxDma7(0x2000, 0x00080020, 0x01000200);
	psxDma7(0x2000, 0x00080020, 0x00000000);
	dump("spu2 dma");
	HW_DMA6_CHCR = 0x11000002;
	psxDma6(0x4000, 16, 0x11000002);
	for (i = 0; i < 17; i++)
		mix(((u32*)s_iopmem.Main)[(0x4000 >> 2) - i]);
	psxDma6(0x4000, 4, 0x01000000);
	HW_DMA8_CHCR = 0x01000201;
	psxDma8(0x8000, 0x00020010, 0x01000201);
	psxDma8(0x8000, 0x00020010, 0x01000200);
	psxDma8(0x8000, 0x00020010, 0x00000000);
	psxDma9(0, 0, 0);
	psxDma10(0, 0, 0);
	note("sif busy %d %d end %d %d", (int)sif0.iop.busy, (int)sif1.iop.busy, (int)sif0.iop.end, (int)sif1.iop.end);
	psxDma2(0, 0, 0);
	HW_DMA11_CHCR = 0x01000201;
	psxDma11(0x9000, 0x00030002, 0x01000201);
	note("dma11 madr %x block %u", HW_DMA11_MADR, sio2.dmaBlockSize);
	psxDma11(0x9000, 0x00030002, 0x01000200);
	psxDMA11Interrupt();
	psxDMA11Interrupt();
	HW_DMA12_CHCR = 0x41000200;
	psxDma12(0xa000, 0x00020003, 0x41000200);
	psxDma12(0xa000, 0x00020003, 0x01000200);
	note("dma12 madr %x", HW_DMA12_MADR);
	psxDMA12Interrupt();
	psxDMA12Interrupt();
	dump("channels");

	/* The CDVD page. */
	for (i = 0; i < 4; i++)
	{
		psxHw4Write8(0x1f402000 + i * 0x101, (u8)(i * 3));
		note("cdvd read %x", psxHw4Read8(0x1f402004 + i));
	}
}

/* ---- the random part --------------------------------------------- */

static u32 s_rng = 0x1234567u;
static u32 rnd(void)
{
	s_rng = s_rng * 1103515245u + 12345u;
	return s_rng >> 8;
}

static void fuzz(int steps)
{
	static const u32 spu_chcr[] = { 0x01000201, 0x01000200, 0x00000201, 0x01000000 };
	int i;
	s_verbose = false;
	for (i = 1; i <= steps; i++)
	{
		const u32 op = rnd() % 32;
		const u32 v = rnd();
		const int i3 = (int)(rnd() % 3);
		switch (op)
		{
			case 0: case 1: case 2: case 3: advance(v % 4000); break;
			case 4: case 5: update(); break;
			case 6: psxRcntWmode16(i3, v & 0x23ff); break;
			case 7: psxRcntWmode32(3 + i3, v & 0x63ff); break;
			case 8: psxRcntWtarget16(i3, v & 0xffff); break;
			case 9: psxRcntWtarget32(3 + i3, (v % 3) ? (v & 0xffff) : (v << 9)); break;
			case 10: psxRcntWcount16(i3, v & 0xffff); break;
			case 11: psxRcntWcount32(3 + i3, (v % 3) ? (v & 0xfff) : (v << 9)); break;
			case 12: mix(psxRcntRcount16(i3)); break;
			case 13: mix(psxRcntRcount32(3 + i3)); break;
			case 14: case 15: psxCheckStartGate16(0); break;
			case 16: psxCheckEndGate16(0); break;
			case 17: psxVBlankStart(); break;
			case 18: psxVBlankEnd(); break;
			case 19: HW_DMA_ICR = v << 8; psxDmaInterrupt((v & 1) ? 33 : (int)(v % 7)); break;
			case 20: HW_DMA_ICR2 = v << 8; psxDmaInterrupt2((v & 1) ? 33 : (int)(v % 6)); break;
			case 21: psxDma4(v & 0x1ffff0, (v & 0x000f000f) | 0x00010001, spu_chcr[v % 4]); break;
			case 22: psxDma7(v & 0x1ffff0, (v & 0x000f000f) | 0x00010001, spu_chcr[v % 4]); break;
			case 23: HW_DMA11_CHCR = (v & 0x10) ? 0x01000000 : 0; psxDma11(v & 0xfffff, (v & 0x00030003) | 0x00010001, (v & 4) ? 0x01000201 : 0); break;
			case 24: HW_DMA12_CHCR = (v & 0x10) ? 0x01000000 : 0; psxDma12(v & 0xfffff, (v & 0x00030003) | 0x00010001, (v & 4) ? 0x41000200 : 0); break;
			case 25: if (v & 1) psxDMA11Interrupt(); else psxDMA12Interrupt(); break;
			case 26: s_dev9_irq = (int)(v & 1); dev9Interrupt(); break;
			case 27: psxDma8(v & 0xffff0, v & 0x000f00ff, (v & 2) ? 0x01000201 : 0x01000200); break;
			case 28: iopIntcIrq(v % 26); break;
			case 29: psxHw4Write8(v, (u8)(v >> 8)); mix(psxHw4Read8(v >> 3)); break;
			case 30: if ((v % 64) == 0) { EmuConfig.UseBOOT2Injection = (v & 64) != 0; psxHwReset(); } break;
			case 31: if ((v % 16) == 0) roundtrip("fuzz"); break;
		}
		mix(op);
		mix_state();
		if (i % 500 == 0)
			fprintf(s_out, "fuzz %d: %016llx\n", i, s_hash);
	}
}

int main(int argc, char** argv)
{
	unsigned i;
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <transcript out>\n", argv[0]);
		return 2;
	}
	s_out = fopen(argv[1], "w");
	if (!s_out)
		return 2;
	/* The first 2 MB of the IOP's space is its RAM, mirrored four times;
	 * the rest falls through to iopMemRead8_slow. */
	for (i = 0; i < 0x80; i++)
		s_rlut[i] = (uptr)(s_iopmem.Main + ((i & 0x1f) << 16));
	s_verbose = true;
	walk();
	mix_state();
	fprintf(s_out, "walk: %016llx\n", s_hash);
	fuzz(20000);
	fclose(s_out);
	return 0;
}
