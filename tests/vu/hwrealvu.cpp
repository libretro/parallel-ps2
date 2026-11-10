/*  VU EFU ops against console captures, driving the real code.
 *
 *  tests/vu/hwefu.c transcribes EATAN, ESIN and EEXP out of
 *  pcsx2/VUops.cpp and scores the copy; the three that go through
 *  ps2float.c it links for real. This links VUops.cpp itself and calls
 *  every one of them through the interpreter's own dispatch table.
 *
 *  Reaching them takes the instruction encoding rather than a function
 *  pointer, because the EFU entry points are static. VU0microInterp.c
 *  indexes VU0_LOWER_OPCODE with code >> 25, and the type-3 and type-4
 *  lower ops encode as bit 31 set, the source register at 11, the field
 *  selector at 21, and the opcode in the low ten bits -- the same layout
 *  ps2autotests' own assembler emits. So the harness builds the word and
 *  dispatches exactly as the interpreter does, which is a stronger check
 *  than calling a function directly: it exercises the table as well.
 *
 *  Usage: tests/vu/hwrealvu <path-to-vu/lower/efu.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "VUops.h"
#include "VUmicro.h"
#include "R5900OpcodeTables.h"
#include "ps2/HwInternal.h"

alignas(16) VURegs vuRegs[2];

/* VUops.cpp reaches for these on paths this harness never takes: XGKICK
 * and the MTVU handoff. The EFU ops touch none of it, so they are storage
 * and no-ops rather than behaviour. */
#include "Config.h"
#include "Gif_Unit.h"
#include "MTVU.h"

alignas(16) cpuRegisters cpuRegs;
alignas(16) u8 eeHw[0x10000];
alignas(16) u8 s_emuconfig[sizeof(Pcsx2Config)];
asm(".globl EmuConfig\n.set EmuConfig, s_emuconfig");

/* vuDouble's clamp of a maximal exponent to the largest finite value is
 * gated on Cpu.Recompiler.vu0ExtraOverflow, which defaults off -- it is a
 * per-game fix, not the normal path. The zeroed EmuConfig above therefore
 * matches the default, and that is deliberate: this harness scores the
 * configuration almost every game runs under.
 *
 * It matters more than it looks. With the clamp off, CVF_MAX and CVF_MIN
 * reach the EFU as host NaNs and propagate, so EATAN returns its own
 * input. With it on they clamp and EATAN lands within an ULP -- but ESADD
 * drops from 11 of 16 to 10 and ESUM from 11 to 9, while ERSADD rises
 * from 8 to 11. Neither setting is uniformly closer to the console.
 *
 * This is also what tests/vu/hwefu.c's transcription differs by: it
 * hardcodes the clamp, so it is modelling the non-default configuration.
 * That, rather than any difference in the arithmetic, is why its EATAN
 * scores 8 of 13 where this scores 0 of 16. */
static void vu_config_is_default(void) { }
/* The GIF unit (Gif_Unit.c): XGKICK's way in, never reached here. */
Gif_Unit gifUnit;
extern "C" {
u32 gif_unit_get_packet_size(GIF_PATH, u8*, u32, u32, bool) { return 0; }
void gif_path_copy(Gif_Path*, const u8*, u32) { }
u32 gif_unit_transfer(GIF_TRANSFER_TYPE, u8*, u32) { return 0; }
}
alignas(16) u8 s_vu1thread[sizeof(VU_Thread)];
asm(".globl vu1Thread\n.set vu1Thread, s_vu1thread");

void CPU_INT(EE_EventType, int) { }
/* VU0.c's other dependencies: the VU scheduling and COP2 dispatch that
 * QMFC2 and QMTC2 do not reach. */
void intUpdateCPUCycles() { }
void vu0ResetRegs() { }
void vu1ResetRegs() { }
void vu1ExecMicro(u32) { }
void vu1Finish(bool) { }
void vucpu_execute_block(const VUmicroCpu*, int) { }
/* Declared as pointers to const, so the storage is ours. */
static VUmicroCpu s_vu0cpu, s_vu1cpu;
const VUmicroCpu* CpuVU0 = &s_vu0cpu;
const VUmicroCpu* CpuVU1 = &s_vu1cpu;
void (*Int_COP2BC2PrintTable[32])(void) = {};
void (*Int_COP2SPECIAL1PrintTable[64])(void) = {};
void (*Int_COP2SPECIAL2PrintTable[128])(void) = {};
bool emu_config_enable_ee(void) { return false; }
RETURNS_R128 vtlb_memRead128(u32) { r128 v; memset(&v, 0, sizeof(v)); return v; }
void vtlb_memWrite128(u32, r128) { }

/* Lower-op encoding, as ps2autotests' assembler builds it. */
#define LOWER_TYPE1345 0x80000000u
#define VS(r)   ((u32)(r) << 11)
#define VT(r)   ((u32)(r) << 16)
#define DEST(d) ((u32)(d) << 21)
#define FSF(f)  ((u32)(f) << 21)

