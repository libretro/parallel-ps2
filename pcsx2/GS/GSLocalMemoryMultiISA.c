/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

/* The format table's transfers of rectangles (gs_lm_psm): images written
 * from a transfer and read back, textures read a block at a time. Built
 * once for each SIMD tier; the first tier's build also holds the table
 * and picks the CPU's tier for it. */

#include <stddef.h>
#include <string.h>

#include "common/VectorIntrin.h"
#include "common/CpuFeatures.h"
#include "GSLocalMemory.h"
#include "gs_block.h"

#ifdef MULTI_ISA_UNSHARED_COMPILATION
#define LM_ISA MULTI_ISA_UNSHARED_COMPILATION
#else
#define LM_ISA isa_native
#endif
#define LM_CAT2(a, b) a##b
#define LM_CAT(a, b) LM_CAT2(a, b)
#define LM_POPULATE LM_CAT(gs_lm_populate_, LM_ISA)

#define LM_INLINE GS_VEC_INLINE

/* The table (C++ sees it as GSLocalMemoryPsm, its view of gs_lm_psm),
 * defined below in one tier's build */
extern gs_lm_psm gs_lm_psm_table[64];

void gs_lm_populate_isa_native(void);
void gs_lm_populate_isa_sse4(void);
void gs_lm_populate_isa_avx(void);
void gs_lm_populate_isa_avx2(void);

/* The swizzle of each format the block transfers take */
static LM_INLINE const gs_swizzle_info* lm_swizzle(u32 psm)
{
	switch (psm)
	{
		case PSMCT16: return &gs_swizzle16;
		case PSMCT16S: return &gs_swizzle16S;
		case PSMT8: return &gs_swizzle8;
		case PSMT4: return &gs_swizzle4;
		case PSMZ32: return &gs_swizzle32Z;
		case PSMZ16: return &gs_swizzle16Z;
		case PSMZ16S: return &gs_swizzle16SZ;
		default: return &gs_swizzle32;
	}
}

static LM_INLINE u8* lm_block_ptr(const gs_local_memory* mem, const gs_swizzle_info* s, int x, int y, u32 bp, u32 bw)
{
	return &mem->m_vm8[gs_swizzle_bn(s, x, y, bp, bw) << 8];
}

static LM_INLINE u8* lm_block_at(const gs_local_memory* mem, u32 bp)
{
	return &mem->m_vm8[(bp % GS_MAX_BLOCKS) << 8];
}

/* The buffer off names, with its shape the constant swizzle's, so the
 * compiler folds it */
static LM_INLINE gs_offset lm_sized(const gs_offset* off, const gs_swizzle_info* s)
{
	gs_offset o = *off;
	o.swz.page_mask_x    = s->page_mask_x;
	o.swz.page_mask_y    = s->page_mask_y;
	o.swz.block_mask_x   = s->block_mask_x;
	o.swz.block_mask_y   = s->block_mask_y;
	o.swz.pixel_row_mask = s->pixel_row_mask;
	o.swz.page_shift_x   = s->page_shift_x;
	o.swz.page_shift_y   = s->page_shift_y;
	o.swz.block_shift_x  = s->block_shift_x;
	o.swz.block_shift_y  = s->block_shift_y;
	return o;
}

/* ---- writing images ---- */

static void lm_write_image_x(gs_local_memory* mem, int* tx, int* ty, const u8* src, int len,
	const GIFRegBITBLTBUF* BITBLTBUF, const GIFRegTRXPOS* TRXPOS, const GIFRegTRXREG* TRXREG);

static LM_INLINE void lm_write_image_column(int psm, int bsx, int bsy, int alignment, gs_local_memory* mem, int l,
	int r, int y, int h, const u8* src, int srcpitch, const GIFRegBITBLTBUF* BITBLTBUF)
{
	const u32 bp = BITBLTBUF->DBP;
	const u32 bw = BITBLTBUF->DBW;
	const gs_swizzle_info* s = lm_swizzle(psm);
	const int csy = bsy / 4;
	const int offset = srcpitch * csy;
	int x;

	for (; h >= csy; h -= csy, y += csy, src += offset)
	{
		for (x = l; x < r; x += bsx)
		{
			u8* dst = lm_block_ptr(mem, s, x, y, bp, bw);
			switch (psm)
			{
				case PSMCT32:
				case PSMZ32: gs_block_write_column32_y(y, alignment, 0xffffffff, dst, &src[x * 4], srcpitch); break;
				case PSMT8: gs_block_write_column8_y(y, alignment, dst, &src[x], srcpitch); break;
				case PSMT4: gs_block_write_column4_y(y, alignment, dst, &src[x >> 1], srcpitch); break;
				default: gs_block_write_column16_y(y, alignment, dst, &src[x * 2], srcpitch); break;
			}
		}
	}
}

static LM_INLINE void lm_write_image_block(int psm, int bsx, int bsy, int alignment, gs_local_memory* mem, int l,
	int r, int y, int h, const u8* src, int srcpitch, const GIFRegBITBLTBUF* BITBLTBUF)
{
	const u32 bp = BITBLTBUF->DBP;
	const u32 bw = BITBLTBUF->DBW;
	const gs_swizzle_info* s = lm_swizzle(psm);
	const int offset = srcpitch * bsy;
	int x;

	for (; h >= bsy; h -= bsy, y += bsy, src += offset)
	{
		for (x = l; x < r; x += bsx)
		{
			u8* dst = lm_block_ptr(mem, s, x, y, bp, bw);
			switch (psm)
			{
				case PSMCT32:
				case PSMZ32: gs_block_write_block32(alignment, 0xffffffff, dst, &src[x * 4], srcpitch); break;
				case PSMT8: gs_block_write_block8(alignment, dst, &src[x], srcpitch); break;
				case PSMT4: gs_block_write_block4(alignment, dst, &src[x >> 1], srcpitch); break;
				default: gs_block_write_block16(alignment, dst, &src[x * 2], srcpitch); break;
			}
		}
	}
}

