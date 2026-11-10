/*  EE integer ops against console captures, driving the real code.
 *
 *  tests/ee/hwalu.c and hwload.c and hwstore.c transcribe the semantics
 *  out of pcsx2/R5900OpcodeImpl.cpp and score the copy, which catches a
 *  wrong answer but not drift. This links that file and calls the ops
 *  through their real entry points, building the instruction word so the
 *  _Rd_, _Rs_, _Rt_, _Sa_ and _Imm_ macros decode what the harness
 *  intends.
 *
 *  Its dependencies are stubbed rather than pulled in. Most are inert --
 *  the BIOS hacks, the savestate helpers, the COP2 print table -- but the
 *  vtlb accessors are backed by a real array, so the loads and stores
 *  execute against memory rather than being skipped. That is what lets
 *  the same harness cover alu.expected and lsu.expected.
 *
 *  Three tables of ops, by shape:
 *    RRR    rd = f(rs, rt)          -- the register arithmetic
 *    RRI    rt = f(rs, immediate)   -- the immediate forms
 *    RRS    rd = f(rt, sa)          -- shift by immediate
 *
 *  The load and store blocks reuse the same memory pattern the standalone
 *  scorers do, at the same two bases: the load harness bases at byte 16
 *  of the pattern and the store harness at byte 4, which is not a typo
 *  but a difference between the two test programs.
 *
 *  Usage: tests/ee/hwrealee <dir-with-the-.expected-files>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "R5900.h"
#include "R5900OpcodeTables.h"
#include "GS.h"
#include "ps2/BiosTools.h"

alignas(16) cpuRegisters cpuRegs;

/* ---- stubs ------------------------------------------------------------
 * Everything R5900OpcodeImpl.cpp reaches for that this harness does not
 * exercise. The memory accessors are real; the rest are inert. */

#define MEMSIZE 4096
static u8 s_mem[MEMSIZE];
static u32 clampaddr(u32 a) { return a & (MEMSIZE - 1); }

u8  vtlb_memRead8 (u32 a) { return s_mem[clampaddr(a)]; }
u16 vtlb_memRead16(u32 a) { u16 v; memcpy(&v, s_mem + clampaddr(a), 2); return v; }
u32 vtlb_memRead32(u32 a) { u32 v; memcpy(&v, s_mem + clampaddr(a), 4); return v; }
u64 vtlb_memRead64(u32 a) { u64 v; memcpy(&v, s_mem + clampaddr(a), 8); return v; }
void vtlb_memWrite8 (u32 a, u8  v) { s_mem[clampaddr(a)] = v; }
void vtlb_memWrite16(u32 a, u16 v) { memcpy(s_mem + clampaddr(a), &v, 2); }
void vtlb_memWrite32(u32 a, u32 v) { memcpy(s_mem + clampaddr(a), &v, 4); }
void vtlb_memWrite64(u32 a, u64 v) { memcpy(s_mem + clampaddr(a), &v, 8); }
void* vtlb_GetPhyPtr(u32 a) { return s_mem + clampaddr(a); }

RETURNS_R128 vtlb_memRead128(u32 a)
{
	r128 v;
	memcpy(&v, s_mem + clampaddr(a), 16);
	return v;
}
void vtlb_memWrite128(u32 a, r128 v) { memcpy(s_mem + clampaddr(a), &v, 16); }

/* Inert stubs: nothing here runs a BIOS, a savestate or the GS. */
bool g_SkipBiosHack = false, AllowParams1 = false, AllowParams2 = false, NoOSD = false;
void cpuException(u32, u32) { }
/* Cpu is only reached by the misaligned-access paths, which call
 * CancelInstruction; the captures never take one. */
static void nop_v() { }
static void nop_u32(u32) { }
static void nop_u32_u32(u32, u32) { }
/* Set when an op takes its misaligned-address path. The captures use
 * aligned addresses, so this firing means the harness set up the address
 * wrongly -- it is recorded rather than ignored, and never null, because a
 * null CancelInstruction turns that mistake into a segfault. */
