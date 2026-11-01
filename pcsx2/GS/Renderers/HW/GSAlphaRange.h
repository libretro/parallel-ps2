/* The alpha range a texture's rtx gives a block aligned rect, read from
 * local memory without expanding it: min and max do not care in which
 * order the swizzle holds the texels. Used where the expanded texels go
 * straight into a mapped texture and the range is wanted beside them.
 * tests/gslm holds it against ReadTexture and GSGetRGBA8AlphaMinMax. */
#pragma once

#include "GS/GSLocalMemory.h"

/* The alpha range of a block aligned rect of a texture as its rtx
 * expands it, taken from local memory: false where that takes more
 * than the expanded texels would. */
static __noinline bool gs_local_alpha_minmax(GSLocalMemory* mem, const GSOffset* off, const GSVector4i* r,
	u32 psm, const GIFRegTEXA* TEXA, u8* amin, u8* amax)
{
	const u32* pal = mem->m_clut;
	u32 n = 256;
	u32 i;
	switch (psm)
	{
		case PSMCT32:
		case PSMZ32:
		case PSMCT24:
		case PSMZ24:
		{
			GSOffset::BNHelper bn = off->bnMulti(r->left, r->top);
			const int right = r->right >> off->blockShiftX();
			const int bottom = r->bottom >> off->blockShiftY();
			const bool is24 = (psm == PSMCT24 || psm == PSMZ24);
			const GSVector4i rgb = GSVector4i::x00ffffff();
			GSVector4i minc = GSVector4i::xffffffff();
			GSVector4i maxc = GSVector4i::zero();
			if (is24 && !TEXA->AEM)
			{
				*amin = *amax = static_cast<u8>(TEXA->TA0);
				return true;
			}
			for (; bn.blkY() < bottom; bn.nextBlockY())
			{
				for (; bn.blkX() < right; bn.nextBlockX())
				{
					const GSVector4i* b = reinterpret_cast<const GSVector4i*>(mem->BlockPtr(bn.value()));
					for (i = 0; i < 16; i++)
					{
						GSVector4i v = b[i];
						/* 24 bits: 0 for black, TA0 for the rest (AEM). */
						if (is24)
							v = (v & rgb) == GSVector4i::zero();
						minc = minc.min_u32(v);
						maxc = maxc.max_u32(v);
					}
				}
			}
			if (is24)
			{
				/* All ones in max: some black; all zero in min: some not. */
				const bool black = maxc.maxv_u32() != 0;
				const bool other = minc.minv_u32() == 0;
				*amin = black ? 0 : static_cast<u8>(TEXA->TA0);
				*amax = other ? static_cast<u8>(TEXA->TA0) : 0;
			}
			else
			{
				*amin = static_cast<u8>(minc.minv_u32() >> 24);
				*amax = static_cast<u8>(maxc.maxv_u32() >> 24);
			}
			return true;
		}
		case PSMCT16:
		case PSMCT16S:
		case PSMZ16:
		case PSMZ16S:
		{
			/* A texel's alpha is TA1 with bit 15 set, else TA0, else 0
			 * for a texel of all zero bits under AEM (Expand16to32). */
			GSOffset::BNHelper bn = off->bnMulti(r->left, r->top);
			const int right = r->right >> off->blockShiftX();
			const int bottom = r->bottom >> off->blockShiftY();
			const GSVector4i ones = GSVector4i::xffffffff();
			GSVector4i any_hi = GSVector4i::zero();
			GSVector4i any_lo = GSVector4i::zero();
			GSVector4i any_zero = GSVector4i::zero();
			u8 a[3];
			u32 na = 0;
			for (; bn.blkY() < bottom; bn.nextBlockY())
			{
				for (; bn.blkX() < right; bn.nextBlockX())
				{
					const GSVector4i* b = reinterpret_cast<const GSVector4i*>(mem->BlockPtr(bn.value()));
					for (i = 0; i < 16; i++)
					{
						const GSVector4i v = b[i];
						const GSVector4i hi = v.sra16<15>();
						const GSVector4i zr = TEXA->AEM ? v.eq16(GSVector4i::zero()) : GSVector4i::zero();
						any_hi |= hi;
						any_zero |= zr;
						any_lo |= ones.andnot(hi | zr);
					}
				}
			}
			if (!any_hi.allfalse())
				a[na++] = static_cast<u8>(TEXA->TA1);
			if (!any_lo.allfalse())
				a[na++] = static_cast<u8>(TEXA->TA0);
			if (!any_zero.allfalse())
				a[na++] = 0;
			if (na == 0)
				return false;
			*amin = *amax = a[0];
			for (i = 1; i < na; i++)
			{
				*amin = static_cast<u8>(pcsx2_min_i(*amin, a[i]));
				*amax = static_cast<u8>(pcsx2_max_i(*amax, a[i]));
			}
			return true;
		}
		case PSMT4:
		case PSMT4HL:
		case PSMT4HH:
			n = 16;
			/* fall through */
		case PSMT8:
		case PSMT8H:
			/* Only a CLUT of one alpha: which entries are used is not
			 * looked at. */
			for (i = 1; i < n; i++)
				if ((pal[i] ^ pal[0]) >> 24)
					return false;
			*amin = *amax = static_cast<u8>(pal[0] >> 24);
			return true;
		default:
			return false;
	}
}
