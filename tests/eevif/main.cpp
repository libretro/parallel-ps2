/* The VIF DMA channels (pcsx2/Vif0_Dma.c, Vif1_Dma.c, Vif1_MFIFO.c),
 * driven with seeded chains, downloads and MFIFO transfers.
 *
 * The VIF code interpreter they feed (VIF0transfer/VIF1transfer) is a
 * stand-in here that records what it was handed and takes some or all of
 * it, with the bookkeeping the real one does on MADR, QWC and the IRQ
 * offset; the GIF unit, the VU1 thread, the MTGS download and the VUs
 * are recorders that answer from a sequence. Main memory is filled with
 * quadwords that all read as sane source tags. Every register, both VIF
 * states, every call made and the memory written go into a running hash,
 * written out every so many scenarios. The transcript is compared with
 * expected.txt, which this harness made from the C++ these units were
 * converted from (EEVIF_TREE=<that checkout>): the conversion changes
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
#include "Vif.h"
#include "Vif_Dma.h"
#include "VUmicro.h"
#include "vtlb.h"
#ifdef EEVIF_CXX_TREE
#include "Gif_Unit.h"
#include "MTVU.h"
#include "GS.h"
#else
#include "Gif_hw.h"
#include "ps2/HwInternal.h"
#include "HotConfig.h"
#endif

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

static u32 s_rng = 0x510e527fu;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static u32 rnd_n(u32 n) { return rnd() % n; }

static u32 s_tick = 0x9b05688cu;
static u32 answer(u32 tag, u32 a, u32 b)
{
	mix(tag); mix(a); mix(b);
	s_tick = s_tick * 1664525u + 1013904223u;
	return s_tick >> 8;
}

/* ---- what the units reach ------------------------------------------ */

alignas(16) cpuRegisters cpuRegs;
alignas(16) VURegs vuRegs[2];
alignas(4096) u8 eeHw[PS2MEM_HARDWARE];
EEVM_MemoryAllocMess* eeMem;
alignas(16) vifStruct vif0, vif1;


u32 emu_hot; /* HotConfig.h; Pcsx2Config.c is not linked */
static bool s_thread_vu1, s_instant_vu1, s_vusync, s_vif1stall;
/* What the core reads is the hot config's bit (HotConfig.h). */
static void set_thread_vu1(bool on)
{
	s_thread_vu1 = on;
	emu_hot = on ? (emu_hot | EMU_HOT_VU1_THREAD) : (emu_hot & ~EMU_HOT_VU1_THREAD);
}
static void set_instant_vu1(bool on)
{
	s_instant_vu1 = on;
	emu_hot = on ? (emu_hot | EMU_HOT_VU1_INSTANT) : (emu_hot & ~EMU_HOT_VU1_INSTANT);
}
alignas(16) u8 s_emuconfig[sizeof(Pcsx2Config)];
asm(".globl EmuConfig\n.set EmuConfig, s_emuconfig");
static void set_config(void)
{
	Pcsx2Config* c = (Pcsx2Config*)s_emuconfig;
	set_thread_vu1(rnd_n(3) == 0);
	set_instant_vu1(rnd_n(2) == 0);
	s_vusync      = rnd_n(3) == 0;
	s_vif1stall   = rnd_n(3) == 0;
	c->Cpu.Recompiler.EnableVU1          = s_thread_vu1;
	c->Speedhacks.vuThread               = s_thread_vu1;
	c->Cpu.Recompiler.EnableVu1SoftFloat = false;
	c->Speedhacks.vu1Instant             = s_instant_vu1;
	c->Gamefixes.VUSyncHack              = s_vusync;
	c->Gamefixes.VIF1StallHack           = s_vif1stall;
}

void CPU_INT(EE_EventType n, s32 ecycle) { answer(0x01, (u32)n, (u32)ecycle); }
void hwDmacIrq(int n) { answer(0x02, (u32)n, 0); }
void hwIntcIrq(int n) { answer(0x03, (u32)n, 0); }
int cpuGetCycles(void) { return (int)(answer(0x04, 0, 0) & 0xfff); }
void FireMFIFOEmpty(void) { answer(0x05, 0, 0); }
void vifExecQueue(int idx) { answer(0x06, (u32)idx, 0); }
void mmap_UnprotectRamRange(u32 paddr, u32 size) { answer(0x07, paddr, size); }
void* vtlb_GetPhyPtr(u32 paddr) { return paddr < PS2MEM_MAIN_RAM ? eeMem->Main + paddr : NULL; }
/* The VUs: each call may finish what is running, which ends the loops
 * that wait on them. */