/* The EFU opcodes, from tests/vu/assemble/ops.cpp. */
enum {
	OP_ESADD  = 0x73C, OP_ERSADD = 0x73D, OP_ELENG = 0x73E, OP_ERLENG = 0x73F,
	OP_EATANxy = 0x77C, OP_EATANxz = 0x77D, OP_ESUM = 0x77E,
	/* Read from tests/vu/assemble/ops.cpp rather than guessed: these six
	 * sit in two adjacent groups and transposing them within a group still
	 * dispatches to a real EFU op, so the scores come out low but
	 * plausible rather than obviously broken. */
	OP_ESQRT  = 0x7BC, OP_ERSQRT = 0x7BD, OP_ERCPR = 0x7BE,
	OP_ESIN   = 0x7FC, OP_EATAN  = 0x7FD, OP_EEXP  = 0x7FE,
};

#define VF_SRC 1     /* the source register this harness loads */
#define VI_D   1     /* the integer destination and its two sources */
#define VI_S   2
#define VI_T   3

/* Lower type-1 puts the destination at bit 6; type-5 carries a signed
 * five-bit immediate in the same place; type-8 is a different shape
 * entirely, with the opcode at 25 and a fifteen-bit immediate split in
 * two -- eleven bits at 0 and four more at 21. */
#define VD(r)    ((u32)(r) << 6)
/* IMM5 sits at bit 6, not bit 0, where it would collide with the
 * six-bit opcode. */
#define IMM5(v)  (((u32)(v) & 0x1F) << 6)
#define IMM15(v) (((u32)(v) & 0x7FF) | (((u32)(v) & 0x7800) << 10))

enum {
	OP_IADD = 0x30, OP_ISUB = 0x31, OP_IADDI = 0x32,
	OP_IAND = 0x34, OP_IOR = 0x35,
	OP_IADDIU = 0x08, OP_ISUBIU = 0x09,
	OP_RNEXT = 0x43C, OP_RGET = 0x43D, OP_RINIT = 0x43E, OP_RXOR = 0x43F,
};

/* Floors, not targets. The three that go through ps2float.c are exact;
 * the polynomial ones are not, and the interpreter's own accuracy is what
 * tests/vu/hwefu.c already measures and documents. What this table adds is
 * a regression bound on the real code rather than on a copy of it.
 *
 * EATAN scoring 0 here against hwefu.c's 8 of 13 is a configuration
 * difference, not an arithmetic one: see the note on vuDouble's clamp
 * above. hwefu.c hardcodes the clamp and so models vu0ExtraOverflow being
 * on; this runs the default. */
static const struct { const char* name; u32 op; int vector; int floor; } kOps[] = {
	/* The scalar forms take one component through the fsf selector; the
	 * vector forms read x, y and z of the register. */
	{ "ESQRT",  OP_ESQRT,  0, 16 },
	{ "ERSQRT", OP_ERSQRT, 0, 16 },
	{ "ERCPR",  OP_ERCPR,  0, 16 },
	{ "EATAN",  OP_EATAN,  0,  0 },
	{ "ESIN",   OP_ESIN,   0,  8 },
	{ "EEXP",   OP_EEXP,   0,  9 },
	{ "ESADD",  OP_ESADD,  1, 11 },
	{ "ERSADD", OP_ERSADD, 1,  8 },
	{ "ELENG",  OP_ELENG,  1, 11 },
	{ "ERLENG", OP_ERLENG, 1,  5 },
	{ "ESUM",   OP_ESUM,   1, 11 },
};
#define NOPS ((int)(sizeof(kOps)/sizeof(kOps[0])))

/* The constants the efu harness sets up, scalar and vector. */
static const struct { const char* name; u32 x, y, z, w; } kConst[] = {
	{ "CVF_ZERO",         0,0,0,0 },
	{ "CVF_NEGZERO",      0x80000000u,0x80000000u,0x80000000u,0x80000000u },
	{ "CVF_MAX_MANTISSA", 0x3FFFFFFFu,0x3FFFFFFFu,0x3FFFFFFFu,0x3FFFFFFFu },
	{ "CVF_MAX_EXP",      0x7F800001u,0x7F800001u,0x7F800001u,0x7F800001u },
	{ "CVF_MIN_EXP",      1,1,1,1 },
	{ "CVF_MAX",          0x7FFFFFFFu,0x7FFFFFFFu,0x7FFFFFFFu,0x7FFFFFFFu },
	{ "CVF_MIN",          0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu },
	{ "CVF_ONE",          0x3F800000u,0x3F800000u,0x3F800000u,0x3F800000u },
	{ "CVF_NEGONE",       0xBF800000u,0xBF800000u,0xBF800000u,0xBF800000u },
	{ "CVF_GARBAGE1",     0x00001337u,0x00001337u,0x00001337u,0x00001337u },
	{ "CVF_GARBAGE2",     0xDEADBEEFu,0xDEADBEEFu,0xDEADBEEFu,0xDEADBEEFu },
	{ "CVF_INCREASING",   0x3F800000u,0x40000000u,0x40400000u,0x40800000u },
	{ "CVF_DECREASING",   0x40800000u,0x40400000u,0x40000000u,0x3F800000u },
	{ "CVF_PI_OVER2",     0x3FC90FDBu,0x3FC90FDBu,0x3FC90FDBu,0x3FC90FDBu },
	{ "CVF_PI",           0x40490FDBu,0x40490FDBu,0x40490FDBu,0x40490FDBu },
	{ "CVF_3PI_OVER2",    0x4096CBE4u,0x4096CBE4u,0x4096CBE4u,0x4096CBE4u },
};

