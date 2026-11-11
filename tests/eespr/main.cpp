/* The scratchpad DMA channels (pcsx2/SPR.c), driven with seeded
 * transfers in both directions: normal, chain and interleave mode, into
 * and out of main memory, the zero sink, the scratchpad's own window and
 * VU memory, through the MFIFO and around it.
 *
 * Each scenario fills the scratchpad and two windows of main memory with
 * quadwords that are all well-formed tags, so whatever either channel
 * takes for a tag points somewhere sane, sets a channel up and then pokes
 * it in a random order: kicks, interrupt deliveries, restarts. Every
 * register the channels own, every call they make outward, their saved
 * state and the memory they write go into a running hash, written out
 * every so many scenarios. The transcript is compared with expected.txt,
 * which this harness made from the C++ SPR.c was converted from
 * (EESPR_TREE=<that checkout>): the conversion changes none of it.
 * Transfers stay clear of the ends of the memories, where the C++ ran a
 * copy past the array it was in, and of the bus errors it dereferenced;
 * the C build checks those separately, in fixes(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "Config.h"
#include "R5900.h"
#include "Dmac.h"
#include "VUmicro.h"
#include "MTVU.h"
#include "SPR.h"
#include "HotConfig.h"

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

static u32 s_rng = 0xbb67ae85u;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static u32 rnd_n(u32 n) { return rnd() % n; }

/* ---- what the unit reaches ----------------------------------------- */

alignas(16) cpuRegisters cpuRegs;
alignas(16) VURegs vuRegs[2];
alignas(4096) u8 eeHw[PS2MEM_HARDWARE];
EEVM_MemoryAllocMess* eeMem;
static u8* s_vumem;

/* THREAD_VU1 reads EmuConfig in the C++; the C asks through a call. */
u32 emu_hot; /* HotConfig.h; Pcsx2Config.c is not linked */
static bool s_thread_vu1;
alignas(16) u8 s_emuconfig[sizeof(Pcsx2Config)];
asm(".globl EmuConfig\n.set EmuConfig, s_emuconfig");
#ifndef EESPR_CXX_TREE
#endif
static void set_thread_vu1(bool on)
{
	Pcsx2Config* c = (Pcsx2Config*)s_emuconfig;
	s_thread_vu1 = on;
	emu_hot = on ? (emu_hot | EMU_HOT_VU1_THREAD) : (emu_hot & ~EMU_HOT_VU1_THREAD);
	c->Cpu.Recompiler.EnableVU1          = on;
	c->Speedhacks.vuThread               = on;
	c->Cpu.Recompiler.EnableVu1SoftFloat = false;
}

static int s_pending[2];

void CPU_INT(EE_EventType n, s32 ecycle)
{
	mix(0x01); mix((u32)n); mix((u32)ecycle);
	if (n == DMAC_FROM_SPR) s_pending[0] = 1;
	if (n == DMAC_TO_SPR) s_pending[1] = 1;
}
void hwDmacIrq(int n) { mix(0x02); mix((u32)n); }
bool hwMFIFOWrite(u32 addr, const u128* data, uint qwc)
{
	mix(0x03); mix(addr); mix(qwc);
	/* The source is the scratchpad: never past its end. */
	mix_bytes(data, (size_t)(qwc > 0x400 ? 0x400 : qwc) * 16);
	return true;
}
void hwMFIFOResume(u32 transferred) { mix(0x04); mix(transferred); }
void _vu0FinishMicro(void) { mix(0x05); }
void vucpu_execute_block(const VUmicroCpu* cpu, int startUp) { mix(0x06); mix((u32)cpu->idx); mix((u32)startUp); }
void mtvu_wait_vu(void) { mix(0x07); }
static void vu_exec(u32 cycles) { mix(0x08); mix(cycles); }
static void vu0_clear(u32 addr, u32 size) { mix(0x09); mix(addr); mix(size); }
static void vu1_clear(u32 addr, u32 size) { mix(0x0a); mix(addr); mix(size); }
static VUmicroCpu s_vu0cpu = { 0, 0, NULL, NULL, NULL, vu_exec, vu0_clear, NULL };
static VUmicroCpu s_vu1cpu = { 1, 0, NULL, NULL, NULL, vu_exec, vu1_clear, NULL };
const VUmicroCpu* CpuVU0 = &s_vu0cpu;
const VUmicroCpu* CpuVU1 = &s_vu1cpu;
alignas(16) u8 s_vu1thread[sizeof(VU_Thread)];
asm(".globl vu1Thread\n.set vu1Thread, s_vu1thread");

