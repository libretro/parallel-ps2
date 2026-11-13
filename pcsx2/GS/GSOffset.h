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

#ifndef GS_OFFSET_H
#define GS_OFFSET_H

/* Where a pixel or block of a buffer lives in local memory, for each
 * swizzle: the block number of (x, y), its pixel address, and the pages a
 * rectangle touches. */

#include "GSRegs.h"
#include "GSTables.h"

/* Its own, not INLINE: parallel-gs redefines that */
#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L)
#define GS_OFS_INLINE inline
#elif defined(_MSC_VER)
#define GS_OFS_INLINE __inline
#elif defined(__GNUC__)
#define GS_OFS_INLINE __inline__
#else
#define GS_OFS_INLINE
#endif

/* A swizzle: its tables and the page and block shape they describe */
typedef struct gs_swizzle_info
{
	const GSBlockSwizzleTable* block_swizzle;              /* blocks within a page */
	const u64* block_below;                                /* gs_block_below* */
	const int* pixel_swizzle_col;                          /* x = 0 for each y in a page */
	const GSPixelRowOffsetTable* const* pixel_swizzle_row; /* each x, by y & row mask */
	int page_mask_x, page_mask_y;   /* page dimensions - 1 */
	int block_mask_x, block_mask_y; /* block dimensions - 1 */
	int pixel_row_mask;             /* y & this picks the row table */
	u8 page_shift_x, page_shift_y;   /* x, y >> this is the page */
	u8 block_shift_x, block_shift_y; /* x, y >> this is the block */
	u32 block_address_xor; /* xored into the block number (Z swizzles) */
	u32 pixel_address_xor; /* xored into the pixel address */
} gs_swizzle_info;

#define GS_ILOG2(n) ((n) >= 128 ? 7 : (n) >= 64 ? 6 : (n) >= 32 ? 5 : (n) >= 16 ? 4 : \
	(n) >= 8 ? 3 : (n) >= 4 ? 2 : (n) >= 2 ? 1 : 0)
#define GS_SWIZZLE_INFO(bits, pw, ph, bwide, bhigh, rowmask, bxor) \
	{&gs_block_table##bits, gs_block_below##bits, gs_pixel_col_offset##bits, gs_pixel_row_list##bits, \
		(pw) - 1, (ph) - 1, (pw) / (bwide) - 1, (ph) / (bhigh) - 1, rowmask, \
		GS_ILOG2(pw), GS_ILOG2(ph), GS_ILOG2((pw) / (bwide)), GS_ILOG2((ph) / (bhigh)), \
		bxor, (u32)(bxor) << (GS_ILOG2((pw) / (bwide)) + GS_ILOG2((ph) / (bhigh)))}

/* Each TU has its own copy, so the shapes fold into the code that uses
 * them; only the tables' addresses are taken */
#ifdef __cplusplus
#define GS_SWIZZLE_CONST static constexpr
#else
#define GS_SWIZZLE_CONST static const
#endif
GS_SWIZZLE_CONST gs_swizzle_info gs_swizzle32   = GS_SWIZZLE_INFO(32,  64,  32, 8, 4, 0, 0x00);
GS_SWIZZLE_CONST gs_swizzle_info gs_swizzle32Z  = GS_SWIZZLE_INFO(32,  64,  32, 8, 4, 0, 0x18);
GS_SWIZZLE_CONST gs_swizzle_info gs_swizzle16   = GS_SWIZZLE_INFO(16,  64,  64, 4, 8, 0, 0x00);
GS_SWIZZLE_CONST gs_swizzle_info gs_swizzle16S  = GS_SWIZZLE_INFO(16S, 64,  64, 4, 8, 0, 0x00);
GS_SWIZZLE_CONST gs_swizzle_info gs_swizzle16Z  = GS_SWIZZLE_INFO(16,  64,  64, 4, 8, 0, 0x18);
GS_SWIZZLE_CONST gs_swizzle_info gs_swizzle16SZ = GS_SWIZZLE_INFO(16S, 64,  64, 4, 8, 0, 0x18);
GS_SWIZZLE_CONST gs_swizzle_info gs_swizzle8    = GS_SWIZZLE_INFO(8,  128,  64, 8, 4, 7, 0x00);
GS_SWIZZLE_CONST gs_swizzle_info gs_swizzle4    = GS_SWIZZLE_INFO(4,  128, 128, 4, 8, 7, 0x00);
#undef GS_SWIZZLE_CONST