static int s_cancelled;
static void harness_cancel() { s_cancelled = 1; }
static R5900cpu s_cpu = {};
R5900cpu* Cpu = &s_cpu;
static struct cpu_init_t { cpu_init_t() { s_cpu.CancelInstruction = harness_cancel; } } s_cpu_init;
BiosDebugInformation CurrentBiosInformation = {};
/* Referenced by inline helpers in GS.h and Dmac.h; only needed when the
 * compiler does not elide them, which -O0 does not. */
alignas(16) u8 g_RealGSMem[0x2000];
alignas(16) u8 eeHw[0x10000];
void gsSetVideoMode(GS_VideoMode) { }
extern "C" void cdvdReadLanguageParams(u8*) { }
tDMA_TAG* dmaGetAddr(u32, bool) { return (tDMA_TAG*)s_mem; }
void SaveState_FreezeMem(SaveStateBase*, void*, int) { }
bool SaveState_FreezeTag(SaveStateBase*, const char*) { return true; }

/* ---- the harness ------------------------------------------------------ */


/* HILO_MOVE: one operand, and the result is a pipeline register or rd
 * depending on the direction. */
enum Shape { RRR, RRI, RRS, RI, LOAD, STORE, MULDIV, HILO_MOVE };

static const struct { const char* name; void (*fn)(); Shape shape; int swap; }
kOps[] = {
	{ "addu",intADDU,RRR,0 },{ "subu",intSUBU,RRR,0 },{ "and",intAND,RRR,0 },
	{ "or",intOR,RRR,0 },{ "xor",intXOR,RRR,0 },{ "nor",intNOR,RRR,0 },
	{ "slt",intSLT,RRR,0 },{ "sltu",intSLTU,RRR,0 },
	{ "daddu",intDADDU,RRR,0 },{ "dsubu",intDSUBU,RRR,0 },
	{ "movn",intMOVN,RRR,0 },{ "movz",intMOVZ,RRR,0 },
	/* The variable shifts assemble as rd, rt, rs -- value first, amount
	 * second -- so the printed operands go in swapped. */
	{ "sllv",intSLLV,RRR,1 },{ "srlv",intSRLV,RRR,1 },{ "srav",intSRAV,RRR,1 },
	{ "dsllv",intDSLLV,RRR,1 },{ "dsrlv",intDSRLV,RRR,1 },{ "dsrav",intDSRAV,RRR,1 },
	{ "addiu",intADDIU,RRI,0 },{ "andi",intANDI,RRI,0 },{ "daddiu",intDADDIU,RRI,0 },
	{ "ori",intORI,RRI,0 },{ "xori",intXORI,RRI,0 },
	{ "slti",intSLTI,RRI,0 },{ "sltiu",intSLTIU,RRI,0 },
	{ "lui",intLUI,RI,0 },
	{ "sll",intSLL,RRS,0 },{ "srl",intSRL,RRS,0 },{ "sra",intSRA,RRS,0 },
	{ "dsll",intDSLL,RRS,0 },{ "dsrl",intDSRL,RRS,0 },{ "dsra",intDSRA,RRS,0 },
	{ "dsll32",intDSLL32,RRS,0 },{ "dsrl32",intDSRL32,RRS,0 },{ "dsra32",intDSRA32,RRS,0 },
	/* Loads and stores run against the backing array; the base register
	 * holds the pattern's base and the offset comes from the line. */
	{ "lb",intLB,LOAD,0 },{ "lbu",intLBU,LOAD,0 },{ "lh",intLH,LOAD,0 },
	{ "lhu",intLHU,LOAD,0 },{ "lw",intLW,LOAD,0 },{ "lwu",intLWU,LOAD,0 },
	{ "ld",intLD,LOAD,0 },{ "lq",intLQ,LOAD,0 },
	{ "sb",intSB,STORE,0 },{ "sh",intSH,STORE,0 },{ "sw",intSW,STORE,0 },
	{ "sd",intSD,STORE,0 },{ "sq",intSQ,STORE,0 },
	{ "div",intDIV,MULDIV,0 },{ "divu",intDIVU,MULDIV,0 },
	{ "mult",intMULT,MULDIV,0 },{ "multu",intMULTU,MULDIV,0 },
	/* The HI/LO moves. Nothing in the tree scored these either -- the
	 * audit that turned up the second pipeline found them in the same
	 * pass. */
	{ "mfhi",intMFHI,HILO_MOVE,0 },{ "mflo",intMFLO,HILO_MOVE,0 },
	{ "mthi",intMTHI,HILO_MOVE,0 },{ "mtlo",intMTLO,HILO_MOVE,0 },
};