/* The savestate calls: saving mixes what the unit keeps. */
static const u8* s_load;
void SaveState_FreezeMem(SaveStateBase* s, void* data, int size)
{
	if (s->is_saving)
		mix_bytes(data, (size_t)size);
	else if (s_load)
	{
		memcpy(data, s_load, (size_t)size);
		s_load += size;
	}
}
bool SaveState_FreezeTag(SaveStateBase* s, const char* src) { (void)s; (void)src; return true; }

/* ---- the layout ---------------------------------------------------- */

static const u32 kMainA  = 0x00200000;
static const u32 kMainB  = 0x01e00000;
static const u32 kWindow = 0x10000;
static const u32 kMargin = 0x8000;

/* A main-memory address with room after it for any one transfer. */
static u32 main_addr(void)
{
	return (rnd() & 1 ? kMainA : kMainB) + (rnd_n((kWindow - kMargin) >> 4) << 4);
}

/* Where data may go or come from: main memory (also through a kseg
 * mirror), the zero sink, VU memory, low enough that a short run stays
 * inside it. Not the scratchpad's own window: a copy of the scratchpad
 * onto itself overlaps, which the C++ left to memcpy; fixes() has it. */
static u32 any_addr(void)
{
	u32 v;
	switch (rnd_n(11))
	{
		case 0: v = 0x08000000u | (rnd_n(0x1000) << 4); break;
		case 1: v = 0x11000000u | (rnd_n(0x40) << 4); break;  /* VU0 micro */
		case 2: v = 0x11004000u | (rnd_n(0x40) << 4); break;  /* VU0 data */
		case 3: v = 0x11008000u | (rnd_n(0x200) << 4); break; /* VU1 micro */
		case 4: v = 0x1100c000u | (rnd_n(0x200) << 4); break; /* VU1 data */
		case 5: v = 0x20000000u | main_addr(); break;
		default: v = main_addr(); break;
	}
	/* Now and then not on a quadword: the DMAC drops the low bits. */
	return v | (rnd_n(4) == 0 ? rnd() & 0xc : 0);
}

/* A quadword that reads as a sane tag whichever way it is taken: a short
 * count, any ID, maybe IRQ, and an address the other side can use. For
 * SPR1 (source chain) the address may become TADR, so it stays in main
 * memory where every quadword is such a tag. */
static void safe_tag(u8* q, bool source)
{
	u32 w[4];
	w[0] = rnd_n(9) | (rnd_n(8) << 28) | (rnd_n(6) == 0 ? 0x80000000u : 0);
	w[1] = source ? main_addr() : any_addr();
	w[2] = rnd();
	w[3] = rnd();
	memcpy(q, w, 16);
}

static void fill_tags(u8* p, u32 bytes, bool source)
{
	u32 i;
	for (i = 0; i < bytes; i += 16)
		safe_tag(p + i, source);
}

static void reset_world(void)
{
	memset(eeHw, 0, sizeof(eeHw));
	memset(&cpuRegs, 0, sizeof(cpuRegs));
	memset(s_pending, 0, sizeof(s_pending));
	fill_tags(eeMem->Scratch, PS2MEM_SCRATCH, false);
	fill_tags(&eeMem->Main[kMainA], kWindow, true);
	fill_tags(&eeMem->Main[kMainB], kWindow, true);
	fill_tags(s_vumem, VU_MEMORY_RESERVE_SIZE, false);
	memset(eeMem->ZeroWrite, 0, 0x1000);
	vuRegs[0].VI[REG_VPU_STAT].UL = rnd() & 0x101;
	set_thread_vu1(rnd_n(3) == 0);
}

static void mix_state(void)
{
	SaveStateBase s;
	mix_bytes(&eeHw[0xd000], 0x500);
	mix_bytes(&eeHw[0xe000], 0x100);
	mix(cpuRegs.cycle);
	mix(vuRegs[1].cycle);
	memset(&s, 0, sizeof(s));
	s.is_saving = true;
	sprFreeze(&s);
}

static void mix_memory(void)
{
	mix_bytes(eeMem->Scratch, PS2MEM_SCRATCH);
	mix_bytes(&eeMem->Main[kMainA], kWindow);
	mix_bytes(&eeMem->Main[kMainB], kWindow);
	mix_bytes(eeMem->ZeroWrite, 0x1000);
	mix_bytes(s_vumem, VU_MEMORY_RESERVE_SIZE);
}

static void random_dmac(void)
{
	dmacRegs.ctrl._u32  = rnd() & 0x1ff;
	dmacRegs.stadr._u32 = main_addr();
	dmacRegs.sqwc._u32  = rnd_n(9) | (rnd_n(9) << 16);
	if (rnd_n(3) == 0)
	{
		/* An MFIFO ring in main memory, sometimes with no size. */
		dmacRegs.rbor._u32 = (rnd() & 1 ? kMainA : kMainB) + 0x1000;
		dmacRegs.rbsr._u32 = rnd_n(4) == 0 ? 0 : 0xff0;
	}
}

