/*  GetSizeFixedTEX0 keeps the texel at the inclusive max coordinate.
 *
 *  The max UV it sizes from is an inclusive texel coordinate (CLAMP limits
 *  it to (1 << TW) - 1, MAXU/MAXV are inclusive), so a size of 1 << n must
 *  cover 0..uv. A max of exactly 1 << n used to shrink or fail to extend
 *  the texture by one power of two and drop that edge texel -- the one a
 *  horizontally flipped sprite samples at its first pixel.
 *
 *  Links the real GSDrawingContext.cpp. Self-contained.
 */

#include <stdio.h>

#include "GS/GSDrawingContext.h"

static int failures;

static void check(const char* name, u32 tw, u32 th, u32 wms, u32 maxu,
                  float smax, u32 want_tw)
{
	GSDrawingContext c;
	GIFRegTEX0 res;

	c.Reset();
	c.TEX0.TW = tw;
	c.TEX0.TH = th;
	c.CLAMP.WMS = wms;
	c.CLAMP.WMT = CLAMP_CLAMP;
	c.CLAMP.MINU = 0;
	c.CLAMP.MAXU = maxu;

	res = c.GetSizeFixedTEX0(GSVector4(0.0f, 0.0f, smax, 1.0f), false, false);
	if (res.TW != want_tw)
	{
		printf("  FAIL %-38s TW = %u, want %u\n", name, (unsigned)res.TW, (unsigned)want_tw);
		failures++;
	}
}

int main(void)
{
	/* reduce(): only large textures are shrunk (TW + TH >= 19). */
	check("clamp, max u 512 keeps 1024",       10, 10, CLAMP_CLAMP, 0, 512.0f, 10);
	check("clamp, max u 511 still shrinks",    10, 10, CLAMP_CLAMP, 0, 511.0f, 9);
	check("clamp, max u 256 keeps 512",        10, 10, CLAMP_CLAMP, 0, 256.0f, 9);
	check("clamp, max u 255 shrinks to 256",   10, 10, CLAMP_CLAMP, 0, 255.0f, 8);

	/* extend(): region modes widen to reach MAXU, which is inclusive. */
	check("region clamp, MAXU 256 extends",     8, 8, CLAMP_REGION_CLAMP, 256, 300.0f, 9);
	check("region clamp, MAXU 255 stays",       8, 8, CLAMP_REGION_CLAMP, 255, 300.0f, 8);

	if (failures)
	{
		printf("gs_texsize: %d failure(s)\n", failures);
		return 1;
	}
	printf("gs_texsize: all cases pass\n");
	return 0;
}