void vucpu_execute_block(const VUmicroCpu* cpu, int startUp)
{
	if (answer(0x08, (u32)(cpu == CpuVU1), (u32)startUp) & 1)
		vuRegs[0].VI[REG_VPU_STAT].UL &= cpu == CpuVU1 ? ~0x100u : ~0x1u;
	vuRegs[cpu == CpuVU1].cycle += 50;
}
void vu0Finish(void) { if (answer(0x09, 0, 0) & 1) vuRegs[0].VI[REG_VPU_STAT].UL &= ~0x1u; vuRegs[0].cycle += 30; }
void vu1Finish(bool add) { if (answer(0x0a, add, 0) & 1) vuRegs[0].VI[REG_VPU_STAT].UL &= ~0x100u; vuRegs[1].cycle += 30; }
static VUmicroCpu s_vu0cpu = { 0, 0, NULL, NULL, NULL, NULL, NULL, NULL };
static VUmicroCpu s_vu1cpu = { 1, 0, NULL, NULL, NULL, NULL, NULL, NULL };
const VUmicroCpu* CpuVU0 = &s_vu0cpu;
const VUmicroCpu* CpuVU1 = &s_vu1cpu;

/* The GIF unit, the VU1 thread and the MTGS download. */
static int rec_check_paths(bool a, bool b, bool c) { return (int)(answer(0x10, a | (b << 1) | (c << 2), 0) & 1); }
static int rec_execute(bool a, bool b) { return (int)answer(0x11, a, b); }
static bool rec_can2(void) { return (answer(0x12, 0, 0) & 3) != 0; }
static bool rec_can2hl(void) { return (answer(0x13, 0, 0) & 3) != 0; }
static bool rec_done(int p) { return (answer(0x14, (u32)p, 0) & 1) != 0; }
static int rec_state(int p) { return (int)(answer(0x15, (u32)p, 0) % 5); }
static void rec_changes(void) { answer(0x16, 0, 0); }
static void rec_kick(void) { answer(0x17, 0, 0); }
static void rec_read_fifo(u8* mem, u32 qwc)
{
	u32 i;
	answer(0x18, (u32)(mem - eeMem->Main), qwc);
	for (i = 0; i < qwc * 16; i++)
		mem[i] = (u8)(i * 13 + qwc);
}
#ifdef EEVIF_CXX_TREE
int  eevif_check_paths(bool p1, bool p2, bool p3) { return rec_check_paths(p1, p2, p3); }
int  eevif_execute(bool a, bool b) { return rec_execute(a, b); }
bool eevif_can_do_path2(void) { return rec_can2(); }
bool eevif_can_do_path2hl(void) { return rec_can2hl(); }
bool eevif_path_is_done(int p) { return rec_done(p); }
int  eevif_path_state(int p) { return rec_state(p); }
void eevif_mtvu_changes(void) { rec_changes(); }
void eevif_mtvu_kick(void) { rec_kick(); }
void eevif_read_fifo(u8* mem, u32 qwc) { rec_read_fifo(mem, qwc); }
EevifUnit gifUnit;
EevifVuThread vu1Thread;
#else
int  gif_unit_check_paths(bool p1, bool p2, bool p3) { return rec_check_paths(p1, p2, p3); }
int  gif_unit_execute(bool a, bool b) { return rec_execute(a, b); }
bool gif_unit_can_do_path2(void) { return rec_can2(); }
bool gif_unit_can_do_path2hl(void) { return rec_can2hl(); }
bool gif_path_is_done(int p) { return rec_done(p); }
bool gif_path_is_idle(int p) { return rec_state(p) == GIF_PATH_IDLE; }
void mtvu_get_changes(void) { rec_changes(); }
void mtvu_kick_pending(void) { rec_kick(); }
void mtgs_init_and_read_fifo(u8* mem, u32 qwc) { rec_read_fifo(mem, qwc); }
bool emu_config_vusync_hack(void) { return s_vusync; }
bool emu_config_vif1stall_hack(void) { return s_vif1stall; }
#endif

