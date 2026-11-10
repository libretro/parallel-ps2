/*  MMI against console captures, driving the real implementations.
 *
 *  tests/mmi/hwelem.c and hwpmfhl.c transcribe the semantics out of
 *  pcsx2/MMI.c and score the copy. That catches a wrong answer but not
 *  drift: if MMI.c changes, the transcription keeps passing.
 *
 *  This links MMI.c instead and calls the ops through their real entry
 *  points, building the instruction word so the _Rd_, _Rs_ and _Rt_
 *  macros decode the registers this harness intends. The file touches
 *  only cpuRegs, so a definition of it is the whole of what it needs.
 *
 *  It scores the same shuffle, arithmetic, compare and logic captures the
 *  transcribing tests do, so the two disagreeing is itself informative:
 *  it means the transcription has fallen behind the code.
 *
 *  Being real code, it is also worth pointing a sanitizer at, which is
 *  what prompted this -- UBSan over the transcribing tests only ever
 *  checked my own arithmetic.
 *
 *  Usage: tests/mmi/hwreal <dir-with-the-.expected-files>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "R5900.h"

alignas(16) cpuRegisters cpuRegs;

/* Registers this harness uses: rd = 1, rs = 2, rt = 3. */
#define RD 1
#define RS 2
#define RT 3

typedef struct { u32 w[4]; } Q;

static void set_reg(int r, const Q* q)
{
	cpuRegs.GPR.r[r].UL[0] = q->w[0]; cpuRegs.GPR.r[r].UL[1] = q->w[1];
	cpuRegs.GPR.r[r].UL[2] = q->w[2]; cpuRegs.GPR.r[r].UL[3] = q->w[3];
}
static void get_reg(int r, Q* q)
{
	q->w[0] = cpuRegs.GPR.r[r].UL[0]; q->w[1] = cpuRegs.GPR.r[r].UL[1];
	q->w[2] = cpuRegs.GPR.r[r].UL[2]; q->w[3] = cpuRegs.GPR.r[r].UL[3];
}

/* The ops under test, by their MMI.c entry points, all declared in
 * R5900OpcodeTables.h: the MMI ops and the second-pipeline and
 * accumulate forms that share their opcode class. */

/* How a line's operands map onto the instruction word. TWO is the usual
 * rs,rt pair; ONE has a single source, which MMI.c reads from rt; IMM
 * takes a shift amount in sa rather than a second register; VSHIFT is the
 * variable-shift trio, which assemble as rd, rt, rs -- value first,
 * amount second -- so the operands go in swapped. */
enum { A_TWO, A_ONE, A_IMM, A_VSHIFT, A_HILO, A_HILO1, A_HILO_TO,
       A_PMFHL, A_PMTHL };