static LM_INLINE void lm_write_image_left_right(int psm, gs_local_memory* mem, int l, int r, int y, int h,
	const u8* src, int srcpitch, const GIFRegBITBLTBUF* BITBLTBUF)
{
	const u32 bp = BITBLTBUF->DBP;
	const u32 bw = BITBLTBUF->DBW;
	const gs_swizzle_info* s = lm_swizzle(psm);
	int x;

	for (; h > 0; y++, h--, src += srcpitch)
	{
		for (x = l; x < r; x++)
		{
			const u32 a = gs_swizzle_pa(s, x, y, bp, bw);
			u32 v;
			switch (psm)
			{
				case PSMCT32:
				case PSMZ32:
					memcpy(&v, &src[x * 4], 4);
					gs_lm_write_pixel32(mem, a, v);
					break;
				case PSMT8: gs_lm_write_pixel8(mem, a, src[x]); break;
				case PSMT4: gs_lm_write_pixel4(mem, a, (u32)(src[x >> 1] >> ((x & 1) << 2))); break;
				default:
				{
					u16 w;
					memcpy(&w, &src[x * 2], 2);
					gs_lm_write_pixel16(mem, a, w);
				}
				break;
			}
		}
	}
}

/* A partial column of rows y.. (h of them, from row y2 of the column):
 * the column read out, the rows merged in, and written back */
static LM_INLINE void lm_merge_column(int psm, int bsx, gs_local_memory* mem, int l, int r, int y, int y2, int h,
	const u8* src, int srcpitch, u32 bp, u32 bw)
{
	PCSX2_ALIGN(32) u8 buff[64];
	const gs_swizzle_info* s = lm_swizzle(psm);
	int x, i;

	for (x = l; x < r; x += bsx)
	{
		u8* dst = lm_block_ptr(mem, s, x, y, bp, bw);
		switch (psm)
		{
			case PSMCT32:
			case PSMZ32:
				gs_block_read_column32_y(y, dst, buff, 32);
				memcpy(&buff[y2 ? 32 : 0], &src[x * 4], 32);
				gs_block_write_column32_y(y, 32, 0xffffffff, dst, buff, 32);
				break;
			case PSMT8:
				gs_block_read_column8_y(y, dst, buff, 16);
				for (i = 0; i < h; i++)
					memcpy(&buff[(i + y2) * 16], &src[i * srcpitch + x], 16);
				gs_block_write_column8_y(y, 32, dst, buff, 16);
				break;
			case PSMT4:
				gs_block_read_column4_y(y, dst, buff, 16);
				for (i = 0; i < h; i++)
					memcpy(&buff[(i + y2) * 16], &src[i * srcpitch + (x >> 1)], 16);
				gs_block_write_column4_y(y, 32, dst, buff, 16);
				break;
			default:
				gs_block_read_column16_y(y, dst, buff, 32);
				memcpy(&buff[y2 ? 32 : 0], &src[x * 2], 32);
				gs_block_write_column16_y(y, 32, dst, buff, 32);
				break;
		}
	}
}

/* Whole columns, at the best alignment the source has */
static LM_INLINE void lm_write_columns(int psm, int bsx, int bsy, int trbpp, gs_local_memory* mem, int l, int r,
	int y, int h, const u8* src, int srcpitch, const GIFRegBITBLTBUF* BITBLTBUF)
{
#if FAST_UNALIGNED
	(void)trbpp;
	lm_write_image_column(psm, bsx, bsy, 0, mem, l, r, y, h, src, srcpitch, BITBLTBUF);
#else
	const size_t addr = (size_t)&src[l * trbpp >> 3];
	if ((addr & 31) == 0 && (srcpitch & 31) == 0)
		lm_write_image_column(psm, bsx, bsy, 32, mem, l, r, y, h, src, srcpitch, BITBLTBUF);
	else if ((addr & 15) == 0 && (srcpitch & 15) == 0)
		lm_write_image_column(psm, bsx, bsy, 16, mem, l, r, y, h, src, srcpitch, BITBLTBUF);
	else
		lm_write_image_column(psm, bsx, bsy, 0, mem, l, r, y, h, src, srcpitch, BITBLTBUF);
#endif
}

static LM_INLINE void lm_write_blocks(int psm, int bsx, int bsy, int trbpp, gs_local_memory* mem, int l, int r,
	int y, int h, const u8* src, int srcpitch, const GIFRegBITBLTBUF* BITBLTBUF)
{
#if FAST_UNALIGNED
	(void)trbpp;
	lm_write_image_block(psm, bsx, bsy, 0, mem, l, r, y, h, src, srcpitch, BITBLTBUF);
#else
	const size_t addr = (size_t)&src[l * trbpp >> 3];
	if ((addr & 31) == 0 && (srcpitch & 31) == 0)
		lm_write_image_block(psm, bsx, bsy, 32, mem, l, r, y, h, src, srcpitch, BITBLTBUF);
	else if ((addr & 15) == 0 && (srcpitch & 15) == 0)
		lm_write_image_block(psm, bsx, bsy, 16, mem, l, r, y, h, src, srcpitch, BITBLTBUF);
	else
		lm_write_image_block(psm, bsx, bsy, 0, mem, l, r, y, h, src, srcpitch, BITBLTBUF);
#endif
}

static LM_INLINE void lm_write_image_top_bottom(int psm, int bsx, int bsy, int trbpp, gs_local_memory* mem, int l,
	int r, int y, int h, const u8* src, int srcpitch, const GIFRegBITBLTBUF* BITBLTBUF)
{
	const u32 bp = BITBLTBUF->DBP;
	const u32 bw = BITBLTBUF->DBW;
	const int csy = bsy / 4;
	const int y2 = y & (csy - 1);
	int h2;

	/* an incomplete column first */
	if (y2 > 0)
	{
		h2 = h < csy - y2 ? h : csy - y2;
		lm_merge_column(psm, bsx, mem, l, r, y, y2, h2, src, srcpitch, bp, bw);
		src += srcpitch * h2;
		y += h2;
		h -= h2;
	}

	/* whole columns */
	h2 = h & ~(csy - 1);
	if (h2 > 0)
	{
		lm_write_columns(psm, bsx, bsy, trbpp, mem, l, r, y, h2, src, srcpitch, BITBLTBUF);
		src += srcpitch * h2;
		y += h2;
		h -= h2;
	}

	/* an incomplete column last */
	if (h >= 1)
		lm_merge_column(psm, bsx, mem, l, r, y, 0, h, src, srcpitch, bp, bw);
}

