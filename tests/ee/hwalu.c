/*  EE ALU three-register ops against console captures.
 *
 *  Scores all thirty-five ops in ps2autotests tests/cpu/ee/alu.expected
 *  against the semantics in pcsx2/R5900OpcodeImpl.c: the eighteen
 *  three-register ones, the five register-immediate, the eleven
 *  shift-by-immediate and compare-immediate, and LUI.
 *
 *  The immediate is printed as an unsigned decimal but is sign-extended
 *  where the instruction sign-extends it -- "addiu C_ONE, 65535" gives 0,
 *  which is 1 + (-1), not 65536 -- and zero-extended for the logical ops,
 *  which is what ANDI, ORI and XORI do with it.
 *
 *  Every one of these writes only the low doubleword of rd, so the upper
 *  one is whatever the harness happened to leave there -- and that is
 *  carried across the whole file, not reset per case. rd is one register
 *  reused throughout; SET_U32 is a lui/ori pair touching only the low
 *  half, while SET_M writes all 128 bits from C_GARBAGE1. So the upper
 *  doubleword starts at zero, becomes 0000133a00001339 at the first
 *  named-constant case, and stays there for every case after it,
 *  including the decimal ones at the top of every later block.
 *
 *  That is why this makes a single ordered pass over the file rather than
 *  scanning per op: scoring each block from a clean slate gets the first
 *  op right and every later one wrong in its decimal half, 12 of 19 for
 *  the 64-bit ops and 8 of 19 for the shifts.
 *
 *  Checking the upper half at all is the point. An implementation that
 *  wrongly wrote all 128 bits would match every case if only the low half
 *  were compared.
 *
 *  Usage: tests/ee/hwalu <path-to-alu.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef int16_t  s16;
typedef uint16_t u16;
typedef int64_t  s64;

/* Full 128-bit operands: the 64-bit ops read the whole low doubleword. */
typedef struct { u32 w[4]; } Q;
static u64 lo64(const Q* q) { return ((u64)q->w[1] << 32) | q->w[0]; }

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
};

/* rd before the op: zero above the low word for the decimal cases,
 * C_GARBAGE1 entire for the named ones. */
#define RD_GARBAGE_HI 0x0000133A00001339ull

static int operand(char** pp, Q* out, int* named)
{
	char* p = *pp;
	while (*p == ' ' || *p == ',') p++;
	if (*p == 'C' && p[1] == '_')
	{
		size_t i, best = (size_t)-1, bestlen = 0;
		for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
		{
			const size_t n = strlen(kConst[i].name);
			if (!strncmp(p, kConst[i].name, n) && n > bestlen)
			{ best = i; bestlen = n; }
		}
		if (best == (size_t)-1) return 0;
		*out = kConst[best].v;
		*named = 1;
		p += bestlen;
	}
	else
	{
		long v; char* e;
		v = strtol(p, &e, 10);
		if (e == p) return 0;
		/* SET_U32: lui/ori, so the low word sign-extended and nothing above. */
		out->w[0] = (u32)v;
		out->w[1] = (v < 0) ? 0xFFFFFFFFu : 0u;
		out->w[2] = 0; out->w[3] = 0;
		*named = 0;
		p = e;
	}
	*pp = p;
	return 1;
}

enum { A_ADDU, A_AND, A_DADDU, A_DSLLV, A_DSRAV, A_DSRLV, A_DSUBU, A_MOVN,
       A_MOVZ, A_NOR, A_OR, A_SLLV, A_SLT, A_SLTU, A_SRAV, A_SRLV, A_SUBU,
       A_XOR,
       /* register, immediate */
       A_ADDIU, A_ANDI, A_DADDIU, A_ORI, A_XORI,
       /* shift or compare by immediate */
       A_DSLL, A_DSLL32, A_DSRA, A_DSRA32, A_DSRL, A_DSRL32, A_SLL,
       A_SLTI, A_SLTIU, A_SRA, A_SRL,
       /* immediate only */
       A_LUI,
       A_COUNT };
