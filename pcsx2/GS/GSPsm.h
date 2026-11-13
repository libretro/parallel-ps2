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

#ifndef GS_PSM_H
#define GS_PSM_H

/* What each of the 64 pixel storage modes is: its swizzle, bits per
 * pixel, block and page size, and the bits of a 32-bit word it covers;
 * and the block range a rectangle of a buffer spans. */

#include "GSOffset.h"

#ifndef GS_INT2_DEFINED
#define GS_INT2_DEFINED
typedef struct gs_int2
{
	int x, y;
} gs_int2;
#endif

enum GS_PSM_FMT
{
	GS_PSM_FMT_32 = 0,
	GS_PSM_FMT_24 = 1,
	GS_PSM_FMT_16 = 2,
	GS_PSM_FMT_NONE = 3 /* indexed */
};

typedef struct gs_psm_attr
{
	gs_swizzle_info info;
	u16 bpp;   /* bits a pixel takes in memory */
	u16 trbpp; /* bits a pixel takes in a transfer */
	u16 pal;   /* palette entries, 0 when not indexed */
	u16 fmt;   /* GS_PSM_FMT_* */
	gs_int2 bs;  /* block size in pixels */
	gs_int2 pgs; /* page size in pixels */
	u8 msk;      /* the bits of the top byte it writes (0xff for all) */
	u8 depth;    /* a Z format */
	u32 fmsk;    /* the bits of a 32-bit pixel it covers */
} gs_psm_attr;

#ifdef __cplusplus
extern "C" {
#endif

extern gs_psm_attr gs_psm[64];

/* Fills gs_psm; called by gs_util_init() */
void gs_psm_init(void);

/* The block holding the rectangle's first pixel; page aligned, the page's
 * first block, which Z formats do not keep at the top left */
u32 gs_psm_start_block(u32 bp, u32 bw, u32 psm, int l, int t);
/* The block holding its last pixel; page aligned, the page's last block */
u32 gs_psm_end_block(u32 bp, u32 bw, u32 psm, int r, int b);
/* The same, past GS_MAX_BLOCKS where it wrapped below bp */
u32 gs_psm_end_block_unwrapped(u32 bp, u32 bw, u32 psm, int r, int b);
/* Whether two rectangles share a page, local memory wrapping around */
int gs_psm_has_overlap(u32 src_bp, u32 src_bw, u32 src_psm, const int* src_rect,
	u32 dst_bp, u32 dst_bw, u32 dst_psm, const int* dst_rect);

#ifdef __cplusplus
}
#endif

#endif
