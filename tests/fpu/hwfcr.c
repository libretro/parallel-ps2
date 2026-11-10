/*  EE FPU control register reads against console captures.
 *
 *  Scores CFC1 across all thirty-two control registers against the
 *  "Initial:" line of ps2autotests tests/cpu/ee_fpu/fcr.expected, which
 *  prints every one of them after reset.
 *
 *  The console aliases: FCR0 through FCR15 all read as FCR0, and FCR16
 *  through FCR31 all read as FCR31. Only those two exist.
 *
 *  Transcribed from CFC1 in pcsx2/FPU.c. The reset values come from
 *  R5900.cpp, which sets fprc[0] to 0x00002e30 and fprc[31] to
 *  0x01000001 -- both matching the console.
 *
 *  Usage: tests/fpu/hwfcr <path-to-fcr.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t u32;
typedef int32_t  s32;

/* Reset values, from cpuReset in pcsx2/R5900.cpp. */
#define FPRC0  0x00002e30u
#define FPRC31 0x01000001u

/* Transcribed from CFC1 in pcsx2/FPU.c. */
static u32 cfc1(int fs, u32 fprc31)
{
	/* FCR31 reads drop the always-zero bits and set the always-one ones. */
	return (fs & 0x10) ? ((fprc31 & 0x0083c078u) | 0x01000001u) : FPRC0;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "fcr.expected";
	FILE* f = fopen(path, "r");
	char buf[4096];
	int seen_initial = 0, pass = 0, total = 0, shown = 0;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }

	while (fgets(buf, sizeof(buf), f))
	{
		char* p;
		if (!seen_initial)
		{ if (!strncmp(buf, "Initial:", 8)) seen_initial = 1; continue; }
		if (buf[0] != ' ') break;

		/* "  fcr0: 00002e30, fcr1: 00002e30, ..." */
		p = buf;
		while ((p = strstr(p, "fcr")) != NULL)
		{
			int fs; unsigned want; u32 got;
			if (sscanf(p, "fcr%d: %x", &fs, &want) != 2) { p += 3; continue; }
			got = cfc1(fs, FPRC31);
			total++;
			if (got == (u32)want) pass++;
			else if (shown++ < 8)
				printf("  fcr%-2d: console %08x  ours %08x\n", fs, want, got);
			p += 3;
		}
		break;
	}
	fclose(f);

	/* The capture also writes all-ones to FCR31 and reads it back; that
	 * value is the writable mask and is checked here directly. */
	{
		const u32 want_ctc1 = 0x0183c079u;
		const u32 got_ctc1 = cfc1(31, 0xffffffffu);
		total++;
		if (got_ctc1 == want_ctc1) pass++;
		else printf("  ctc1 0xffffffff -> cfc1: console %08x  ours %08x\n",
		            want_ctc1, got_ctc1);
	}

	printf("hwfcr: %d/%d control register reads match the console\n", pass, total);
	return pass != total;
}
