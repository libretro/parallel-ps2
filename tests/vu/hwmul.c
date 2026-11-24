/* VMUL and VMULx against a physical PS2 (silver slim SCPH-90000 series),
 * scored through pcsx2/ps2float.c's ps2f_mul, which the interpreter's
 * VMUL family calls.
 *
 * Seven ordered Fs/Ft pairs, each loaded into every lane of vf1 and vf2,
 * run as VMUL.xyzw vf3,vf1,vf2 (4BE208EA) and VMULx.xyzw vf3,vf1,vf2x
 * (4BE208D8), each after 32 and after 64 VNOPs: 28 records, record order
 * VMUL32[0..6], VMULx32[0..6], VMUL64[0..6], VMULx64[0..6]. Case 2 tells
 * the multiplier's discarded-carry correction apart (3F800001 * 3F808000
 * is 3F808001, not 3F808000), case 1 is 1.0 * 129.5 coming out one ulp
 * short, cases 3 and 4 are further discarded-carry pairs and 5 and 6 the
 * operand swaps of 2 and 1.
 *
 * Provenance, kept apart: the x lane of records 0-13 is read off the
 * console's screen in a photograph of the run (PHOTO below). The y, z and
 * w lanes and records 14-27 (RECONCILED) follow from the test program's
 * own zero lane-mismatch and zero 32/64-gap-mismatch checks, which it ran
 * on the console and reported as 0 errors; they are not a RAM dump. No
 * value comes from an emulator or a software model. The program's
 * checksum over all 112 result words, shown in the photograph, is checked
 * here against the table, so a mistyped word fails before any multiply.
 *
 *   payload VUMULTEST.ELF  sha256 E705D41805F64FA78A459D2DD345FCA0D44C87963E81080FD31511B1D1BA53E1
 *   photograph             sha256 F62F86E3382C861BE07B24476A267F9A45C2F40C143389EE1E4EBDFE8DEB5FE2
 *   checksum 74A99055: h = 2166136261; h = (h ^ word) * 16777619 for
 *   each result word, x y z w, in record order.
 */

#include <stdio.h>
#include "ps2float.h"

#define PHOTO      1 /* x lane read off the photographed screen */
#define RECONCILED 0 /* from the program's own on-console cross-checks */

static const unsigned kPairs[7][3] = {
	/* Fs          Ft           result */
	{ 0x3F800000u, 0x40000000u, 0x40000000u }, /* 0 control */
	{ 0x3F800000u, 0x43018000u, 0x43017FFFu }, /* 1 1.0 * 129.5 */
	{ 0x3F800001u, 0x3F808000u, 0x3F808001u }, /* 2 primary discriminator */
	{ 0x3F800040u, 0x3F800020u, 0x3F800060u }, /* 3 discarded carry */
	{ 0x3F800100u, 0x3F800008u, 0x3F800108u }, /* 4 discarded carry */
	{ 0x3F808000u, 0x3F800001u, 0x3F808001u }, /* 5 case 2 swapped */
	{ 0x43018000u, 0x3F800000u, 0x43018000u }  /* 6 case 1 swapped */
};

int main(void)
{
	unsigned h = 2166136261u;
	int rec, lane, pass = 0, photo = 0, bad = 0;

	/* The checksum the console printed, over the table as written. */
	for (rec = 0; rec < 28; rec++)
		for (lane = 0; lane < 4; lane++)
			h = (h ^ kPairs[rec % 7][2]) * 16777619u;
	if (h != 0x74A99055u)
	{
		printf("hwmul: table checksum %08X, the console printed 74A99055\n", h);
		return 1;
	}

	for (rec = 0; rec < 28; rec++)
	{
		const unsigned fs = kPairs[rec % 7][0], ft = kPairs[rec % 7][1];
		const unsigned want = kPairs[rec % 7][2];
		const int vmulx = (rec / 7) & 1;
		for (lane = 0; lane < 4; lane++)
		{
			/* VMULx multiplies every lane by Ft.x; with Ft in every lane
			 * the operands are the same, the instruction is not. */
			const unsigned got = ps2f_raw(ps2f_mul(fs, ft));
			const int source = (rec < 14 && lane == 0) ? PHOTO : RECONCILED;
			if (got == want)
			{
				pass++;
				photo += source == PHOTO;
			}
			else if (bad++ < 8)
				printf("  record %2d %s %s lane %d: %08X * %08X console %08X ours %08X (%s)\n",
				       rec, vmulx ? "VMULx" : "VMUL ", rec < 14 ? "32" : "64", lane,
				       fs, ft, want, got, source == PHOTO ? "photographed" : "reconciled");
		}
	}
	printf("hwmul: %d/112 result words (%d/14 photographed), checksum 74A99055\n", pass, photo);
	return pass != 112;
}