/* A buffer: a swizzle at a base pointer and width */
typedef struct gs_offset
{
	gs_swizzle_info swz;
	int bp;    /* base pointer, in blocks */
	int bw_pg; /* width in pages (not the GS's bw for 8 and 4 bits) */
	int psm;   /* kept for the caller; nothing here reads it */
} gs_offset;

static GS_OFS_INLINE gs_offset gs_offset_make(const gs_swizzle_info* swz, u32 bp, u32 bw, u32 psm)
{
	gs_offset o;
	o.swz   = *swz;
	o.bp    = (int)bp;
	o.bw_pg = (int)(bw >> (swz->page_shift_x - 6));
	o.psm   = (int)psm;
	return o;
}

static GS_OFS_INLINE u32 gs_offset_bw(const gs_offset* o)
{
	return (u32)o->bw_pg << (o->swz.page_shift_x - 6);
}

/* Block numbers along a scan, x then y */
typedef struct gs_bn_helper
{
	const GSBlockSwizzleTable* block_swizzle;
	int base_bp;    /* bp at the start of this row */
	int bp;         /* bp here */
	int base_blk_x; /* the origin's x, in blocks */
	int blk_x, blk_y;
	int page_mask_x, page_mask_y; /* in blocks, to see a page crossed */
	int add_y;      /* bp of one page down */
	u32 bxor;
} gs_bn_helper;

static GS_OFS_INLINE void gs_bn_init(gs_bn_helper* h, const gs_offset* o, int x, int y)
{
	int y_amt = ((y >> (o->swz.page_shift_y - 5)) & ~0x1f) * o->bw_pg;
	int x_amt = ((x >> (o->swz.page_shift_x - 5)) & ~0x1f);
	h->block_swizzle = o->swz.block_swizzle;
	h->base_bp     = h->bp = o->bp + y_amt + x_amt;
	h->base_blk_x  = h->blk_x = x >> o->swz.block_shift_x;
	h->blk_y       = y >> o->swz.block_shift_y;
	h->page_mask_x = (1 << (o->swz.page_shift_x - o->swz.block_shift_x)) - 1;
	h->page_mask_y = (1 << (o->swz.page_shift_y - o->swz.block_shift_y)) - 1;
	h->add_y       = 32 * o->bw_pg;
	h->bxor        = o->swz.block_address_xor;
}

static GS_OFS_INLINE void gs_bn_next_x(gs_bn_helper* h)
{
	h->blk_x++;
	if (!(h->blk_x & h->page_mask_x))
		h->bp += 32;
}

/* One block down, back to the origin's x */
static GS_OFS_INLINE void gs_bn_next_y(gs_bn_helper* h)
{
	h->blk_y++;
	if (!(h->blk_y & h->page_mask_y))
		h->base_bp += h->add_y;
	h->blk_x = h->base_blk_x;
	h->bp    = h->base_bp;
}

/* The block number here, not wrapped at GS_MAX_BLOCKS */
static GS_OFS_INLINE u32 gs_bn_value_nowrap(const gs_bn_helper* h)
{
	return ((u32)h->bp + h->block_swizzle->value[h->blk_y & 7][h->blk_x & 7]) ^ h->bxor;
}

static GS_OFS_INLINE u32 gs_bn_value(const gs_bn_helper* h)
{
	return gs_bn_value_nowrap(h) % GS_MAX_BLOCKS;
}

static GS_OFS_INLINE u32 gs_offset_bn(const gs_offset* o, int x, int y)
{
	gs_bn_helper h;
	gs_bn_init(&h, o, x, y);
	return gs_bn_value(&h);
}

static GS_OFS_INLINE u32 gs_offset_bn_nowrap(const gs_offset* o, int x, int y)
{
	gs_bn_helper h;
	gs_bn_init(&h, o, x, y);
	return gs_bn_value_nowrap(&h);
}

