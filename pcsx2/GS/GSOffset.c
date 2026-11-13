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

#include "GSOffset.h"

/* The 8x8 cells of x1..x2, y1..y2 (in blocks), each taken & 7 as the
 * block tables are */
static GS_OFS_INLINE u64 gs_cells(int x1, int x2, int y1, int y2)
{
	u64 cols, rows;
	if (x2 <= x1 || y2 <= y1)
		return 0;
	cols = x2 - x1 >= 8 ? 0xff : (1u << (x2 - x1)) - 1;
	rows = y2 - y1 >= 8 ? 0xff : (1u << (y2 - y1)) - 1;
	cols = ((cols << (x1 & 7)) | (cols >> (8 - (x1 & 7)))) & 0xff;
	rows = ((rows << (y1 & 7)) | (rows >> (8 - (y1 & 7)))) & 0xff;
	/* row bit i to bit i * 8, then a copy of the columns in each */
	rows = (rows | (rows << 28)) & 0x0000000F0000000FULL;
	rows = (rows | (rows << 14)) & 0x0003000300030003ULL;
	rows = (rows | (rows << 7))  & 0x0101010101010101ULL;
	return rows * cols;
}

/* An inner rectangle inside out stays in one page; with none, so does an
 * outer one on one side of the page line */
static GS_OFS_INLINE int gs_stays_within_one_page(int o1, int o2, int i1, int i2)
{
	if (i2 < i1)
		return 1;
	if (i2 == i1)
		return o1 == i1 || o2 == i1;
	return 0;
}

/* Block-aligned outer and page-aligned inner rectangles, in blocks, and
 * what a row's ends are tested against */
typedef struct gs_aligned
{
	int ox1, oy1, ox2, oy2;
	int ix1, iy1, ix2, iy2;
	u64 below; /* cells whose block is on the lower page */
	int one_page_x;
	int blk_w;
} gs_aligned;

/* Drops the row's first page if no block of it is used, and its last */
static GS_OFS_INLINE void gs_adjust_start_end(const gs_aligned* a, int* start, int* end, int y1, int y2)
{
	int s1, s2, e1, e2;
	if (a->one_page_x)
	{
		s1 = e1 = a->ox1;
		s2 = e2 = a->ox2;
	}
	else
	{
		s1 = (a->ox1 == a->ix1) ? 0        : a->ox1;
		s2 = (a->ox1 == a->ix1) ? a->blk_w : a->ix1;
		e1 = (a->ox2 == a->ix2) ? 0        : a->ix2;
		e2 = (a->ox2 == a->ix2) ? a->blk_w : a->ox2;
	}
	if (!(gs_cells(s1, s2, y1, y2) & a->below))
		(*start)++;
	if (!(gs_cells(e1, e2, y1, y2) & ~a->below))
		(*end)--;
}

/* Each page-sized tile of a rectangle touches the page under it and, when
 * bp is not page aligned, the next one too. The span of pages a row can
 * touch is narrowed where its first tile misses its lower page or its last
 * misses its upper; the first and last rows of pages are worked out apart
 * from the middle ones, which cover whole pages in y. */
void gs_offset_page_looper(gs_page_looper* out, const gs_offset* o, int l, int t, int r, int b)
{
	/* Read once: the stores to *out could alias them */
	const int psx = o->swz.page_shift_x, psy = o->swz.page_shift_y;
	const int bsx = o->swz.block_shift_x, bsy = o->swz.block_shift_y;
	const int pmx = o->swz.page_mask_x, pmy = o->swz.page_mask_y;
	const int bmx = o->swz.block_mask_x, bmy = o->swz.block_mask_y;
	const int bp = o->bp, bw_pg = o->bw_pg;
	const int top_pg    = t >> psy;
	const int bot_pg    = (b + pmy) >> psy;
	const int block_off = bp & 0x1f;
	const int aligned   = block_off == 0;
	const int x_start   = l >> psx;
	const int x_end     = ((r + pmx) >> psx) + !aligned;
	int fs = x_start, fe = x_end, ms = x_start, me = x_end, ls = x_start, le = x_end;
	gs_aligned a;

	out->bp    = (bp >> 5) + top_pg * bw_pg;
	out->y_inc = bw_pg;
	out->y_cnt = bot_pg - top_pg;
	out->slow  = (u32)((bot_pg - top_pg) * bw_pg + x_end - x_start) > GS_MAX_PAGES;

	/* With bp page aligned, each tile touches its own page and no other */
	if (!aligned)
	{
		a.ox1 = (l & ~bmx) >> bsx;
		a.oy1 = (t & ~bmy) >> bsy;
		a.ox2 = ((r + bmx) & ~bmx) >> bsx;
		a.oy2 = ((b + bmy) & ~bmy) >> bsy;
		a.ix1 = (((l & ~bmx) + pmx) & ~pmx) >> bsx;
		a.iy1 = (((t & ~bmy) + pmy) & ~pmy) >> bsy;
		a.ix2 = ((r + bmx) & ~bmx & ~pmx) >> bsx;
		a.iy2 = ((b + bmy) & ~bmy & ~pmy) >> bsy;
		a.below         = o->swz.block_below[32 - block_off];
		a.blk_w         = 1 << (psx - bsx);
		a.one_page_x    = gs_stays_within_one_page(a.ox1, a.ox2, a.ix1, a.ix2);

		/* Within one page in y, only the first row is read */
		if (gs_stays_within_one_page(a.oy1, a.oy2, a.iy1, a.iy2))
			gs_adjust_start_end(&a, &fs, &fe, a.oy1, a.oy2);
		else
		{
			gs_adjust_start_end(&a, &ms, &me, 0, 1 << (psy - bsy));
			if (a.oy1 != a.iy1)
				gs_adjust_start_end(&a, &fs, &fe, a.oy1, a.iy1);
			else
			{
				fs = ms;
				fe = me;
			}
			if (a.oy2 != a.iy2)
				gs_adjust_start_end(&a, &ls, &le, a.iy2, a.oy2);
			else
			{
				ls = ms;
				le = me;
			}
		}
	}
	out->first_start = fs;
	out->first_end   = fe;
	out->mid_start   = ms;
	out->mid_end     = me;
	out->last_start  = ls;
	out->last_end    = le;
}