/* An odd-width 4-bit transfer, a pixel at a time: the block paths step
 * the source a whole byte a row, and an odd row is not. From where the
 * last piece stopped, for the pixels this one carries. */
static LM_INLINE void lm_write_odd4(void (*write)(gs_local_memory*, u32, u32), const gs_swizzle_info* s,
	gs_local_memory* mem, int* tx, int* ty, const u8* src, int len, const GIFRegBITBLTBUF* BITBLTBUF,
	const GIFRegTRXPOS* TRXPOS, const GIFRegTRXREG* TRXREG)
{
	const int l = (int)TRXPOS->DSAX;
	const int r = l + (int)TRXREG->RRW;
	int pixels = len * 2;
	int count = 0;
	int x = *tx;
	int y = *ty;

	while (pixels-- > 0)
	{
		write(mem, gs_swizzle_pa(s, x, y, BITBLTBUF->DBP, BITBLTBUF->DBW),
			(u32)(src[count >> 1] >> ((count & 1) << 2)));
		count++;
		if (++x >= r)
		{
			x = l;
			y++;
		}
	}
	*tx = x;
	*ty = y;
}

static void lm_write_pixel4(gs_local_memory* mem, u32 a, u32 c) { gs_lm_write_pixel4(mem, a, c); }
static void lm_write_pixel4hl(gs_local_memory* mem, u32 a, u32 c) { gs_lm_write_pixel4hl(mem, a, c); }
static void lm_write_pixel4hh(gs_local_memory* mem, u32 a, u32 c) { gs_lm_write_pixel4hh(mem, a, c); }

static LM_INLINE void lm_write_image(int psm, int bsx, int bsy, int trbpp, gs_local_memory* mem, int* tx, int* ty,
	const u8* src, int len, const GIFRegBITBLTBUF* BITBLTBUF, const GIFRegTRXPOS* TRXPOS,
	const GIFRegTRXREG* TRXREG)
{
	int l, r, la, ra, srcpitch, h;

	if (TRXREG->RRW == 0)
		return;

	l = (int)TRXPOS->DSAX;
	r = l + (int)TRXREG->RRW;

	/* the incomplete row first */
	if (*tx != l)
	{
		int n = (r - *tx) * trbpp >> 3;
		if (len < n)
			n = len;
		lm_write_image_x(mem, tx, ty, src, n, BITBLTBUF, TRXPOS, TRXREG);
		src += n;
		len -= n;
	}

	la = (l + (bsx - 1)) & ~(bsx - 1);
	ra = r & ~(bsx - 1);
	/* rounded up to a byte (NFL 2K5: one 4-bit pixel a row) */
	srcpitch = (((r - l) * trbpp) + 7) >> 3;
	h = len / srcpitch;

	/* GT3 demo, Jak 2's Japanese subtitles, the BG Dark Alliance minimap.
	 * And an odd DSAX: a row starts on a byte, so its pixels sit a nibble
	 * off the ones the paths below take from x / 2 - which read the byte
	 * before the source for the first. */
	if (trbpp == 4 && ((TRXREG->RRW | TRXPOS->DSAX) & 1))
	{
		lm_write_odd4(lm_write_pixel4, &gs_swizzle4, mem, tx, ty, src, len, BITBLTBUF, TRXPOS, TRXREG);
		return;
	}

	/* at least one block wide, and at least one whole row */
	if (ra - la >= bsx && h > 0)
	{
		const u8* s = &src[-l * trbpp >> 3];

		src += srcpitch * h;
		len -= srcpitch * h;

		if (l < la)
			lm_write_image_left_right(psm, mem, l, la, *ty, h, s, srcpitch, BITBLTBUF);
		if (ra < r)
			lm_write_image_left_right(psm, mem, ra, r, *ty, h, s, srcpitch, BITBLTBUF);

		if (la < ra)
		{
			int h2 = bsy - (*ty & (bsy - 1));
			if (h < h2)
				h2 = h;
			if (h2 < bsy)
			{
				lm_write_image_top_bottom(psm, bsx, bsy, trbpp, mem, la, ra, *ty, h2, s, srcpitch, BITBLTBUF);
				s += srcpitch * h2;
				*ty += h2;
				h -= h2;
			}

			h2 = h & ~(bsy - 1);
			if (h2 > 0)
			{
				lm_write_blocks(psm, bsx, bsy, trbpp, mem, la, ra, *ty, h2, s, srcpitch, BITBLTBUF);
				s += srcpitch * h2;
				*ty += h2;
				h -= h2;
			}

			if (h > 0)
			{
				lm_write_image_top_bottom(psm, bsx, bsy, trbpp, mem, la, ra, *ty, h, s, srcpitch, BITBLTBUF);
				*ty += h;
			}
		}
	}

	if (len > 0)
		lm_write_image_x(mem, tx, ty, src, len, BITBLTBUF, TRXPOS, TRXREG);
}

#define LM_WRITE_IMAGE(name, psm, bsx, bsy, trbpp) \
	static void name(gs_local_memory* mem, int* tx, int* ty, const u8* src, int len, \
		const GIFRegBITBLTBUF* BITBLTBUF, const GIFRegTRXPOS* TRXPOS, const GIFRegTRXREG* TRXREG) \
	{ \
		lm_write_image(psm, bsx, bsy, trbpp, mem, tx, ty, src, len, BITBLTBUF, TRXPOS, TRXREG); \
	}
LM_WRITE_IMAGE(lm_write_image32, PSMCT32, 8, 8, 32)
LM_WRITE_IMAGE(lm_write_image16, PSMCT16, 16, 8, 16)
LM_WRITE_IMAGE(lm_write_image16s, PSMCT16S, 16, 8, 16)
LM_WRITE_IMAGE(lm_write_image8, PSMT8, 16, 16, 8)
LM_WRITE_IMAGE(lm_write_image4, PSMT4, 32, 16, 4)
LM_WRITE_IMAGE(lm_write_image32z, PSMZ32, 8, 8, 32)
LM_WRITE_IMAGE(lm_write_image16z, PSMZ16, 16, 8, 16)
LM_WRITE_IMAGE(lm_write_image16sz, PSMZ16S, 16, 8, 16)

