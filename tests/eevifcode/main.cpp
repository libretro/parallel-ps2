/* The VIFcode interpreter (pcsx2/Vif_Transfer.c, Vif_Codes.c), driven
 * with seeded VIFcode streams on both units.
 *
 * Each scenario builds a stream of VIFcodes with their data -- every
 * command, unknown ones and the interrupt bit included -- and feeds it to
 * VIF0transfer/VIF1transfer in chunks of random size, as source data and
 * as DMA tags, picking up after a stall where the interpreter left off.
 * The GIF unit, the VU1 thread, the VUs and the unpackers are recorders
 * that answer from a sequence; the unpacker stand-in does the real one's
 * bookkeeping on the tag. Both VIF states, their registers and channels,
 * the GIF registers, every call made and the VU micro memory written go
 * into a running hash, written out every so many scenarios. The
 * transcript is compared with expected.txt, which this harness made from
 * the C++ these units were converted from (EEVIFCODE_TREE=<that
 * checkout>): the conversion changes none of it. */
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
#ifdef EEVIFCODE_CXX_TREE
#include "Gif_Unit.h"
#include "MTVU.h"
#else
#include "Gif_hw.h"
#endif
#include "ps2/HwInternal.h"
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

static u32 s_rng = 0x3c6ef372u;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static u32 rnd_n(u32 n) { return rnd() % n; }

static u32 s_tick = 0xa54ff53au;
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
u32 g_vif0Cycles, g_vif1Cycles;
alignas(16) static u8 s_micro0[0x1000], s_micro1[0x4000];

u32 emu_hot; /* HotConfig.h; Pcsx2Config.c is not linked */
static bool s_thread_vu1, s_instant_vu1, s_vif1stall;
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
	s_vif1stall   = rnd_n(3) == 0;
	c->Cpu.Recompiler.EnableVU1          = s_thread_vu1;
	c->Speedhacks.vuThread               = s_thread_vu1;
	c->Cpu.Recompiler.EnableVu1SoftFloat = false;
	c->Speedhacks.vu1Instant             = s_instant_vu1;
	c->Gamefixes.VIF1StallHack           = s_vif1stall;
}

void CPU_INT(EE_EventType n, s32 ecycle) { answer(0x01, (u32)n, (u32)ecycle); }
void hwDmacIrq(int n) { answer(0x02, (u32)n, 0); }
void hwIntcIrq(int n) { answer(0x03, (u32)n, 0); }
void gifInterrupt(void) { answer(0x04, 0, 0); }

/* The VUs: starting a program may leave the unit busy, which holds the
 * next one queued. */
void vu0ExecMicro(u32 addr)
{
	if (answer(0x08, addr, 0) & 1)
		vuRegs[0].VI[REG_VPU_STAT].UL |= 0x1;
}
void vu1ExecMicro(u32 addr)
{
	if (answer(0x09, addr, 0) & 1)
		vuRegs[0].VI[REG_VPU_STAT].UL |= 0x100;
}
static void clear0(u32 addr, u32 size) { answer(0x0a, addr, size); }
static void clear1(u32 addr, u32 size) { answer(0x0b, addr, size); }
static VUmicroCpu s_vu0cpu = { 0, 0, NULL, NULL, NULL, NULL, clear0, NULL };
static VUmicroCpu s_vu1cpu = { 1, 0, NULL, NULL, NULL, NULL, clear1, NULL };
const VUmicroCpu* CpuVU0 = &s_vu0cpu;
const VUmicroCpu* CpuVU1 = &s_vu1cpu;
void mtvu_write_micro_mem(u32 addr, const void* data, u32 size)
{
	answer(0x0c, addr, size);
	mix_bytes(data, size);
}

/* The VIF DMA's flush: the VU may still be running, and a VIF waiting on
 * it stalls. */
static void flush(int idx)
{
	vifStruct* v = idx ? &vif1 : &vif0;
	if ((answer(0x0d, (u32)idx, v->waitforvu) & 7) == 0)
		v->waitforvu = true;
	if (v->waitforvu)
	{
		v->vifstalled.enabled = (idx ? vif1ch : vif0ch).chcr.STR;
		v->vifstalled.value   = VIF_TIMING_BREAK;
		(idx ? vif1Regs : vif0Regs).stat.VEW = true;
	}
}
void vif0FLUSH(void) { flush(0); }
void vif1FLUSH(void) { flush(1); }

/* The GIF unit and the VU1 thread. */
static int rec_check_paths(bool a, bool b, bool c) { return (int)((answer(0x10, a | (b << 1) | (c << 2), 0) % 3) == 0); }
static u32 rec_transfer(u8* mem, u32 size, bool hl)
{
	const u32 a = answer(0x11, size, hl);
	mix_bytes(mem, size);
	return (a & 3) ? size : (a % (size / 16 + 1)) * 16;
}
static void rec_col(void) { answer(0x12, 0, 0); mix_bytes(&vif1.MaskCol, 16); }
static void rec_row(void) { answer(0x13, 0, 0); mix_bytes(&vif1.MaskRow, 16); }

