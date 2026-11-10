/*  EE branch conditions against console captures.
 *
 *  Scores the twenty branches and jumps in ps2autotests
 *  tests/cpu/ee/branch.expected against pcsx2/Interpreter.c.
 *
 *  Each line reports three things -- followed or skipped, ra set or
 *  ignored, delay slot run or skipped -- and on the EE all three vary,
 *  which is what makes this worth scoring without modelling execution.
 *  Taken is a function of the operands. Linking is a property of the
 *  opcode, and the AL forms link unconditionally: _SetLink(31) runs
 *  before the condition is tested, so ra is written even when the branch
 *  is not taken. The delay slot is where the EE differs from the IOP --
 *  the likely forms skip it when not taken, which is the whole of what
 *  the L suffix means, so it is a function of taken and the form rather
 *  than of the surrounding code.
 *
 *  Comparisons are 64-bit here, unlike the IOP's 32. The operands the
 *  harness prints as small decimals do not distinguish the two, but the
 *  named constants do: C_S32_MIN and C_S64_MIN are different values in
 *  64 bits and compare differently against zero.
 *
 *  Usage: tests/ee/hwbranch <path-to-ee-branch.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  s64;

/* From tests/cpu/ee/shared.h, as full 64-bit values: these ops read SD[0]. */
static const struct { const char* name; u64 v; } kConst[] = {
	{ "C_ZERO",     0x0000000000000000ull },
	{ "C_S16_MAX",  0x0000000000007FFFull },
	{ "C_S16_MIN",  0xFFFFFFFFFFFF8000ull },
	{ "C_S32_MAX",  0x000000007FFFFFFFull },
	{ "C_S32_MIN",  0xFFFFFFFF80000000ull },
	{ "C_S64_MAX",  0x7FFFFFFFFFFFFFFFull },
	{ "C_S64_MIN",  0x8000000000000000ull },
	{ "C_NEGONE",   0xFFFFFFFFFFFFFFFFull },
	{ "C_ONE",      0x0000000000000001ull },
	{ "C_GARBAGE1", 0x0000133800001337ull },
	{ "C_GARBAGE2", 0xDEADBEEEDEADBEEFull },
};

static int operand(char** pp, u64* out)
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
		p += bestlen;
	}
	else
	{
		long long v; char* e;
		v = strtoll(p, &e, 10);
		if (e == p) return 0;
		*out = (u64)v;
		p = e;
	}
	*pp = p;
	return 1;
}

enum { E_BEQ, E_BEQL, E_BGEZ, E_BGEZAL, E_BGEZALL, E_BGEZL, E_BGTZ, E_BGTZL,
       E_BLEZ, E_BLEZL, E_BLTZ, E_BLTZAL, E_BLTZALL, E_BLTZL, E_BNE, E_BNEL,
       E_J, E_JAL, E_JALR, E_JR, E_COUNT };
static const char* const kName[E_COUNT] = {
	"beq","beql","bgez","bgezal","bgezall","bgezl","bgtz","bgtzl",
	"blez","blezl","bltz","bltzal","bltzall","bltzl","bne","bnel",
	"j","jal","jalr","jr" };

static int operands(int op)
{
	if (op == E_BEQ || op == E_BEQL || op == E_BNE || op == E_BNEL) return 2;
	if (op >= E_J) return 0;
	return 1;
}
static int links(int op)
{
	return op == E_BGEZAL || op == E_BGEZALL || op == E_BLTZAL
	    || op == E_BLTZALL || op == E_JAL || op == E_JALR;
}
/* The likely forms: they skip the delay slot when not taken. */
static int likely_form(int op)
{
	return op == E_BEQL || op == E_BNEL || op == E_BGEZL || op == E_BGTZL
	    || op == E_BLEZL || op == E_BLTZL || op == E_BGEZALL || op == E_BLTZALL;
}

/* Transcribed from pcsx2/Interpreter.c; comparisons are on SD[0]. */
static int taken(int op, u64 rs, u64 rt)
{
	switch (op)
	{
	case E_BEQ:  case E_BEQL:  return rs == rt;
	case E_BNE:  case E_BNEL:  return rs != rt;
	case E_BGEZ: case E_BGEZL:
	case E_BGEZAL: case E_BGEZALL: return (s64)rs >= 0;
	case E_BGTZ: case E_BGTZL: return (s64)rs >  0;
	case E_BLEZ: case E_BLEZL: return (s64)rs <= 0;
	case E_BLTZ: case E_BLTZL:
	case E_BLTZAL: case E_BLTZALL: return (s64)rs <  0;
	default:                   return 1;
	}
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "branch.expected";
	int op, failures = 0, total = 0;

	for (op = 0; op < E_COUNT; op++)
	{
		FILE* f = fopen(path, "r");
		char head[32], buf[512];
		int inblock = 0, pass = 0, cases = 0;

		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kName[op]);

		while (fgets(buf, sizeof(buf), f))
		{
			u64 rs = 0, rt = 0;
			int want_taken, want_link, want_slot, got_taken, got_slot;
			char* p;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;

			p = buf + 2 + strlen(kName[op]);
			if (operands(op) >= 1 && !operand(&p, &rs)) continue;
			if (operands(op) == 2 && !operand(&p, &rt)) continue;
			p = strchr(buf, ':');
			if (!p) continue;

			want_taken = strstr(p, "followed") != NULL;
			want_link  = strstr(p, "set ra")   != NULL;
			want_slot  = strstr(p, "ran delay slot") != NULL;
			if (!want_taken && !strstr(p, "skipped,")) continue;

			got_taken = taken(op, rs, rt);
			/* Only a likely form that is not taken skips its slot. */
			got_slot  = got_taken || !likely_form(op);

			cases++;
			if (got_taken == want_taken && links(op) == want_link
			 && got_slot == want_slot)
				pass++;
			else if (failures++ < 8)
				printf("  %-8s %016llx, %016llx: console %s/%s/%s  ours %s/%s/%s\n",
				       kName[op], (unsigned long long)rs, (unsigned long long)rt,
				       want_taken ? "followed" : "skipped",
				       want_link ? "set ra" : "ignored",
				       want_slot ? "slot" : "no slot",
				       got_taken ? "followed" : "skipped",
				       links(op) ? "set ra" : "ignored",
				       got_slot ? "slot" : "no slot");
		}
		fclose(f);
		printf("%-8s %2d/%-3d console cases\n", kName[op], pass, cases);
		total += cases;
		if (pass != cases) failures++;
	}

	printf("ee hwbranch: %d cases across %d ops\n", total, E_COUNT);
	return failures != 0;
}