static int lm_top_left_aligned(int dsax, int tx, int ty, int bw, int bh)
{
	return (dsax & (bw - 1)) == 0 && (tx & (bw - 1)) == 0 && dsax == tx && (ty & (bh - 1)) == 0;
}

/* The 24-bit and high-byte formats: whole 8x8 blocks unpacked when the
 * rectangle is made of them, pixel by pixel otherwise */
static LM_INLINE void lm_write_image_unpacked(int kind, const gs_swizzle_info* s, int srcpitch, gs_local_memory* mem,
	int* tx, int* ty, const u8* src, int len, const GIFRegBITBLTBUF* BITBLTBUF, const GIFRegTRXPOS* TRXPOS,
	const GIFRegTRXREG* TRXREG)
{
	const u32 bp = BITBLTBUF->DBP;
	const u32 bw = BITBLTBUF->DBW;
	const int tw = (int)(TRXPOS->DSAX + TRXREG->RRW);
	int th = len / srcpitch;
	int x, y;

	if (!lm_top_left_aligned((int)TRXPOS->DSAX, *tx, *ty, 8, 8) || (tw & 7) || (th & 7) || (len % srcpitch))
	{
		lm_write_image_x(mem, tx, ty, src, len, BITBLTBUF, TRXPOS, TRXREG);
		return;
	}

	th += *ty;
	for (y = *ty; y < th; y += 8, src += srcpitch * 8)
	{
		for (x = *tx; x < tw; x += 8)
		{
			u8* dst = lm_block_ptr(mem, s, x, y, bp, bw);
			switch (kind)
			{
				case 0: gs_block_unpack_and_write_block24(src + (x - *tx) * 3, srcpitch, dst); break;
				case 1: gs_block_unpack_and_write_block8h(src + (x - *tx), srcpitch, dst); break;
				case 2: gs_block_unpack_and_write_block4hl(src + (x - *tx) / 2, srcpitch, dst); break;
				default: gs_block_unpack_and_write_block4hh(src + (x - *tx) / 2, srcpitch, dst); break;
			}
		}
	}
	*ty = th;
}

static void lm_write_image24(gs_local_memory* mem, int* tx, int* ty, const u8* src, int len,
	const GIFRegBITBLTBUF* BITBLTBUF, const GIFRegTRXPOS* TRXPOS, const GIFRegTRXREG* TRXREG)
{
	if (TRXREG->RRW == 0)
		return;
	lm_write_image_unpacked(0, &gs_swizzle32, (int)TRXREG->RRW * 3, mem, tx, ty, src, len, BITBLTBUF, TRXPOS,
		TRXREG);
}

static void lm_write_image24z(gs_local_memory* mem, int* tx, int* ty, const u8* src, int len,
	const GIFRegBITBLTBUF* BITBLTBUF, const GIFRegTRXPOS* TRXPOS, const GIFRegTRXREG* TRXREG)
{
	if (TRXREG->RRW == 0)
		return;
	lm_write_image_unpacked(0, &gs_swizzle32Z, (int)TRXREG->RRW * 3, mem, tx, ty, src, len, BITBLTBUF, TRXPOS,
		TRXREG);
}

static void lm_write_image8h(gs_local_memory* mem, int* tx, int* ty, const u8* src, int len,
	const GIFRegBITBLTBUF* BITBLTBUF, const GIFRegTRXPOS* TRXPOS, const GIFRegTRXREG* TRXREG)
{
	if (TRXREG->RRW == 0)
		return;
	lm_write_image_unpacked(1, &gs_swizzle32, (int)TRXREG->RRW, mem, tx, ty, src, len, BITBLTBUF, TRXPOS, TRXREG);
}

/* A 4-bit row is ceil(RRW / 2) bytes; an odd one goes a pixel at a time
 * (lm_write_odd4) */
static void lm_write_image4hl(gs_local_memory* mem, int* tx, int* ty, const u8* src, int len,
	const GIFRegBITBLTBUF* BITBLTBUF, const GIFRegTRXPOS* TRXPOS, const GIFRegTRXREG* TRXREG)
{
	if (TRXREG->RRW == 0)
		return;
	if (TRXREG->RRW & 1)
	{
		lm_write_odd4(lm_write_pixel4hl, &gs_swizzle32, mem, tx, ty, src, len, BITBLTBUF, TRXPOS, TRXREG);
		return;
	}
	lm_write_image_unpacked(2, &gs_swizzle32, (int)(TRXREG->RRW + 1) / 2, mem, tx, ty, src, len, BITBLTBUF,
		TRXPOS, TRXREG);
}

static void lm_write_image4hh(gs_local_memory* mem, int* tx, int* ty, const u8* src, int len,
	const GIFRegBITBLTBUF* BITBLTBUF, const GIFRegTRXPOS* TRXPOS, const GIFRegTRXREG* TRXREG)
{
	if (TRXREG->RRW == 0)
		return;
	if (TRXREG->RRW & 1)
	{
		lm_write_odd4(lm_write_pixel4hh, &gs_swizzle32, mem, tx, ty, src, len, BITBLTBUF, TRXPOS, TRXREG);
		return;
	}
	lm_write_image_unpacked(3, &gs_swizzle32, (int)(TRXREG->RRW + 1) / 2, mem, tx, ty, src, len, BITBLTBUF,
		TRXPOS, TRXREG);
}

/* ---- any format, pixel by pixel ---- */

/* The cursor tx, ty walked over len pixels of rows sx .. sx + w, calling
 * the body for every xinc of them with the row's addresses in pa and the
 * offset x along it. len in pixels, not bytes. */
#define LM_WALK(off, tx, ty, len_px, xinc, sx, w, BODY) \
	do \
	{ \
		int y_ = *(ty); \
		const int ex_ = (sx) + (w); \
		int rem_ = ex_ - *(tx); \
		int left_ = (len_px); \
		gs_pa_helper pa; \
		gs_pa_init(&pa, &(off), *(tx), y_); \
		while (left_ > 0) \
		{ \
			int x; \
			const int stop_ = rem_ < left_ ? rem_ : left_; \
			left_ -= stop_; \
			rem_ -= stop_; \
			for (x = 0; x < stop_; x += (xinc)) \
			{ \
				BODY; \
			} \
			if (rem_ == 0) \
			{ \
				y_++; \
				rem_ = (w); \
				gs_pa_init(&pa, &(off), (sx), y_); \
			} \
		} \
		*(tx) = ex_ - rem_; \
		*(ty) = y_; \
	} while (0)