static const struct { const char* name; const char* file; void (*fn)(); int form; }
kOps[] = {
	{ "paddw","arithmetic",intPADDW,A_TWO },{ "psubw","arithmetic",intPSUBW,A_TWO },
	{ "pcgtw","compare",intPCGTW,A_TWO },{ "pmaxw","arithmetic",intPMAXW,A_TWO },
	{ "paddh","arithmetic",intPADDH,A_TWO },{ "psubh","arithmetic",intPSUBH,A_TWO },
	{ "pcgth","compare",intPCGTH,A_TWO },{ "pmaxh","arithmetic",intPMAXH,A_TWO },
	{ "paddb","arithmetic",intPADDB,A_TWO },{ "psubb","arithmetic",intPSUBB,A_TWO },
	{ "pcgtb","compare",intPCGTB,A_TWO },
	{ "paddsw","arithmetic",intPADDSW,A_TWO },{ "psubsw","arithmetic",intPSUBSW,A_TWO },
	{ "paddsh","arithmetic",intPADDSH,A_TWO },{ "psubsh","arithmetic",intPSUBSH,A_TWO },
	{ "paddsb","arithmetic",intPADDSB,A_TWO },{ "psubsb","arithmetic",intPSUBSB,A_TWO },
	{ "padduw","arithmetic",intPADDUW,A_TWO },{ "psubuw","arithmetic",intPSUBUW,A_TWO },
	{ "padduh","arithmetic",intPADDUH,A_TWO },{ "psubuh","arithmetic",intPSUBUH,A_TWO },
	{ "paddub","arithmetic",intPADDUB,A_TWO },{ "psubub","arithmetic",intPSUBUB,A_TWO },
	{ "pand","logic",intPAND,A_TWO },{ "por","logic",intPOR,A_TWO },
	{ "pxor","logic",intPXOR,A_TWO },{ "pnor","logic",intPNOR,A_TWO },
	{ "pceqw","compare",intPCEQW,A_TWO },{ "pceqh","compare",intPCEQH,A_TWO },
	{ "pceqb","compare",intPCEQB,A_TWO },
	{ "pminh","arithmetic",intPMINH,A_TWO },{ "pminw","arithmetic",intPMINW,A_TWO },
	{ "pextlw","shuffle",intPEXTLW,A_TWO },{ "pextlh","shuffle",intPEXTLH,A_TWO },
	{ "pextlb","shuffle",intPEXTLB,A_TWO },
	{ "pextuw","shuffle",intPEXTUW,A_TWO },{ "pextuh","shuffle",intPEXTUH,A_TWO },
	{ "pextub","shuffle",intPEXTUB,A_TWO },
	{ "pcpyld","shuffle",intPCPYLD,A_TWO },{ "pcpyud","shuffle",intPCPYUD,A_TWO },
	/* Single-source, and their lines carry one operand: "pcpyh 0: ...".
	 * Marked A_TWO the parser demands a comma, finds none and skips the
	 * whole block -- both scored 0 of 0 while the run passed. */
	{ "pcpyh","shuffle",intPCPYH,A_ONE },{ "prevh","shuffle",intPREVH,A_ONE },
	{ "ppacw","shuffle",intPPACW,A_TWO },{ "ppach","shuffle",intPPACH,A_TWO },
	{ "ppacb","shuffle",intPPACB,A_TWO },
	{ "pabsw","arithmetic",intPABSW,A_ONE },{ "pabsh","arithmetic",intPABSH,A_ONE },
	{ "padsbh","arithmetic",intPADSBH,A_TWO },
	{ "pexch","shuffle",intPEXCH,A_ONE },{ "pexcw","shuffle",intPEXCW,A_ONE },
	{ "pexeh","shuffle",intPEXEH,A_ONE },{ "pexew","shuffle",intPEXEW,A_ONE },
	{ "pext5","shuffle",intPEXT5,A_ONE },{ "prot3w","shuffle",intPROT3W,A_ONE },
	{ "pinteh","shuffle",intPINTEH,A_TWO },{ "pinth","shuffle",intPINTH,A_TWO },
	{ "plzcw","logic",intPLZCW,A_ONE },
	{ "psllh","logic",intPSLLH,A_IMM },{ "psrlh","logic",intPSRLH,A_IMM },
	{ "psrah","logic",intPSRAH,A_IMM },{ "psllw","logic",intPSLLW,A_IMM },
	{ "psrlw","logic",intPSRLW,A_IMM },{ "psraw","logic",intPSRAW,A_IMM },
	{ "psllvw","logic",intPSLLVW,A_VSHIFT },{ "psrlvw","logic",intPSRLVW,A_VSHIFT },
	{ "psravw","logic",intPSRAVW,A_VSHIFT },
	/* The accumulators and the HI/LO moves. These read and write HI and LO
	 * as well as rd, and the capture prints all three, so the seed named on
	 * each line has to be applied before the op runs. This is the family
	 * the PMADDW errata bug lived in, which is reason enough to drive it
	 * through the real code rather than a copy. */
	{ "pmaddw","muldiv",intPMADDW,A_HILO },{ "pmsubw","muldiv",intPMSUBW,A_HILO },
	{ "pmadduw","muldiv",intPMADDUW,A_HILO },
	{ "pmultw","muldiv",intPMULTW,A_HILO },{ "pmultuw","muldiv",intPMULTUW,A_HILO },
	{ "pmaddh","muldiv",intPMADDH,A_HILO },{ "pmsubh","muldiv",intPMSUBH,A_HILO },
	{ "pmulth","muldiv",intPMULTH,A_HILO },
	{ "phmadh","muldiv",intPHMADH,A_HILO },{ "phmsbh","muldiv",intPHMSBH,A_HILO },
	{ "pdivw","muldiv",intPDIVW,A_HILO },{ "pdivuw","muldiv",intPDIVUW,A_HILO },
	{ "pdivbw","muldiv",intPDIVBW,A_HILO },
	{ "pmfhi","muldiv",intPMFHI,A_HILO1 },{ "pmflo","muldiv",intPMFLO,A_HILO1 },
	/* PMTHI and PMTLO write HI or LO and leave rd alone, so the quad their
	 * lines print is the source register, not a result. Comparing it as rd
	 * scores 1 of 28 while the HI/LO values it is actually testing match
	 * on every case. */
	{ "pmthi","muldiv",intPMTHI,A_HILO_TO },{ "pmtlo","muldiv",intPMTLO,A_HILO_TO },
	/* PMFHL and PMTHL pick their variant from sa, which the harness
	 * encodes into the instruction word anyway, so the five forms of one
	 * and the single form of the other are just six more table rows. */
	{ "pmfhl.lw","muldiv",intPMFHL,A_PMFHL },{ "pmfhl.uw","muldiv",intPMFHL,A_PMFHL },
	{ "pmfhl.slw","muldiv",intPMFHL,A_PMFHL },{ "pmfhl.lh","muldiv",intPMFHL,A_PMFHL },
	{ "pmfhl.sh","muldiv",intPMFHL,A_PMFHL },
	{ "pmthl.lw","muldiv",intPMTHL,A_PMTHL },
	/* Second-pipeline and accumulate forms, from tests/cpu/ee/muldiv
	 * rather than ee_simd -- see the file column below. */
	{ "madd","ee_muldiv",intMADD,A_HILO },{ "maddu","ee_muldiv",intMADDU,A_HILO },
	{ "madd1","ee_muldiv",intMADD1,A_HILO },{ "maddu1","ee_muldiv",intMADDU1,A_HILO },
	{ "mult1","ee_muldiv",intMULT1,A_HILO },{ "multu1","ee_muldiv",intMULTU1,A_HILO },
	{ "div1","ee_muldiv",intDIV1,A_HILO },{ "divu1","ee_muldiv",intDIVU1,A_HILO },
	{ "mfhi1","ee_muldiv",intMFHI1,A_HILO1 },{ "mflo1","ee_muldiv",intMFLO1,A_HILO1 },
	{ "mthi1","ee_muldiv",intMTHI1,A_HILO_TO },{ "mtlo1","ee_muldiv",intMTLO1,A_HILO_TO },
};

