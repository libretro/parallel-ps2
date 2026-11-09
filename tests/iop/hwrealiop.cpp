/*  IOP integer ops against console captures, driving the real code.
 *
 *  The counterpart to tests/ee/hwrealee.cpp. tests/iop/hwalu.c,
 *  hwmuldiv.c and hwlsu.c transcribe the semantics out of
 *  R3000AOpcodeTables.cpp and score the copy; this links that file and
 *  calls the ops through their real entry points.
 *
 *  Worth doing here in particular because two of the bugs this series
 *  found were in the IOP interpreter -- MTSA not masking its shift amount
 *  and JALR reading its target after writing the link -- and both were
 *  found by reading rather than by a test. A harness on the real code is
 *  what would have caught them.
 *
 *  IopGte.cpp is linked rather than stubbed, since the opcode table names
 *  the GTE ops and they are real code worth having under the same
 *  sanitizer run. The memory accessors are backed by an array so the load
 *  and store blocks execute rather than being skipped.
 *
 *  Usage: tests/iop/hwrealiop <dir-with-the-.expected-files>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "R3000A.h"
#include "IopMem.h"
#include "IopGte.h"
#include "Config.h"

alignas(16) psxRegisters psxRegs;

/* ---- memory and stubs ------------------------------------------------- */

#define MEMSIZE 4096
static u8 s_mem[MEMSIZE];
static u32 clampaddr(u32 a) { return a & (MEMSIZE - 1); }

u8  iopMemRead8_slow (u32 a) { return s_mem[clampaddr(a)]; }
u16 iopMemRead16_slow(u32 a) { u16 v; memcpy(&v, s_mem + clampaddr(a), 2); return v; }
u32 iopMemRead32_slow(u32 a) { u32 v; memcpy(&v, s_mem + clampaddr(a), 4); return v; }
void iopMemWrite8 (u32 a, u8  v) { s_mem[clampaddr(a)] = v; }
void iopMemWrite16(u32 a, u16 v) { memcpy(s_mem + clampaddr(a), &v, 2); }
void iopMemWrite32(u32 a, u32 v) { memcpy(s_mem + clampaddr(a), &v, 4); }

/* The lookup tables the inline fast paths consult; one page each is
 * enough, since every address is masked into s_mem anyway. */
/* IopMem.h declares these as pointers to const, not arrays, so the
 * storage is ours and the exported names point into it. */
static uptr s_rlut_store[0x10000];
static uptr s_wlut_store[0x10000];
const uptr* psxMemRLUT = s_rlut_store;
uptr* psxMemWLUT = s_wlut_store;

void psxException(u32, u32) { }
/* Reached only by the branch and syscall paths, which these blocks do not
 * exercise; the ALU, load, store and muldiv ops never touch them. */
alignas(16) u8 iopHw[0x10000];
int iopIsDelaySlot = 0;
void iopEventTest() { }
bool psxBiosCall() { return false; }
/* The interpreter's IRX injection check and J-stub hook; these blocks
 * neither reach 0x1630 nor jump. */
extern "C" bool emu_config_irx_injected(void) { return false; }
extern "C" int irxImportExecCached(u32, u16) { return 0; }

/* ---- the harness ------------------------------------------------------ */

/* The opcode tables are C (R3000AOpcodeTables.c). */
extern "C" {

	void psxADDU(); void psxSUBU(); void psxAND(); void psxOR();
	void psxXOR(); void psxNOR(); void psxSLT(); void psxSLTU();
	void psxSLLV(); void psxSRLV(); void psxSRAV();
	void psxADDIU(); void psxANDI(); void psxORI(); void psxXORI();
	void psxSLTI(); void psxSLTIU(); void psxLUI();
	void psxSLL(); void psxSRL(); void psxSRA();
	void psxDIV(); void psxDIVU(); void psxMULT(); void psxMULTU();
	void psxLB(); void psxLBU(); void psxLH(); void psxLHU(); void psxLW();
	void psxSB(); void psxSH(); void psxSW();
	void psxLWL(); void psxLWR(); void psxSWL(); void psxSWR();
	void psxMTSA(); void psxSLTI();
}

enum Shape { RRR, RRI, RRS, RI, MULDIV, LOAD, STORE };