/* The VIF code interpreter: what it was handed goes into the record, it
 * takes all or some of it, may raise an IRQ stall, and does the real
 * one's bookkeeping on the channel. */
static int s_last_size;
static bool vif_transfer(int idx, u32* data, int size, bool TTE)
{
	vifStruct* vifX = idx ? &vif1 : &vif0;
	DMACh* ch       = idx ? &vif1ch : &vif0ch;
	int transferred = vifX->irqoffset.enabled ? vifX->irqoffset.value : 0;
	const u32 a     = answer(0x20 + idx, (u32)size, TTE);
	int took;

	s_last_size = size;
	mix_bytes(data, (size_t)(size < 64 ? size : 64) * 4);
	took = (a & 3) ? size : (int)(a % (u32)(size + 1));
	if (a & 0x40)
		vifX->irq = 1;
	if (a & 0x80)
		vifX->cmd = (u8)(a >> 8);
	transferred += took;
	if (idx) g_vif1Cycles += pcsx2_max_i(1, (transferred * BIAS) >> 2);
	else     g_vif0Cycles += pcsx2_max_i(1, (transferred * BIAS) >> 2);
	vifX->irqoffset.value = transferred % 4;
	if (vifX->irq && vifX->cmd == 0)
	{
		vifX->vifstalled.enabled = ch->chcr.STR;
		vifX->vifstalled.value   = VIF_IRQ_STALL;
	}
	if (!TTE)
	{
		transferred = transferred >> 2;
		transferred = pcsx2_min_i((int)ch->qwc, transferred);
		ch->madr += transferred << 4;
		ch->qwc  -= transferred;
		hwDmacSrcTadrInc(ch);
		vifX->irqoffset.enabled = false;
		if (!ch->qwc)
			vifX->inprogress &= ~0x1;
		else if (vifX->irqoffset.value != 0)
			vifX->irqoffset.enabled = true;
	}
	else
		vifX->irqoffset.enabled = vifX->irqoffset.value != 0;
	return !vifX->vifstalled.enabled;
}
bool VIF0transfer(u32* data, int size, bool TTE) { return vif_transfer(0, data, size, TTE); }
bool VIF1transfer(u32* data, int size, bool TTE) { return vif_transfer(1, data, size, TTE); }

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

static void random_vif(vifStruct* v)
{
	v->done        = rnd() & 1;
	v->inprogress  = (u8)(rnd() & 0x11);
	v->irq         = rnd_n(4) == 0;
	v->cmd         = rnd_n(3) == 0 ? (u8)(0x50 + rnd_n(2)) : (u8)(rnd() & 0x7f);
	v->waitforvu   = rnd_n(4) == 0;
	v->dmamode     = (u8)rnd_n(3);
	v->irqoffset.enabled  = rnd_n(4) == 0;
	v->irqoffset.value    = rnd_n(4);
	v->vifstalled.enabled = rnd_n(4) == 0;
	v->vifstalled.value   = rnd_n(3);
	v->queued_program     = rnd() & 1;
	v->GSLastDownloadSize = rnd_n(40);
}

static void random_channel(DMACh* ch)
{
	ch->chcr._u32 = 0x100 | (rnd_n(2) << 2) | (rnd() & 0xc1);
	if (rnd_n(4) == 0)
		ch->chcr._u32 |= (rnd_n(8) << 28) | (rnd() & 0x80000000u);
	ch->qwc  = rnd_n(3) == 0 ? 0 : rnd_n(24);
	ch->madr = main_addr();
	ch->tadr = main_addr();
}

/* The MFIFO cases worth having: VIF1 draining just behind SPR0, near the
 * ring's end, a tag at its very top, the drain caught up. */
