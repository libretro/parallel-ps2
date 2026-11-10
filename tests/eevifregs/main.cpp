/* The VIF registers (pcsx2/Vif.c), driven with seeded reads and writes on
 * both units.
 *
 * Every register the units handle themselves -- STAT, FBRST, ERR, MARK,
 * MODE, the rows and columns -- and a few they leave to the caller are
 * read and written with random values, the reset bits, the stall cancel
 * and the FIFO direction included, against random channel, DMAC and VIF
 * state, with the VU1 thread on and off. The VU1 thread, the interrupt
 * scheduler and the DMAC IRQ are recorders. Both VIF states, their
 * registers and channels, the GIF status, the VU1 thread's copy, every
 * call made and every value read go into a running hash, written out
 * every so many scenarios. The transcript is compared with expected.txt,
 * which this harness made from the C++ these units were converted from
 * (EEVIFREGS_TREE=<that checkout>): the conversion changes none of it,
 * save that VIF0's row and column are no longer handed to the VU1 thread,
 * which fixes() checks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "Config.h"
#include "R5900.h"
#include "Dmac.h"
#include "Hw.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "VU.h"
#include "VUmicro.h"
#include "Vif_Unpack.h"
#include "SaveState.h"
#include "Gif_hw.h"
#ifdef EEVIFREGS_CXX_TREE
#include "MTVU.h"
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

static u32 s_rng = 0x5be0cd19u;
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
alignas(4096) u8 eeHw[PS2MEM_HARDWARE];
u32 g_vif0Cycles, g_vif1Cycles;
PCSX2_ALIGN(16) VifUnpackBuffer vifUnpackBuffer[2];

u32 emu_hot; /* HotConfig.h; Pcsx2Config.cpp is not linked */
static bool s_thread_vu1;
alignas(16) u8 s_emuconfig[sizeof(Pcsx2Config)];
asm(".globl EmuConfig\n.set EmuConfig, s_emuconfig");

void CPU_INT(EE_EventType n, s32 ecycle) { mix(0x01); mix((u32)n); mix((u32)ecycle); }
void hwDmacIrq(int n) { mix(0x02); mix((u32)n); }
void resetNewVif(int idx) { mix(0x03); mix((u32)idx); }
void mtvu_wait_vu(void) { mix(0x04); }
bool SaveState_FreezeTag(SaveStateBase* s, const char* tag)
{
	mix(0x07);
	mix_bytes(tag, strlen(tag));
	return !s->error;
}
/* Saving records what is written; loading fills the counts and lengths --
 * the 4-byte fields -- with values from the sequence, some past what the
 * buffer can hold. */
void SaveState_FreezeMem(SaveStateBase* s, void* data, int size)
{
	mix(0x08);
	mix((u32)size);
	if (!s->is_saving && size == 4)
	{
		const u32 v = rnd_n(3) ? rnd_n(0x1100) : rnd();
		memcpy(data, &v, 4);
	}
	else
		mix_bytes(data, (size_t)size);
}

/* What the VU1 thread is handed. VIF0's row and column reached it too in
 * the C++, overwriting VIF1's there; those are counted, not recorded. */
static int s_rows, s_cols, s_vif0_forwards;
static void handed(const vifStruct* v, bool col)
{
	if (v == &vif0)
	{
		s_vif0_forwards++;
		return;
	}
	if (col) s_cols++;
	else     s_rows++;
	mix(col ? 0x06 : 0x05);
	mix_bytes(col ? (const void*)&v->MaskCol : (const void*)&v->MaskRow, 16);
}

#ifdef EEVIFREGS_CXX_TREE
EevifregsVuThread vu1Thread;
void eevifregs_write_row(vifStruct& v) { handed(&v, false); }
void eevifregs_write_col(vifStruct& v) { handed(&v, true); }
#define MTVU_VIF (vu1Thread.vif)
#else
alignas(16) static vifStruct s_mtvu_vif;
extern "C" {
vifStruct* mtvu_vif(void) { return &s_mtvu_vif; }
void mtvu_write_row(void) { handed(&vif1, false); }
void mtvu_write_col(void) { handed(&vif1, true); }
}
#define MTVU_VIF (s_mtvu_vif)
#endif

