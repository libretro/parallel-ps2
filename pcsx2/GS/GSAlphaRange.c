/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023 PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include "GSAlphaRange.h"
#include "gs_vector.h"

/* the least and greatest of a vector's four u32 */
static u32 gs_alpha_minv(gs_vec4i v)
{
	u32 a[4], m;
	int i;
	gs_v4i_storeu(a, v);
	for (m = a[0], i = 1; i < 4; i++)
		m = a[i] < m ? a[i] : m;
	return m;
}

static u32 gs_alpha_maxv(gs_vec4i v)
{
	u32 a[4], m;
	int i;
	gs_v4i_storeu(a, v);
	for (m = a[0], i = 1; i < 4; i++)
		m = a[i] > m ? a[i] : m;
	return m;
}

GSAlphaMinMax gs_rgba8_alpha_minmax(const void* data, u32 width, u32 height, u32 stride)
{
	/* (alpha is the top byte: the u32 order is the alpha order) */
	gs_vec4i minc = gs_v4i_ones();
	gs_vec4i maxc = gs_v4i_zero();
	const u8* ptr = (const u8*)data;
	const u32 whole = width & ~3u;
	const u32 rest  = width & 3u;
	gs_vec4i last_or = gs_v4i_zero();
	GSAlphaMinMax range;
	u32 r, c;

	/* the lanes a row's last 1-3 texels leave empty: zero, so never the
	 * greatest, and all ones here, so never the least */
	if (rest)
		last_or = gs_v4i_set4(0, rest > 1 ? 0 : -1, rest > 2 ? 0 : -1, -1);

	for (r = 0; r < height; r++, ptr += stride)
	{
		const u8* rptr = ptr;
		for (c = 0; c < whole; c += 4, rptr += 16)
		{
			const gs_vec4i v = gs_v4i_loadu(rptr);
			minc = gs_v4i_min_u32(minc, v);
			maxc = gs_v4i_max_u32(maxc, v);
		}
		if (rest)
		{
			u32 t[4] = {0, 0, 0, 0};
			gs_vec4i v;
			memcpy(t, rptr, rest * sizeof(u32));
			v    = gs_v4i_loadu(t);
			minc = gs_v4i_min_u32(minc, gs_v4i_or(v, last_or));
			maxc = gs_v4i_max_u32(maxc, v);
		}
	}

	range.min = (u8)(gs_alpha_minv(minc) >> 24);
	range.max = (u8)(gs_alpha_maxv(maxc) >> 24);
	return range;
}

/* The 16-byte rows of each block of the rect, in turn */
#define GS_ALPHA_FOR_ROWS(mem, off, r, row)                                         \
	{                                                                              \
		gs_bn_helper bn_;                                                          \
		const int right_  = (r)[2] >> (off)->swz.block_shift_x;                    \
		const int bottom_ = (r)[3] >> (off)->swz.block_shift_y;                    \
		int i_;                                                                    \
		for (gs_bn_init(&bn_, (off), (r)[0], (r)[1]); bn_.blk_y < bottom_; gs_bn_next_y(&bn_)) \
			for (; bn_.blk_x < right_; gs_bn_next_x(&bn_))                         \
				for (i_ = 0; i_ < 16; i_++)                                        \
				{                                                                  \
					const gs_vec4i row = gs_v4i_load((mem)->m_vm8 + ((size_t)gs_bn_value(&bn_) << 8) + i_ * 16);
#define GS_ALPHA_END_ROWS \
				}         \
	}