static void mfifo_shape(void)
{
	const u32 base = dmacRegs.rbor._u32, top = base + dmacRegs.rbsr._u32;
	switch (rnd_n(6))
	{
		case 0: spr0ch.madr = vif1ch.madr + (rnd_n(16) << 4); break;
		case 1: vif1ch.madr = top - (rnd_n(8) << 4); break;
		case 2: vif1ch.tadr = top - (rnd_n(2) << 4); break;
		case 3: spr0ch.madr = vif1ch.tadr; break;
		case 4:
		{
			/* A REFS tag whose data runs past the stall address. */
			u32 w[2];
			const u32 addr = main_addr();
			w[0] = 8 | (TAG_REFS << 28);
			w[1] = addr;
			if (vif1ch.tadr < PS2MEM_MAIN_RAM - 16)
				memcpy(&eeMem->Main[vif1ch.tadr], w, 8);
			dmacRegs.stadr._u32 = addr + (rnd_n(16) << 4);
			dmacRegs.ctrl._u32  = (dmacRegs.ctrl._u32 & ~0xc0u) | (STD_VIF1 << 6);
			break;
		}
		default: break;
	}
	(void)base;
}

static void reset_world(void)
{
	memset(eeHw, 0, sizeof(eeHw));
	memset(&cpuRegs, 0, sizeof(cpuRegs));
	memset(&vif0, 0, sizeof(vif0));
	memset(&vif1, 0, sizeof(vif1));
	memset(vuRegs, 0, sizeof(vuRegs));
	fill_tags(&eeMem->Main[kMainA], kWindow);
	fill_tags(&eeMem->Main[kMainB], kWindow);
	set_config();
	random_vif(&vif0);
	random_vif(&vif1);
	random_channel(&vif0ch);
	random_channel(&vif1ch);
	dmacRegs.ctrl._u32  = rnd() & 0x3ff;
	dmacRegs.stadr._u32 = main_addr();
	/* The MFIFO ring in main memory, SPR0 somewhere in it. */
	dmacRegs.rbor._u32  = kMainA + 0x8000;
	dmacRegs.rbsr._u32  = 0x7ff0;
	spr0ch.madr         = dmacRegs.rbor._u32 + (rnd_n(0x800) << 4);
	if (rnd_n(2))
	{
		vif1ch.tadr = dmacRegs.rbor._u32 + (rnd_n(0x800) << 4);
		vif1ch.madr = dmacRegs.rbor._u32 + (rnd_n(0x700) << 4);
	}
	vuRegs[0].VI[REG_VPU_STAT].UL = rnd() & 0x505;
	cpuRegs.cycle = 1000 + rnd_n(500);
	/* The VUs a little behind, level, or ahead of the EE. */
	vuRegs[0].cycle = cpuRegs.cycle - 150 + rnd_n(300);
	vuRegs[1].cycle = cpuRegs.cycle - 150 + rnd_n(300);
	vuRegs[0].nextBlockCycles = rnd_n(200);
	vuRegs[1].nextBlockCycles = rnd_n(200);
	mfifo_shape();
	vif0Regs.stat._u32 = rnd() & 0x3f0f;
	vif1Regs.stat._u32 = rnd() & 0x3f0f;
	vif1Regs.code      = rnd();
	psHu32(0x10003020) = rnd() & 0xe00;
}

static void mix_state(void)
{
	mix_bytes(&eeHw[0x3800], 0x800);
	mix_bytes(&eeHw[0x3020], 4);
	mix_bytes(&eeHw[0x8000], 0x2000);
	mix_bytes(&eeHw[0xd000], 0x100);
	mix_bytes(&eeHw[0xe000], 0x100);
	mix_bytes(&vif0, sizeof(vif0));
	mix_bytes(&vif1, sizeof(vif1));
	mix(g_vif0Cycles);
	mix(g_vif1Cycles);
	mix(cpuRegs.dmastall);
	mix(vuRegs[0].VI[REG_VPU_STAT].UL);
}

