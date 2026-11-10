/*  FPU branch conditions against console captures.
 *
 *  Scores BC1F, BC1FL, BC1T and BC1TL against ps2autotests
 *  tests/cpu/ee_fpu/branch.expected, using the macros in pcsx2/FPU.c.
 *
 *  These branch on FCR31's C flag rather than on register operands, so
 *  the harness sets it two ways and the capture prints which: either a
 *  literal 0 or 1 written to the flag, or the name of a compare that
 *  leaves it set -- "c.eq" comparing equal values, which sets C, and
 *  "c.f" which is the always-false compare and clears it.
 *
 *  The same three properties as the integer branches are reported, but
 *  only two carry information here: none of these four link, so the ra
 *  field is constant across the file. The delay slot still distinguishes
 *  the likely forms from the others, and that is checked.
 *
 *  Usage: tests/fpu/hwbranch <path-to-ee_fpu-branch.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;

enum { F_BC1F, F_BC1FL, F_BC1T, F_BC1TL, F_COUNT };
static const char* const kName[F_COUNT] = { "bc1f", "bc1fl", "bc1t", "bc1tl" };

static int likely_form(int op) { return op == F_BC1FL || op == F_BC1TL; }

/* From the BC1 and BC1L macros in FPU.c: the F forms branch when the C
 * flag is clear, the T forms when it is set. */
static int taken(int op, int cflag)
{
	if (op == F_BC1F || op == F_BC1FL) return cflag == 0;
	return cflag != 0;
}

/* What the printed operand leaves in the C flag. */
static int operand_cflag(const char* p, int* out)
{
	while (*p == ' ') p++;
	if (!strncmp(p, "c.eq", 4)) { *out = 1; return 1; }  /* equal: sets C */
	if (!strncmp(p, "c.f", 3))  { *out = 0; return 1; }  /* always false */
	if (*p == '0') { *out = 0; return 1; }
	if (*p == '1') { *out = 1; return 1; }
	return 0;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "branch.expected";
	int op, failures = 0, total = 0;

	for (op = 0; op < F_COUNT; op++)
	{
		FILE* f = fopen(path, "r");
		char head[32], buf[512];
		int inblock = 0, pass = 0, cases = 0;

		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kName[op]);

		while (fgets(buf, sizeof(buf), f))
		{
			int cflag, want_taken, want_link, want_slot, got_taken, got_slot;
			char* p;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;

			if (!operand_cflag(buf + 2 + strlen(kName[op]), &cflag)) continue;
			p = strchr(buf, ':');
			if (!p) continue;

			want_taken = strstr(p, "followed") != NULL;
			want_link  = strstr(p, "set ra")   != NULL;
			want_slot  = strstr(p, "ran delay slot") != NULL;
			if (!want_taken && !strstr(p, "skipped,")) continue;

			got_taken = taken(op, cflag);
			got_slot  = got_taken || !likely_form(op);

			cases++;
			/* None of the four link; assert that rather than skip it. */
			if (got_taken == want_taken && !want_link && got_slot == want_slot)
				pass++;
			else if (failures++ < 6)
				printf("  %-6s C=%d: console %s/%s  ours %s/%s\n",
				       kName[op], cflag,
				       want_taken ? "followed" : "skipped",
				       want_slot ? "slot" : "no slot",
				       got_taken ? "followed" : "skipped",
				       got_slot ? "slot" : "no slot");
		}
		fclose(f);
		printf("%-6s %2d/%-3d console cases\n", kName[op], pass, cases);
		total += cases;
		if (pass != cases) failures++;
	}

	printf("fpu hwbranch: %d cases across %d ops\n", total, F_COUNT);
	return failures != 0;
}