static void lm_write_image_x(gs_local_memory* mem, int* tx, int* ty, const u8* src, int len,
	const GIFRegBITBLTBUF* BITBLTBUF, const GIFRegTRXPOS* TRXPOS, const GIFRegTRXREG* TRXREG)
{
	const u8* pb = src;
	u8* vm8 = mem->m_vm8;
	u16* vm16 = GS_LM_VM16(mem);
	u32* vm32 = GS_LM_VM32(mem);
	const u32 bp = BITBLTBUF->DBP;
	const u32 bw = BITBLTBUF->DBW;
	const u32 psm = BITBLTBUF->DPSM;
	const int sx = (int)TRXPOS->DSAX;
	const int w = (int)TRXREG->RRW;
	gs_offset off;

	if (len <= 0)
		return;

	off = gs_offset_make(&gs_lm_psm_table[psm].attr.info, bp, bw, psm);
	switch (psm)
	{
		case PSMCT32:
		case PSMZ32:
		{
			const gs_offset o = lm_sized(&off, &gs_swizzle32);
			LM_WALK(o, tx, ty, len / 4, 1, sx, w, {
				memcpy(&vm32[gs_pa_value(&pa, x)], pb, 4);
				pb += 4;
			});
		}
		break;
		case PSMCT24:
		case PSMZ24:
		{
			const gs_offset o = lm_sized(&off, &gs_swizzle32);
			/* the pixel's three bytes, at any alignment, and none past */
			LM_WALK(o, tx, ty, len / 3, 1, sx, w, {
				gs_lm_write24_at(&vm32[gs_pa_value(&pa, x)], (u32)pb[0] | ((u32)pb[1] << 8) | ((u32)pb[2] << 16));
				pb += 3;
			});
		}
		break;
		case PSMCT16:
		case PSMCT16S:
		case PSMZ16:
		case PSMZ16S:
		{
			const gs_offset o = lm_sized(&off, &gs_swizzle16);
			LM_WALK(o, tx, ty, len / 2, 1, sx, w, {
				memcpy(&vm16[gs_pa_value(&pa, x)], pb, 2);
				pb += 2;
			});
		}
		break;
		case PSMT8:
		{
			const gs_offset o = gs_offset_make(&gs_swizzle8, bp, bw, psm);
			LM_WALK(o, tx, ty, len, 1, sx, w, {
				vm8[gs_pa_value(&pa, x)] = *pb;
				pb++;
			});
		}
		break;
		case PSMT4:
		{
			const gs_offset o = gs_offset_make(&gs_swizzle4, bp, bw, psm);
			LM_WALK(o, tx, ty, len * 2, 2, sx, w, {
				gs_lm_write_pixel4(mem, gs_pa_value(&pa, x), *pb & 0xf);
				gs_lm_write_pixel4(mem, gs_pa_value(&pa, x + 1), *pb >> 4);
				pb++;
			});
		}
		break;
		case PSMT8H:
		{
			const gs_offset o = gs_offset_make(&gs_swizzle32, bp, bw, psm);
			LM_WALK(o, tx, ty, len, 1, sx, w, {
				gs_lm_write8h_at(&vm32[gs_pa_value(&pa, x)], *pb);
				pb++;
			});
		}
		break;
		case PSMT4HL:
		{
			const gs_offset o = gs_offset_make(&gs_swizzle32, bp, bw, psm);
			LM_WALK(o, tx, ty, len * 2, 2, sx, w, {
				gs_lm_write4hl_at(&vm32[gs_pa_value(&pa, x)], *pb & 0xf);
				gs_lm_write4hl_at(&vm32[gs_pa_value(&pa, x + 1)], *pb >> 4);
				pb++;
			});
		}
		break;
		case PSMT4HH:
		{
			const gs_offset o = gs_offset_make(&gs_swizzle32, bp, bw, psm);
			LM_WALK(o, tx, ty, len * 2, 2, sx, w, {
				gs_lm_write4hh_at(&vm32[gs_pa_value(&pa, x)], *pb & 0xf);
				gs_lm_write4hh_at(&vm32[gs_pa_value(&pa, x + 1)], *pb >> 4);
				pb++;
			});
		}
		break;
	}
}

/* ---- reading images ---- */

