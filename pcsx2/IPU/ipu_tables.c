/*  PCSX2 - PS2 Emulator for PCs
 *
 *  The IPU's decode tables, expanded from the standard's code lists.
 *  One copy for every instruction set tier's decoder; see ipu_tables.h.
 */

#include <string.h>

#include "ipu_tables.h"
#include "ipu_vlc_codes.h"
#include "ipu_dct_codes.h"

PCSX2_ALIGN(16) IPUTables ipu_tables;
static bool s_ipu_tables_built;

/* Each code expanded into the indexed array a decode path reads.
 *
 *   W  width of the bit window the lookup takes
 *   S  right shift applied to the window before indexing
 *   B  index base subtracted after the shift
 *
 * A code of length L covers every window value sharing its prefix; each
 * of those maps to index (value >> S) - B, and entries outside the array
 * are values the call site's guard never reaches. Every entry type is the
 * value byte then the length byte, so one expander fills them all. */
static void vlc_expand(void* out, int n, const VLCcode* codes, int m, int w, int sh, int base)
{
	u8* e = (u8*)out;
	int c, v;
	for (c = 0; c < m; c++)
	{
		const int pw    = codes[c].len < w ? codes[c].len : w;
		const int shift = w - pw;
		const int start = codes[c].bits << shift;
		for (v = start; v < start + (1 << shift); v++)
		{
			const int idx = (v >> sh) - base;
			if (idx >= 0 && idx < n)
			{
				e[idx * 2]     = (u8)codes[c].value;
				e[idx * 2 + 1] = codes[c].len;
			}
		}
	}
}

#define VLC_EXPAND(out, codes, w, s, b) \
	vlc_expand(out, (int)(sizeof(out) / sizeof((out)[0])), codes, \
		(int)(sizeof(codes) / sizeof((codes)[0])), w, s, b)

/* Codes of 12 bits or fewer fill the span of hi[] their prefix covers;
 * longer ones, below 256 in the 16-bit window, fill lo[] directly. */
static void dct_fill(DCTlutHi* hi, DCTlutLo* lo, const DCTcode* codes, int n)
{
	int c, idx;
	for (c = 0; c < n; c++)
	{
		DCTtabLut v;
		v.run   = codes[c].run;
		v.level = codes[c].level;
		v.len   = codes[c].len;
		if (codes[c].len <= 12)
		{
			const int shift = 12 - codes[c].len;
			const int start = codes[c].bits << shift;
			for (idx = start; idx < start + (1 << shift); idx++)
				if (idx >= 16)
					hi->e[idx] = v;
		}
		else
		{
			const int shift = 16 - codes[c].len;
			const int start = codes[c].bits << shift;
			for (idx = start; idx < start + (1 << shift); idx++)
				if (idx >= 16 && idx < 256)
					lo->e[idx] = v;
		}
	}
}

#define DCT_FILL(hi, codes) \
	dct_fill(hi, &t->dct.lo, codes, (int)(sizeof(codes) / sizeof((codes)[0])))

void ipu_tables_init(void)
{
	IPUTables* t = &ipu_tables;
	if (s_ipu_tables_built)
		return;
	memset(t, 0, sizeof(*t));

	/* macroblock_type, Tables B-2/B-3/B-4 */
	VLC_EXPAND(t->vlc.mb_i, VLC_MB_I, 2, 1, 0);
	VLC_EXPAND(t->vlc.mb_p, VLC_MB_P, 6, 1, 0);
	VLC_EXPAND(t->vlc.mb_b, VLC_MB_B, 6, 0, 0);
	/* motion_code, Table B-10, and dmvector, Table B-11 */
	VLC_EXPAND(t->vlc.mv_4, VLC_MV_4, 4, 0, 0);
	VLC_EXPAND(t->vlc.mv_10, VLC_MV_10, 10, 0, 0);
	VLC_EXPAND(t->vlc.dmv_2, VLC_DMV_2, 2, 0, 0);
	/* coded_block_pattern, Table B-9 */
	VLC_EXPAND(t->vlc.cbp_7, VLC_CBP_7, 7, 0, 16);
	VLC_EXPAND(t->vlc.cbp_9, VLC_CBP_9, 9, 0, 0);
	/* macroblock_address_increment, Table B-1 */
	VLC_EXPAND(t->mba.mba5, VLC_MBA5, 5, 0, 2);
	VLC_EXPAND(t->mba.mba11, VLC_MBA11, 11, 0, 24);
	/* dct_dc_size_luminance / _chrominance, Tables B-12 and B-13 */
	VLC_EXPAND(t->dc.lum0, VLC_DC_LUM0, 5, 0, 0);
	VLC_EXPAND(t->dc.lum1, VLC_DC_LUM1, 9, 0, 0x1f0);
	VLC_EXPAND(t->dc.chrom0, VLC_DC_CHROM0, 5, 0, 0);
	VLC_EXPAND(t->dc.chrom1, VLC_DC_CHROM1, 10, 0, 0x3e0);
	/* DCT coefficients, Tables B-14 and B-15 */
	DCT_FILL(&t->dct.hi[DCT_LUT_NEXT], DCT_CODES_NEXT);
	DCT_FILL(&t->dct.hi[DCT_LUT_FIRST], DCT_CODES_FIRST);
	DCT_FILL(&t->dct.hi[DCT_LUT_INTRA], DCT_CODES_INTRA);

	s_ipu_tables_built = true;
}
