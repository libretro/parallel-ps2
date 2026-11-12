/*  IOP opcode table audit.
 *
 *  Every other test in tests/iop calls the op implementations directly,
 *  which leaves one thing unchecked: whether the dispatch tables in
 *  pcsx2/R3000AOpcodeTables.cpp actually point each encoding at the right
 *  implementation. A swapped pair there passes every one of those tests
 *  and breaks every game.
 *
 *  This reads the tables out of the source and compares them against the
 *  MIPS I encoding, which is fixed and published, so there is a right
 *  answer to compare to rather than a plausible-looking one.
 *
 *  Entries the tree deliberately leaves unimplemented are checked too, as
 *  psxNULL: an encoding that should be NULL but is not, or the reverse,
 *  is exactly as wrong as a mismatched pair.
 *
 *  The recompiler has its own copies of the same three tables in
 *  x86/iR3000Atables.c, named with an r prefix and pointing at the
 *  emitters, and they are checked against the same reference. A
 *  recompiler that dispatches an encoding to the wrong emitter is the
 *  same catastrophe as the interpreter doing it, and equally invisible to
 *  a test that calls the implementations directly. The prefix is the only
 *  difference, so one reference serves both.
 *
 *  Usage: tests/iop/tabaudit <R3000AOpcodeTables.c> [iR3000Atables.c]
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* MIPS I primary opcodes, 0..63. NULL here means "not a MIPS I encoding
 * the IOP implements"; the R3000A has no COP1 and no 64-bit ops. */
static const char* const kBSC[64] = {
	"psxSPECIAL","psxREGIMM","psxJ","psxJAL","psxBEQ","psxBNE","psxBLEZ","psxBGTZ",
	"psxADDI","psxADDIU","psxSLTI","psxSLTIU","psxANDI","psxORI","psxXORI","psxLUI",
	"psxCOP0","psxNULL","psxCOP2","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxLB","psxLH","psxLWL","psxLW","psxLBU","psxLHU","psxLWR","psxNULL",
	"psxSB","psxSH","psxSWL","psxSW","psxNULL","psxNULL","psxSWR","psxNULL",
	"psxNULL","psxNULL","gteLWC2","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxNULL","psxNULL","gteSWC2","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL"
};

/* SPECIAL function field, 0..63. */
static const char* const kSPC[64] = {
	"psxSLL","psxNULL","psxSRL","psxSRA","psxSLLV","psxNULL","psxSRLV","psxSRAV",
	"psxJR","psxJALR","psxNULL","psxNULL","psxSYSCALL","psxBREAK","psxNULL","psxNULL",
	"psxMFHI","psxMTHI","psxMFLO","psxMTLO","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxMULT","psxMULTU","psxDIV","psxDIVU","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxADD","psxADDU","psxSUB","psxSUBU","psxAND","psxOR","psxXOR","psxNOR",
	"psxNULL","psxNULL","psxSLT","psxSLTU","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL"
};

/* REGIMM rt field, 0..31: the conditional branches on rs. */
static const char* const kREG[32] = {
	"psxBLTZ","psxBGEZ","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxBLTZAL","psxBGEZAL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL",
	"psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL","psxNULL"
};

/* Pull one table's initialiser out of the file and split it on commas. */
/* Find the table's definition, not one of its extern declarations: the
 * recompiler forward-declares all of these at the top of the file, and
 * an extern has no initialiser, so anchoring on the first match reads a
 * single token and reports a bogus size mismatch. */
static const char* find_def(const char* src, const char* decl)
{
	const char* p = src;
	while ((p = strstr(p, decl)) != NULL)
	{
		const char* q = p + strlen(decl);
		/* the parameter list: () in C++, (void) in C */
		for (;;)
		{
			while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '(' || *q == ')') q++;
			if (!strncmp(q, "void", 4)) { q += 4; continue; }
			break;
		}
		if (*q == '=') return p;
		p += strlen(decl);
	}
	return NULL;
}

static int read_table(const char* src, const char* decl, char out[][32], int n)
{
	const char* p = find_def(src, decl);
	const char* end;
	int count = 0;
	if (!p) return -1;
	p = strchr(p, '{');
	if (!p) return -1;
	end = strchr(p, '}');
	if (!end) return -1;
	p++;
	while (p < end && count < n)
	{
		const char* q;
		int len = 0;
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')) p++;
		if (p >= end) break;
		if (p[0] == '/' && p[1] == '/')            /* line comment */
		{ while (p < end && *p != '\n') p++; continue; }
		if (p[0] == '/' && p[1] == '*')            /* block comment */
		{ const char* c = strstr(p + 2, "*/"); p = c ? c + 2 : end; continue; }
		q = p;
		while (q < end && *q != ',' && *q != ' ' && *q != '\t' && *q != '\n') q++;
		len = (int)(q - p);
		if (len > 31) len = 31;
		memcpy(out[count], p, len);
		out[count][len] = '\0';
		count++;
		p = q;
	}
	return count;
}