static void lm_read_image_x(const gs_local_memory* mem, int* tx, int* ty, u8* dst, int len,
	const GIFRegBITBLTBUF* BITBLTBUF, const GIFRegTRXPOS* TRXPOS, const GIFRegTRXREG* TRXREG)
{
	u8* pb = dst;
	const u32* vm32 = GS_LM_VM32(mem);
	const u16* vm16 = GS_LM_VM16(mem);
	const u8* vm8 = mem->m_vm8;
	const u32 bp = BITBLTBUF->SBP;
	const u32 bw = BITBLTBUF->SBW;
	const u32 psm = BITBLTBUF->SPSM;
	const int sx = (int)TRXPOS->SSAX;
	const int w = (int)TRXREG->RRW;
	gs_offset off;

	if (len <= 0)
		return;

	off = gs_offset_make(&gs_lm_psm_table[psm].attr.info, bp, bw, psm);
	switch (psm)
	{
		case PSMCT32:
		case PSMZ32:
		{
			/* MGS1's intro, the fade between the plane's outside and in */
			const gs_offset o = lm_sized(&off, &gs_swizzle32);
			int x = *tx;
			int y = *ty;
			const int ex = sx + w;
			gs_pa_helper pa;

			len /= 4;
			gs_pa_init(&pa, &o, 0, y);
			while (len > 0)
			{
				for (; len > 0 && x < ex && (x & 7); len--, x++, pb += 4)
					memcpy(pb, &vm32[gs_pa_value(&pa, x)], 4);

				/* a column at a time: two pixels from each of four rows */
				for (; len >= 8 && x <= ex - 8; len -= 8, x += 8, pb += 32)
				{
					const u32* ps = &vm32[gs_pa_value(&pa, x)];
					memcpy(pb + 0, ps + 0, 8);
					memcpy(pb + 8, ps + 4, 8);
					memcpy(pb + 16, ps + 8, 8);
					memcpy(pb + 24, ps + 12, 8);
				}

				for (; len > 0 && x < ex; len--, x++, pb += 4)
					memcpy(pb, &vm32[gs_pa_value(&pa, x)], 4);

				if (x == ex)
				{
					y++;
					x = sx;
					gs_pa_init(&pa, &o, 0, y);
				}
			}
			*tx = x;
			*ty = y;
		}
		break;
		case PSMCT24:
		case PSMZ24:
		{
			const gs_offset o = lm_sized(&off, &gs_swizzle32);
			LM_WALK(o, tx, ty, len / 3, 1, sx, w, {
				const u32 c = vm32[gs_pa_value(&pa, x)];
				pb[0] = (u8)c;
				pb[1] = (u8)(c >> 8);
				pb[2] = (u8)(c >> 16);
				pb += 3;
			});
		}
		break;
		case PSMCT16:
		case PSMCT16S:
		case PSMZ16:
		case PSMZ16S:
		{
			const gs_offset o = lm_sized(&off, &gs_swizzle16);
			LM_WALK(o, tx, ty, len / 2, 1, sx, w, {
				memcpy(pb, &vm16[gs_pa_value(&pa, x)], 2);
				pb += 2;
			});
		}
		break;
		case PSMT8:
		{
			const gs_offset o = gs_offset_make(&gs_swizzle8, bp, bw, psm);
			LM_WALK(o, tx, ty, len, 1, sx, w, {
				*pb = vm8[gs_pa_value(&pa, x)];
				pb++;
			});
		}
		break;
		case PSMT4:
		{
			const gs_offset o = gs_offset_make(&gs_swizzle4, bp, bw, psm);
			LM_WALK(o, tx, ty, len * 2, 2, sx, w, {
				const u32 lo = gs_lm_read_pixel4(mem, gs_pa_value(&pa, x));
				const u32 hi = gs_lm_read_pixel4(mem, gs_pa_value(&pa, x + 1));
				*pb = (u8)(lo | (hi << 4));
				pb++;
			});
		}
		break;
		case PSMT8H:
		{
			const gs_offset o = gs_offset_make(&gs_swizzle32, bp, bw, psm);
			LM_WALK(o, tx, ty, len, 1, sx, w, {
				*pb = (u8)(vm32[gs_pa_value(&pa, x)] >> 24);
				pb++;
			});
		}
		break;
		case PSMT4HL:
		{
			const gs_offset o = gs_offset_make(&gs_swizzle32, bp, bw, psm);
			LM_WALK(o, tx, ty, len * 2, 2, sx, w, {
				const u32 c0 = vm32[gs_pa_value(&pa, x)] >> 24 & 0x0f;
				const u32 c1 = vm32[gs_pa_value(&pa, x + 1)] >> 20 & 0xf0;
				*pb = (u8)(c0 | c1);
				pb++;
			});
		}
		break;
		case PSMT4HH:
		{
			const gs_offset o = gs_offset_make(&gs_swizzle32, bp, bw, psm);
			LM_WALK(o, tx, ty, len * 2, 2, sx, w, {
				const u32 c0 = vm32[gs_pa_value(&pa, x)] >> 28 & 0x0f;
				const u32 c1 = vm32[gs_pa_value(&pa, x + 1)] >> 24 & 0xf0;
				*pb = (u8)(c0 | c1);
				pb++;
			});
		}
		break;
	}
}

/* ---- reading textures, a block at a time ---- */

/* Every block of r (block aligned), its texels into dst at bpp bits each */
#define LM_FOREACH_BLOCK(off, mem, r, dst, dstpitch, bpp, BODY) \
	do \
	{ \
		gs_bn_helper bn_; \
		const int right_ = (r)[2] >> (off).swz.block_shift_x; \
		const int bottom_ = (r)[3] >> (off).swz.block_shift_y; \
		const int offset_ = (dstpitch) << (off).swz.block_shift_y; \
		const int xadd_ = (1 << (off).swz.block_shift_x) * ((bpp) / 8); \
		u8* row_ = (dst); \
		for (gs_bn_init(&bn_, &(off), (r)[0], (r)[1]); bn_.blk_y < bottom_; gs_bn_next_y(&bn_), row_ += offset_) \
		{ \
			int x_ = 0; \
			for (; bn_.blk_x < right_; gs_bn_next_x(&bn_), x_ += xadd_) \
			{ \
				const u8* src = lm_block_at((mem), gs_bn_value(&bn_)); \
				u8* read_dst = row_ + x_; \
				BODY; \
			} \
		} \
	} while (0)

#define LM_READ_TEXTURE(name, swz, bpp, BODY) \
	static void name(gs_local_memory* mem, const gs_offset* off, const int r[4], u8* dst, int dstpitch, \
		const GIFRegTEXA* TEXA) \
	{ \
		const gs_offset o = lm_sized(off, &(swz)); \
		const u32* pal = mem->m_clut32; \
		(void)pal; \
		(void)TEXA; \
		LM_FOREACH_BLOCK(o, mem, r, dst, dstpitch, bpp, BODY); \
	}

LM_READ_TEXTURE(lm_read_texture32, gs_swizzle32, 32, gs_block_read_block32(src, read_dst, dstpitch))
/* (AEM out of the loop: each expansion with it constant) */
LM_READ_TEXTURE(lm_read_texture24_aem, gs_swizzle32, 32,
	gs_block_read_and_expand_block24(1, src, read_dst, dstpitch, TEXA->TA0))
LM_READ_TEXTURE(lm_read_texture24_noaem, gs_swizzle32, 32,
	gs_block_read_and_expand_block24(0, src, read_dst, dstpitch, TEXA->TA0))
LM_READ_TEXTURE(lm_read_texture16_aem, gs_swizzle16, 32,
	gs_block_read_and_expand_block16(1, src, read_dst, dstpitch, TEXA->TA0, TEXA->TA1))
LM_READ_TEXTURE(lm_read_texture16_noaem, gs_swizzle16, 32,
	gs_block_read_and_expand_block16(0, src, read_dst, dstpitch, TEXA->TA0, TEXA->TA1))