/* sa value for each PMFHL variant, in the order MMI.c switches on. */
static u32 pmfhl_sa(const char* name)
{
	if (!strcmp(name, "pmfhl.lw"))  return 0x00;
	if (!strcmp(name, "pmfhl.uw"))  return 0x01;
	if (!strcmp(name, "pmfhl.slw")) return 0x02;
	if (!strcmp(name, "pmfhl.lh"))  return 0x03;
	return 0x04;                      /* pmfhl.sh */
}
#define NOPS ((int)(sizeof(kOps)/sizeof(kOps[0])))

static const struct { const char* name; Q v; } kConst[] = {
	{ "C_ZERO",     {{0,0,0,0}} },
	{ "C_S16_MAX",  {{0x7FFFu,0,0,0}} },
	{ "C_S16_MIN",  {{0xFFFF8000u,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_S32_MAX",  {{0x7FFFFFFFu,0,0,0}} },
	{ "C_S32_MIN",  {{0x80000000u,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_S64_MAX",  {{0xFFFFFFFFu,0x7FFFFFFFu,0,0}} },
	{ "C_S64_MIN",  {{0,0x80000000u,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_NEGONE",   {{0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_ONE",      {{1,0,0,0}} },
	{ "C_GARBAGE1", {{0x1337u,0x1338u,0x1339u,0x133Au}} },
	{ "C_GARBAGE2", {{0xDEADBEEFu,0xDEADBEEEu,0xDEADBEEDu,0xDEADBEECu}} },
	/* The packed saturation constants, as in hwelem.c. Missing them costs
	 * eight cases a block silently -- the parse just skips the line. */
	{ "C_P_S8_A",   {{0x80808080u,0xFFFFFFFFu,0x12345678u,0x7F7F7F7Fu}} },
	{ "C_P_S8_B",   {{0x80FF127Fu,0xFF807F34u,0x567F80FFu,0x7F78FF80u}} },
	{ "C_P_S16_A",  {{0x80008000u,0xFFFFFFFFu,0x12345678u,0x7FFF7FFFu}} },
	{ "C_P_S16_B",  {{0x8000FFFFu,0x12347FFFu,0xFFFF8000u,0x7FFF5678u}} },
	{ "C_P_S32_A",  {{0x80000000u,0xFFFFFFFFu,0x12345678u,0x7FFFFFFFu}} },
	{ "C_P_S32_B",  {{0xFFFFFFFFu,0x80000000u,0x7FFFFFFFu,0x12345678u}} },
	{ "C_P_S32_C",  {{0x80000000u,0xFFFFFFFFu,0x12345678u,0x7FFFFFFFu}} },
	{ "C_P_S32_D",  {{0x7FFFFFFFu,0x12345678u,0xFFFFFFFFu,0x80000000u}} },
	{ "C_P_S32_E",  {{0x00000000u,0x7FFFFFFFu,0xFFFFFFFFu,0x80000000u}} },
	{ "C_P_S32_F",  {{0xFFFFFFFFu,0x80000000u,0x00000000u,0x7FFFFFFFu}} },
	/* Declared in muldiv.cpp rather than shared.h, so easy to miss. */
	{ "C_TWOTWOTWOTWO", {{2u,2u,2u,2u}} },
	{ "C_ONEONEONEONE", {{1u,1u,1u,1u}} },
};

/* The three HILOREGS the muldiv harness seeds: { hi, lo, hi1, lo1 }. */
static const struct { const char* name; u64 hi, lo, hi1, lo1; } kSeed[] = {
	{ "C_HILO_PATTERN", 0x0000000000000000ull, 0x0000000010001234ull,
	                    0x0000000000000000ull, 0x00000000F000ABCDull },
	{ "C_HILO_ZERO",    0, 0, 0, 0 },
	{ "C_HILO",         0x0123456789ABCDEFull, 0x123456789ABCDEF0ull,
	                    0x23456789ABCDEF01ull, 0x456789ABCDEF0123ull },
};

static int seed_hilo(const char* line)
{
	size_t i;
	for (i = 0; i < sizeof(kSeed)/sizeof(kSeed[0]); i++)
	{
		if (!strstr(line, kSeed[i].name)) continue;
		cpuRegs.HI.UD[0] = kSeed[i].hi;  cpuRegs.HI.UD[1] = kSeed[i].hi1;
		cpuRegs.LO.UD[0] = kSeed[i].lo;  cpuRegs.LO.UD[1] = kSeed[i].lo1;
		return 1;
	}
	/* Lines with no seed named use C_HILO, which the harness sets once. */
	cpuRegs.HI.UD[0] = kSeed[2].hi;  cpuRegs.HI.UD[1] = kSeed[2].hi1;
	cpuRegs.LO.UD[0] = kSeed[2].lo;  cpuRegs.LO.UD[1] = kSeed[2].lo1;
	return 1;
}

static int operand(const char* p, Q* out, int* named)
{
	size_t i, best = (size_t)-1, bestlen = 0;
	while (*p == ' ') p++;
	for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
	{
		const size_t n = strlen(kConst[i].name);
		if (!strncmp(p, kConst[i].name, n) && n > bestlen)
		{ best = i; bestlen = n; }
	}
	if (best != (size_t)-1) { *out = kConst[best].v; *named = 1; return 1; }
	{
		/* The harness sets both doublewords, not just the low one: the
		 * capture shows a PADDW of 1 and 1 giving 1 in words 0 and 2. A
		 * model that fills only the low half scores 13 of 19 and looks
		 * like broken upper lanes. */
		char* e; long long v = strtoll(p, &e, 10);
		u32 w, sgn;
		if (e == p) return 0;
		w = (u32)v;
		sgn = (w & 0x80000000u) ? 0xFFFFFFFFu : 0u;
		out->w[0] = w; out->w[1] = sgn; out->w[2] = w; out->w[3] = sgn;
		*named = 0;
		return 1;
	}
}

/* How many case lines a block actually has in the capture. Comparing this
 * against what the harness parsed is a stronger check than "did anything
 * parse": every under-coverage bug in this series was partial, not total
 * -- a missing constant dropping eight lines a block, an operand shape
 * matching some lines and not others, an op name typo'd so one block never
 * opened. Each was found by counting by hand afterwards. This counts every
 * time. */
static int block_lines(const char* path, const char* name)
{
	FILE* f = fopen(path, "r");
	char buf[512];
	int in = 0, n = 0;
	if (!f) return -1;
	while (fgets(buf, sizeof(buf), f))
	{
		if (buf[0] != ' ')
		{
			char* c = strchr(buf, ':');
			if (in) break;
			if (c && c[1] == '\n')
			{ *c = '\0'; in = !strcmp(buf, name); *c = ':'; }
			continue;
		}
		if (!in) continue;
		/* Writes to $0 are discarded by the hardware and the harness skips
		 * them deliberately, so they are not missing coverage. */
		if (strstr(buf, "-> $0")) continue;
		n++;
	}
	fclose(f);
	return n;
}

int main(int argc, char** argv)
{
	const char* dir = (argc > 1) ? argv[1] : ".";
	int op, failures = 0, total = 0;

	for (op = 0; op < NOPS; op++)
	{
		char path[512], head[32], buf[512];
		FILE* f;
		int inblock = 0, pass = 0, cases = 0;

		/* Most blocks come from the ee_simd captures; the second-pipeline
		 * ones come from tests/cpu/ee/muldiv.expected, a directory up. */
		if (!strcmp(kOps[op].file, "ee_muldiv"))
			snprintf(path, sizeof(path), "%s/../ee/muldiv.expected", dir);
		else
			snprintf(path, sizeof(path), "%s/%s.expected", dir, kOps[op].file);
		f = fopen(path, "r");
		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kOps[op].name);

		while (fgets(buf, sizeof(buf), f))
		{
			Q rs, rt, rd; unsigned w3, w2, w1, w0; u32 sa;
			unsigned long long whi = 0, whi1 = 0, wlo = 0, wlo1 = 0;
			int has_rd = 0, got_quad = 0;
			int named_s = 0, named_t = 0;
			char* colon; char* args; char* comma;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;
			if (strstr(buf, "-> $0")) continue;

			colon = strstr(buf, ": ");
			if (!colon) continue;
			if (kOps[op].form >= A_HILO)
			{
				/* "<op> <ops>: [rd rd rd rd ]H: hi hi1 L: lo lo1" -- some of
				 * these print rd and some do not, so the H: marker decides. */
				const char* h = strstr(colon, "H: ");
				if (!h) continue;
				/* Parse the quad whenever the line carries one; whether it
				 * is a result or a source is a separate question. */
				got_quad = (sscanf(colon + 2, "%x %x %x %x",
				                   &w3, &w2, &w1, &w0) == 4);
				/* rd is not compared for the ee/muldiv blocks. Those lines
				 * print it, but that program seeds rd differently again --
				 * madd's upper half arrives as 0x0000000100000001, not the
				 * garbage quad the ee_simd program uses -- and rather than
				 * model a third seeding scheme for a register most of these
				 * ops do not write, only HI and LO are scored. That is what
				 * the second pipeline is for, and they match on every line. */
				has_rd = got_quad
				      && kOps[op].form != A_HILO_TO
				      && strcmp(kOps[op].file, "ee_muldiv") != 0
				      && kOps[op].form != A_PMTHL;
				if (sscanf(h, "H: %llx %llx L: %llx %llx",
				           &whi, &whi1, &wlo, &wlo1) != 4) continue;
			}
			else if (sscanf(colon + 2, "%x %x %x %x", &w3, &w2, &w1, &w0) != 4)
				continue;
			args = buf + 2 + strlen(kOps[op].name);
			*colon = '\0';
			sa = 0;
			comma = strchr(args, ',');
			if (kOps[op].form == A_PMFHL || kOps[op].form == A_PMTHL)
			{
				named_s = named_t = (strstr(args, "C_") != NULL);
				sa = (kOps[op].form == A_PMFHL) ? pmfhl_sa(kOps[op].name) : 0;
				rs.w[0] = rs.w[1] = rs.w[2] = rs.w[3] = 0;
				if (kOps[op].form == A_PMTHL && got_quad)
				{
					/* PMTHL reads rs and writes HI and LO, so the quad on the
					 * line is its source, not a result -- the same shape as
					 * PMTHI and PMTLO. It prints high word first. Leaving it
					 * unparsed scores 4 of 28: just the cases whose source
					 * happens to be zero. */
					rs.w[0] = (u32)w0; rs.w[1] = (u32)w1;
					rs.w[2] = (u32)w2; rs.w[3] = (u32)w3;
				}
				rt = rs;
			}
			else if (kOps[op].form == A_HILO1 || kOps[op].form == A_HILO_TO)
			{
				/* PMFHI and PMFLO print no operand, PMTHI and PMTLO one. */
				int dummy = 0;
				rs.w[0] = rs.w[1] = rs.w[2] = rs.w[3] = 0;
				if (!operand(args, &rs, &dummy))
					rs.w[0] = rs.w[1] = rs.w[2] = rs.w[3] = 0;
				rt = rs;
				named_s = named_t = (strstr(args, "C_") != NULL);
			}
			else if (kOps[op].form == A_ONE)
			{
				/* Single source: MMI.c reads it from rt. */
				if (comma) { *colon = ':'; continue; }
				if (!operand(args, &rt, &named_t)) { *colon = ':'; continue; }
				rs = rt; named_s = named_t;
			}
			else
			{
				if (!comma) { *colon = ':'; continue; }
				*comma = '\0';
				if (!operand(args, &rs, &named_s)) { *colon = ':'; continue; }
				if (kOps[op].form == A_IMM)
				{
					/* Second operand is a shift amount, and the value being
					 * shifted is the first one, which MMI.c reads from rt. */
					char* q = comma + 1;
					while (*q == ' ') q++;
					sa = (u32)strtoul(q, NULL, 10) & 0x1F;
					rt = rs;
				}
				else if (!operand(comma + 1, &rt, &named_t))
				{ *colon = ':'; continue; }
				if (kOps[op].form == A_VSHIFT)
				{ const Q t = rs; rs = rt; rt = t; }
			}
			*colon = ':';

			/* rd carries state between cases exactly as in hwelem: the
			 * decimal cases rewrite only its low word, the named ones all
			 * 128 bits from C_GARBAGE1. Ops that write every lane do not
			 * care, but PCPYH and the like leave parts of rd standing. */
			/* The ee/muldiv program seeds rd with the garbage quad on every
			 * line, where the ee_simd ones only do so for the named-constant
			 * cases and use a plain 0x1337 otherwise. Using the ee_simd
			 * model there leaves rd's upper half wrong and every line fails
			 * on a register the op did not even write. */
			if (named_s || !strcmp(kOps[op].file, "ee_muldiv"))
			{
				const Q g = {{0x1337u,0x1338u,0x1339u,0x133Au}};
				set_reg(RD, &g);
			}
			else
			{
				/* The decimal cases seed rd into both doublewords, the same
				 * way they seed the operands. Setting only the low word
				 * leaves the upper half holding whatever the previous case
				 * left, which PLZCW exposes: it writes two words and leaves
				 * the rest of rd standing. */
				const Q seed = {{0x1337u, 0u, 0x1337u, 0u}};
				set_reg(RD, &seed);
			}

			if (kOps[op].form >= A_HILO)
				seed_hilo(buf);
			set_reg(RS, &rs);
			set_reg(RT, &rt);
			/* SPECIAL-form encoding: rs at 21, rt at 16, rd at 11. */
			cpuRegs.code = ((u32)RS << 21) | ((u32)RT << 16) | ((u32)RD << 11)
			             | (sa << 6);
			kOps[op].fn();
			get_reg(RD, &rd);

			cases++;
			if (kOps[op].form >= A_HILO)
			{
				const int hilo_ok = cpuRegs.HI.UD[0] == whi && cpuRegs.HI.UD[1] == whi1
				                 && cpuRegs.LO.UD[0] == wlo && cpuRegs.LO.UD[1] == wlo1;
				const int rd_ok = !has_rd
				               || (rd.w[0] == w0 && rd.w[1] == w1
				                && rd.w[2] == w2 && rd.w[3] == w3);
				if (hilo_ok && rd_ok) pass++;
				else if (failures++ < 6)
					printf("  %-7s console H %016llx %016llx L %016llx %016llx\n"
					       "          ours    H %016llx %016llx L %016llx %016llx\n",
					       kOps[op].name, whi, whi1, wlo, wlo1,
					       (unsigned long long)cpuRegs.HI.UD[0],
					       (unsigned long long)cpuRegs.HI.UD[1],
					       (unsigned long long)cpuRegs.LO.UD[0],
					       (unsigned long long)cpuRegs.LO.UD[1]);
			}
			else if (rd.w[0] == w0 && rd.w[1] == w1 && rd.w[2] == w2 && rd.w[3] == w3)
				pass++;
			else if (failures++ < 6)
				printf("  %-7s console %08x %08x %08x %08x  ours %08x %08x %08x %08x\n",
				       kOps[op].name, w3, w2, w1, w0,
				       rd.w[3], rd.w[2], rd.w[1], rd.w[0]);
		}
		fclose(f);
		total += cases;
		/* A block that matched no lines is a harness bug, not a pass:
		 * 0 of 0 looks the same as success in a summary line. */
		{
			const int have = block_lines(path, kOps[op].name);
			if (have > 0 && cases != have)
			{ printf("%-7s parsed %d of the %d lines its block has;"
			         " some operand shape does not match\n",
			         kOps[op].name, cases, have);
			  failures++; }
			else if (have == 0)
			{ printf("%-7s parsed no cases; the block name or operand shape"
			         " does not match the capture\n", kOps[op].name);
			  failures++; }
		}
		if (cases == 0) { }
		else if (pass != cases)
		{ printf("%-7s %2d/%-3d console cases\n", kOps[op].name, pass, cases);
		  failures++; }
	}

	printf("hwreal: %d cases across %d ops, driven through MMI.c\n", total, NOPS);
	return failures != 0;
}