/* A channel: interleave and chain need no tag in the way; normal mode
 * moves a short run. */
static void lay(DMACh* ch, bool source)
{
	const u32 mod = rnd_n(5);
	ch->chcr._u32 = 0x100 | (mod < 2 ? CHAIN_MODE << 2 : mod < 4 ? NORMAL_MODE : 2 << 2);
	if (rnd_n(2)) ch->chcr.TIE = 1;
	if (source && rnd_n(4) == 0) ch->chcr.TTE = 1;
	if (rnd_n(3) == 0) ch->chcr._u32 |= (rnd_n(8) << 28) | (rnd() & 0x80000000u);
	ch->qwc  = ch->chcr.MOD == CHAIN_MODE && rnd_n(2) ? 0 : rnd_n(40);
	/* Interleave runs madr across TQWC+SQWC per block: main memory. */
	ch->madr = ch->chcr.MOD == 2 ? main_addr() : any_addr();
	ch->sadr = rnd_n(0x400) << 4;
	ch->tadr = main_addr();
	random_dmac();
	/* Straight into the MFIFO ring, from near the scratchpad's end. */
	if (!source && dmacRegs.rbor._u32 && rnd_n(3) == 0)
	{
		ch->madr = dmacRegs.rbor._u32 + (rnd_n(4) << 4);
		ch->sadr = 0x3f00 + (rnd_n(16) << 4);
	}
}

static void step(int dir)
{
	DMACh* ch = dir ? &spr1ch : &spr0ch;
	switch (rnd_n(6))
	{
		case 0:
		case 1:
			if (dir) dmaSPR1(); else dmaSPR0();
			break;
		case 2:
		case 3:
			if (s_pending[dir])
			{
				s_pending[dir] = 0;
				if (dir) SPRTOinterrupt(); else SPRFROMinterrupt();
			}
			break;
		case 4:
			if (!ch->chcr.STR && rnd_n(2))
			{
				ch->chcr.STR = 1;
				if (rnd_n(2)) ch->qwc = rnd_n(12);
				/* A fresh address, so no run creeps up on a window's end. */
				ch->madr = ch->chcr.MOD == 2 ? main_addr() : any_addr();
				if (dir) dmaSPR1(); else dmaSPR0();
			}
			break;
		default:
			if (rnd_n(4) == 0) random_dmac();
			if (rnd_n(4) == 0) vuRegs[0].VI[REG_VPU_STAT].UL = rnd() & 0x101;
			break;
	}
}

static void run(FILE* out)
{
	int dir, scen, i;
	static const char* names[] = { "spr0", "spr1" };

	for (dir = 0; dir < 2; dir++)
	{
		for (scen = 0; scen < 1500; scen++)
		{
			reset_world();
			lay(dir ? &spr1ch : &spr0ch, dir == 1);
			mix_state();
			for (i = 0; i < 30; i++)
			{
				step(dir);
				mix_state();
			}
			mix_memory();
			if ((scen + 1) % 50 == 0)
				fprintf(out, "%s %4d %016llx\n", names[dir], scen + 1, s_hash);
		}
		mix_bytes(eeMem->Main, PS2MEM_MAIN_RAM);
		fprintf(out, "%s  all %016llx\n", names[dir], s_hash);
	}
}

/* ---- what the conversion fixed ------------------------------------- */

#ifndef EESPR_CXX_TREE
static int s_failed;

static void expect(bool ok, const char* what)
{
	if (!ok)
	{
		fprintf(stderr, "eespr: %s\n", what);
		s_failed = 1;
	}
}

static void fill_bytes(u8* p, u8 v, size_t n) { memset(p, v, n); }