static void lm_read_texture24(gs_local_memory* mem, const gs_offset* off, const int r[4], u8* dst, int dstpitch,
	const GIFRegTEXA* TEXA)
{
	if (TEXA->AEM)
		lm_read_texture24_aem(mem, off, r, dst, dstpitch, TEXA);
	else
		lm_read_texture24_noaem(mem, off, r, dst, dstpitch, TEXA);
}
static void lm_read_texture16(gs_local_memory* mem, const gs_offset* off, const int r[4], u8* dst, int dstpitch,
	const GIFRegTEXA* TEXA)
{
	if (TEXA->AEM)
		lm_read_texture16_aem(mem, off, r, dst, dstpitch, TEXA);
	else
		lm_read_texture16_noaem(mem, off, r, dst, dstpitch, TEXA);
}
LM_READ_TEXTURE(lm_read_texture8, gs_swizzle8, 32, gs_block_read_and_expand_block8_32(src, read_dst, dstpitch, pal))
LM_READ_TEXTURE(lm_read_texture4, gs_swizzle4, 32, gs_block_read_and_expand_block4_32(src, read_dst, dstpitch, pal))
LM_READ_TEXTURE(lm_read_texture8h, gs_swizzle32, 32,
	gs_block_read_and_expand_block8h_32(src, read_dst, dstpitch, pal))
LM_READ_TEXTURE(lm_read_texture4hl, gs_swizzle32, 32,
	gs_block_read_and_expand_block4hl_32(src, read_dst, dstpitch, pal))
LM_READ_TEXTURE(lm_read_texture4hh, gs_swizzle32, 32,
	gs_block_read_and_expand_block4hh_32(src, read_dst, dstpitch, pal))
LM_READ_TEXTURE(lm_read_texture8p, gs_swizzle8, 8, gs_block_read_block8(src, read_dst, dstpitch))
LM_READ_TEXTURE(lm_read_texture4p, gs_swizzle4, 8, gs_block_read_block4p(src, read_dst, dstpitch))
LM_READ_TEXTURE(lm_read_texture8hp, gs_swizzle32, 8, gs_block_read_block8hp(src, read_dst, dstpitch))
LM_READ_TEXTURE(lm_read_texture4hlp, gs_swizzle32, 8, gs_block_read_block4hlp(src, read_dst, dstpitch))
LM_READ_TEXTURE(lm_read_texture4hhp, gs_swizzle32, 8, gs_block_read_block4hhp(src, read_dst, dstpitch))
/* (Haswell and Zen 1-4: a slow vpgatherdd) */
LM_READ_TEXTURE(lm_read_texture8_hsw, gs_swizzle8, 32,
	gs_block_read_and_expand_block8_32hsw(src, read_dst, dstpitch, pal))
LM_READ_TEXTURE(lm_read_texture8h_hsw, gs_swizzle32, 32,
	gs_block_read_and_expand_block8h_32hsw(src, read_dst, dstpitch, pal))

/* The display's 24-bit packed RGB (PSGPU24), widened to 32-bit texels */
static void lm_read_texture_gpu24(gs_local_memory* mem, const gs_offset* off, const int r[4], u8* dst, int dstpitch,
	const GIFRegTEXA* TEXA)
{
	const gs_offset o = lm_sized(off, &gs_swizzle16);
	int x, y;
	(void)TEXA;

	LM_FOREACH_BLOCK(o, mem, r, dst, dstpitch, 16, gs_block_read_block16(src, read_dst, dstpitch));

	for (y = r[1]; y < r[3]; y++)
	{
		u8* line = dst + y * dstpitch;
		for (x = r[2]; x >= r[0]; x--)
		{
			u32 c;
			memcpy(&c, &line[x * 3], 4);
			c &= 0xFFFFFF;
			memcpy(&line[x * 4], &c, 4);
		}
	}
}

#define LM_READ_BLOCK(name, BODY) \
	static void name(const gs_local_memory* mem, u32 bp, u8* dst, int dstpitch, const GIFRegTEXA* TEXA) \
	{ \
		const u8* src = lm_block_at(mem, bp); \
		const u32* pal = mem->m_clut32; \
		(void)pal; \
		(void)TEXA; \
		BODY; \
	}

LM_READ_BLOCK(lm_read_block32, gs_block_read_block32(src, dst, dstpitch))
LM_READ_BLOCK(lm_read_block24, if (TEXA->AEM) gs_block_read_and_expand_block24(1, src, dst, dstpitch, TEXA->TA0);
	else gs_block_read_and_expand_block24(0, src, dst, dstpitch, TEXA->TA0))
LM_READ_BLOCK(lm_read_block16, if (TEXA->AEM) gs_block_read_and_expand_block16(1, src, dst, dstpitch, TEXA->TA0, TEXA->TA1);
	else gs_block_read_and_expand_block16(0, src, dst, dstpitch, TEXA->TA0, TEXA->TA1))
LM_READ_BLOCK(lm_read_block8, gs_block_read_and_expand_block8_32(src, dst, dstpitch, pal))
LM_READ_BLOCK(lm_read_block4, gs_block_read_and_expand_block4_32(src, dst, dstpitch, pal))
LM_READ_BLOCK(lm_read_block8h, gs_block_read_and_expand_block8h_32(src, dst, dstpitch, pal))
LM_READ_BLOCK(lm_read_block4hl, gs_block_read_and_expand_block4hl_32(src, dst, dstpitch, pal))
LM_READ_BLOCK(lm_read_block4hh, gs_block_read_and_expand_block4hh_32(src, dst, dstpitch, pal))
LM_READ_BLOCK(lm_read_block8p, gs_block_read_block8(src, dst, dstpitch))
LM_READ_BLOCK(lm_read_block4p, gs_block_read_block4p(src, dst, dstpitch))
LM_READ_BLOCK(lm_read_block8hp, gs_block_read_block8hp(src, dst, dstpitch))
LM_READ_BLOCK(lm_read_block4hlp, gs_block_read_block4hlp(src, dst, dstpitch))
LM_READ_BLOCK(lm_read_block4hhp, gs_block_read_block4hhp(src, dst, dstpitch))
LM_READ_BLOCK(lm_read_block8_hsw, gs_block_read_and_expand_block8_32hsw(src, dst, dstpitch, pal))
LM_READ_BLOCK(lm_read_block8h_hsw, gs_block_read_and_expand_block8h_32hsw(src, dst, dstpitch, pal))