static const struct { const char* name; void (*fn)(); Shape shape; int swap; }
kOps[] = {
	{ "addu",psxADDU,RRR,0 },{ "subu",psxSUBU,RRR,0 },
	{ "and",psxAND,RRR,0 },{ "or",psxOR,RRR,0 },
	{ "xor",psxXOR,RRR,0 },{ "nor",psxNOR,RRR,0 },
	{ "slt",psxSLT,RRR,0 },{ "sltu",psxSLTU,RRR,0 },
	/* The variable shifts take the value first and the amount second. */
	{ "sllv",psxSLLV,RRR,1 },{ "srlv",psxSRLV,RRR,1 },{ "srav",psxSRAV,RRR,1 },
	{ "addiu",psxADDIU,RRI,0 },{ "andi",psxANDI,RRI,0 },
	{ "ori",psxORI,RRI,0 },{ "xori",psxXORI,RRI,0 },
	{ "slti",psxSLTI,RRI,0 },{ "sltiu",psxSLTIU,RRI,0 },
	{ "lui",psxLUI,RI,0 },
	{ "sll",psxSLL,RRS,0 },{ "srl",psxSRL,RRS,0 },{ "sra",psxSRA,RRS,0 },
	{ "div",psxDIV,MULDIV,0 },{ "divu",psxDIVU,MULDIV,0 },
	{ "mult",psxMULT,MULDIV,0 },{ "multu",psxMULTU,MULDIV,0 },
	{ "lb",psxLB,LOAD,0 },{ "lbu",psxLBU,LOAD,0 },
	{ "lh",psxLH,LOAD,0 },{ "lhu",psxLHU,LOAD,0 },{ "lw",psxLW,LOAD,0 },
	{ "sb",psxSB,STORE,0 },{ "sh",psxSH,STORE,0 },{ "sw",psxSW,STORE,0 },
};
#define NOPS ((int)(sizeof(kOps)/sizeof(kOps[0])))

#define RD 1
#define RS 2
#define RT 3

/* The pattern the lsu harness reads, and its two bases: loads run from
 * byte 16, stores from byte 4, which is a row index against a flat word
 * index rather than a mistake. */
static const u32 kPattern[12] = {
	0x45678123u, 0x9ABCDEF0u, 0xDEADBEEFu, 0xC0DE1337u,
	0x23456789u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu,
	0x8899AABBu, 0xCCDDEEFFu, 0x00112233u, 0x44556677u,
};
#define LOAD_BASE  16
#define STORE_BASE  4

static const struct { const char* name; u32 v; } kConst[] = {
	{ "C_ZERO",0 },{ "C_S16_MAX",0x7FFFu },{ "C_S16_MIN",0xFFFF8000u },
	{ "C_S32_MAX",0x7FFFFFFFu },{ "C_S32_MIN",0x80000000u },
	{ "C_NEGONE",0xFFFFFFFFu },{ "C_ONE",1u },
	{ "C_GARBAGE1",0x1337u },{ "C_GARBAGE2",0xDEADBEEFu },
	{ "C_S64_MAX",0xFFFFFFFFu },{ "C_S64_MIN",0u },
	{ "C_TWOTWOTWOTWO",2u },{ "C_ONEONEONEONE",1u },
};

static int operand(char** pp, u32* out)
{
	char* p = *pp;
	size_t i, best = (size_t)-1, bestlen = 0;
	while (*p == ' ' || *p == ',') p++;
	for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
	{
		const size_t n = strlen(kConst[i].name);
		if (!strncmp(p, kConst[i].name, n) && n > bestlen) { best = i; bestlen = n; }
	}
	if (best != (size_t)-1) { *out = kConst[best].v; *pp = p + bestlen; return 1; }
	{
		char* e; long v = strtol(p, &e, 10);
		if (e == p) return 0;
		*out = (u32)v; *pp = e; return 1;
	}
}

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