static int gs_alpha_32(const gs_local_memory* mem, const gs_offset* off, const int r[4], int is24,
	const GIFRegTEXA* TEXA, u8* amin, u8* amax)
{
	const gs_vec4i rgb = gs_v4i_set32(0x00ffffff);
	gs_vec4i minc      = gs_v4i_ones();
	gs_vec4i maxc      = gs_v4i_zero();

	/* 24 bits: TA0 for every texel, unless black under AEM */
	if (is24 && !TEXA->AEM)
	{
		*amin = *amax = (u8)TEXA->TA0;
		return 1;
	}
	GS_ALPHA_FOR_ROWS(mem, off, r, v)
		/* (24 bits: all ones where black) */
		const gs_vec4i w = is24 ? gs_v4i_eq32(gs_v4i_and(v, rgb), gs_v4i_zero()) : v;
		minc = gs_v4i_min_u32(minc, w);
		maxc = gs_v4i_max_u32(maxc, w);
	GS_ALPHA_END_ROWS
	if (is24)
	{
		/* all ones in max: some black; all zero in min: some not */
		const int black = gs_alpha_maxv(maxc) != 0;
		const int other = gs_alpha_minv(minc) == 0;
		*amin = black ? 0 : (u8)TEXA->TA0;
		*amax = other ? (u8)TEXA->TA0 : 0;
	}
	else
	{
		*amin = (u8)(gs_alpha_minv(minc) >> 24);
		*amax = (u8)(gs_alpha_maxv(maxc) >> 24);
	}
	return 1;
}

/* A texel's alpha is TA1 with bit 15 set, else TA0, else 0 for a texel of
 * all zero bits under AEM */
static int gs_alpha_16(const gs_local_memory* mem, const gs_offset* off, const int r[4], const GIFRegTEXA* TEXA,
	u8* amin, u8* amax)
{
	const gs_vec4i ones = gs_v4i_ones();
	gs_vec4i any_hi = gs_v4i_zero(), any_lo = gs_v4i_zero(), any_zero = gs_v4i_zero();
	u8 a[3];
	u32 na = 0, i;

	GS_ALPHA_FOR_ROWS(mem, off, r, v)
		const gs_vec4i hi = GS_V4I_SRA16(v, 15);
		const gs_vec4i zr = TEXA->AEM ? gs_v4i_eq16(v, gs_v4i_zero()) : gs_v4i_zero();
		any_hi   = gs_v4i_or(any_hi, hi);
		any_zero = gs_v4i_or(any_zero, zr);
		any_lo   = gs_v4i_or(any_lo, gs_v4i_andnot(gs_v4i_or(hi, zr), ones));
	GS_ALPHA_END_ROWS

	if (!gs_v4i_allfalse(any_hi))
		a[na++] = (u8)TEXA->TA1;
	if (!gs_v4i_allfalse(any_lo))
		a[na++] = (u8)TEXA->TA0;
	if (!gs_v4i_allfalse(any_zero))
		a[na++] = 0;
	if (na == 0)
		return 0;
	*amin = *amax = a[0];
	for (i = 1; i < na; i++)
	{
		*amin = a[i] < *amin ? a[i] : *amin;
		*amax = a[i] > *amax ? a[i] : *amax;
	}
	return 1;
}

int gs_lm_alpha_minmax(const gs_local_memory* mem, const gs_offset* off, const int r[4], u32 psm,
	const GIFRegTEXA* TEXA, u8* amin, u8* amax)
{
	const u32* pal = mem->m_clut32;
	u32 n          = 256;
	u32 i;

	switch (psm)
	{
		case PSMCT32:
		case PSMZ32:
			return gs_alpha_32(mem, off, r, 0, TEXA, amin, amax);
		case PSMCT24:
		case PSMZ24:
			return gs_alpha_32(mem, off, r, 1, TEXA, amin, amax);
		case PSMCT16:
		case PSMCT16S:
		case PSMZ16:
		case PSMZ16S:
			return gs_alpha_16(mem, off, r, TEXA, amin, amax);
		case PSMT4:
		case PSMT4HL:
		case PSMT4HH:
			n = 16;
			/* fall through */
		case PSMT8:
		case PSMT8H:
			/* only a palette of one alpha: which entries are used is not
			 * looked at */
			for (i = 1; i < n; i++)
				if ((pal[i] ^ pal[0]) >> 24)
					return 0;
			*amin = *amax = (u8)(pal[0] >> 24);
			return 1;
		default:
			return 0;
	}
}
