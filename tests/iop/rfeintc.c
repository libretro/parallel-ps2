/*  IOP recompiler RFE audit.
 *
 *  RFE pops the Status interrupt-enable stack, and with IEc set again an
 *  interrupt that was raised while it was clear is deliverable at once.
 *  The recompilers only test events at branches gated on
 *  iopNextEventCycle, so after the shuffle they call iopTestIntc, which
 *  pulls that gate to within two cycles; without the call the interrupt
 *  waits for whatever event was next scheduled, up to iopWaitCycles
 *  later. The x86 recompiler's rpsxRFE does this; the arm64 recompiler
 *  emits RFE inline in EmitSimple and has to make the same call there.
 *
 *  For the x86 rpsxRFE body and the arm64 RFE case (rs field 0x10 of
 *  COP0), this checks that each calls iopTestIntc.
 *
 *  Usage: tests/iop/rfeintc <iR3000Atables.c> <recR3000A_arm64.cpp>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* read_file(const char* path)
{
	FILE* f = fopen(path, "rb");
	long n;
	char* buf;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (char*)malloc((size_t)n + 1);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		free(buf);
		buf = NULL;
	}
	if (buf)
		buf[n] = '\0';
	fclose(f);
	return buf;
}

/* Does [start, end) of the file at path contain needle? start and end are
 * markers searched for in order; 1 yes, 0 no, -1 markers not found. */
static int region_has(const char* path, const char* start, const char* end, const char* needle)
{
	char* src = read_file(path);
	const char* s;
	const char* e;
	const char* n;
	int rc;

	if (!src)
		return -1;
	s = strstr(src, start);
	e = s ? strstr(s + strlen(start), end) : NULL;
	if (!s || !e)
	{
		free(src);
		return -1;
	}
	n = strstr(s, needle);
	rc = (n && n < e) ? 1 : 0;
	free(src);
	return rc;
}

static int report(const char* what, int r)
{
	if (r < 0)
	{
		printf("  %s: not found -- has it moved?\n", what);
		return 1;
	}
	printf("  %s: %s\n", what, r ? "calls iopTestIntc" : "does NOT call iopTestIntc");
	return r ? 0 : 1;
}

int main(int argc, char** argv)
{
	int bad = 0;

	if (argc != 3)
	{
		fprintf(stderr, "usage: %s <iR3000Atables.c> <recR3000A_arm64.cpp>\n", argv[0]);
		return 2;
	}
	bad += report("x86 rpsxRFE", region_has(argv[1], "static void rpsxRFE(void)", "\n}\n", "iopTestIntc"));
	bad += report("arm64 RFE", region_has(argv[2], "if (rs_field == 0x10)", "return true;", "iopTestIntc"));
	if (bad)
	{
		printf("FAIL\n");
		return 1;
	}
	printf("PASS\n");
	return 0;
}
