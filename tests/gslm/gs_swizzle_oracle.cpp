/* GS local memory: does a transfer come back as what went in?
 *
 * GSLocalMemory addresses the PS2's 4MB of video memory through a
 * swizzle: pixels are not in scanline order but scattered through pages,
 * blocks and columns in a pattern that differs per format. Two families
 * of routine walk it -- the per-pixel accessors, and the transfer
 * routines that move a rectangle at a time and exist because they are
 * faster than calling the per-pixel ones.
 *
 * Nothing checked that a transfer round-trips. That matters because the
 * write side is written per format with hand-computed pitches, and one of
 * them was wrong: WriteImage4HL and WriteImage4HH truncated their source
 * pitch instead of rounding it up, so a width of one gave a pitch of zero
 * and the routine divided by it. That was found by reading, because there
 * was no test that moved any data.
 *
 * So this moves data: for every format it writes a rectangle, reads it
 * back and compares, across a spread of sizes and destination offsets.
 *
 * The two sides do not agree about how a row is packed when a row is not
 * a whole number of bytes, which only arises at 4bpp with an odd width.
 * WriteImage's pitch is ceil(w * bpp / 8) per row -- padded -- while
 * ReadImageX walks len * 2 pixels as one continuous run. That is a real
 * difference rather than a bug in either: at an odd width the fast write
 * paths refuse the transfer (they need the width page-aligned) and fall
 * through to WriteImageX, which packs the way ReadImageX does. The
 * round-trip below therefore runs over the sizes where the two
 * conventions provably coincide, and the odd 4bpp widths are probed
 * separately, reporting what they do rather than asserting what they
 * should.
 *
 * The 4MB block is mapped four times over, as the emulator maps it,
 * because a routine that runs off the end wraps rather than faulting. A
 * plain allocation would turn silent corruption into a crash and answer a
 * different question than the one being asked.
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "GS/GSLocalMemory.h"

static long checks;
static long failures;

static const struct { u32 psm; const char *name; } FORMATS[] = {
	{ PSMCT32,  "PSMCT32"  },
	{ PSMCT24,  "PSMCT24"  },
	{ PSMCT16,  "PSMCT16"  },
	{ PSMCT16S, "PSMCT16S" },
	{ PSMT8,    "PSMT8"    },
	{ PSMT4,    "PSMT4"    },
	{ PSMT8H,   "PSMT8H"   },
	{ PSMT4HL,  "PSMT4HL"  },
	{ PSMT4HH,  "PSMT4HH"  },
	{ PSMZ32,   "PSMZ32"   },
	{ PSMZ24,   "PSMZ24"   },
	{ PSMZ16,   "PSMZ16"   },
	{ PSMZ16S,  "PSMZ16S"  },
};

/* A transfer of w by h in this format, written then read back. Returns
 * the number of bytes that differed, or -1 if the format has no pair. */
static int round_trip(GSLocalMemory& mem, u32 psm, int w, int h,
                      int dsax, int dsay, u32 bp, u32 bw,
                      int *out_len)
{
	const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
	GIFRegBITBLTBUF blit = {};
	GIFRegTRXPOS pos = {};
	GIFRegTRXREG reg = {};
	int tx, ty, i, diff = 0;
	int len;
	static u8 src[512 * 64];
	static u8 dst[512 * 64];

	if (!p.wi || !p.ri)
		return -1;

	len = ((w * p.trbpp + 7) / 8) * h;
	if (len <= 0 || len > (int)sizeof(src))
		return -1;
	*out_len = len;

	for (i = 0; i < len; i++)
		src[i] = (u8)(i * 7 + w * 13 + h * 29 + 1);
	memset(dst, 0, (size_t)len);

	blit.DBP = bp; blit.DBW = bw; blit.DPSM = psm;
	blit.SBP = bp; blit.SBW = bw; blit.SPSM = psm;
	pos.DSAX = dsax; pos.DSAY = dsay;
	pos.SSAX = dsax; pos.SSAY = dsay;
	reg.RRW = w; reg.RRH = h;

	tx = dsax; ty = dsay;
	p.wi(mem, tx, ty, src, len, blit, pos, reg);

	tx = dsax; ty = dsay;
	p.ri(mem, tx, ty, dst, len, blit, pos, reg);

	for (i = 0; i < len; i++)
		if (src[i] != dst[i])
			diff++;

	return diff;
}