/* The pixel address of (0, y), before the final xor */
static GS_OFS_INLINE int gs_offset_pa_zero_x(const gs_offset* o, int y)
{
	const gs_swizzle_info* s = &o->swz;
	int base = o->bp << (s->page_shift_x + s->page_shift_y - 5);
	base += ((y & ~s->page_mask_y) * o->bw_pg) << s->page_shift_x;
	base &= (GS_MAX_PAGES << (s->page_shift_x + s->page_shift_y)) - 1;
	base += s->pixel_swizzle_col[y & s->page_mask_y];
	return base;
}

/* Pixel addresses along one row */
typedef struct gs_pa_helper
{
	const int* row; /* from the helper's x */
	int base;
	u32 pxor;
} gs_pa_helper;

static GS_OFS_INLINE void gs_pa_init(gs_pa_helper* h, const gs_offset* o, int x, int y)
{
	h->row  = o->swz.pixel_swizzle_row[y & o->swz.pixel_row_mask]->value + x;
	h->base = gs_offset_pa_zero_x(o, y);
	h->pxor = o->swz.pixel_address_xor;
}

/* The address x past the helper's own */
static GS_OFS_INLINE u32 gs_pa_value(const gs_pa_helper* h, int x)
{
	return (u32)(h->base + h->row[x]) ^ h->pxor;
}

static GS_OFS_INLINE u32 gs_offset_pa(const gs_offset* o, int x, int y)
{
	gs_pa_helper h;
	gs_pa_init(&h, o, 0, y);
	return gs_pa_value(&h, x);
}

/* Whether the rectangle's edges are on mask + 1 boundaries, with room */
static GS_OFS_INLINE int gs_rect_is_aligned(int l, int t, int r, int b, int mask_x, int mask_y)
{
	return r - l > mask_x && b - t > mask_y && !(l & mask_x) && !(t & mask_y)
		&& !(r & mask_x) && !(b & mask_y);
}

/* The pages a rectangle touches, row by row of pages. Rows other than the
 * first and last cover the same span; a rectangle that can wrap local
 * memory onto itself takes the slow path, which skips pages seen. */
typedef struct gs_page_looper
{
	int first_start, first_end; /* the first row's pages, from bp's x = 0 */
	int mid_start, mid_end;     /* the rows between */
	int last_start, last_end;   /* the last row */
	int bp;      /* page of x = 0 on the first row */
	int y_inc;   /* pages one row down */
	int y_cnt;   /* rows */
	int slow;
} gs_page_looper;

/* Walks a looper's rows of pages: each row's [start, end), before taking
 * pages modulo GS_MAX_PAGES. Off the slow path a row starts no earlier
 * than the last ended, so no page comes twice; on it, the caller skips
 * pages it has seen. */
typedef struct gs_page_rows
{
	const gs_page_looper* l;
	int y, line_bp, start_off, end_off;
	u32 next_min;
} gs_page_rows;

static GS_OFS_INLINE void gs_page_rows_init(gs_page_rows* it, const gs_page_looper* l)
{
	it->l         = l;
	it->y         = 0;
	it->line_bp   = l->bp;
	it->start_off = l->first_start;
	it->end_off   = l->first_end;
	it->next_min  = 0;
}

/* The next row into *start and *end, or 0 when there are none */
static GS_OFS_INLINE int gs_page_rows_next(gs_page_rows* it, u32* start, u32* end)
{
	const gs_page_looper* l = it->l;
	u32 s;
	if (it->y >= l->y_cnt)
		return 0;
	s = (u32)(it->line_bp + it->start_off);
	if (!l->slow && s < it->next_min)
		s = it->next_min;
	*start       = s;
	*end         = (u32)(it->line_bp + it->end_off);
	it->next_min = *end;
	it->line_bp += l->y_inc;
	/* The next row is the last, or one between */
	if (it->y < l->y_cnt - 2)
	{
		it->start_off = l->mid_start;
		it->end_off   = l->mid_end;
	}
	else
	{
		it->start_off = l->last_start;
		it->end_off   = l->last_end;
	}
	it->y++;
	return 1;
}

#ifdef __cplusplus
extern "C" {
#endif

void gs_offset_page_looper(gs_page_looper* out, const gs_offset* o, int l, int t, int r, int b);

#ifdef __cplusplus
}
#endif

#endif
