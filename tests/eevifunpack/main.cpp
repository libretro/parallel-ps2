/* The VIF unpackers (pcsx2/Vif_Unpack.c), driven with seeded UNPACKs on
 * both units.
 *
 * Each step sets an UNPACK up from a random VIFcode and register state,
 * then hands it its data in random chunks, so whole and partial transfers
 * both happen; the recompiled unpacker is a stand-in that takes the
 * fallback every recompiler has, the interpreted loop, so the loop and
 * the per-vector workers write VU memory for real. With the VU1 thread
 * on, VIF1's data goes to a recorder. The interpreted kernels the loop
 * calls when MODE is not in use (nVifUpk) are filled with the table's own
 * workers. VU memory, both VIF states and registers, the partial-transfer
 * buffers and every call made go into a running hash, written out every
 * so many scenarios. The transcript is compared with expected.txt, which
 * this harness made from the C++ these units were converted from
 * (EEVIFUNPACK_TREE=<that checkout>): the conversion changes none of it.
 *
 * Every UNPACK is set up before its data comes, as on the console; the C
 * build checks in fixes() the states only a savestate can produce, which
 * the C++ wrote past its buffer or divided by zero on. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "Config.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "VU.h"
#include "Vif_Unpack.h"
#ifdef EEVIFUNPACK_CXX_TREE
#include "MTVU.h"
#include "Vif_Dynarec.h"
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

static u32 s_rng = 0x1f83d9abu;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static u32 rnd_n(u32 n) { return rnd() % n; }

/* ---- what the unit reaches ----------------------------------------- */

alignas(16) u8 eeHw[0x10000];
alignas(16) VURegs vuRegs[2];
alignas(16) vifStruct vif0, vif1;
alignas(16) static u8 s_vumem0[0x1000], s_vumem1[0x4000];
alignas(16) static u8 s_src[0x10000];

static bool s_thread_vu1;
alignas(16) u8 s_emuconfig[sizeof(Pcsx2Config)];
asm(".globl EmuConfig\n.set EmuConfig, s_emuconfig");

void vifExecQueue(int idx) { mix(0x01); mix((u32)idx); }
void dVifReset(int idx) { mix(0x02); mix((u32)idx); }
/* The VU1 thread takes VIF1's state with the data and runs the same loop
 * on its own copy. */
static void mtvu_unpack(const u8* data, u32 size);

/* The recompiler, taking its interpreter fallback. */
static void dynarec(int idx, const u8* data, int isFill)
{
	VIFregisters& regs = idx ? vif1Regs : vif0Regs;
	mix(0x04);
	mix((u32)idx);
	mix((u32)isFill);
	_nVifUnpack(idx, data, regs.mode, isFill != 0);
}

static void mtvu_run(vifStruct* mvif, VIFregisters* mregs, const u8* data, u32 size)
{
	const u32 wl = vif1Regs.cycle.wl ? vif1Regs.cycle.wl : 256;
	mix(0x03);
	mix(size);
	mix_bytes(data, size);
	memcpy(mvif, &vif1, sizeof(vif1));
	memcpy(mregs, &vif1Regs, sizeof(vif1Regs));
	_nVifUnpack(1, data, mregs->mode, vif1Regs.cycle.cl < wl);
	mix_bytes(mvif, sizeof(*mvif));
	mix_bytes(mregs, sizeof(*mregs));
}

#ifdef EEVIFUNPACK_CXX_TREE
EevifunpackVuThread vu1Thread;
void eevifunpack_mtvu_unpack(const u8* data, u32 size) { mtvu_unpack(data, size); }
template <> void dVifUnpack<0>(const u8* data, int isFill) { dynarec(0, data, isFill); }
template <> void dVifUnpack<1>(const u8* data, int isFill) { dynarec(1, data, isFill); }
#define MTVU_VIF      (vu1Thread.vif)
#define MTVU_VIFREGS  (vu1Thread.vifRegs)
static void mtvu_unpack(const u8* data, u32 size) { mtvu_run(&vu1Thread.vif, &vu1Thread.vifRegs, data, size); }
#define SETUP(i, d)   ((i) ? vifUnpackSetup<1>(d) : vifUnpackSetup<0>(d))
#define UNPACK(i, d)  ((i) ? nVifUnpack<1>(d) : nVifUnpack<0>(d))
#define PARTIAL(i)    (nVif[i])
#else
alignas(16) static vifStruct s_mtvu_vif;
alignas(16) static VIFregisters s_mtvu_regs;
extern "C" {
vifStruct* mtvu_vif(void) { return &s_mtvu_vif; }
VIFregisters* mtvu_vif_regs(void) { return &s_mtvu_regs; }
void mtvu_vif_unpack(const u8* data, u32 size) { mtvu_unpack(data, size); }
void dvif_unpack(int idx, const u8* data, int isFill) { dynarec(idx, data, isFill); }
bool emu_config_thread_vu1(void) { return s_thread_vu1; }
}
#define MTVU_VIF      (s_mtvu_vif)
#define MTVU_VIFREGS  (s_mtvu_regs)
static void mtvu_unpack(const u8* data, u32 size) { mtvu_run(&s_mtvu_vif, &s_mtvu_regs, data, size); }
#define SETUP(i, d)   vif_unpack_setup((i), (d))
#define UNPACK(i, d)  vif_unpack((i), (d))
#define PARTIAL(i)    (vifUnpackBuffer[i])
#endif