static int lookup(const char* tok, u32* x, u32* y, u32* z, u32* w)
{
	size_t i, best = (size_t)-1, bestlen = 0;
	for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
	{
		const size_t n = strlen(kConst[i].name);
		if (!strncmp(tok, kConst[i].name, n) && n > bestlen) { best = i; bestlen = n; }
	}
	if (best == (size_t)-1) return 0;
	*x = kConst[best].x; *y = kConst[best].y;
	*z = kConst[best].z; *w = kConst[best].w;
	return 1;
}

/* ---- the integer ops --------------------------------------------------
 *
 * These take their operands in VI registers rather than VF, and the
 * capture prints the destination as four hex digits. The harness that
 * produced it cannot load an arbitrary sixteen-bit value in one
 * instruction, so what a line prints as an operand is not always what the
 * register held: WrSetIntegerRegister emits ISUBIU(r, VI00, v & ~0x8000)
 * for anything with bit 15 set, and the register ends up holding the
 * negation. That model is tests/vu/hwint.c's; it is reproduced here
 * because it belongs to the test program, not to the emulator. */

static const struct { const char* name; u16 v; } kIConst[] = {
	{ "CVI_ZERO",     0x0000u }, { "CVI_MAX",      0x7FFFu },
	{ "CVI_MIN",      0x8000u }, { "CVI_NEGONE",   0xFFFFu },
	{ "CVI_ONE",      0x0001u },
	{ "CVI_GARBAGE1", 0x1337u }, { "CVI_GARBAGE2", 0xBEEFu },
	/* The wider names all narrow to sixteen bits in a VI register. */
	{ "CVI_S16_MAX",  0x7FFFu }, { "CVI_S16_MIN",  0x8000u },
	{ "CVI_S32_MAX",  0xFFFFu }, { "CVI_S32_MIN",  0x0000u },
	{ "CVI_S64_MAX",  0xFFFFu }, { "CVI_S64_MIN",  0x0000u },
};

static u16 setvi(u32 v)
{
	if (v == 0x8000u) return (u16)((u16)0xFFFFu - (u16)0x7FFFu);
	if (v & 0x8000u)  return (u16)(0u - (v & ~0x8000u));
	return (u16)v;
}

/* `named` reports whether the operand was a CVI_ constant. Those are
 * loaded into the register from VU memory, so they arrive intact; a plain
 * decimal goes through WrSetIntegerRegister and is transformed. Applying
 * setvi to both scores 17 to 19 of 21 per block -- close enough to look
 * like an arithmetic edge case rather than a loading model. */
static int resolve_i(const char* tok, u16* out, int* named)
{
	size_t i, best = (size_t)-1, bestlen = 0;
	char* end; long v;
	for (i = 0; i < sizeof(kIConst)/sizeof(kIConst[0]); i++)
	{
		const size_t n = strlen(kIConst[i].name);
		if (!strncmp(tok, kIConst[i].name, n) && n > bestlen) { best = i; bestlen = n; }
	}
	if (best != (size_t)-1) { *out = kIConst[best].v; *named = 1; return 1; }
	v = strtol(tok, &end, 10);
	if (end == tok) return 0;
	*out = (u16)v; *named = 0;
	return 1;
}

enum IForm { I_RRR, I_IMM5, I_IMM15 };

static const struct { const char* name; u32 op; IForm form; } kIOps[] = {
	{ "iadd",   OP_IADD,   I_RRR },
	{ "isub",   OP_ISUB,   I_RRR },
	{ "iand",   OP_IAND,   I_RRR },
	{ "ior",    OP_IOR,    I_RRR },
	{ "iaddi",  OP_IADDI,  I_IMM5 },
	{ "iaddiu", OP_IADDIU, I_IMM15 },
	{ "isubiu", OP_ISUBIU, I_IMM15 },
};
#define NIOPS ((int)(sizeof(kIOps)/sizeof(kIOps[0])))