/* ---- the world ----------------------------------------------------- */

static void set_thread_vu1(bool on)
{
	Pcsx2Config* c = (Pcsx2Config*)s_emuconfig;
	s_thread_vu1 = on;
	emu_hot = on ? (emu_hot | EMU_HOT_VU1_THREAD) : (emu_hot & ~EMU_HOT_VU1_THREAD);
	c->Cpu.Recompiler.EnableVU1          = on;
	c->Speedhacks.vuThread               = on;
	c->Cpu.Recompiler.EnableVu1SoftFloat = false;
}

static void random_unit(int idx)
{
	vifStruct& v       = idx ? vif1 : vif0;
	VIFregisters& regs = idx ? vif1Regs : vif0Regs;
	DMACh& ch          = idx ? vif1ch : vif0ch;
	int i;

	for (i = 0; i < 4; i++)
	{
		v.MaskRow._u32[i] = rnd();
		v.MaskCol._u32[i] = rnd();
	}
	v.inprogress = (u8)(rnd() & 0x13);
	v.irq        = rnd() & 1;
	v.cmd        = rnd_n(3) ? 0 : (int)(rnd() & 0x7f);
	v.GSLastDownloadSize = rnd_n(40);
	regs.stat._u32 = rnd() & 0x1f003f7f;
	regs.err._u32  = rnd() & 7;
	ch.chcr._u32   = (rnd() & 0x105);
	ch.qwc         = rnd_n(3) ? 0 : rnd_n(64);
}

static void reset_world(void)
{
	int i;
	memset(eeHw, 0, sizeof(eeHw));
	memset(&cpuRegs, 0, sizeof(cpuRegs));
	memset(&vif0, 0, sizeof(vif0));
	memset(&vif1, 0, sizeof(vif1));
	memset(&MTVU_VIF, 0, sizeof(MTVU_VIF));
	for (i = 0; i < 4; i++)
	{
		MTVU_VIF.MaskRow._u32[i] = rnd();
		MTVU_VIF.MaskCol._u32[i] = rnd();
	}
	set_thread_vu1(rnd_n(3) == 0);
	random_unit(0);
	random_unit(1);
	dmacRegs.ctrl._u32 = rnd() & 0xc;
	cpuRegs.interrupt  = rnd();
	g_vif0Cycles = rnd_n(100);
	g_vif1Cycles = rnd_n(100);
	gifRegs.stat._u32 = rnd() & 0x3;
	for (i = 0; i < 0x20; i++)
		eeHw[0x4000 + i] = (u8)rnd();
}

static void mix_state(void)
{
	mix_bytes(&eeHw[0x3000], 0x100);
	mix_bytes(&eeHw[0x3800], 0x800);
	mix_bytes(&eeHw[0x4000], 0x20);
	mix_bytes(&eeHw[0x8000], 0x100);
	mix_bytes(&eeHw[0x9000], 0x100);
	mix_bytes(&vif0, sizeof(vif0));
	mix_bytes(&vif1, sizeof(vif1));
	mix(cpuRegs.interrupt);
	mix(g_vif0Cycles);
	mix(g_vif1Cycles);
}

/* The VIF0 offsets of the registers worth hitting; VIF1's are 0x400 up. */
static const u32 kRegs[] = {
	0x10003800, 0x10003810, 0x10003820, 0x10003830, 0x10003840, 0x10003850,
	0x10003860, 0x10003870, 0x10003880, 0x10003890, 0x100038a0, 0x100038b0,
	0x10003900, 0x10003910, 0x10003920, 0x10003930,
	0x10003940, 0x10003950, 0x10003960, 0x10003970,
};

