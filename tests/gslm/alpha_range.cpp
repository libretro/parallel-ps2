/* The alpha range of a texture rect, read from local memory as it lies.
 *
 * Target::Update and PreloadTexture want the alpha range of the texels
 * they expand into a mapped texture. Expanding a second time into a
 * scratch buffer just to scan it was the old way; gs_local_alpha_minmax
 * (GS/Renderers/HW/GSAlphaRange.h) takes the range from local memory
 * instead, since min and max do not care where the swizzle put a texel.
 *
 * Held here against the truth: ReadTexture expands the same rect, and
 * the range is taken over the expanded alpha bytes. Every format the
 * helper answers for, both AEM settings, TEXA alphas that order either
 * way, and memory filled four ways - random, all zero, every bit 15
 * set, and a mix of zero, bit-15 and other texels - so each alpha the
 * expansion can give appears alone and together with the others.
 *
 * A helper that ignores AEM is run beside it as the negative control:
 * it must disagree somewhere, or this test could not see that bug. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/HW/GSAlphaRange.h"

static const struct { u32 psm; const char* name; } FORMATS[] = {
	{ PSMCT32,  "PSMCT32"  },
	{ PSMZ32,   "PSMZ32"   },
	{ PSMCT24,  "PSMCT24"  },
	{ PSMZ24,   "PSMZ24"   },
	{ PSMCT16,  "PSMCT16"  },
	{ PSMCT16S, "PSMCT16S" },
	{ PSMZ16,   "PSMZ16"   },
	{ PSMZ16S,  "PSMZ16S"  },
};

static u32 s_rng = 1;
static u32 rnd(void)
{
	s_rng = s_rng * 1103515245u + 12345u;
	return s_rng >> 8;
}

/* Fills the 4MB: 0 random, 1 zero, 2 bit 15 (and 31) set in every
 * halfword, 3 halfwords drawn from {0, 0x8000, other}. */
static void fill(u8* vm, int how)
{
	u16* h = reinterpret_cast<u16*>(vm);
	u32 i;
	for (i = 0; i < GSLocalMemory::m_vmsize / 2; i++)
	{
		switch (how)
		{
			case 0: h[i] = static_cast<u16>(rnd()); break;
			case 1: h[i] = 0; break;
			case 2: h[i] = static_cast<u16>(0x8000u | rnd()); break;
			default:
			{
				const u32 k = rnd() % 3;
				h[i] = k == 0 ? 0 : k == 1 ? static_cast<u16>(0x8000u | (rnd() & 0x7fff)) : static_cast<u16>(1u + (rnd() % 0x7fffu));
				break;
			}
		}
	}
}

/* The control: the 16-bit branch as it would be written forgetting
 * AEM, so a texel of zero gets TA0. */
static bool no_aem(GSLocalMemory* mem, const GSOffset* off, const GSVector4i* r,
	u32 psm, const GIFRegTEXA* TEXA, u8* amin, u8* amax)
{
	GIFRegTEXA t = *TEXA;
	t.AEM = 0;
	return gs_local_alpha_minmax(mem, off, r, psm, &t, amin, amax);
}

int main(void)
{
	GSLocalMemory mem;
	std::vector<u8> buf;
	long checks = 0, bad = 0, control_caught = 0, declined = 0;
	unsigned f;
	int how, round;

	for (how = 0; how < 4; how++)
	{
		fill(mem.m_vm8, how);
		for (f = 0; f < sizeof(FORMATS) / sizeof(FORMATS[0]); f++)
		{
			const u32 psm = FORMATS[f].psm;
			const GSVector2i bs = GSLocalMemory::m_psm[psm].bs;
			for (round = 0; round < 48; round++)
			{
				GIFRegTEXA TEXA = {};
				const u32 bp = (rnd() % 0x3000u) & ~31u;
				const u32 bw = 1u + rnd() % 10u;
				const int bx = static_cast<int>(rnd() % 8u), by = static_cast<int>(rnd() % 8u);
				const int nbx = 1 + static_cast<int>(rnd() % 6u), nby = 1 + static_cast<int>(rnd() % 6u);
				const GSVector4i r(bx * bs.x, by * bs.y, (bx + nbx) * bs.x, (by + nby) * bs.y);
				const GSOffset off = mem.GetOffset(bp, bw, psm);
				const int pitch = r.width() * 4;
				u8 want_min = 255, want_max = 0, amin = 0, amax = 0, cmin = 0, cmax = 0;
				int x, y;

				TEXA.AEM = rnd() & 1;
				TEXA.TA0 = rnd() & 0xff;
				TEXA.TA1 = rnd() & 0xff;
				if ((round & 3) == 0)
					TEXA.TA0 = 0; /* TA0 equal to the AEM zero */

				buf.assign(static_cast<size_t>(pitch) * r.height() + 64, 0);
				u8* dst = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(buf.data()) + 31) & ~uintptr_t(31));
				mem.ReadTexture(off, r, dst, pitch, TEXA);
				for (y = 0; y < r.height(); y++)
					for (x = 0; x < r.width(); x++)
					{
						const u8 a = dst[y * pitch + x * 4 + 3];
						want_min = a < want_min ? a : want_min;
						want_max = a > want_max ? a : want_max;
					}

				checks++;
				if (!gs_local_alpha_minmax(&mem, &off, &r, psm, &TEXA, &amin, &amax))
				{
					declined++;
					continue;
				}
				if (amin != want_min || amax != want_max)
				{
					if (bad < 10)
						printf("  %s fill %d AEM %u TA0 %02x TA1 %02x: %02x..%02x, expanded %02x..%02x\n",
							FORMATS[f].name, how, (unsigned)TEXA.AEM, (unsigned)TEXA.TA0, (unsigned)TEXA.TA1,
							amin, amax, want_min, want_max);
					bad++;
				}
				if (no_aem(&mem, &off, &r, psm, &TEXA, &cmin, &cmax) && (cmin != want_min || cmax != want_max))
					control_caught++;
			}
		}
	}

	printf("  alpha range: %ld rects, %ld wrong, %ld declined\n", checks, bad, declined);
	if (declined)
	{
		printf("  a format the helper answers for was declined\n");
		bad++;
	}
	if (!control_caught)
	{
		printf("  the control that ignores AEM was never caught\n");
		bad++;
	}
	else
		printf("  control (AEM ignored): caught on %ld rects\n", control_caught);
	printf(bad ? "FAIL\n" : "PASS\n");
	return bad ? 1 : 0;
}
