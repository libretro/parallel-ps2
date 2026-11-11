/*  VU FDIV latencies against console captures.
 *
 *  Scores the Q-pipeline latency our interpreter uses for DIV, SQRT and
 *  RSQRT (the `cycles` fields in _vuRegsDIV, _vuRegsSQRT and _vuRegsRSQRT
 *  in pcsx2/VUops.c) against ps2autotests
 *  tests/vu/lower/fdivdelay.expected.
 *
 *  The capture's "single delayed" sections issue the divide and then a run
 *  of MULq writing VF01, VF02, ... in turn, so the index of the first VF
 *  holding the new Q value *is* the latency, read directly off the
 *  hardware. The file is parsed for that index rather than the numbers
 *  being transcribed, so the expectation cannot drift from the capture.
 *
 *  This deliberately does not model the pipeline. The rest of
 *  fdivdelay.expected covers stalls, branches and back-to-back divides,
 *  which need a real model of the FDIV stage and its interaction with
 *  WAITQ; scoring those wants a pipeline simulator rather than a constant
 *  check. What this pins is the single number each of those behaviours is
 *  built on, and which is easy to change while "improving" timing.
 *
 *  Usage: tests/vu/hwfdiv <path-to-fdivdelay.expected>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The latencies pcsx2/VUops.c assigns. Keep in step with _vuRegsDIV,
 * _vuRegsSQRT and _vuRegsRSQRT. */
static const struct { const char* name; int cycles; } kOps[] = {
	{ "DIV",   7  },
	{ "SQRT",  7  },
	{ "RSQRT", 13 },
};

/* Pull the first VF index whose value differs from the first one printed,
 * within the "<OP> single delayed:" section. */
static int observed(const char* path, const char* op)
{
	char head[64], buf[512], base[16];
	FILE* f = fopen(path, "r");
	int inblock = 0, have_base = 0;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return -2; }
	snprintf(head, sizeof(head), "%s single delayed:", op);

	while (fgets(buf, sizeof(buf), f))
	{
		char* p;
		int idx;
		char word[16];

		if (!inblock)
		{ if (!strncmp(buf, head, strlen(head))) inblock = 1; continue; }
		if (strncmp(buf, "  VF", 4) != 0) break;

		if (sscanf(buf, "  VF%d:", &idx) != 1) break;
		/* Each line prints X Y Z W and only W carries the Q result, so it is
		 * the last field that matters -- reading the first sees 00000000 on
		 * every line and finds no change at all. */
		p = strrchr(buf, ' ');
		if (!p || sscanf(p + 1, "%15[0-9a-f]", word) != 1) break;

		if (!have_base) { strcpy(base, word); have_base = 1; continue; }
		if (strcmp(word, base) != 0) { fclose(f); return idx; }
	}
	fclose(f);
	return -1;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "fdivdelay.expected";
	int i, bad = 0;

	for (i = 0; i < (int)(sizeof(kOps)/sizeof(kOps[0])); i++)
	{
		const int got = observed(path, kOps[i].name);
		if (got == -2) return 2;
		if (got != kOps[i].cycles)
		{
			bad++;
			printf("  %-6s console shows the new Q at VF%02d, we use %d cycles\n",
			       kOps[i].name, got, kOps[i].cycles);
		}
		else
			printf("  %-6s %2d cycles, matches console\n", kOps[i].name, got);
	}

	printf("vufdiv: %d/%d latencies match console\n",
	       (int)(sizeof(kOps)/sizeof(kOps[0])) - bad,
	       (int)(sizeof(kOps)/sizeof(kOps[0])));
	return bad != 0;
}
