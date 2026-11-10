/*  VU0 macro-mode control register writes against console captures.
 *
 *  ps2autotests tests/cpu/vu0_macro/transfer.expected writes 0xffffffff
 *  to each VI control register with CTC2 and reads it back with CFC2, so
 *  each line is that register's writable mask. This scores the CTC2 and
 *  CFC2 semantics in pcsx2/VU0.c against them.
 *
 *  Only the masking is modelled. The registers whose writes have side
 *  effects beyond the store -- FBRST resetting a VU, CMSAR1 kicking off a
 *  VU1 microprogram -- are scored on the value they leave behind, which
 *  is what the capture prints; the side effects themselves need the
 *  emulator running and are out of scope here.
 *
 *  Six of the sixteen match, and this asserts that as a floor rather than
 *  demanding all sixteen, because the other ten are a known gap and not
 *  one to close casually. We store the written value whole where the
 *  hardware keeps only some bits of it -- the status flag reads back
 *  0x00000fc0, the clipping flag 0x00ffffff, CMSAR0 and VI01 sixteen bits,
 *  the three reserved registers various fixed values -- and FBRST goes the
 *  other way, reading back zero on console while we deliberately keep
 *  0x0c0c because the VU1 stall logic reads those bits back out.
 *
 *  Note also that a single write of all-ones cannot tell a write mask from
 *  a read mask; it only shows what survives the round trip. Closing any of
 *  these properly means knowing which end does the masking, and for the
 *  registers the emulator itself reads back that distinction decides
 *  whether internal users see the masked value too.
 *
 *  Usage: tests/vu/hwctc2 <path-to-transfer.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t u32;

/* Register numbers, from VUmicro's REG_ constants. */
enum {
	REG_STATUS_FLAG = 16, REG_MAC_FLAG = 17, REG_CLIP_FLAG = 18, REG_R = 20,
	REG_I = 21, REG_Q = 22, REG_TPC = 26, REG_CMSAR0 = 27, REG_FBRST = 28,
	REG_VPU_STAT = 29, REG_CMSAR1 = 31
};

/* Transcribed from CTC2 followed by CFC2 in pcsx2/VU0.c: what a write of
 * `v` leaves readable in VI[fs]. */
static u32 ctc2_then_cfc2(int fs, u32 v)
{
	u32 stored;

	if (fs == 0)
		return 0;                       /* VI00 is hardwired zero */

	switch (fs)
	{
	case REG_MAC_FLAG:
	case REG_TPC:
	case REG_VPU_STAT:
		return 0;                       /* read-only, write ignored */
	case REG_R:
		/* CTC2 stores it in float form; CFC2 masks the mantissa back out. */
		stored = ((v & 0x7FFFFF) | 0x3F800000) & 0x7FFFFF;
		break;
	case REG_FBRST:
		stored = v & 0x0C0C;
		break;
	case REG_CMSAR1:
		/* Kicks VU1 and stores nothing, so the register keeps whatever it
		 * had -- zero after reset. */
		return 0;
	default:
		stored = v;
		break;
	}

	return stored;
}

/* What matches today. Raise it when a mask is added, never lower it. */
static const int kFloor = 6;

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "transfer.expected";
	FILE* f = fopen(path, "r");
	char buf[512];
	int pass = 0, total = 0;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }

	while (fgets(buf, sizeof(buf), f))
	{
		int fs; unsigned want; u32 got;
		char* p = strstr(buf, "ctc2 -> vi");
		if (!p) continue;
		if (sscanf(p, "ctc2 -> vi%d: %x", &fs, &want) != 2) continue;

		got = ctc2_then_cfc2(fs, 0xffffffffu);
		total++;
		if (got == (u32)want) pass++;
		else printf("  vi%02d: console %08x  ours %08x\n", fs, want, got);
	}
	fclose(f);

	printf("hwctc2: %d/%d control register round trips match the console"
	       " (floor %d)\n", pass, total, kFloor);
	if (pass < kFloor)
	{
		printf("  regression: this used to match %d\n", kFloor);
		return 1;
	}
	return 0;
}