int main(int argc, char** argv)
{
	const char* dir = (argc > 1) ? argv[1] : ".";
	int op, failures = 0, total = 0, i;

	for (i = 0; i < 0x10000; i++)
	{ s_rlut_store[i] = (uptr)s_mem; s_wlut_store[i] = (uptr)s_mem; }

	for (op = 0; op < NOPS; op++)
	{
		char path[512], head[32], buf[512];
		FILE* f;
		int inblock = 0, pass = 0, cases = 0, block_total = 0;
		const char* file =
			(kOps[op].shape == MULDIV) ? "muldiv"
		  : (kOps[op].shape == LOAD || kOps[op].shape == STORE) ? "lsu" : "alu";

		snprintf(path, sizeof(path), "%s/%s.expected", dir, file);
		f = fopen(path, "r");
		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kOps[op].name);
		block_total = count_block(path, kOps[op].name);

		while (fgets(buf, sizeof(buf), f))
		{
			char* p;
			u32 a = 0, b = 0;
			unsigned want, wh, wl;
			int off = 0;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;
			if (strstr(buf, "-> $0")) continue;

			p = buf + 2 + strlen(kOps[op].name);
			memset(&psxRegs.GPR, 0, sizeof(psxRegs.GPR));

			switch (kOps[op].shape)
			{
			case MULDIV:
				if (!operand(&p, &a) || !operand(&p, &b)) continue;
				p = strchr(buf, ':');
				/* Anchored on the marker rather than the first colon. The
				 * IOP lines have no rd quad so either works here, but the
				 * EE ones do, and scanning from the colon silently dropped
				 * every mult case there. */
				p = strstr(buf, "H: ");
				if (!p || sscanf(p, "H: %x L: %x", &wh, &wl) != 2) continue;
				psxRegs.GPR.r[RS] = a; psxRegs.GPR.r[RT] = b;
				psxRegs.code = ((u32)RS << 21) | ((u32)RT << 16);
				kOps[op].fn();
				cases++;
				if (psxRegs.GPR.n.hi == wh && psxRegs.GPR.n.lo == wl) pass++;
				else if (failures++ < 6)
					printf("  %-5s %08x, %08x: console H %08x L %08x  ours H %08x L %08x\n",
					       kOps[op].name, a, b, wh, wl,
					       psxRegs.GPR.n.hi, psxRegs.GPR.n.lo);
				continue;

			case LOAD:
				if (!read_off(&p, &off)) continue;
				p = strchr(buf, ':');
				if (!p || sscanf(p + 1, " %x", &want) != 1) continue;
				memcpy(s_mem, kPattern, sizeof(kPattern));
				psxRegs.GPR.r[RS] = LOAD_BASE;
				psxRegs.code = ((u32)RS << 21) | ((u32)RT << 16) | (u32)(off & 0xFFFF);
				kOps[op].fn();
				cases++;
				if (psxRegs.GPR.r[RT] == want) pass++;
				else if (failures++ < 6)
					printf("  %-4s %+d: console %08x  ours %08x\n",
					       kOps[op].name, off, want, psxRegs.GPR.r[RT]);
				continue;

			case STORE:
			{
				u32 g0, g1;
				int base;
				if (!read_off(&p, &off)) continue;
				p = strchr(buf, ':');
				if (!p || sscanf(p + 1, " %x %x", &wh, &wl) != 2) continue;
				memcpy(s_mem, kPattern, sizeof(kPattern));
				/* Each block runs its offsets twice, from the immediate seed
				 * and then from C_GARBAGE1, switching at its midpoint. */
				psxRegs.GPR.r[RT] = (cases < block_total / 2)
				                  ? 0xABCD4321u : 0x1337u;
				psxRegs.GPR.r[RS] = STORE_BASE;
				psxRegs.code = ((u32)RS << 21) | ((u32)RT << 16) | (u32)(off & 0xFFFF);
				kOps[op].fn();
				cases++;
				base = (STORE_BASE + off) & ~3;
				memcpy(&g0, s_mem + (base & (MEMSIZE - 1)), 4);
				memcpy(&g1, s_mem + ((base + 4) & (MEMSIZE - 1)), 4);
				if (g0 == wh && g1 == wl) pass++;
				else if (failures++ < 6)
					printf("  %-4s %+d: console %08x %08x  ours %08x %08x\n",
					       kOps[op].name, off, wh, wl, g0, g1);
				continue;
			}

			case RI:
				if (!operand(&p, &b)) continue;
				break;
			case RRS:
			case RRI:
			case RRR:
			default:
				if (!operand(&p, &a) || !operand(&p, &b)) continue;
				break;
			}

			p = strchr(buf, ':');
			if (!p || sscanf(p + 1, " %x", &want) != 1) continue;

			{
				int dest = RD;
				if (kOps[op].shape == RRR)
				{
					if (kOps[op].swap) { const u32 t = a; a = b; b = t; }
					psxRegs.GPR.r[RS] = a; psxRegs.GPR.r[RT] = b;
					psxRegs.code = ((u32)RS << 21) | ((u32)RT << 16) | ((u32)RD << 11);
				}
				else if (kOps[op].shape == RRI)
				{
					psxRegs.GPR.r[RS] = a;
					psxRegs.code = ((u32)RS << 21) | ((u32)RT << 16) | (b & 0xFFFF);
					dest = RT;
				}
				else if (kOps[op].shape == RI)
				{
					psxRegs.code = ((u32)RT << 16) | (b & 0xFFFF);
					dest = RT;
				}
				else /* RRS */
				{
					psxRegs.GPR.r[RT] = a;
					psxRegs.code = ((u32)RT << 16) | ((u32)RD << 11) | ((b & 0x1F) << 6);
				}
				kOps[op].fn();
				cases++;
				if (psxRegs.GPR.r[dest] == want) pass++;
				else if (failures++ < 6)
					printf("  %-6s %08x, %08x: console %08x  ours %08x\n",
					       kOps[op].name, a, b, want, psxRegs.GPR.r[dest]);
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
		{ printf("%-6s parsed %d of the %d lines its block has;"
		         " some operand shape does not match\n",
		         kOps[op].name, cases, block_total);
		  failures++; }
		else if (cases == 0)
		{ printf("%-6s parsed no cases; the block name or line shape changed\n",
		         kOps[op].name);
		  failures++; }
		else if (pass != cases)
		{ printf("%-6s %2d/%-3d console cases\n", kOps[op].name, pass, cases);
		  failures++; }
	}

	/* ---- cases the capture cannot supply -------------------------------
	 *
	 * SLTIU and SLTI sign-extend their immediate and then compare, SLTIU
	 * comparing the result as unsigned. Every sltiu line in
	 * tests/cpu/iop/alu.expected uses a small positive immediate -- not
	 * one has bit 15 set -- so the capture cannot tell a sign-extending
	 * implementation from a zero-extending one, and swapping _Imm_ for
	 * _ImmU_ passes all 558 console cases.
	 *
	 * The architecture does define it, so these three cases come from the
	 * MIPS definition rather than from hardware. With an immediate of
	 * -1 the sign-extended comparand is 0xffffffff, which almost nothing
	 * is below; zero-extended it would be 0x0000ffff, which most values
	 * are above. The two disagree on every line here.
	 */
	{
		static const struct { const char* name; u32 rs; u32 imm; u32 want; }
		kSpec[] = {
			/* sltiu rs, -1: unsigned compare against 0xffffffff. */
			{ "sltiu",  0x00000000u, 0xFFFFu, 1 },  /* 0 < 0xffffffff      */
			{ "sltiu",  0x00010000u, 0xFFFFu, 1 },  /* zero-extend says 0  */
			{ "sltiu",  0xFFFFFFFFu, 0xFFFFu, 0 },  /* equal, so not less  */
			/* slti rs, -1: signed compare against -1. */
			{ "slti",   0x00000000u, 0xFFFFu, 0 },
			{ "slti",   0xFFFFFFFEu, 0xFFFFu, 1 },  /* -2 < -1             */
		};
		const int n = (int)(sizeof(kSpec)/sizeof(kSpec[0]));
		int i, pass = 0;

		for (i = 0; i < n; i++)
		{
			const int is_u = !strcmp(kSpec[i].name, "sltiu");
			memset(&psxRegs.GPR, 0, sizeof(psxRegs.GPR));
			psxRegs.GPR.r[RS] = kSpec[i].rs;
			psxRegs.code = ((u32)RS << 21) | ((u32)RT << 16) | kSpec[i].imm;
			if (is_u) psxSLTIU(); else psxSLTI();
			if (psxRegs.GPR.r[RT] == kSpec[i].want) pass++;
			else
				printf("  %s %08x, -1: MIPS says %u, ours %u\n",
				       kSpec[i].name, kSpec[i].rs, kSpec[i].want,
				       psxRegs.GPR.r[RT]);
		}
		total += n;
		printf("spec    %2d/%-3d immediate-extension cases from the MIPS definition\n",
		       pass, n);
		if (pass != n) failures++;
	}

	printf("hwrealiop: %d cases across %d ops, driven through R3000AOpcodeTables.c\n",
	       total, NOPS);
	return failures != 0;
}