static void fixes(void)
{
	u32 i;
	bool ok;

	/* fromSPR into VU0 data memory, more than its 4 KiB: the window
	 * mirrors, and VU1's micro memory after it is left alone. */
	reset_world();
	fill_bytes(vuRegs[1].Micro, 0x5a, VU1_PROGSIZE);
	for (i = 0; i < 0x2000; i++) eeMem->Scratch[i] = (u8)(i * 7);
	memset(&spr0ch, 0, sizeof(spr0ch));
	spr0ch.chcr._u32 = 0x100;
	spr0ch.qwc  = 0x200;            /* 8 KiB */
	spr0ch.madr = 0x11004000;
	spr0ch.sadr = 0;
	dmaSPR0();
	ok = true;
	for (i = 0; i < VU1_PROGSIZE; i++) ok = ok && vuRegs[1].Micro[i] == 0x5a;
	expect(ok, "fromSPR stays inside VU0 data memory");
	expect(vuRegs[0].Mem[0] == eeMem->Scratch[0x1000] && vuRegs[0].Mem[0xfff] == eeMem->Scratch[0x1fff],
		"fromSPR mirrors through the VU0 data window");
	expect(spr0ch.qwc == 0 && spr0ch.madr == 0x11006000, "fromSPR moves the whole run");

	/* toSPR from the last quadwords of main RAM reads on from the zero
	 * source, not from the scratchpad behind it. */
	reset_world();
	memset(eeMem->ZeroRead, 0, 32);
	memset(&eeMem->Main[PS2MEM_MAIN_RAM - 16], 0x66, 16);
	memset(eeMem->Scratch, 0x11, 64);
	memset(&spr1ch, 0, sizeof(spr1ch));
	spr1ch.chcr._u32 = 0x101;
	spr1ch.qwc  = 2;
	spr1ch.madr = PS2MEM_MAIN_RAM - 16;
	spr1ch.sadr = 0x100;
	dmaSPR1();
	expect(eeMem->Scratch[0x100] == 0x66 && eeMem->Scratch[0x110] == 0 && eeMem->Scratch[0x11f] == 0,
		"toSPR reads past main RAM from the zero source");

	/* Into the scratchpad's own window near its end: it wraps rather
	 * than running into the ROM after it. */
	reset_world();
	memset(eeMem->ROM, 0x44, 64);
	memset(&spr0ch, 0, sizeof(spr0ch));
	spr0ch.chcr._u32 = 0x100;
	spr0ch.qwc  = 4;
	spr0ch.madr = 0x70003fe0u;
	spr0ch.sadr = 0x1000;
	dmaSPR0();
	expect(eeMem->ROM[0] == 0x44 && eeMem->ROM[63] == 0x44, "fromSPR into its own window stays in the scratchpad");

	/* The scratchpad onto itself, one quadword on: the DMAC copies front
	 * to back, so the first quadword is repeated down the run. */
	reset_world();
	memset(&spr0ch, 0, sizeof(spr0ch));
	spr0ch.chcr._u32 = 0x100;
	spr0ch.qwc  = 4;
	spr0ch.madr = 0x70000110u;
	spr0ch.sadr = 0x100;
	{
		u8 first[16];
		memcpy(first, &eeMem->Scratch[0x100], 16);
		dmaSPR0();
		ok = true;
		for (i = 0; i < 5; i++) ok = ok && memcmp(&eeMem->Scratch[0x100 + i * 16], first, 16) == 0;
		expect(ok, "fromSPR onto the scratchpad copies front to back");
	}

	/* Interleave with nothing behind the address: the C++ copied from
	 * and to NULL. */
	reset_world();
	memset(&spr1ch, 0, sizeof(spr1ch));
	spr1ch.chcr._u32 = 0x100 | (2 << 2);
	spr1ch.qwc  = 4;
	spr1ch.madr = 0x12000000;
	dmacRegs.sqwc._u32 = 1 | (2 << 16);
	dmaSPR1();
	expect(spr1ch.qwc == 0, "toSPR interleave from a bus error finishes");
	memset(&spr0ch, 0, sizeof(spr0ch));
	spr0ch.chcr._u32 = 0x100 | (2 << 2);
	spr0ch.qwc  = 4;
	spr0ch.madr = 0x12000000;
	dmaSPR0();
	expect(spr0ch.qwc == 0, "fromSPR interleave to a bus error finishes");

	/* A source-chain tag address with nothing behind it: BEIS, and the
	 * channel stops; the C++ read the tag through NULL. */
	reset_world();
	memset(&spr1ch, 0, sizeof(spr1ch));
	spr1ch.chcr._u32 = 0x100 | (CHAIN_MODE << 2);
	spr1ch.tadr = 0x12000000;
	dmaSPR1();
	expect(dmacRegs.stat.BEIS && !spr1ch.chcr.STR, "toSPR chain stops on a bus error at TADR");
}
#endif

int main(int argc, char** argv)
{
	FILE* out;

	eeMem   = (EEVM_MemoryAllocMess*)calloc(1, sizeof(*eeMem));
	s_vumem = (u8*)calloc(1, VU_MEMORY_RESERVE_SIZE + 0x10000);
	if (!eeMem || !s_vumem || argc < 2)
		return 2;
	vuRegs[0].Micro = s_vumem;
	vuRegs[0].Mem   = s_vumem + VU0_PROGSIZE;
	vuRegs[1].Micro = s_vumem + VU0_PROGSIZE + VU0_MEMSIZE;
	vuRegs[1].Mem   = s_vumem + VU0_PROGSIZE + VU0_MEMSIZE + VU1_PROGSIZE;
	out = fopen(argv[1], "w");
	if (!out)
		return 2;
	run(out);
	fclose(out);
#ifndef EESPR_CXX_TREE
	fixes();
	free(eeMem);
	free(s_vumem);
	return s_failed;
#else
	free(eeMem);
	free(s_vumem);
	return 0;
#endif
}