/* C_PATTERN from the lsu harness, and the two bases its load and store
 * halves use -- byte 16 for loads, byte 4 for stores, which is a row index
 * against a flat word index rather than a mistake. */
static const u32 kPattern[12] = {
	0x45678123u, 0x9ABCDEF0u, 0xDEADBEEFu, 0xC0DE1337u,
	0x23456789u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu,
	0x8899AABBu, 0xCCDDEEFFu, 0x00112233u, 0x44556677u,
};
#define LOAD_BASE  16
/* The EE store harness bases at &buffer[1] of a u128 array, so byte 16 --
 * the same base its loads use. The IOP one bases at byte 4 because its
 * buffer is a flat u32 array; that difference cost a crash here, since SD
 * and SQ at byte 4 take their misaligned path. */
#define STORE_BASE 16

/* C_HILO, the seed the muldiv harness restores before each case. */
#define HI0 0x0123456789ABCDEFull
#define LO0 0x123456789ABCDEF0ull
#define HI1 0x23456789ABCDEF01ull
#define LO1 0x456789ABCDEF0123ull
#define NOPS ((int)(sizeof(kOps)/sizeof(kOps[0])))

#define RD 1
#define RS 2
#define RT 3

typedef struct { u32 w[4]; } Q;

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
	{ "C_TWOTWOTWOTWO", {{2u,2u,2u,2u}} },
	{ "C_ONEONEONEONE", {{1u,1u,1u,1u}} },
};

static int operand(char** pp, Q* out, int* named)
{
	char* p = *pp;
	size_t i, best = (size_t)-1, bestlen = 0;
	while (*p == ' ' || *p == ',') p++;
	for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
	{
		const size_t n = strlen(kConst[i].name);
		if (!strncmp(p, kConst[i].name, n) && n > bestlen) { best = i; bestlen = n; }
	}
	if (best != (size_t)-1)
	{ *out = kConst[best].v; *named = 1; p += bestlen; *pp = p; return 1; }
	{
		char* e;
		long long v = strtoll(p, &e, 10);
		if (e == p) return 0;
		/* SET_U32: lui/ori, so the low doubleword sign-extended, nothing above. */
		out->w[0] = (u32)v;
		out->w[1] = (v < 0) ? 0xFFFFFFFFu : 0u;
		out->w[2] = out->w[3] = 0;
		*named = 0;
		*pp = e;
		return 1;
	}
}

static void set_reg(int r, const Q* q)
{
	cpuRegs.GPR.r[r].UL[0] = q->w[0]; cpuRegs.GPR.r[r].UL[1] = q->w[1];
	cpuRegs.GPR.r[r].UL[2] = q->w[2]; cpuRegs.GPR.r[r].UL[3] = q->w[3];
}

/* Case count for a block, so the store seed can switch at its midpoint.
 * Each store block runs its offsets twice, first from SET_U32<0xABCD4321>
 * and then from SET_M(C_GARBAGE1), and the number of offsets is not
 * constant -- three for the byte through doubleword stores, four for SQ.
 * Assuming three seeds the wrong half of the larger block. */