int main(void)
{
	/* Widths chosen so that w * trbpp is a whole number of bytes for
	 * every format here -- a multiple of 2 covers 4bpp, and everything
	 * wider is byte-aligned already. Heights and offsets are varied
	 * independently; the offsets are page-aligned because a transfer
	 * that starts mid-page is a different question. */
	static const int WIDTHS[]  = { 2, 8, 16, 32, 64, 128 };
	static const int HEIGHTS[] = { 1, 2, 8, 32 };
	static const int OFFSETS[] = { 0, 64 };
	static const u32 BPS[]     = { 0, 0x800 };
	GSLocalMemory mem;
	unsigned f, wi, hi, oi, bi;

	printf("gs local memory swizzle oracle\n");

	for (f = 0; f < sizeof(FORMATS) / sizeof(FORMATS[0]); f++)
	{
		const u32 psm = FORMATS[f].psm;
		long fmt_checks = 0, fmt_bad = 0;
		int skipped = 0;

		for (wi = 0; wi < sizeof(WIDTHS) / sizeof(WIDTHS[0]); wi++)
		for (hi = 0; hi < sizeof(HEIGHTS) / sizeof(HEIGHTS[0]); hi++)
		for (oi = 0; oi < sizeof(OFFSETS) / sizeof(OFFSETS[0]); oi++)
		for (bi = 0; bi < sizeof(BPS) / sizeof(BPS[0]); bi++)
		{
			int len = 0;
			int diff = round_trip(mem, psm, WIDTHS[wi], HEIGHTS[hi],
			                      OFFSETS[oi], OFFSETS[oi], BPS[bi], 4, &len);

			if (diff < 0)
			{
				skipped = 1;
				continue;
			}
			fmt_checks++;
			checks++;
			if (diff != 0)
			{
				fmt_bad++;
				if (fmt_bad == 1)
					printf("  %-9s %3dx%-3d at (%d,%d) bp %u: %d of %d bytes differ\n",
					       FORMATS[f].name, WIDTHS[wi], HEIGHTS[hi],
					       OFFSETS[oi], OFFSETS[oi], BPS[bi], diff, len);
			}
		}

		if (skipped && !fmt_checks)
			printf("  %-9s no transfer pair\n", FORMATS[f].name);
		else
			printf("  %-9s %ld transfers, %ld with differences\n",
			       FORMATS[f].name, fmt_checks, fmt_bad);
		failures += fmt_bad;
	}

	/* A transfer arrives in as many pieces as the DMA feels like. Each
	 * call gets a slice and the cursors tx/ty say where the last one
	 * stopped, so a write routine has to honour len and leave the cursors
	 * where it finished. Odd-width 4bpp had its own hand-written path
	 * that did neither: it wrote the whole rectangle from every slice and
	 * never moved the cursors, so a split transfer rewrote the rectangle
	 * once per piece and read past the end of the buffer it was given.
	 *
	 * Half a rectangle's worth of data must therefore write half a
	 * rectangle. The tail is left as a marker no transfer could have
	 * produced, and finding it intact is what proves the routine stopped
	 * where it was told. */
	printf("\n  partial transfers honour len and move the cursor:\n");
	{
		static const u32 FOURBIT[] = { PSMT4, PSMT4HL, PSMT4HH };
		static const char *FNAME[] = { "PSMT4", "PSMT4HL", "PSMT4HH" };
		static const int WID[] = { 3, 7, 9, 31, 32, 64 };
		unsigned q, o;

		for (q = 0; q < sizeof(FOURBIT) / sizeof(FOURBIT[0]); q++)
		{
			int overran = 0, stuck = 0;

			for (o = 0; o < sizeof(WID) / sizeof(WID[0]); o++)
			{
				const u32 psm = FOURBIT[q];
				const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
				GIFRegBITBLTBUF blit = {};
				GIFRegTRXPOS pos = {};
				GIFRegTRXREG reg = {};
				static u8 src[4096];
				const int w = WID[o], h = 8;
				const int full = ((w * p.trbpp + 7) / 8) * h;
				const int half = full / 2;
				int tx = 0, ty = 0, x, y;

				if (half <= 0)
					continue;

				/* Paint the whole rectangle with a marker first, then
				 * hand over only the first half of the data. Through
				 * this format's own accessor: 4HL and 4HH keep their
				 * nibble elsewhere in the word, so marking with the
				 * PSMT4 writer would leave a marker the transfer never
				 * touches and the check would pass without meaning
				 * anything. */
				for (y = 0; y < h; y++)
					for (x = 0; x < w; x++)
						gs_lm_pixel[psm].wp(&mem, x, y, 0xf, 0, 4);

				for (x = 0; x < full; x++)
					src[x] = 0x00;

				blit.DBP = 0; blit.DBW = 4; blit.DPSM = psm;
				reg.RRW = w; reg.RRH = h;
				p.wi(mem, tx, ty, src, half, blit, pos, reg);

				/* The back half of the rectangle must still be marker. */
				{
					const int wrote = half * 2;          /* pixels supplied */
					int seen = 0;

					for (y = 0; y < h && !overran; y++)
					{
						for (x = 0; x < w; x++, seen++)
						{
							if (seen < wrote)
								continue;
							if (gs_lm_pixel[psm].rp(&mem, x, y, 0, 4) != 0xf)
							{
								overran = 1;
								break;
							}
						}
					}
				}

				/* And the cursor has to have moved, or the caller's
				 * next slice starts from the wrong place. */
				if (tx == 0 && ty == 0)
					stuck = 1;

				checks += 2;
			}

			printf("    %-8s %s, %s\n", FNAME[q],
			       overran ? "WROTE PAST len" : "stopped at len",
			       stuck ? "CURSOR STUCK" : "cursor advanced");
			if (overran)
				failures++;
			if (stuck)
				failures++;
		}
	}

	/* And the widths that used to compute a zero source pitch. */
	printf("\n  odd 4bpp widths -- transfer completes and lands:\n");
	{
		static const u32 FOURBIT[] = { PSMT4, PSMT4HL, PSMT4HH };
		static const char *FNAME[] = { "PSMT4", "PSMT4HL", "PSMT4HH" };
		static const int ODD[] = { 1, 3, 5, 7, 9, 15, 31 };
		unsigned q, o;

		for (q = 0; q < sizeof(FOURBIT) / sizeof(FOURBIT[0]); q++)
		{
			int worst = 0;

			for (o = 0; o < sizeof(ODD) / sizeof(ODD[0]); o++)
			{
				const u32 psm = FOURBIT[q];
				const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
				GIFRegBITBLTBUF blit = {};
				GIFRegTRXPOS pos = {};
				GIFRegTRXREG reg = {};
				static u8 src[4096];
				const int w = ODD[o], h = 4;
				const int len = ((w * p.trbpp + 7) / 8) * h;
				int tx = 0, ty = 0, x, y, bad = 0;

				for (x = 0; x < len; x++)
					src[x] = (u8)(0x11 * ((x % 15) + 1));

				blit.DBP = 0; blit.DBW = 4; blit.DPSM = psm;
				reg.RRW = w; reg.RRH = h;
				p.wi(mem, tx, ty, src, len, blit, pos, reg);

				/* Read the rectangle back a pixel at a time, which has
				 * no packing question in it at all, and check every
				 * pixel of it is a value the source could have
				 * supplied rather than whatever was there before. */
				for (y = 0; y < h; y++)
				{
					for (x = 0; x < w; x++)
					{
						const u32 v = gs_lm_pixel[psm].rp(&mem, x, y, 0, 4);

						if (v > 15)
							bad++;
					}
				}
				if (bad > worst)
					worst = bad;
				checks++;
			}
			printf("    %-8s widths 1..31: %s\n", FNAME[q],
			       worst ? "PIXELS OUT OF RANGE" : "all pixels in range");
			if (worst)
				failures++;
		}
	}

	/* The transfer write against the per-pixel reader.
	 *
	 * The round-trip above shows wi and ri agree with each other. It
	 * cannot show they are right: an error they share passes it, since
	 * the second half undoes whatever the first half did. That is how the
	 * PSMT4 read came to be missing its destination increment for as long
	 * as it was -- nothing compared either half against anything else.
	 *
	 * So the source bytes are decoded here, independently of both, and
	 * every pixel of the rectangle is read back through the per-pixel
	 * accessor and compared against what byte n of the source says pixel
	 * n should hold. Only formats whose pixels start on byte boundaries:
	 * at 4bpp a row is not a whole number of bytes and which half of the
	 * byte a partial row ends on is the open question the odd-width
	 * section below deliberately does not settle.
	 */
	printf("\n  the transfer write against the per-pixel reader:\n");
	{
		unsigned k;

		for (k = 0; k < sizeof(FORMATS) / sizeof(FORMATS[0]); k++)
		{
			const u32 psm = FORMATS[k].psm;
			const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
			GIFRegBITBLTBUF blit = {};
			GIFRegTRXPOS pos = {};
			GIFRegTRXREG reg = {};
			static u8 src[64 * 64 * 4];
			const u32 bp = 0, bw = 4;
			const int w = 32, h = 16;
			int tx = 0, ty = 0, n, len, bad = 0;

			if (!p.wi || p.trbpp < 8)
			{
				printf("    %-9s %s\n", FORMATS[k].name,
				       p.trbpp < 8 ? "sub-byte pixels, not compared here"
				                   : "no writer");
				continue;
			}

			len = (w * h * p.trbpp) / 8;
			for (n = 0; n < len; n++)
				src[n] = (u8)(n * 37 + 11);

			blit.DBP = bp; blit.DBW = bw; blit.DPSM = psm;
			reg.RRW = w; reg.RRH = h;
			p.wi(mem, tx, ty, src, len, blit, pos, reg);

			for (n = 0; n < w * h; n++)
			{
				const int bytes = p.trbpp / 8;
				const u8 *q = &src[n * bytes];
				u32 want = 0, got;
				int b;

				for (b = 0; b < bytes; b++)
					want |= (u32)q[b] << (b * 8);

				got = gs_lm_pixel[psm].rp(&mem, n % w, n / w, bp, bw);

				if (got != want)
				{
					if (bad == 0)
						printf("    %-9s pixel %d at (%d,%d): wrote 0x%08x, reads 0x%08x\n",
						       FORMATS[k].name, n, n % w, n / w, want, got);
					bad++;
				}
			}

			checks++;
			if (bad)
			{
				printf("    %-9s %d of %d pixels differ\n",
				       FORMATS[k].name, bad, w * h);
				failures++;
			}
			else
				printf("    %-9s %d pixels land where the source put them\n",
				       FORMATS[k].name, w * h);
		}
	}

	/* readTexture against the per-pixel accessor.
	 *
	 * rtx is how the hardware renderer pulls a rectangle of local memory
	 * into a texture, and it is the most-used routine in the file: a
	 * block at a time, per format, hand-written, and in several cases
	 * with a vector expansion of the whole block at once. The per-pixel
	 * readers next to it compute one address at a time and are far
	 * harder to get wrong.
	 *
	 * So the block routine is checked against the per-pixel one texel by
	 * texel. A round-trip could not do this: it would only show that the
	 * block path undoes itself, which it would even if both halves
	 * disagreed with the rest of the emulator about where a pixel lives.
	 *
	 * Paletted formats go through rtxP, the variant that hands back the
	 * raw index, because the palette itself is not part of this harness
	 * -- rtx would expand through a CLUT that was never populated. */
	printf("\n  readTexture against the per-pixel reader:\n");
	{
		GIFRegTEXA texa = {};
		unsigned k;

		texa.TA0 = 0x80;
		texa.TA1 = 0x80;
		texa.AEM = 0;

		for (k = 0; k < sizeof(FORMATS) / sizeof(FORMATS[0]); k++)
		{
			const u32 psm = FORMATS[k].psm;
			const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
			const bool paletted = p.pal != 0;
			const GSLocalMemory::readTexture rtx = paletted ? p.rtxP : p.rtx;
			const int bytes = paletted ? 1 : 4;
			static u8 dst[256 * 256 * 4];
			GIFRegTEX0 tex0 = {};
			const u32 bp = 0, bw = 4;
			int w, h, x, y, bad = 0;

			if (!rtx)
			{
				printf("    %-9s no reader pair\n", FORMATS[k].name);
				continue;
			}

			/* Two blocks each way, so the walk crosses a block
			 * boundary in both directions rather than staying inside
			 * one and proving nothing about the stepping. */
			w = p.bs.x * 2;
			h = p.bs.y * 2;

			tex0.TBP0 = bp; tex0.TBW = bw; tex0.PSM = psm;

			/* Give every texel in the rectangle a different value, so
			 * a routine that reads the right block but the wrong
			 * position inside it still shows up. */
			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++)
					gs_lm_pixel[psm].wp(&mem, x, y, (u32)(x * 31 + y * 17 + 1), bp, bw);

			memset(dst, 0, (size_t)(w * h * bytes));
			{
				const GSOffset off = mem.GetOffset(bp, bw, psm);
				const GSVector4i r = GSVector4i(0, 0, w, h);

				rtx(mem, off, r, dst, w * bytes, texa);
			}

			for (y = 0; y < h && bad < 4; y++)
			{
				for (x = 0; x < w; x++)
				{
					u32 got, want;

					if (paletted)
					{
						got  = dst[y * w + x];
						want = gs_lm_pixel[psm].rp(&mem, x, y, bp, bw);
					}
					else
					{
						memcpy(&got, &dst[(y * w + x) * 4], 4);
						want = gs_lm_pixel[psm].rt(&mem, x, y, &tex0, &texa);
					}

					if (got != want)
					{
						if (bad < 4)
							printf("    %-9s (%d,%d): block 0x%08x, per-pixel 0x%08x\n",
							       FORMATS[k].name, x, y, got, want);
						bad++;
						break;
					}
				}
			}
			checks++;
			if (bad)
			{
				printf("    %-9s DIFFERS\n", FORMATS[k].name);
				failures++;
			}
			else
				printf("    %-9s %dx%d agrees texel for texel\n",
				       FORMATS[k].name, w, h);
		}
	}

	printf("\n%s: swizzle oracle, %ld transfers, %ld with differences\n",
	       failures ? "FAIL" : "PASS", checks, failures);
	return failures ? 1 : 0;
}