/* The interpreted kernels the loop calls when MODE is not in use, as the
 * recompilers' would: the table's own worker for the unit's state. */
template <int idx>
static u32 kernel(void* dest, const void* src)
{
	vifStruct& vif     = (idx && s_thread_vu1) ? MTVU_VIF : (idx ? vif1 : vif0);
	VIFregisters& regs = (idx && s_thread_vu1) ? MTVU_VIFREGS : (idx ? vif1Regs : vif0Regs);
	const int usn      = !!vif.usn;
	const int upkNum   = vif.cmd & 0x1f;
	const int ftIdx    = ((upkNum & 0x10) ? 32 : 0) + (usn ? 16 : 0) + (upkNum & 0x0f);
#ifdef EEVIFUNPACK_CXX_TREE
	VIFfuncTable[idx][0][ftIdx](dest, src);
#else
	VIFfuncTable[0][ftIdx](dest, src, &vif, &regs);
#endif
	return 0;
}

static void set_kernels(int idx)
{
	int i;
	for (i = 0; i < (2 * 2 * 16) * 4; i++)
		nVifUpk[i] = idx ? kernel<1> : kernel<0>;
}

/* ---- the world ----------------------------------------------------- */

static const u8 kUnpacks[] = {
	0x60, 0x61, 0x62, 0x64, 0x65, 0x66, 0x68, 0x69, 0x6a, 0x6c, 0x6d, 0x6e, 0x6f,
	0x70, 0x71, 0x72, 0x74, 0x75, 0x76, 0x78, 0x79, 0x7a, 0x7c, 0x7d, 0x7e, 0x7f,
	0x63, 0x6b,
};

static void random_unit(int idx)
{
	vifStruct& v       = idx ? vif1 : vif0;
	VIFregisters& regs = idx ? vif1Regs : vif0Regs;

	memset(&v, 0, sizeof(v));
	regs.cycle.cl = (u8)rnd_n(6);
	regs.cycle.wl = (u8)(rnd_n(8) == 0 ? 0 : rnd_n(6));
	if (regs.cycle.cl == 0 && rnd_n(2))
		regs.cycle.cl = 1;
	regs.mode  = rnd_n(4);
	regs.mask  = rnd();
	regs.tops  = rnd() & 0x3ff;
	v.unpackcalls = rnd_n(5);
	{
		int i;
		for (i = 0; i < 4; i++)
		{
			v.MaskRow._u32[i] = rnd();
			v.MaskCol._u32[i] = rnd();
		}
	}
	PARTIAL(idx).bSize = 0;
}

static void reset_world(void)
{
	u32 i;
	memset(eeHw, 0, sizeof(eeHw));
	memset(s_vumem0, 0, sizeof(s_vumem0));
	memset(s_vumem1, 0, sizeof(s_vumem1));
	vuRegs[0].Mem = s_vumem0;
	vuRegs[1].Mem = s_vumem1;
	for (i = 0; i < sizeof(s_src); i += 4)
	{
		const u32 w = rnd();
		memcpy(&s_src[i], &w, 4);
	}
	s_thread_vu1 = rnd_n(4) == 0;
	{
		Pcsx2Config* c = (Pcsx2Config*)s_emuconfig;
		c->Cpu.Recompiler.EnableVU1          = s_thread_vu1;
		c->Speedhacks.vuThread               = s_thread_vu1;
		c->Cpu.Recompiler.EnableVu1SoftFloat = false;
	}
	resetNewVif(0);
	resetNewVif(1);
	random_unit(0);
	random_unit(1);
	memset(&MTVU_VIF, 0, sizeof(MTVU_VIF));
	memset(&MTVU_VIFREGS, 0, sizeof(MTVU_VIFREGS));
}

static void mix_state(void)
{
	mix_bytes(&eeHw[0x3800], 0x100);
	mix_bytes(&eeHw[0x3c00], 0x100);
	mix_bytes(&vif0, sizeof(vif0));
	mix_bytes(&vif1, sizeof(vif1));
	mix(PARTIAL(0).bSize);
	mix(PARTIAL(1).bSize);
	mix_bytes(nVifMask, sizeof(nVifMask));
}