/* ---- the table ---- */

void LM_POPULATE(void)
{
	gs_lm_psm* t = gs_lm_psm_table;
	int i;

	gs_lm_read_image_x = lm_read_image_x;

	for (i = 0; i < 64; i++)
	{
		t[i].wi    = lm_write_image32;
		t[i].ri    = lm_read_image_x;
		t[i].rtx   = lm_read_texture32;
		t[i].rtxP  = lm_read_texture32;
		t[i].rtxb  = lm_read_block32;
		t[i].rtxbP = lm_read_block32;
	}

	t[PSMCT24].wi  = lm_write_image24;
	t[PSMCT16].wi  = lm_write_image16;
	t[PSMCT16S].wi = lm_write_image16s;
	t[PSMT8].wi    = lm_write_image8;
	t[PSMT4].wi    = lm_write_image4;
	t[PSMT8H].wi   = lm_write_image8h;
	t[PSMT4HL].wi  = lm_write_image4hl;
	t[PSMT4HH].wi  = lm_write_image4hh;
	t[PSMZ32].wi   = lm_write_image32z;
	t[PSMZ24].wi   = lm_write_image24z;
	t[PSMZ16].wi   = lm_write_image16z;
	t[PSMZ16S].wi  = lm_write_image16sz;

	t[PSMCT24].rtx  = lm_read_texture24;
	t[PSGPU24].rtx  = lm_read_texture_gpu24;
	t[PSMCT16].rtx  = lm_read_texture16;
	t[PSMCT16S].rtx = lm_read_texture16;
	t[PSMT8].rtx    = lm_read_texture8;
	t[PSMT4].rtx    = lm_read_texture4;
	t[PSMT8H].rtx   = lm_read_texture8h;
	t[PSMT4HL].rtx  = lm_read_texture4hl;
	t[PSMT4HH].rtx  = lm_read_texture4hh;
	t[PSMZ32].rtx   = lm_read_texture32;
	t[PSMZ24].rtx   = lm_read_texture24;
	t[PSMZ16].rtx   = lm_read_texture16;
	t[PSMZ16S].rtx  = lm_read_texture16;

	t[PSMCT24].rtxP  = lm_read_texture24;
	t[PSMCT16].rtxP  = lm_read_texture16;
	t[PSMCT16S].rtxP = lm_read_texture16;
	t[PSMT8].rtxP    = lm_read_texture8p;
	t[PSMT4].rtxP    = lm_read_texture4p;
	t[PSMT8H].rtxP   = lm_read_texture8hp;
	t[PSMT4HL].rtxP  = lm_read_texture4hlp;
	t[PSMT4HH].rtxP  = lm_read_texture4hhp;
	t[PSMZ32].rtxP   = lm_read_texture32;
	t[PSMZ24].rtxP   = lm_read_texture24;
	t[PSMZ16].rtxP   = lm_read_texture16;
	t[PSMZ16S].rtxP  = lm_read_texture16;

	t[PSMCT24].rtxb  = lm_read_block24;
	t[PSMCT16].rtxb  = lm_read_block16;
	t[PSMCT16S].rtxb = lm_read_block16;
	t[PSMT8].rtxb    = lm_read_block8;
	t[PSMT4].rtxb    = lm_read_block4;
	t[PSMT8H].rtxb   = lm_read_block8h;
	t[PSMT4HL].rtxb  = lm_read_block4hl;
	t[PSMT4HH].rtxb  = lm_read_block4hh;
	t[PSMZ32].rtxb   = lm_read_block32;
	t[PSMZ24].rtxb   = lm_read_block24;
	t[PSMZ16].rtxb   = lm_read_block16;
	t[PSMZ16S].rtxb  = lm_read_block16;

	t[PSMCT24].rtxbP  = lm_read_block24;
	t[PSMCT16].rtxbP  = lm_read_block16;
	t[PSMCT16S].rtxbP = lm_read_block16;
	t[PSMT8].rtxbP    = lm_read_block8p;
	t[PSMT4].rtxbP    = lm_read_block4p;
	t[PSMT8H].rtxbP   = lm_read_block8hp;
	t[PSMT4HL].rtxbP  = lm_read_block4hlp;
	t[PSMT4HH].rtxbP  = lm_read_block4hhp;
	t[PSMZ32].rtxbP   = lm_read_block32;
	t[PSMZ24].rtxbP   = lm_read_block24;
	t[PSMZ16].rtxbP   = lm_read_block16;
	t[PSMZ16S].rtxbP  = lm_read_block16;

	if (CPU_HAS_AVX2)
	{
		t[PSMT8].rtx   = lm_read_texture8_hsw;
		t[PSMT8H].rtx  = lm_read_texture8h_hsw;
		t[PSMT8].rtxb  = lm_read_block8_hsw;
		t[PSMT8H].rtxb = lm_read_block8h_hsw;
	}
}

/* What one build holds for all: the first tier's, or the native build's */
#if !defined(MULTI_ISA_UNSHARED_COMPILATION) || MULTI_ISA_IS_FIRST

PCSX2_ALIGN(64) gs_lm_psm gs_lm_psm_table[64];
gs_lm_read_image_fn gs_lm_read_image_x;

void gs_local_memory_populate_transfers(void)
{
	int i;
	for (i = 0; i < 64; i++)
		gs_lm_psm_table[i].attr = gs_psm[i];
#ifdef MULTI_ISA_UNSHARED_COMPILATION
	if (CPU_HAS_AVX2)
		gs_lm_populate_isa_avx2();
	else if (CPU_HAS_AVX)
		gs_lm_populate_isa_avx();
	else
		gs_lm_populate_isa_sse4();
#else
	gs_lm_populate_isa_native();
#endif
}

void gs_local_memory_read_texture_blocks(gs_local_memory* mem, const gs_offset* off, const int r[4], u8* dst,
	int dstpitch, const GIFRegTEXA* TEXA)
{
	gs_lm_psm_table[off->psm].rtx(mem, off, r, dst, dstpitch, TEXA);
}

#endif