static int count_block(const char* path, const char* name)
{
	FILE* f = fopen(path, "r");
	char buf[512];
	int in = 0, n = 0;
	if (!f) return 0;
	while (fgets(buf, sizeof(buf), f))
	{
		if (buf[0] != ' ')
		{
			char* c = strchr(buf, ':');
			if (in) break;
			if (c && c[1] == '\n') { *c = '\0'; in = !strcmp(buf, name); *c = ':'; }
			continue;
		}
		if (!in || strstr(buf, "-> $0")) continue;
		n++;
	}
	fclose(f);
	return n;
}

/* Memory backing for the load and store blocks. */
static void reset_pattern(void) { memcpy(s_mem, kPattern, sizeof(kPattern)); }

/* Read "+0", "-16" or "+-16": the second half of each block prints a
 * negative offset through a "+%d" format, so the sign can follow the plus. */
static int read_off(char** pp, int* out)
{
	char* p = *pp;
	while (*p == ' ') p++;
	if (*p == '+') p++;
	if (*p != '-' && (*p < '0' || *p > '9')) return 0;
	*out = (int)strtol(p, &p, 10);
	*pp = p;
	return 1;
}

/* The load, store and multiply/divide blocks: different files, different
 * line shapes, and for the stores the result is memory rather than a
 * register. Returns 0 when the line is not a case. */