static u32 random_value(u32 reg)
{
	switch (reg & 0x3ff)
	{
		case 0x010: return rnd() & 0xf;           /* FBRST */
		case 0x000: return rnd() & 0x00800000u ? rnd() : (rnd() & 0x00800000u); /* STAT: FDR */
		default:    return rnd();
	}
}

static void step(void)
{
	const int idx = (int)(rnd() & 1);
	const u32 mem = kRegs[rnd_n(sizeof(kRegs) / sizeof(kRegs[0]))] + (idx ? 0x400u : 0u);

	switch (rnd_n(9))
	{
		case 0:
		case 1:
		case 2:
			mix(idx ? vif1Read32(mem) : vif0Read32(mem));
			break;
		case 3:
		case 4:
		case 5:
		case 6:
		{
			const u32 value = random_value(mem);
			mix(idx ? vif1Write32(mem, value) : vif0Write32(mem, value));
			break;
		}
		case 7:
			if (rnd_n(3) == 0)
			{
				SaveStateBase st;
				memset(&st, 0, sizeof(st));
				st.is_saving = rnd() & 1;
				st.error     = rnd_n(8) == 0;
				mix(idx ? vif1Freeze(&st) : vif0Freeze(&st));
				mix(vifUnpackBuffer[idx].bSize);
				break;
			}
			/* fallthrough */
		default:
			if (rnd_n(4) == 0)
			{
				if (idx) vif1Reset();
				else     vif0Reset();
			}
			else
				random_unit(idx);
			break;
	}
}

static void run(FILE* out)
{
	int scen, i;

	for (scen = 0; scen < 3000; scen++)
	{
		reset_world();
		mix_state();
		for (i = 0; i < 40; i++)
		{
			step();
			mix_state();
		}
		mix_bytes(&MTVU_VIF, sizeof(MTVU_VIF));
		if ((scen + 1) % 50 == 0)
			fprintf(out, "%4d %016llx\n", scen + 1, s_hash);
	}
	fprintf(out, " all %016llx\n", s_hash);
}

/* ---- what the conversion fixed ------------------------------------- */

#ifndef EEVIFREGS_CXX_TREE
static int s_failed;

static void expect(bool ok, const char* what)
{
	if (!ok)
	{
		fprintf(stderr, "eevifregs: %s\n", what);
		s_failed = 1;
	}
}

static void fixes(void)
{
	/* VIF0's row and column are not handed to the VU1 thread, whose copy
	 * is VIF1's; VIF1's are, with the thread on or off. */
	reset_world();
	set_thread_vu1(true);
	s_rows = s_cols = 0;
	vif0Write32(0x10003900, 0x1234);
	vif0Write32(0x10003940, 0x5678);
	expect(s_rows == 0 && s_cols == 0, "VIF0's row and column stay off the VU1 thread");
	expect(vif0.MaskRow._u32[0] == 0x1234 && vif0.MaskCol._u32[0] == 0x5678, "VIF0's row and column are written");

	vif1Write32(0x10003d10, 0x9abc);
	vif1Write32(0x10003d50, 0xdef0);
	expect(s_rows == 1 && s_cols == 1, "VIF1's row and column reach the VU1 thread");

	set_thread_vu1(false);
	vif1Write32(0x10003d20, 1);
	expect(s_rows == 2, "VIF1's row reaches the VU1 thread with it off too");
}
#endif

int main(int argc, char** argv)
{
	FILE* out;

	if (argc < 2)
		return 2;
	out = fopen(argv[1], "w");
	if (!out)
		return 2;
	run(out);
	fclose(out);
#ifndef EEVIFREGS_CXX_TREE
	fixes();
	if (s_vif0_forwards)
		expect(false, "VIF0's row or column was handed to the VU1 thread");
	return s_failed;
#else
	fprintf(stderr, "eevifregs: the C++ handed VIF0's row or column to the VU1 thread %d times\n", s_vif0_forwards);
	return 0;
#endif
}