/* The unpackers: setup sizes the transfer, the data is taken until it is
 * all there, as the real ones count it. */
static void unpack_setup(int idx, const u32* data)
{
	vifStruct* v = idx ? &vif1 : &vif0;
	const u32 a  = answer(0x14, (u32)idx, (idx ? vif1Regs : vif0Regs).code);
	v->unpackcalls++;
	v->tag.size = 1 + a % 24;
	v->tag.addr = (a >> 8) & 0x3ff0;
	v->pass     = 1;
	(void)data;
}
static int unpack_data(int idx, const u8* data)
{
	vifStruct* v  = idx ? &vif1 : &vif0;
	const int ret = pcsx2_min_i(v->vifpacketsize, v->tag.size);
	answer(0x15, (u32)idx, (u32)ret);
	mix_bytes(data, (size_t)ret * 4);
	if ((u32)ret == v->tag.size)
	{
		v->pass     = 0;
		v->tag.size = 0;
		v->cmd      = 0;
	}
	else
		v->tag.size -= ret;
	return ret;
}

#ifdef EEVIFCODE_CXX_TREE
int eevifcode_check_paths(bool p1, bool p2, bool p3) { return rec_check_paths(p1, p2, p3); }
u32 eevifcode_transfer(u8* mem, u32 size, bool hl) { return rec_transfer(mem, size, hl); }
void eevifcode_write_col(void) { rec_col(); }
void eevifcode_write_row(void) { rec_row(); }
EevifcodeUnit gifUnit;
EevifcodeVuThread vu1Thread;
template <> void vifUnpackSetup<0>(const u32* data) { unpack_setup(0, data); }
template <> void vifUnpackSetup<1>(const u32* data) { unpack_setup(1, data); }
template <> int nVifUnpack<0>(const u8* data) { return unpack_data(0, data); }
template <> int nVifUnpack<1>(const u8* data) { return unpack_data(1, data); }
#else
int  gif_unit_check_paths(bool p1, bool p2, bool p3) { return rec_check_paths(p1, p2, p3); }
u32  gif_unit_transfer_direct(u8* mem, u32 size, bool hl) { return rec_transfer(mem, size, hl); }
void mtvu_write_col(void) { rec_col(); }
void mtvu_write_row(void) { rec_row(); }
void vif_unpack_setup(int idx, const u32* data) { unpack_setup(idx, data); }
int  vif_unpack(int idx, const u8* data) { return unpack_data(idx, data); }
bool emu_config_vif1stall_hack(void) { return s_vif1stall; }
#endif

/* ---- the stream ---------------------------------------------------- */

static const u8 kCodes[] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x10, 0x11, 0x13, 0x14, 0x15, 0x17,
	0x20, 0x30, 0x31, 0x4a, 0x50, 0x51,
	0x60, 0x65, 0x6c, 0x70, 0x75, 0x7c,
	0x08, 0x12, 0x16, 0x21, 0x40, 0x67, 0x7f,
};

enum { kStreamWords = 4096 };
alignas(16) static u32 s_stream[kStreamWords + 64];
static u32 s_len;

static void put(u32 w)
{
	if (s_len < kStreamWords)
		s_stream[s_len++] = w;
}

static void build_stream(void)
{
	s_len = 0;
	while (s_len < kStreamWords - 600)
	{
		const u8 cmd = kCodes[rnd_n(sizeof(kCodes))];
		u32 imm = rnd() & 0xffff, num = rnd_n(256), i, n = 0;
		switch (cmd)
		{
			case 0x20: n = 1; break;
			case 0x30:
			case 0x31: n = 4; break;
			case 0x4a: num = rnd_n(8) ? 1 + rnd_n(16) : 0; n = num ? num * 2 : 512; break;
			case 0x50:
			case 0x51: imm = 1 + rnd_n(4); n = imm * 4; break;
			default:
				if (cmd >= 0x60)
					n = rnd_n(30);
				break;
		}
		put(imm | (num << 16) | ((u32)cmd << 24) | (rnd_n(8) == 0 ? 0x80000000u : 0));
		for (i = 0; i < n; i++)
			put(rnd());
	}
}

/* ---- the world ----------------------------------------------------- */