static void step(void)
{
	switch (rnd_n(14))
	{
		case 0: dmaVIF0(); break;
		case 1: dmaVIF1(); break;
		case 2:
		case 3: vif0Interrupt(); break;
		case 4:
		case 5: vif1Interrupt(); break;
		case 6: vifMFIFOInterrupt(); break;
		case 7: vif0VUFinish(); break;
		case 8: vif1VUFinish(); break;
		case 9: vif0FLUSH(); break;
		case 10: vif1FLUSH(); break;
		case 11:
			if (!vif1ch.chcr.STR && rnd_n(2)) { vif1ch.chcr.STR = 1; vif1ch.madr = main_addr(); dmaVIF1(); }
			if (!vif0ch.chcr.STR && rnd_n(2)) { vif0ch.chcr.STR = 1; vif0ch.madr = main_addr(); dmaVIF0(); }
			break;
		case 12:
			vuRegs[0].VI[REG_VPU_STAT].UL = rnd() & 0x505;
			cpuRegs.cycle += rnd_n(200);
			break;
		default:
			dmacRegs.ctrl._u32 = rnd() & 0x3ff;
			spr0ch.madr = dmacRegs.rbor._u32 + (rnd_n(0x800) << 4);
			vif1.cmd = rnd_n(2) ? (u8)(0x50 + rnd_n(2)) : (u8)(rnd() & 0x7f);
			mfifo_shape();
			break;
	}
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
		mix_bytes(&eeMem->Main[kMainA], kWindow);
		mix_bytes(&eeMem->Main[kMainB], kWindow);
		if ((scen + 1) % 50 == 0)
			fprintf(out, "%4d %016llx\n", scen + 1, s_hash);
	}
	mix_bytes(eeMem->Main, PS2MEM_MAIN_RAM);
	fprintf(out, " all %016llx\n", s_hash);
}

/* ---- what the conversion fixed ------------------------------------- */

#ifndef EEVIF_CXX_TREE
/* The chain steps on their own (Vif0_Dma.c, Vif1_Dma.c). */
extern "C" bool _VIF0chain(void);
extern "C" bool _VIF1chain(void);

static int s_failed;

static void expect(bool ok, const char* what)
{
	if (!ok)
	{
		fprintf(stderr, "eevif: %s\n", what);
		s_failed = 1;
	}
}

static void fixes(void)
{
	/* VIF1 from the last quadword of main RAM hands the interpreter that
	 * one quadword, not the run past it. */
	reset_world();
	memset(&vif1, 0, sizeof(vif1));
	memset(&vif1ch, 0, sizeof(vif1ch));
	vif1ch.chcr._u32 = 0x101;
	vif1ch.qwc  = 4;
	vif1ch.madr = PS2MEM_MAIN_RAM - 16;
	vif1.inprogress = 1;
	vif1.done = true;
	vif1.dmamode = VIF_NORMAL_FROM_MEM_MODE;
	s_last_size = -1;
	{
		_VIF1chain();
		expect(s_last_size == 4, "VIF1 takes only what is mapped behind MADR");
	}

	/* The same for VIF0. */
	reset_world();
	memset(&vif0, 0, sizeof(vif0));
	memset(&vif0ch, 0, sizeof(vif0ch));
	vif0ch.chcr._u32 = 0x101;
	vif0ch.qwc  = 3;
	vif0ch.madr = PS2MEM_MAIN_RAM - 32;
	vif0.inprogress = 1;
	s_last_size = -1;
	{
		_VIF0chain();
		expect(s_last_size == 8, "VIF0 takes only what is mapped behind MADR");
	}

	/* An MFIFO tag at an address with nothing behind it: a bus error
	 * that stops the channel. The C++ read the tag through NULL. */
	reset_world();
	memset(&vif1, 0, sizeof(vif1));
	memset(&vif1ch, 0, sizeof(vif1ch));
	vif1ch.chcr._u32 = 0x105;
	dmacRegs.rbor._u32 = 0x12000000;
	dmacRegs.rbsr._u32 = 0xff0;
	spr0ch.madr = 0x12000100;
	vif1ch.tadr = 0x12000000;
	mfifoVIF1transfer();
	expect(dmacRegs.stat.BEIS && !vif1ch.chcr.STR && vif1.done, "an MFIFO tag at a bus error stops VIF1");
}
#endif

int main(int argc, char** argv)
{
	FILE* out;

	eeMem = (EEVM_MemoryAllocMess*)calloc(1, sizeof(*eeMem));
	if (!eeMem || argc < 2)
		return 2;
#ifdef EEVIF_CXX_TREE
	gifUnit.gifPath[0].idx = 0; gifUnit.gifPath[0].state.idx = 0;
	gifUnit.gifPath[1].idx = 1; gifUnit.gifPath[1].state.idx = 1;
	gifUnit.gifPath[2].idx = 2; gifUnit.gifPath[2].state.idx = 2;
#endif
	out = fopen(argv[1], "w");
	if (!out)
		return 2;
	run(out);
	fclose(out);
#ifndef EEVIF_CXX_TREE
	fixes();
	free(eeMem);
	return s_failed;
#else
	free(eeMem);
	return 0;
#endif
}