static int run_special(int op, char* buf, int* cases, int* pass, int* failures,
                       int block_total)
{
	unsigned w3, w2, w1, w0;
	int off = 0;
	char* p;

	if (strstr(buf, "-> $0")) return 0;
	p = buf + 2;
	while (*p && *p != ' ') p++;

	if (kOps[op].shape == HILO_MOVE)
	{
		/* "mfhi <n>: <rd quad> H: hi hi1 L: lo lo1". The MF forms write rd
		 * from a pipeline register and the MT forms write the pipeline from
		 * rs, so both directions are covered by comparing rd and the
		 * pipelines together -- which is also what tells a move from HI
		 * apart from a move from LO. */
		unsigned long long wh, wh1, wl, wl1;
		unsigned q3, q2, q1, q0;
		Q rs_q;
		char* q = strstr(buf, "H: ");
		const int is_mt = kOps[op].name[1] == 't';
		if (!q) return 0;
		if (sscanf(q, "H: %llx %llx L: %llx %llx", &wh, &wh1, &wl, &wl1) != 4)
			return 0;
		{
			char* c = strchr(buf, ':');
			if (!c || sscanf(c + 2, "%x %x %x %x", &q3, &q2, &q1, &q0) != 4)
				return 0;
			/* Half these lines name a constant rather than a decimal --
			 * "mthi C_NEGONE:" -- and strtol reads those as zero, which
			 * makes the MT forms score 8 of 18 while looking like a
			 * pipeline-write bug. operand() resolves both. */
			{
				char* ap = p;
				int dummy = 0;
				if (!operand(&ap, &rs_q, &dummy)) return 0;
			}
		}

		memset(&cpuRegs.GPR, 0, sizeof(cpuRegs.GPR));
		cpuRegs.HI.UD[0] = HI0; cpuRegs.HI.UD[1] = HI1;
		cpuRegs.LO.UD[0] = LO0; cpuRegs.LO.UD[1] = LO1;
		cpuRegs.GPR.r[RS].UL[0] = rs_q.w[0];
		cpuRegs.GPR.r[RS].UL[1] = rs_q.w[1];
		cpuRegs.GPR.r[RS].UL[2] = rs_q.w[2];
		cpuRegs.GPR.r[RS].UL[3] = rs_q.w[3];
		/* rd carries the same seed the ee/muldiv program leaves. */
		cpuRegs.GPR.r[RD].UD[1] = 0x0000133A00001339ull;
		cpuRegs.code = ((u32)RS << 21) | ((u32)RD << 11);
		kOps[op].fn();

		(*cases)++;
		{
			const int hilo_ok = cpuRegs.HI.UD[0] == wh && cpuRegs.HI.UD[1] == wh1
			                 && cpuRegs.LO.UD[0] == wl && cpuRegs.LO.UD[1] == wl1;
			/* The MT forms leave rd alone, so only the pipelines are the
			 * result there. */
			const int rd_ok = is_mt
			               || (cpuRegs.GPR.r[RD].UL[0] == q0
			                && cpuRegs.GPR.r[RD].UL[1] == q1);
			if (hilo_ok && rd_ok) (*pass)++;
			else if ((*failures)++ < 6)
				printf("  %-5s %d: console rd %08x %08x  H %016llx %016llx\n"
				       "           ours    rd %08x %08x  H %016llx %016llx\n",
				       kOps[op].name, (int)rs_q.w[0], q1, q0, wh, wh1,
				       cpuRegs.GPR.r[RD].UL[1], cpuRegs.GPR.r[RD].UL[0],
				       (unsigned long long)cpuRegs.HI.UD[0],
				       (unsigned long long)cpuRegs.HI.UD[1]);
		}
		return 0;
	}

	if (kOps[op].shape == MULDIV)
	{
		u32 a, b;
		unsigned long long wh, wh1, wl, wl1;
		char* q = p;
		int na = 0, nb = 0;
		Q qa, qb;
		if (!operand(&q, &qa, &na)) return 0;
		if (!operand(&q, &qb, &nb)) return 0;
		a = qa.w[0]; b = qb.w[0];
		/* Anchor on the H: marker rather than the first colon. The
		 * multiply lines print rd between the two -- "mult a, b: <quad>
		 * H: ... L: ..." -- while the divide lines go straight to H:, so
		 * scanning from the colon parses div and drops every mult and
		 * multu line. They scored 0 of 0 and the run still passed. */
		q = strstr(buf, "H: ");
		if (!q || sscanf(q, "H: %llx %llx L: %llx %llx",
		                 &wh, &wh1, &wl, &wl1) != 4) return 0;

		cpuRegs.HI.UD[0] = HI0; cpuRegs.HI.UD[1] = HI1;
		cpuRegs.LO.UD[0] = LO0; cpuRegs.LO.UD[1] = LO1;
		cpuRegs.GPR.r[RS].UD[0] = (u64)(s64)(s32)a;
		cpuRegs.GPR.r[RT].UD[0] = (u64)(s64)(s32)b;
		cpuRegs.code = ((u32)RS << 21) | ((u32)RT << 16);
		kOps[op].fn();

		(*cases)++;
		if (cpuRegs.HI.UD[0] == wh && cpuRegs.HI.UD[1] == wh1
		 && cpuRegs.LO.UD[0] == wl && cpuRegs.LO.UD[1] == wl1) (*pass)++;
		else if ((*failures)++ < 6)
			printf("  %-6s %d, %d: console H %016llx %016llx L %016llx %016llx\n"
			       "         ours    H %016llx %016llx L %016llx %016llx\n",
			       kOps[op].name, (int)a, (int)b, wh, wh1, wl, wl1,
			       (unsigned long long)cpuRegs.HI.UD[0],
			       (unsigned long long)cpuRegs.HI.UD[1],
			       (unsigned long long)cpuRegs.LO.UD[0],
			       (unsigned long long)cpuRegs.LO.UD[1]);
		return 0;
	}

	if (!read_off(&p, &off)) return 0;
	p = strchr(buf, ':');
	if (!p) return 0;

	reset_pattern();
	if (kOps[op].shape == LOAD)
	{
		if (sscanf(p + 1, " %x %x %x %x", &w3, &w2, &w1, &w0) != 4) return 0;
		/* rt carries between cases; seed it as the lsu harness leaves it. */
		cpuRegs.GPR.r[RT].UD[1] = 0x0000133A00001339ull;
		cpuRegs.GPR.r[RS].UD[0] = LOAD_BASE;
		cpuRegs.code = ((u32)RS << 21) | ((u32)RT << 16) | (u32)(off & 0xFFFF);
		kOps[op].fn();
		(*cases)++;
		{
			const u32 g0 = cpuRegs.GPR.r[RT].UL[0], g1 = cpuRegs.GPR.r[RT].UL[1];
			const u32 g2 = cpuRegs.GPR.r[RT].UL[2], g3 = cpuRegs.GPR.r[RT].UL[3];
			if (g0 == w0 && g1 == w1 && g2 == w2 && g3 == w3) (*pass)++;
			else if ((*failures)++ < 6)
				printf("  %-4s %+d: console %08x %08x %08x %08x  ours %08x %08x %08x %08x\n",
				       kOps[op].name, off, w3, w2, w1, w0, g3, g2, g1, g0);
		}
		return 0;
	}

	/* STORE: the line prints the whole sixteen-byte row the store landed
	 * in, high word first, which is row 1 + offset/16 of the pattern. */
	{
		unsigned m3, m2, m1, m0;
		int row;
		if (sscanf(p + 1, " %x %x %x %x", &m3, &m2, &m1, &m0) != 4) return 0;
		cpuRegs.GPR.r[RT].UD[0] = (*cases < block_total / 2)
		                        ? (u64)(s64)(s32)0xABCD4321u
		                        : ((u64)0x1338u << 32) | 0x1337u;
		cpuRegs.GPR.r[RT].UD[1] = 0x0000133A00001339ull;
		cpuRegs.GPR.r[RS].UD[0] = STORE_BASE;
		cpuRegs.code = ((u32)RS << 21) | ((u32)RT << 16) | (u32)(off & 0xFFFF);
		s_cancelled = 0;
		kOps[op].fn();
		(*cases)++;
		row = 16 + (off / 16) * 16;   /* base row plus the printed offset */
		{
			u32 g[4];
			int i;
			for (i = 0; i < 4; i++)
				memcpy(&g[i], s_mem + ((row + i * 4) & (MEMSIZE - 1)), 4);
			if (!s_cancelled && g[0] == m0 && g[1] == m1
			 && g[2] == m2 && g[3] == m3) (*pass)++;
			else if ((*failures)++ < 6)
				printf("  %-4s %+d: console %08x %08x %08x %08x  ours %08x %08x %08x %08x%s\n",
				       kOps[op].name, off, m3, m2, m1, m0,
				       g[3], g[2], g[1], g[0],
				       s_cancelled ? " (cancelled)" : "");
		}
	}
	return 0;
}