static int run_integer(const char* dir, int* total)
{
	char path[512];
	int op, bad = 0;

	snprintf(path, sizeof(path), "%s/integer.expected", dir);
	for (op = 0; op < NIOPS; op++)
	{
		FILE* f = fopen(path, "r");
		char buf[512];
		int inblock = 0, pass = 0, cases = 0;

		if (!f) return 0;
		while (fgets(buf, sizeof(buf), f))
		{
			u16 a = 0, b = 0;
			int a_named = 0, b_named = 0;
			unsigned want;
			char *p, *comma, *colon;

			if (!inblock)
			{ if (!strncmp(buf, kIOps[op].name, strlen(kIOps[op].name))
			   && buf[strlen(kIOps[op].name)] == '\n') inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;

			colon = strstr(buf, ": ");
			if (!colon) continue;
			if (sscanf(colon + 2, "%x", &want) != 1) continue;
			p = buf + 2 + strlen(kIOps[op].name);
			while (*p == ' ') p++;
			*colon = '\0';
			comma = strchr(p, ',');
			if (!comma) { *colon = ':'; continue; }
			*comma = '\0';
			if (!resolve_i(p, &a, &a_named)) { *colon = ':'; continue; }
			{ char* q = comma + 1; while (*q == ' ') q++;
			  if (!resolve_i(q, &b, &b_named)) { *colon = ':'; continue; } }
			*colon = ':';

			memset(&vuRegs[0], 0, sizeof(vuRegs[0]));
			vuRegs[0].VI[VI_S].UL = a_named ? a : setvi(a);
			switch (kIOps[op].form)
			{
			case I_RRR:
				vuRegs[0].VI[VI_T].UL = b_named ? b : setvi(b);
				vuRegs[0].code = LOWER_TYPE1345 | VT(VI_T) | VS(VI_S)
				               | VD(VI_D) | kIOps[op].op;
				break;
			case I_IMM5:
				vuRegs[0].code = LOWER_TYPE1345 | VT(VI_D) | VS(VI_S)
				               | IMM5(b) | kIOps[op].op;
				break;
			default:
				vuRegs[0].code = ((u32)kIOps[op].op << 25) | VT(VI_D)
				               | VS(VI_S) | IMM15(b);
				break;
			}
			VU0_LOWER_OPCODE[vuRegs[0].code >> 25]();

			cases++;
			if ((vuRegs[0].VI[VI_D].UL & 0xFFFFu) == want) pass++;
		}
		fclose(f);
		*total += cases;
		/* A block that matched no lines is a harness bug, not a pass. */
		if (cases == 0)
		{ printf("%-7s parsed no cases; the block name or line shape does"
		         " not match the capture\n", kIOps[op].name); bad++; }
		printf("%-7s %2d/%-3d console cases\n", kIOps[op].name, pass, cases);
		if (pass != cases) bad++;
	}
	return bad;
}

/* ---- the random unit --------------------------------------------------
 *
 * RINIT and RXOR keep only the low 23 bits and force the exponent to
 * 0x3f800000, so R always reads back as a float in [1,2); RNEXT advances
 * an LFSR over those bits. The expected values are the same ones
 * tests/vu/hwrandom.c inlines from ps2autotests
 * tests/vu/lower/random.expected -- what differs is that these go through
 * VUops.cpp and its dispatch rather than a copy of the algorithm.
 *
 * RINIT and RXOR are type-4 lower ops taking a field selector; RGET and
 * RNEXT are type-3 and write a VF register, which is where the result is
 * read from rather than from R itself. */

static void vu_rinit(u32 v, int field)
{
	vuRegs[0].VF[VF_SRC].UL[field] = v;
	vuRegs[0].code = LOWER_TYPE1345 | VS(VF_SRC) | FSF((u32)field) | OP_RINIT;
	VU0_LOWER_OPCODE[vuRegs[0].code >> 25]();
}
static void vu_rxor(u32 v, int field)
{
	vuRegs[0].VF[VF_SRC].UL[field] = v;
	vuRegs[0].code = LOWER_TYPE1345 | VS(VF_SRC) | FSF((u32)field) | OP_RXOR;
	VU0_LOWER_OPCODE[vuRegs[0].code >> 25]();
}
static u32 vu_rop(u32 op)
{
	const u32 dst = 2;
	vuRegs[0].VF[dst].UL[0] = 0;
	/* The dest mask runs x at bit 24 down to w at bit 21, so DEST(1) is w,
	 * not x -- writing that and reading lane 0 gets zero every time. All
	 * four lanes are enabled here and lane 0 read back. */
	vuRegs[0].code = LOWER_TYPE1345 | DEST(0xF) | VT(dst) | op;
	VU0_LOWER_OPCODE[vuRegs[0].code >> 25]();
	return vuRegs[0].VF[dst].UL[0];
}

static int run_random(int* total)
{
	static const u32 GARBAGE[4] =
		{ 0x01234567u, 0x89ABCDEFu, 0xFEDCBA98u, 0x76543210u };
	static const u32 ZEROONE[4] =
		{ 0x3F800000u, 0x3FFFFFFFu, 0x00000000u, 0xFFFFFFFFu };
	static const u32 want_init[4] =
		{ 0x3fa34567u, 0x3fabcdefu, 0x3fdcba98u, 0x3fd43210u };
	static const u32 want_xor[4] =
		{ 0x3fa34567u, 0x3f888888u, 0x3fd43210u, 0x3f800000u };
	static const u32 want_next_zeroone[4][4] = {
		{ 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u },
		{ 0x3ffffffeu, 0x3ffffffcu, 0x3ffffff8u, 0x3ffffff0u },
		{ 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u },
		{ 0x3ffffffeu, 0x3ffffffcu, 0x3ffffff8u, 0x3ffffff0u },
	};
	static const u32 want_next_garbage[4][4] = {
		{ 0x3fc68aceu, 0x3f8d159du, 0x3f9a2b3bu, 0x3fb45677u },
		{ 0x3fd79bdeu, 0x3faf37bcu, 0x3fde6f79u, 0x3fbcdef2u },
		{ 0x3fb97530u, 0x3ff2ea61u, 0x3fe5d4c3u, 0x3fcba987u },
		{ 0x3fa86420u, 0x3fd0c840u, 0x3fa19081u, 0x3fc32102u },
	};
	int bad = 0, pass = 0, cases = 0, i, f;

	memset(&vuRegs[0], 0, sizeof(vuRegs[0]));

	for (i = 0; i < 4; i++)
	{
		u32 got;
		vu_rinit(GARBAGE[i], 0);
		got = vu_rop(OP_RGET);
		cases++;
		if (got == want_init[i]) pass++;
		else if (bad++ < 4)
			printf("  RINIT %d: console %08x  ours %08x\n", i, want_init[i], got);
	}

	vu_rinit(0, 0);
	for (i = 0; i < 4; i++)
	{
		u32 got;
		vu_rxor(GARBAGE[i], 0);
		got = vu_rop(OP_RGET);
		cases++;
		if (got == want_xor[i]) pass++;
		else if (bad++ < 4)
			printf("  RXOR %d: console %08x  ours %08x\n", i, want_xor[i], got);
	}

	for (f = 0; f < 4; f++)
	{
		vu_rinit(ZEROONE[f], 0);
		for (i = 0; i < 4; i++)
		{
			const u32 got = vu_rop(OP_RNEXT);
			cases++;
			if (got == want_next_zeroone[f][i]) pass++;
			else if (bad++ < 4)
				printf("  RNEXT zeroone %d step %d: console %08x  ours %08x\n",
				       f, i + 1, want_next_zeroone[f][i], got);
		}
	}
	for (f = 0; f < 4; f++)
	{
		vu_rinit(GARBAGE[f], 0);
		for (i = 0; i < 4; i++)
		{
			const u32 got = vu_rop(OP_RNEXT);
			cases++;
			if (got == want_next_garbage[f][i]) pass++;
			else if (bad++ < 4)
				printf("  RNEXT garbage %d step %d: console %08x  ours %08x\n",
				       f, i + 1, want_next_garbage[f][i], got);
		}
	}

	*total += cases;
	printf("random  %2d/%-3d console cases\n", pass, cases);
	return pass != cases;
}

/* ---- CLIP -------------------------------------------------------------
 *
 * CLIP is an upper op, so it goes through VU0_UPPER_OPCODE[code & 0x3f]
 * rather than the lower table. It compares each of fs's x, y and z
 * against +ft.w and -ft.w and shifts six bits into the clip flag, which
 * the capture reads back with FCGET.
 *
 * The scenarios and their expected flags are the fourteen basic cases in
 * ps2autotests tests/vu/upper/clip.cpp, the same ones tests/vu/hwclip.c
 * inlines; these go through VUops.cpp instead of a copy.
 *
 * What they cover is the comparison: relaxing one of the six tests from
 * greater-than to greater-or-equal fails a case. What they do not cover is
 * anything about how the flag accumulates or about denormals. Each case
 * runs CLIP once from a cleared flag, so the six-bit shift never shows --
 * changing it to five leaves all fourteen passing -- and none of the
 * operands has a denormal w, so dropping the clamp that pins one to the
 * largest denormal passes too. Both are worth saying: 14 of 14 reads like
 * the op is covered, and two thirds of it is not. */

#define OP_CLIP 0x1FF

static const u32 CV_FULLPOS[4]  = {0x40000000u,0x40000000u,0x40000000u,0x40000000u};
static const u32 CV_FULLNEG[4]  = {0xC0000000u,0xC0000000u,0xC0000000u,0xC0000000u};
static const u32 CV_HALFPOS[4]  = {0x40000000u,0xC0000000u,0x40000000u,0xC0000000u};
static const u32 CV_HALFNEG[4]  = {0xC0000000u,0x40000000u,0xC0000000u,0x40000000u};
static const u32 CV_WZERO[4]    = {0,0,0,0};
static const u32 CV_WNEGZERO[4] = {0x80000000u,0x80000000u,0x80000000u,0x80000000u};
static const u32 CV_WNEGONE[4]  = {0xBF800000u,0xBF800000u,0xBF800000u,0xBF800000u};
static const u32 CV_MAX[4]      = {0x7FFFFFFFu,0x7FFFFFFFu,0x7FFFFFFFu,0x7FFFFFFFu};
static const u32 CV_MIN[4]      = {0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu};

static int run_clip(int* total)
{
	static const struct { const char* name; const u32* s; const u32* t; u32 want; }
	kCases[] = {
		{ "FULL POS x -1",       CV_FULLPOS,  CV_WNEGONE,  0x0015 },
		{ "FULL NEG x -1",       CV_FULLNEG,  CV_WNEGONE,  0x002a },
		{ "HALF POS x -1",       CV_HALFPOS,  CV_WNEGONE,  0x0019 },
		{ "HALF NEG x -1",       CV_HALFNEG,  CV_WNEGONE,  0x0026 },
		{ "HALF NEG x  0",       CV_HALFNEG,  CV_WZERO,    0x0026 },
		{ "HALF NEG x -0",       CV_HALFNEG,  CV_WNEGZERO, 0x0026 },
		{ " 0 x -0",             CV_WZERO,    CV_WNEGZERO, 0x0000 },
		{ "-0 x  0",             CV_WNEGZERO, CV_WZERO,    0x0000 },
		{ "FULL POS x FULL POS", CV_FULLPOS,  CV_FULLPOS,  0x0000 },
		{ "HALF NEG x HALF NEG", CV_HALFNEG,  CV_HALFNEG,  0x0000 },
		{ "MIN x 0",             CV_MIN,      CV_WZERO,    0x002a },
		{ "MAX x 0",             CV_MAX,      CV_WZERO,    0x0015 },
		{ "0 x MIN",             CV_WZERO,    CV_MIN,      0x0000 },
		{ "0 x MAX",             CV_WZERO,    CV_MAX,      0x0000 },
	};
	const int n = (int)(sizeof(kCases)/sizeof(kCases[0]));
	int i, pass = 0, shown = 0;

	for (i = 0; i < n; i++)
	{
		u32 got;
		memset(&vuRegs[0], 0, sizeof(vuRegs[0]));
		memcpy(&vuRegs[0].VF[1], kCases[i].s, 16);
		memcpy(&vuRegs[0].VF[2], kCases[i].t, 16);
		vuRegs[0].code = DEST(0xE) | VT(2) | VS(1) | OP_CLIP;
		VU0_UPPER_OPCODE[vuRegs[0].code & 0x3f]();
		got = vuRegs[0].clipflag & 0xFFFFFF;

		if (got == kCases[i].want) pass++;
		else if (shown++ < 4)
			printf("  CLIP %-20s console %04x  ours %04x\n",
			       kCases[i].name, kCases[i].want, got);
	}
	*total += n;
	printf("clip    %2d/%-3d console cases\n", pass, n);
	return pass != n;
}

/* ---- VU0 macro mode --------------------------------------------------
 *
 * The same integer ops again, but reached the way the EE reaches them: as
 * COP2 instructions rather than through the VU's own dispatch. VIADD and
 * friends copy cpuRegs.code into vuRegs[0].code and call the same _vu
 * helpers, so what this adds over the lower-op sweep is the operand
 * decode -- _Id_, _Is_ and _It_ take the low four bits of the VF fields,
 * so a macro-mode instruction naming register 17 addresses VI1.
 *
 * From ps2autotests tests/cpu/vu0_macro/integer.expected. Its harness
 * loads the VI registers the same way the lower-op one does, so setvi
 * applies to the decimal operands here too.
 */

/* The macro-mode entry points (VUops.cpp) and QMFC2/QMTC2 (VU0.c, linked
 * for them) come from R5900OpcodeTables.h. VU0.c's other dependencies are
 * the VU scheduling machinery these two do not reach. */

/* ---- the VU0 quadword moves ------------------------------------------
 *
 * From tests/cpu/vu0_macro/transfer.expected, whose ctc2 half is already
 * scored by tests/vu/hwctc2.c. What was left is QMFC2 and QMTC2, the
 * full-width moves between a VF register and a GPR pair, and the two $0
 * and $vf0 cases: writing either is discarded, and reading VF0 gives the
 * constant (1, 0, 0, 0) rather than whatever was last stored.
 */
static int run_quad_moves(int* total)
{
	int pass = 0, cases = 0;

	/* QMFC2 with rt = 0 discards. */
	memset(&vuRegs[0], 0, sizeof(vuRegs[0]));
	memset(&cpuRegs.GPR, 0, sizeof(cpuRegs.GPR));
	vuRegs[0].VF[1].UD[0] = 0x00000000CAFEBABEull;
	cpuRegs.code = ((u32)1 << 11);            /* fs 1, rt 0 */
	QMFC2();
	cases++;
	if (cpuRegs.GPR.r[0].UD[0] == 0 && cpuRegs.GPR.r[0].UD[1] == 0) pass++;
	else printf("  qmfc2 -> $0 wrote %016llx\n",
	            (unsigned long long)cpuRegs.GPR.r[0].UD[0]);

	/* QMFC2 moves all 128 bits. */
	vuRegs[0].VF[1].UL[0] = 0x3F800000u; vuRegs[0].VF[1].UL[1] = 0;
	vuRegs[0].VF[1].UL[2] = 0;           vuRegs[0].VF[1].UL[3] = 0;
	cpuRegs.code = ((u32)2 << 16) | ((u32)1 << 11);
	QMFC2();
	cases++;
	if (cpuRegs.GPR.r[2].UL[0] == 0x3F800000u && cpuRegs.GPR.r[2].UL[1] == 0
	 && cpuRegs.GPR.r[2].UL[2] == 0 && cpuRegs.GPR.r[2].UL[3] == 0) pass++;
	else printf("  qmfc2: %08x %08x %08x %08x\n",
	            cpuRegs.GPR.r[2].UL[3], cpuRegs.GPR.r[2].UL[2],
	            cpuRegs.GPR.r[2].UL[1], cpuRegs.GPR.r[2].UL[0]);

	/* QMTC2 the other way, into a real VF register. */
	{
		int i;
		for (i = 0; i < 4; i++) cpuRegs.GPR.r[2].UL[i] = 0x12345678u;
		memset(&vuRegs[0].VF[1], 0, 16);
		cpuRegs.code = ((u32)2 << 16) | ((u32)1 << 11);
		QMTC2();
		cases++;
		if (vuRegs[0].VF[1].UL[0] == 0x12345678u
		 && vuRegs[0].VF[1].UL[3] == 0x12345678u) pass++;
		else printf("  qmtc2: vf1 is %08x %08x %08x %08x\n",
		            vuRegs[0].VF[1].UL[3], vuRegs[0].VF[1].UL[2],
		            vuRegs[0].VF[1].UL[1], vuRegs[0].VF[1].UL[0]);
	}

	/* QMTC2 into VF0 is discarded: VF0 reads back as (0, 0, 0, 1.0f),
	 * printed by the capture high word first as 3f800000 0 0 0. */
	{
		int i;
		for (i = 0; i < 4; i++) cpuRegs.GPR.r[2].UL[i] = 0x12345678u;
		vuRegs[0].VF[0].UL[0] = 0; vuRegs[0].VF[0].UL[1] = 0;
		vuRegs[0].VF[0].UL[2] = 0; vuRegs[0].VF[0].UL[3] = 0x3F800000u;
		cpuRegs.code = ((u32)2 << 16) | ((u32)0 << 11);
		QMTC2();
		cases++;
		if (vuRegs[0].VF[0].UL[3] == 0x3F800000u
		 && vuRegs[0].VF[0].UL[0] == 0) pass++;
		else printf("  qmtc2 -> $vf0: vf0 is %08x %08x %08x %08x\n",
		            vuRegs[0].VF[0].UL[3], vuRegs[0].VF[0].UL[2],
		            vuRegs[0].VF[0].UL[1], vuRegs[0].VF[0].UL[0]);
	}

	*total += cases;
	printf("qmove   %2d/%-3d quadword moves from vu0_macro/transfer\n",
	       pass, cases);
	return pass != cases;
}

static int run_vu0_macro(const char* dir, int* total)
{
	static const struct { const char* name; void (*fn)(); int imm; } kM[] = {
		{ "viadd",  VIADD,  0 },
		{ "viaddi", VIADDI, 1 },
		{ "viand",  VIAND,  0 },
		{ "vior",   VIOR,   0 },
		{ "visub",  VISUB,  0 },
	};
	const int nm = (int)(sizeof(kM)/sizeof(kM[0]));
	char path[512];
	int op, bad = 0;

	snprintf(path, sizeof(path), "%s/integer.expected", dir);
	for (op = 0; op < nm; op++)
	{
		FILE* f = fopen(path, "r");
		char buf[512];
		int inblock = 0, pass = 0, cases = 0;

		if (!f) return 0;
		while (fgets(buf, sizeof(buf), f))
		{
			u16 a = 0, b = 0;
			int a_named = 0, b_named = 0;
			unsigned want;
			char *p, *comma, *colon;

			(void)a_named; (void)b_named;
			if (!inblock)
			{ if (!strncmp(buf, kM[op].name, strlen(kM[op].name))
			   && buf[strlen(kM[op].name)] == ':') inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;

			colon = strstr(buf, ": ");
			if (!colon || sscanf(colon + 2, "%x", &want) != 1) continue;
			p = buf + 2 + strlen(kM[op].name);
			while (*p == ' ') p++;
			*colon = '\0';
			comma = strchr(p, ',');
			if (!comma) { *colon = ':'; continue; }
			*comma = '\0';
			if (!resolve_i(p, &a, &a_named)) { *colon = ':'; continue; }
			{ char* q = comma + 1; while (*q == ' ') q++;
			  if (!resolve_i(q, &b, &b_named)) { *colon = ':'; continue; } }
			*colon = ':';

			memset(&vuRegs[0], 0, sizeof(vuRegs[0]));
			/* No setvi here. The lower-op harness builds its VI values with
			 * WrSetIntegerRegister, which cannot load an arbitrary sixteen
			 * bits and negates anything with bit 15 set; the macro-mode one
			 * loads them through COP2 and they arrive intact. Applying the
			 * lower-op transform scores 17 of 21 -- only the lines whose
			 * operands have bit 15 clear, which is the same shape of error
			 * as the one it was written to fix. */
			vuRegs[0].VI[VI_S].UL = a;
			if (kM[op].imm)
			{
				/* VIADDI takes a signed five-bit immediate in the same
				 * field the lower-op form uses. */
				cpuRegs.code = ((u32)VI_S << 11) | ((u32)VI_D << 16)
				             | (((u32)b & 0x1F) << 6);
			}
			else
			{
				vuRegs[0].VI[VI_T].UL = b;
				cpuRegs.code = ((u32)VI_T << 16) | ((u32)VI_S << 11)
				             | ((u32)VI_D << 6);
			}
			kM[op].fn();

			cases++;
			if ((vuRegs[0].VI[VI_D].UL & 0xFFFFu) == want) pass++;
			else if (bad < 4)
				printf("  %-7s %04x, %04x: console %04x  ours %04x\n",
				       kM[op].name, a, b, want,
				       vuRegs[0].VI[VI_D].UL & 0xFFFFu);
		}
		fclose(f);
		*total += cases;
		if (cases == 0)
		{ printf("%-7s parsed no cases; the block name or line shape does"
		         " not match the capture\n", kM[op].name); bad++; }
		printf("%-7s %2d/%-3d console cases\n", kM[op].name, pass, cases);
		if (pass != cases) bad++;
	}
	return bad;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "efu.expected";
	int op, failures = 0, total = 0, shown = 0;

	vu_config_is_default();

	for (op = 0; op < NOPS; op++)
	{
		FILE* f = fopen(path, "r");
		char head[32], buf[512];
		int inblock = 0, pass = 0, cases = 0;

		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kOps[op].name);

		while (fgets(buf, sizeof(buf), f))
		{
			u32 x, y, z, w;
			unsigned want;
			char *a, *colon;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;

			colon = strstr(buf, ": ");
			if (!colon) continue;
			if (sscanf(colon + 2, "%x", &want) != 1) continue;
			a = buf + 2 + strlen(kOps[op].name);
			while (*a == ' ') a++;
			*colon = '\0';
			if (!lookup(a, &x, &y, &z, &w)) { *colon = ':'; continue; }
			*colon = ':';

			memset(&vuRegs[0], 0, sizeof(vuRegs[0]));
			vuRegs[0].VF[VF_SRC].UL[0] = x;
			vuRegs[0].VF[VF_SRC].UL[1] = y;
			vuRegs[0].VF[VF_SRC].UL[2] = z;
			vuRegs[0].VF[VF_SRC].UL[3] = w;

			/* Scalar forms read one component through fsf, which the
			 * capture always exercises as x; vector forms read xyz and
			 * carry a dest mask instead. */
			vuRegs[0].code = LOWER_TYPE1345 | VS(VF_SRC) | VT(0)
			               /* The scalar forms are issued with FIELD_Z by the
			                * efu harness (PerformS_M in lower/efu.cpp), not
			                * FIELD_X. With x the constants that hold the same
			                * value in every lane still pass, so the wrong
			                * field only shows on CVF_INCREASING and its kin. */
			               | (kOps[op].vector ? DEST(0xE) : FSF(2))
			               | (u32)kOps[op].op;
			VU0_LOWER_OPCODE[vuRegs[0].code >> 25]();

			cases++;
			/* The EFU writes VU->p; VI[REG_P] only receives it when the
			 * pipeline flushes, which no single op here does. */
			if (vuRegs[0].p.UL == want) pass++;
			else if (shown++ < 4)
				printf("  %-7s %08x: console %08x  ours %08x\n",
				       kOps[op].name, x, want, vuRegs[0].p.UL);
		}
		fclose(f);
		total += cases;
		printf("%-7s %2d/%-3d console cases (floor %d)\n",
		       kOps[op].name, pass, cases, kOps[op].floor);
		if (pass < kOps[op].floor)
		{ printf("  regression: this used to match %d\n", kOps[op].floor);
		  failures++; }
	}

	{
		/* integer.expected sits beside efu.expected. */
		char dir[512];
		const char* slash = strrchr(path, '/');
		size_t n = slash ? (size_t)(slash - path) : 1;
		if (n >= sizeof(dir)) n = sizeof(dir) - 1;
		memcpy(dir, path, n);
		dir[n] = '\0';
		failures += run_integer(slash ? dir : ".", &total);
	}
	failures += run_random(&total);
	failures += run_clip(&total);
	{
		/* vu0_macro sits beside vu/lower in the checkout. */
		char mdir[512];
		const char* slash = strrchr(path, '/');
		size_t n = slash ? (size_t)(slash - path) : 1;
		if (n >= sizeof(mdir) - 32) n = sizeof(mdir) - 32;
		memcpy(mdir, path, n);
		mdir[n] = '\0';
		strncat(mdir, "/../../cpu/vu0_macro", sizeof(mdir) - strlen(mdir) - 1);
		failures += run_vu0_macro(slash ? mdir : ".", &total);
	}
	failures += run_quad_moves(&total);

	printf("hwrealvu: %d cases across %d ops, driven through VUops.cpp\n",
	       total, NOPS + NIOPS + 10);
	return failures != 0;
}
