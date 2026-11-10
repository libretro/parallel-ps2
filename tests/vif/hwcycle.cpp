/*  VIF write and skip cycles against console captures.
 *
 *  Scores pcsx2/Vif_Unpack.c's unpack loop against ps2autotests
 *  tests/dma/vif/stcycl.expected. Each line sets CYCLE to one (cl, wl)
 *  pair, a mask of 0xAAAAAAAA and a column of (1,2,3,4), then unpacks
 *  eight V4-32 vectors and prints word 0 of each of the first 64
 *  quadwords as a single hex digit.
 *
 *  Two things make it worth having separately from the unpack and stmod
 *  scorers. It is the loop rather than the per-vector write: cl and wl
 *  decide which vectors are written, which are skipped and which repeat,
 *  and none of that is visible in a single unpack. And the mask is all
 *  twos, so every write takes the MaskCol arm -- the one the stmod
 *  scenarios never reach, as recorded in hwunpack.cpp.
 *
 *  The loop is reached through _nVifUnpack, the entry the recompilers
 *  fall back to, so it is the unit's own loop that is scored rather than
 *  a transcription of it.
 *
 *  Usage: tests/vif/hwcycle <path-to-vif/stcycl.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "Common.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "VU.h"
#include "Vif_Unpack.h"

alignas(16) u8 eeHw[0x10000];
alignas(16) VURegs vuRegs[2];
alignas(16) vifStruct vif0, vif1;
extern "C" {
void dVifReset(int) { }
void vifExecQueue(int) { }
void dvif_unpack(int, const u8*, int) { }
vifStruct* mtvu_vif(void) { return &vif1; }
VIFregisters* mtvu_vif_regs(void) { return &vif1Regs; }
void mtvu_vif_unpack(const u8*, u32) { }
}

/* The loop calls nVifUpk, the recompiled per-cycle kernels. On arm64 it
 * falls back to the interpreted VIFfuncTable entry when those are null;
 * on x86 it does not, so this harness fills the table with wrappers
 * around the same interpreted function the fallback would use. The loop
 * logic under test -- which vectors are written, skipped or repeated for
 * a given cl and wl -- is common to both paths, and the per-vector write
 * is already scored by hwunpack.cpp against the same captures.
 *
 * The four slots for a format all get the same wrapper: writeXYZW selects
 * the mask row from vif.cl itself, so the per-cycle kernels differ only
 * in what the recompiler folded in. */
template <int idx>
static u32 interp_unpack(void* dest, const void* src)
{
	vifStruct& vif = idx ? vif1 : vif0;
	VIFregisters& regs = idx ? vif1Regs : vif0Regs;
	const int usn = !!vif.usn;
	const int upkNum = vif.cmd & 0x1f;
	/* VIFfuncTable's own layout: the mask at +32, usn at +16. */
	const int ftIdx = ((upkNum & 0x10) ? 32 : 0) + (usn ? 16 : 0) + (upkNum & 0x0f);
	VIFfuncTable[0][ftIdx](dest, src, &vif, &regs);
	return 0;
}

static void fill_upk_table(int unit, int usn, int upkNum)
{
	const int base = ((usn * 2 * 16) + upkNum) * 4;
	int i;
	for (i = 0; i < 4; i++)
		nVifUpk[base + i] = unit ? interp_unpack<1> : interp_unpack<0>;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "stcycl.expected";
	FILE* f = fopen(path, "r");
	char buf[1024];
	int pass = 0, cases = 0, shown = 0, unit = 0;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }

	/* VURegs::Mem is a pointer the emulator allocates at startup; the
	 * unpack writes straight into it, so the harness has to provide it.
	 * VU0 has 4K of data memory and VU1 16K. */
	vuRegs[0].Mem = (u8*)calloc(1, 0x1000);
	vuRegs[1].Mem = (u8*)calloc(1, 0x4000);
	if (!vuRegs[0].Mem || !vuRegs[1].Mem) return 2;

	while (fgets(buf, sizeof(buf), f))
	{
		unsigned cl, wl, cyclereg;
		char digits[256];
		int truncated;

		if (!strncmp(buf, "== VIF1", 7)) { unit = 1; continue; }
		if (!strncmp(buf, "== VIF0", 7)) { unit = 0; continue; }

		truncated = !strncmp(buf, "truncated", 9);
		{
			const char* p = strstr(buf, "(cl ");
			if (!p) continue;
			if (sscanf(p, "(cl %u, wl %u): CYCLE: %x, %255s",
			           &cl, &wl, &cyclereg, digits) != 4) continue;
		}
		/* The truncated variants send the data in two halves, which is a
		 * transfer-level behaviour rather than a loop one. Skipped here
		 * and counted as skipped rather than silently dropped. */
		if (truncated) continue;

		{
			vifStruct& vif = unit ? vif1 : vif0;
			VIFregisters& regs = unit ? vif1Regs : vif0Regs;
			alignas(16) u32 data[8 * 4];
			char got[256];
			int i, n;

			memset(vuRegs[unit].Mem, 0, 64 * 16);
			memset(&vif, 0, sizeof(vif));
			memset(&regs, 0, sizeof(regs));
			for (i = 0; i < 8 * 4; i++) data[i] = (u32)i;

			regs.cycle.cl = (u8)cl;
			regs.cycle.wl = (u8)wl;
			regs.mask = 0xAAAAAAAAu;   /* every element takes the column */
			regs.mode = 0;
			regs.num  = 8;
			vif.MaskCol._u32[0] = 1; vif.MaskCol._u32[1] = 2;
			vif.MaskCol._u32[2] = 3; vif.MaskCol._u32[3] = 4;
			vif.cmd = 0x6C | 0x10;     /* V4-32 with masking enabled */
			vif.usn = 0;
			vif.tag.addr = 0;
			vif.start_aligned = 0;
			vif.cl = 0;

			fill_upk_table(unit, 0, vif.cmd & 0x1f);
			{
				const uint wl_eff = regs.cycle.wl ? regs.cycle.wl : 256;
				const bool isFill = (regs.cycle.cl < wl_eff);
				_nVifUnpack(unit, (const u8*)data, 0, isFill);
			}

			/* Word 0 of each of the first 64 quadwords, one hex digit
			 * each, exactly as the capture prints them. */
			n = (int)strlen(digits);
			if (n > 64) n = 64;
			for (i = 0; i < n; i++)
			{
				const u32 v = ((u32*)vuRegs[unit].Mem)[i * 4];
				got[i] = "0123456789abcdef"[v & 0xF];
			}
			got[n] = '\0';

			cases++;
			if (!strncmp(got, digits, (size_t)n)) pass++;
			else if (shown++ < 6)
				printf("  VIF%d cl %u wl %u:\n    console %.32s\n    ours    %.32s\n",
				       unit, cl, wl, digits, got);
		}
	}
	fclose(f);

	printf("hwcycle:  %d/%d VIF write-cycle scenarios match the console\n",
	       pass, cases);
	return pass != cases;
}