int main(int argc, char** argv)
{
	const char* dir = (argc > 1) ? argv[1] : ".";
	int op, failures = 0, total = 0;
	/* rd carries between cases across the whole file, as in hwalu.c. */
	u64 rd_hi = 0;

	for (op = 0; op < NOPS; op++)
	{
		char path[512], head[32], buf[512];
		FILE* f;
		int inblock = 0, pass = 0, cases = 0, block_total = 0;

		snprintf(path, sizeof(path), "%s/%s.expected", dir,
		         (kOps[op].shape == LOAD || kOps[op].shape == STORE) ? "lsu"
		       : (kOps[op].shape == MULDIV
		       || kOps[op].shape == HILO_MOVE) ? "muldiv" : "alu");
		f = fopen(path, "r");
		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kOps[op].name);
		block_total = count_block(path, kOps[op].name);

		while (fgets(buf, sizeof(buf), f))
		{
			Q a, b; unsigned w3, w2, w1, w0;
			int named_a = 0, named_b = 0;
			char* p; u32 sa = 0, imm = 0;
			int dest;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;
			if (strstr(buf, "-> $0")) continue;

			if (kOps[op].shape == LOAD || kOps[op].shape == STORE
			 || kOps[op].shape == MULDIV || kOps[op].shape == HILO_MOVE)
			{
				if (!run_special(op, buf, &cases, &pass, &failures, block_total)) continue;
				continue;
			}

			p = buf + 2 + strlen(kOps[op].name);
			if (kOps[op].shape == RI)
			{
				a.w[0] = a.w[1] = a.w[2] = a.w[3] = 0;
				if (!operand(&p, &b, &named_b)) continue;
			}
			else
			{
				if (!operand(&p, &a, &named_a)) continue;
				if (!operand(&p, &b, &named_b)) continue;
			}
			while (*p == ' ' || *p == ':') p++;
			if (sscanf(p, "%x %x %x %x", &w3, &w2, &w1, &w0) != 4) continue;

			/* rd's carried upper doubleword, as hwalu.c models it. */
			if (named_a || named_b) rd_hi = 0x0000133A00001339ull;
			cpuRegs.GPR.r[RD].UD[1] = rd_hi;
			cpuRegs.GPR.r[RD].UD[0] = (named_a || named_b)
			                        ? ((u64)0x1338u << 32) | 0x1337u : 0x1337u;

			switch (kOps[op].shape)
			{
			case RRR:
				if (kOps[op].swap) { const Q t = a; a = b; b = t; }
				set_reg(RS, &a); set_reg(RT, &b);
				cpuRegs.code = ((u32)RS << 21) | ((u32)RT << 16) | ((u32)RD << 11);
				dest = RD;
				break;
			case RRI:
				set_reg(RS, &a);
				imm = b.w[0] & 0xFFFF;
				cpuRegs.code = ((u32)RS << 21) | ((u32)RT << 16) | imm;
				dest = RT;
				break;
			case RI:
				imm = b.w[0] & 0xFFFF;
				cpuRegs.code = ((u32)RT << 16) | imm;
				dest = RT;
				break;
			default: /* RRS */
				set_reg(RT, &a);
				sa = b.w[0] & 0x1F;
				cpuRegs.code = ((u32)RT << 16) | ((u32)RD << 11) | (sa << 6);
				dest = RD;
				break;
			}
			/* The immediate forms write rt, so it has to carry the seed too. */
			if (dest == RT)
			{
				cpuRegs.GPR.r[RT].UD[1] = rd_hi;
				cpuRegs.GPR.r[RT].UD[0] = (named_a || named_b)
				                        ? ((u64)0x1338u << 32) | 0x1337u : 0x1337u;
			}

			kOps[op].fn();

			cases++;
			{
				const u32 g0 = cpuRegs.GPR.r[dest].UL[0];
				const u32 g1 = cpuRegs.GPR.r[dest].UL[1];
				const u32 g2 = cpuRegs.GPR.r[dest].UL[2];
				const u32 g3 = cpuRegs.GPR.r[dest].UL[3];
				if (g0 == w0 && g1 == w1 && g2 == w2 && g3 == w3) pass++;
				else if (failures++ < 6)
					printf("  %-7s console %08x %08x %08x %08x  ours %08x %08x %08x %08x\n",
					       kOps[op].name, w3, w2, w1, w0, g3, g2, g1, g0);
				rd_hi = ((u64)w3 << 32) | w2;
			}
		}
		fclose(f);
		total += cases;
		/* block_total is already the number of case lines the block has --
		 * it is computed above for the store seed's midpoint -- so compare
		 * against it rather than only checking for zero. Every
		 * under-coverage bug in this series was partial: a missing
		 * constant, an operand shape matching some lines and not others.
		 * Checking "did anything parse" would have caught none of them. */
		if (block_total > 0 && cases != block_total)
		{ printf("%-7s parsed %d of the %d lines its block has;"
		         " some operand shape does not match\n",
		         kOps[op].name, cases, block_total);
		  failures++; }
		else if (cases == 0)
		{ printf("%-7s parsed no cases; the block name or line shape changed\n",
		         kOps[op].name);
		  failures++; }
		else if (pass != cases)
		{ printf("%-7s %2d/%-3d console cases\n", kOps[op].name, pass, cases);
		  failures++; }
	}

	/* ---- cases the capture cannot supply -------------------------------
	 *
	 * MIPS splits the immediate forms: the arithmetic and compare ones --
	 * ADDIU, DADDIU, SLTI, SLTIU -- sign-extend their immediate, and the
	 * logical ones -- ANDI, ORI, XORI -- zero-extend it. Getting that
	 * split wrong is a classic implementation mistake and the capture
	 * cannot see it: not one of the 144 lines across those seven ops in
	 * tests/cpu/ee/alu.expected uses a negative or high-bit immediate, so
	 * swapping _Imm_ for _ImmU_ in SLTIU passes all 814 console cases.
	 *
	 * These come from the MIPS definition rather than from hardware, and
	 * every one uses an immediate with bit 15 set, which is the only place
	 * the two extensions differ.
	 */
	{
		static const struct { const char* name; void (*fn)(); u64 rs; u32 imm; u64 want; }
		kSpec[] = {
			/* Sign-extending: 0xffff means -1. */
			{ "addiu",  intADDIU,  0x0000000000000001ull, 0xFFFFu, 0x0000000000000000ull },
			{ "addiu",  intADDIU,  0x0000000000000000ull, 0xFFFFu, 0xFFFFFFFFFFFFFFFFull },
			{ "daddiu", intDADDIU, 0x0000000000000001ull, 0xFFFFu, 0x0000000000000000ull },
			{ "slti",   intSLTI,   0xFFFFFFFFFFFFFFFEull, 0xFFFFu, 1 },  /* -2 < -1 */
			{ "slti",   intSLTI,   0x0000000000000000ull, 0xFFFFu, 0 },
			{ "sltiu",  intSLTIU,  0x0000000000000000ull, 0xFFFFu, 1 },  /* 0 < ffff..ff */
			{ "sltiu",  intSLTIU,  0x0000000000010000ull, 0xFFFFu, 1 },  /* zero-ext says 0 */
			/* Zero-extending: 0xffff means 65535, not -1. */
			{ "andi",   intANDI,   0xFFFFFFFFFFFFFFFFull, 0xFFFFu, 0x000000000000FFFFull },
			{ "ori",    intORI,    0x0000000000000000ull, 0xFFFFu, 0x000000000000FFFFull },
			{ "xori",   intXORI,   0x0000000000000000ull, 0xFFFFu, 0x000000000000FFFFull },
		};
		const int n = (int)(sizeof(kSpec)/sizeof(kSpec[0]));
		int i, pass = 0;

		for (i = 0; i < n; i++)
		{
			memset(&cpuRegs.GPR, 0, sizeof(cpuRegs.GPR));
			cpuRegs.GPR.r[RS].UD[0] = kSpec[i].rs;
			cpuRegs.code = ((u32)RS << 21) | ((u32)RT << 16) | kSpec[i].imm;
			kSpec[i].fn();
			if (cpuRegs.GPR.r[RT].UD[0] == kSpec[i].want) pass++;
			else
				printf("  %-7s %016llx, 0xffff: MIPS says %016llx, ours %016llx\n",
				       kSpec[i].name,
				       (unsigned long long)kSpec[i].rs,
				       (unsigned long long)kSpec[i].want,
				       (unsigned long long)cpuRegs.GPR.r[RT].UD[0]);
		}
		total += n;
		printf("spec    %2d/%-3d immediate-extension cases from the MIPS definition\n",
		       pass, n);
		if (pass != n) failures++;
	}

	printf("hwrealee: %d cases across %d ops, driven through R5900OpcodeImpl.cpp\n",
	       total, NOPS);
	return failures != 0;
}