/* Compare an entry against the reference, allowing the recompiler's
 * prefixes: rpsx for psx, rgte for gte. */
static int same_entry(const char* got, const char* want, const char* prefix)
{
	char expect[40];
	if (!prefix) return !strcmp(got, want);
	if (!strncmp(want, "psx", 3))
		snprintf(expect, sizeof(expect), "rpsx%s", want + 3);
	else if (!strncmp(want, "gte", 3))
		snprintf(expect, sizeof(expect), "rgte%s", want + 3);
	else
		snprintf(expect, sizeof(expect), "%s", want);
	return !strcmp(got, expect);
}

static int check_p(const char* what, char got[][32], int got_n,
                   const char* const* want, int want_n, const char* prefix)
{
	int i, bad = 0;
	if (got_n != want_n)
	{ printf("  %s: read %d entries, expected %d\n", what, got_n, want_n); return 1; }
	for (i = 0; i < want_n; i++)
		if (!same_entry(got[i], want[i], prefix))
		{
			/* Report the name this table should hold, prefix and all. */
			char expect[40];
			if (prefix && !strncmp(want[i], "psx", 3))
				snprintf(expect, sizeof(expect), "rpsx%s", want[i] + 3);
			else if (prefix && !strncmp(want[i], "gte", 3))
				snprintf(expect, sizeof(expect), "rgte%s", want[i] + 3);
			else
				snprintf(expect, sizeof(expect), "%s", want[i]);
			printf("  %s[%d]: table has %s, the encoding says %s\n",
			       what, i, got[i], expect);
			bad++;
		}
	printf("%-12s %3d/%-3d entries match the MIPS I encoding\n",
	       what, want_n - bad, want_n);
	return bad;
}

static int check(const char* what, char got[][32], int got_n,
                 const char* const* want, int want_n)
{
	int i, bad = 0;
	if (got_n != want_n)
	{
		printf("  %s: read %d entries, expected %d\n", what, got_n, want_n);
		return 1;
	}
	for (i = 0; i < want_n; i++)
		if (strcmp(got[i], want[i]))
		{
			printf("  %s[%d]: table has %s, MIPS I says %s\n",
			       what, i, got[i], want[i]);
			bad++;
		}
	printf("%-10s %d/%d entries match the MIPS I encoding\n",
	       what, want_n - bad, want_n);
	return bad;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "R3000AOpcodeTables.c";
	FILE* f = fopen(path, "rb");
	char* src;
	long len;
	static char bsc[64][32], spc[64][32], reg[32][32];
	int bad = 0, n;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
	src = (char*)malloc((size_t)len + 1);
	if (fread(src, 1, (size_t)len, f) != (size_t)len) { fclose(f); return 2; }
	src[len] = '\0';
	fclose(f);

	n = read_table(src, "psxBSC[64])", bsc, 64);
	bad += (n < 0) ? (printf("  could not read psxBSC\n"), 1)
	               : check("psxBSC", bsc, n, kBSC, 64);
	n = read_table(src, "psxSPC[64])", spc, 64);
	bad += (n < 0) ? (printf("  could not read psxSPC\n"), 1)
	               : check("psxSPC", spc, n, kSPC, 64);
	n = read_table(src, "psxREG[32])", reg, 32);
	bad += (n < 0) ? (printf("  could not read psxREG\n"), 1)
	               : check("psxREG", reg, n, kREG, 32);
	free(src);

	/* The recompiler's copies, if a path was given. */
	if (argc > 2)
	{
		f = fopen(argv[2], "rb");
		if (!f) { fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
		fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
		src = (char*)malloc((size_t)len + 1);
		if (!src || fread(src, 1, (size_t)len, f) != (size_t)len) { fclose(f); return 2; }
		src[len] = '\0';
		fclose(f);

		n = read_table(src, "rpsxBSC[64])", bsc, 64);
		bad += (n < 0) ? (printf("  could not read rpsxBSC\n"), 1)
		               : check_p("rpsxBSC", bsc, n, kBSC, 64, "r");
		n = read_table(src, "rpsxSPC[64])", spc, 64);
		bad += (n < 0) ? (printf("  could not read rpsxSPC\n"), 1)
		               : check_p("rpsxSPC", spc, n, kSPC, 64, "r");
		n = read_table(src, "rpsxREG[32])", reg, 32);
		bad += (n < 0) ? (printf("  could not read rpsxREG\n"), 1)
		               : check_p("rpsxREG", reg, n, kREG, 32, "r");
		free(src);
	}

	printf("iop tabaudit: %s\n", bad ? "MISMATCHES FOUND" : "dispatch matches the encoding");
	return bad != 0;
}