static void random_vif(int idx)
{
	vifStruct* v       = idx ? &vif1 : &vif0;
	VIFregisters& regs = idx ? vif1Regs : vif0Regs;
	DMACh& ch          = idx ? vif1ch : vif0ch;

	v->irq             = rnd_n(8) == 0;
	v->queued_program  = rnd_n(3) == 0;
	v->queued_pc       = rnd() & 0x7ff;
	v->queued_gif_wait = rnd() & 1;
	v->unpackcalls     = rnd_n(5);
	v->irqoffset.enabled = rnd_n(4) == 0;
	v->irqoffset.value   = rnd_n(4);
	v->inprogress      = (u8)(rnd() & 0x13);
	regs.err._u32   = rnd_n(3) == 0 ? (rnd() & 7) : 0;
	regs.stat._u32  = rnd() & 0x3f0f;
	regs.cycle.cl   = (u8)rnd_n(5);
	regs.cycle.wl   = (u8)rnd_n(5);
	regs.base       = rnd() & 0x3ff;
	regs.ofst       = rnd() & 0x3ff;
	regs.tops       = rnd() & 0x3ff;
	regs.itops      = rnd() & 0x7ff;
	ch.chcr._u32    = 0x100 | (rnd() & 0x5);
	ch.qwc          = rnd_n(64);
	ch.madr         = (rnd() & 0xffff0);
	ch.tadr         = (rnd() & 0xffff0);
}

static void reset_world(void)
{
	memset(eeHw, 0, sizeof(eeHw));
	memset(&cpuRegs, 0, sizeof(cpuRegs));
	memset(&vif0, 0, sizeof(vif0));
	memset(&vif1, 0, sizeof(vif1));
	memset(s_micro0, 0, sizeof(s_micro0));
	memset(s_micro1, 0, sizeof(s_micro1));
	vuRegs[0].Micro = s_micro0;
	vuRegs[1].Micro = s_micro1;
	g_vif0Cycles = g_vif1Cycles = 0;
	set_config();
	random_vif(0);
	random_vif(1);
	vuRegs[0].VI[REG_VPU_STAT].UL = rnd() & 0x101;
	dmacRegs.ctrl._u32 = rnd() & 0xc;
	gifRegs.stat._u32  = rnd() & 0xc00;
	build_stream();
}

static void mix_state(void)
{
	mix_bytes(&eeHw[0x3000], 0x100);
	mix_bytes(&eeHw[0x3800], 0x800);
	mix_bytes(&eeHw[0x8000], 0x100);
	mix_bytes(&eeHw[0x9000], 0x100);
	mix_bytes(&vif0, sizeof(vif0));
	mix_bytes(&vif1, sizeof(vif1));
	mix(g_vif0Cycles);
	mix(g_vif1Cycles);
	mix(cpuRegs.dmastall);
	mix(vuRegs[0].VI[REG_VPU_STAT].UL);
}

/* Resume after whatever stopped the last transfer, the way the DMA side
 * and the VUs finishing would. */
static void unstall(vifStruct* v)
{
	if (v->vifstalled.enabled && rnd_n(3))
		v->vifstalled.enabled = false;
	if (v->waitforvu && rnd_n(2))
		v->waitforvu = false;
	if (v->irq && rnd_n(3) == 0)
		v->irq = 0;
	if (rnd_n(4) == 0)
		vuRegs[0].VI[REG_VPU_STAT].UL &= ~(rnd_n(2) ? 0x1u : 0x100u);
	if (rnd_n(6) == 0)
		vif1Regs.stat.VGW = false;
}

static void run(FILE* out)
{
	int scen, i;

	for (scen = 0; scen < 2000; scen++)
	{
		u32 pos[2] = { 0, 0 };
		reset_world();
		mix_state();
		for (i = 0; i < 120; i++)
		{
			const int idx = (int)(rnd() & 1);
			vifStruct* v  = idx ? &vif1 : &vif0;
			const bool tte = rnd_n(6) == 0;
			int size, took;
			bool ok;

			unstall(v);
			if (pos[idx] >= s_len - 64)
				pos[idx] = 0;
			if (tte)
				size = rnd_n(2) ? 2 : 4;
			else
				size = 1 + (int)rnd_n(rnd_n(3) ? 16 : 64);
			ok = idx ? VIF1transfer(&s_stream[pos[idx]], size, tte)
			         : VIF0transfer(&s_stream[pos[idx]], size, tte);
			took = size - (int)v->vifpacketsize;
			mix(ok);
			mix((u32)took);
			pos[idx] += (u32)took;
			mix_state();
		}
		mix_bytes(s_micro0, sizeof(s_micro0));
		mix_bytes(s_micro1, sizeof(s_micro1));
		if ((scen + 1) % 50 == 0)
			fprintf(out, "%4d %016llx\n", scen + 1, s_hash);
	}
	fprintf(out, " all %016llx\n", s_hash);
}

int main(int argc, char** argv)
{
	FILE* out;

	eeMem = (EEVM_MemoryAllocMess*)calloc(1, sizeof(*eeMem));
	if (!eeMem || argc < 2)
		return 2;
	out = fopen(argv[1], "w");
	if (!out)
		return 2;
	run(out);
	fclose(out);
	free(eeMem);
	return 0;
}