/* One UNPACK on a unit: set up from a random code, then its data in
 * chunks until it is all taken. */
static void one_unpack(int idx)
{
	vifStruct& v       = idx ? vif1 : vif0;
	VIFregisters& regs = idx ? vif1Regs : vif0Regs;
	const u8 cmd       = kUnpacks[rnd_n(sizeof(kUnpacks))];
	u32 pos            = rnd_n(0x8000) & ~3u;
	int guard          = 0;

	if (rnd_n(5) == 0)
	{
		regs.cycle.cl = (u8)rnd_n(6);
		regs.cycle.wl = (u8)rnd_n(6);
		if (regs.cycle.cl == 0)
			regs.cycle.cl = 1;
	}
	if (rnd_n(4) == 0)
		regs.mode = rnd_n(4);
	if (rnd_n(4) == 0)
		regs.mask = rnd();

	regs.code = ((u32)cmd << 24) | ((rnd_n(6) == 0 ? 0 : rnd_n(64)) << 16) | (rnd() & 0xc3ff);
	v.cmd     = cmd;
	v.vifpacketsize = 1 + rnd_n(64);
	set_kernels(idx);
	SETUP(idx, (const u32*)&s_src[pos]);
	mix_state();

	while (v.pass == 1 && guard++ < 400)
	{
		int took;
		v.vifpacketsize = 1 + rnd_n(rnd_n(3) ? 32 : 400);
		took = UNPACK(idx, &s_src[pos]);
		mix((u32)took);
		pos = (pos + (u32)took * 4) & 0x7ffc;
		mix_state();
	}
}

static void run(FILE* out)
{
	int scen, i;

	for (scen = 0; scen < 2000; scen++)
	{
		reset_world();
		for (i = 0; i < 12; i++)
			one_unpack((int)(rnd() & 1));
		mix_bytes(s_vumem0, sizeof(s_vumem0));
		mix_bytes(s_vumem1, sizeof(s_vumem1));
		if ((scen + 1) % 50 == 0)
			fprintf(out, "%4d %016llx\n", scen + 1, s_hash);
	}
	fprintf(out, " all %016llx\n", s_hash);
}

/* ---- what the conversion fixed ------------------------------------- */

#ifndef EEVIFUNPACK_CXX_TREE
static int s_failed;

static void expect(bool ok, const char* what)
{
	if (!ok)
	{
		fprintf(stderr, "eevifunpack: %s\n", what);
		s_failed = 1;
	}
}

static void fixes(void)
{
	int took;

	/* A partial UNPACK restored nearly full, then more data than the
	 * buffer has room for: dropped, not written past it. */
	reset_world();
	s_thread_vu1 = false;
	vif0.cmd = 0x6c;
	vif0.pass = 1;
	vif0.tag.size = 100;
	vif0.vifpacketsize = 8;
	vif0Regs.cycle.cl = 4;
	vif0Regs.cycle.wl = 4;
	vif0Regs.num = 100;
	vifUnpackBuffer[0].bSize = sizeof(vifUnpackBuffer[0].buffer) - 16;
	took = vif_unpack(0, s_src);
	expect(took == 8 && vifUnpackBuffer[0].bSize == 0 && vif0.cmd == 0 && vif0.tag.size == 0,
	       "an UNPACK with no room left in the buffer is dropped");

	/* A partial UNPACK of a reserved format, which sizes to nothing: NUM
	 * would be divided by its zero size. */
	reset_world();
	s_thread_vu1 = false;
	vif1.cmd = 0x63;
	vif1.pass = 1;
	vif1.tag.size = 10;
	vif1.vifpacketsize = 4;
	vif1Regs.cycle.cl = 4;
	vif1Regs.cycle.wl = 4;
	vif1Regs.num = 10;
	took = vif_unpack(1, s_src);
	expect(took == 4 && vif1.cmd == 0 && vifUnpackBuffer[1].bSize == 0,
	       "a partial UNPACK of a reserved format is dropped");

	/* A partial filling UNPACK with CL 0: NUM would be divided by CL. */
	reset_world();
	s_thread_vu1 = false;
	vif0.cmd = 0x6c;
	vif0.pass = 1;
	vif0.tag.size = 10;
	vif0.vifpacketsize = 4;
	vif0Regs.cycle.cl = 0;
	vif0Regs.cycle.wl = 4;
	vif0Regs.num = 10;
	took = vif_unpack(0, s_src);
	expect(took == 4 && vif0.cmd == 0 && vifUnpackBuffer[0].bSize == 0,
	       "a partial filling UNPACK with CL 0 is dropped");
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
#ifndef EEVIFUNPACK_CXX_TREE
	fixes();
	return s_failed;
#else
	return 0;
#endif
}