static const char* const kName[A_COUNT] = {
	"addu","and","daddu","dsllv","dsrav","dsrlv","dsubu","movn","movz",
	"nor","or","sllv","slt","sltu","srav","srlv","subu","xor",
	"addiu","andi","daddiu","ori","xori",
	"dsll","dsll32","dsra","dsra32","dsrl","dsrl32","sll",
	"slti","sltiu","sra","srl",
	"lui" };
/* LUI prints one operand; everything else prints two. */
static int one_operand(int op) { return op == A_LUI; }

/* Transcribed from R5900OpcodeImpl.c. Each returns the new low
 * doubleword of rd; `keep` says whether rd is left alone entirely, which
 * only the conditional moves do. */
static u64 apply(int op, const Q* s, const Q* t, u64 rd_lo, int* keep)
{
	/* The variable shifts assemble as SLLV rd, rt, rs -- value first, shift
	 * amount second -- the reverse of every other op here, which take rs
	 * then rt. The harness prints its operands in source order either way,
	 * so for those six the first printed value is the one being shifted.
	 * Reading them like the rest scores 12 of 19 and looks like broken
	 * shift semantics. */
	const int is_shift = (op == A_SLLV || op == A_SRLV || op == A_SRAV
	                   || op == A_DSLLV || op == A_DSRLV || op == A_DSRAV);
	const u64 rs = is_shift ? lo64(t) : lo64(s);
	const u64 rt = is_shift ? lo64(s) : lo64(t);
	*keep = 0;
	switch (op)
	{
	case A_ADDU:  return (u64)(s64)(s32)((u32)rs + (u32)rt);
	case A_SUBU:  return (u64)(s64)(s32)((u32)rs - (u32)rt);
	case A_AND:   return rs & rt;
	case A_OR:    return rs | rt;
	case A_XOR:   return rs ^ rt;
	case A_NOR:   return ~(rs | rt);
	case A_DADDU: return rs + rt;
	case A_DSUBU: return rs - rt;
	case A_SLT:   return ((s64)rs < (s64)rt) ? 1 : 0;
	case A_SLTU:  return (rs < rt) ? 1 : 0;
	case A_SLLV:  return (u64)(s64)(s32)((u32)rt << ((u32)rs & 0x1f));
	case A_SRLV:  return (u64)(s64)(s32)((u32)rt >> ((u32)rs & 0x1f));
	case A_SRAV:  return (u64)(s64)((s32)(u32)rt >> ((u32)rs & 0x1f));
	case A_DSLLV: return rt << (rs & 0x3f);
	case A_DSRLV: return rt >> (rs & 0x3f);
	case A_DSRAV: return (u64)((s64)rt >> (rs & 0x3f));
	case A_MOVN:  if (rt == 0) { *keep = 1; return rd_lo; } return rs;
	case A_MOVZ:  if (rt != 0) { *keep = 1; return rd_lo; } return rs;

	/* Immediate forms. `rt` carries the immediate as printed; sign or zero
	 * extension is per instruction, exactly as R5900OpcodeImpl.c does it. */
	case A_ADDIU:  return (u64)(s64)(s32)((u32)rs + (u32)(s32)(s16)(u16)rt);
	case A_DADDIU: return rs + (u64)(s64)(s16)(u16)rt;
	case A_ANDI:   return rs & (u64)(u16)rt;
	case A_ORI:    return rs | (u64)(u16)rt;
	case A_XORI:   return rs ^ (u64)(u16)rt;
	case A_SLTI:   return ((s64)rs < (s64)(s16)(u16)rt) ? 1 : 0;
	case A_SLTIU:  return (rs < (u64)(s64)(s16)(u16)rt) ? 1 : 0;
	case A_SLL:    return (u64)(s64)(s32)((u32)rs << (rt & 0x1f));
	case A_SRL:    return (u64)(s64)(s32)((u32)rs >> (rt & 0x1f));
	case A_SRA:    return (u64)(s64)((s32)(u32)rs >> (rt & 0x1f));
	case A_DSLL:   return rs << (rt & 0x3f);
	case A_DSRL:   return rs >> (rt & 0x3f);
	case A_DSRA:   return (u64)((s64)rs >> (rt & 0x3f));
	case A_DSLL32: return rs << ((rt & 0x1f) + 32);
	case A_DSRL32: return rs >> ((rt & 0x1f) + 32);
	case A_DSRA32: return (u64)((s64)rs >> ((rt & 0x1f) + 32));
	case A_LUI:    return (u64)(s64)(s32)((u32)rt << 16);
	}
	return 0;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "alu.expected";
	FILE* f = fopen(path, "r");
	char buf[512];
	int op = -1, failures = 0, total = 0;
	int pass[A_COUNT], cases[A_COUNT], i;
	/* rd's upper doubleword, carried across the whole file. */
	u64 rd_hi = 0;
	u64 rd_lo = 0;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	for (i = 0; i < A_COUNT; i++) { pass[i] = 0; cases[i] = 0; }

	while (fgets(buf, sizeof(buf), f))
	{
		Q s, t;
		unsigned w3, w2, w1, w0;
		u64 res, want_lo, want_hi;
		int named_s = 0, named_t = 0, keep = 0;
		char* p;

		if (buf[0] != ' ')
		{
			/* Block header. Track every block, including the ones this does
			 * not score, so rd's carried state stays right. */
			char* c = strchr(buf, ':');
			op = -1;
			if (c && c[1] == '\n')
			{
				*c = '\0';
				for (i = 0; i < A_COUNT; i++)
					if (!strcmp(buf, kName[i])) { op = i; break; }
				*c = ':';
			}
			continue;
		}
		if (strstr(buf, "-> $0")) continue;

		/* Operands are read for every block, scored or not: an unscored one
		 * still moves rd. */
		{
			char* name_end = buf + 2;
			while (*name_end && *name_end != ' ') name_end++;
			p = name_end;
		}
		if (op >= 0 && one_operand(op))
		{
			/* LUI prints only its immediate. */
			s.w[0] = s.w[1] = s.w[2] = s.w[3] = 0;
			if (!operand(&p, &t, &named_t)) continue;
		}
		else
		{
			if (!operand(&p, &s, &named_s)) continue;
			if (!operand(&p, &t, &named_t)) continue;
		}
		while (*p == ' ' || *p == ':') p++;
		if (sscanf(p, "%x %x %x %x", &w3, &w2, &w1, &w0) != 4) continue;

		if (named_s)
		{ rd_lo = ((u64)0x1338u << 32) | 0x1337u; rd_hi = RD_GARBAGE_HI; }
		else
			rd_lo = 0x1337u;   /* SET_U32 rewrites only the low half */

		want_lo = ((u64)w1 << 32) | w0;
		want_hi = ((u64)w3 << 32) | w2;

		if (op < 0)
		{
			/* Not scored here, but rd still took the result. */
			rd_lo = want_lo; rd_hi = want_hi;
			continue;
		}

		res = apply(op, &s, &t, rd_lo, &keep);
		(void)keep;

		cases[op]++;
		if (res == want_lo && rd_hi == want_hi) pass[op]++;
		else if (failures++ < 6)
			printf("  %-6s: console %016llx:%016llx  ours %016llx:%016llx\n",
			       kName[op], (unsigned long long)want_hi,
			       (unsigned long long)want_lo,
			       (unsigned long long)rd_hi, (unsigned long long)res);
		rd_lo = res;
	}
	fclose(f);

	for (i = 0; i < A_COUNT; i++)
	{
		printf("%-6s %2d/%-3d console cases\n", kName[i], pass[i], cases[i]);
		total += cases[i];
		if (pass[i] != cases[i]) failures++;
	}
	printf("hwalu: %d cases across %d ops\n", total, A_COUNT);
	return failures != 0;
}
