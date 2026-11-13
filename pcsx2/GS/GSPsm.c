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

#include "GSPsm.h"

gs_psm_attr gs_psm[64];

static void gs_psm_set2(gs_int2* v, int x, int y)
{
	v->x = x;
	v->y = y;
}

void gs_psm_init(void)
{
	int i;

	/* Unlisted modes read as PSMCT32 */
	for (i = 0; i < 64; i++)
	{
		gs_psm_attr* p = &gs_psm[i];
		p->info  = gs_swizzle32;
		p->bpp   = p->trbpp = 32;
		p->pal   = 0;
		p->fmt   = GS_PSM_FMT_NONE;
		gs_psm_set2(&p->bs, 8, 8);
		gs_psm_set2(&p->pgs, 64, 32);
		p->msk   = 0xff;
		p->depth = 0;
		p->fmsk  = 0xffffffff;
	}

	gs_psm[PSGPU24].info  = gs_swizzle16;
	gs_psm[PSMCT16].info  = gs_swizzle16;
	gs_psm[PSMCT16S].info = gs_swizzle16S;
	gs_psm[PSMT8].info    = gs_swizzle8;
	gs_psm[PSMT4].info    = gs_swizzle4;
	gs_psm[PSMZ32].info   = gs_swizzle32Z;
	gs_psm[PSMZ24].info   = gs_swizzle32Z;
	gs_psm[PSMZ16].info   = gs_swizzle16Z;
	gs_psm[PSMZ16S].info  = gs_swizzle16SZ;

	gs_psm[PSGPU24].bpp = 16;
	gs_psm[PSMCT16].bpp = gs_psm[PSMCT16S].bpp = 16;
	gs_psm[PSMT8].bpp   = 8;
	gs_psm[PSMT4].bpp   = 4;
	gs_psm[PSMZ16].bpp  = gs_psm[PSMZ16S].bpp = 16;

	gs_psm[PSMCT24].trbpp = 24;
	gs_psm[PSGPU24].trbpp = 16;
	gs_psm[PSMCT16].trbpp = gs_psm[PSMCT16S].trbpp = 16;
	gs_psm[PSMT8].trbpp   = gs_psm[PSMT8H].trbpp = 8;
	gs_psm[PSMT4].trbpp   = gs_psm[PSMT4HL].trbpp = gs_psm[PSMT4HH].trbpp = 4;
	gs_psm[PSMZ24].trbpp  = 24;
	gs_psm[PSMZ16].trbpp  = gs_psm[PSMZ16S].trbpp = 16;

	gs_psm[PSMT8].pal = gs_psm[PSMT8H].pal = 256;
	gs_psm[PSMT4].pal = gs_psm[PSMT4HL].pal = gs_psm[PSMT4HH].pal = 16;

	gs_psm[PSMCT32].fmt  = gs_psm[PSMZ32].fmt  = GS_PSM_FMT_32;
	gs_psm[PSMCT24].fmt  = gs_psm[PSMZ24].fmt  = GS_PSM_FMT_24;
	gs_psm[PSMCT16].fmt  = gs_psm[PSMZ16].fmt  = GS_PSM_FMT_16;
	gs_psm[PSMCT16S].fmt = gs_psm[PSMZ16S].fmt = GS_PSM_FMT_16;

	gs_psm_set2(&gs_psm[PSGPU24].bs, 16, 8);
	gs_psm_set2(&gs_psm[PSMCT16].bs, 16, 8);
	gs_psm_set2(&gs_psm[PSMCT16S].bs, 16, 8);
	gs_psm_set2(&gs_psm[PSMT8].bs, 16, 16);
	gs_psm_set2(&gs_psm[PSMT4].bs, 32, 16);
	gs_psm_set2(&gs_psm[PSMZ16].bs, 16, 8);
	gs_psm_set2(&gs_psm[PSMZ16S].bs, 16, 8);

	gs_psm_set2(&gs_psm[PSGPU24].pgs, 64, 64);
	gs_psm_set2(&gs_psm[PSMCT16].pgs, 64, 64);
	gs_psm_set2(&gs_psm[PSMCT16S].pgs, 64, 64);
	gs_psm_set2(&gs_psm[PSMT8].pgs, 128, 64);
	gs_psm_set2(&gs_psm[PSMT4].pgs, 128, 128);
	gs_psm_set2(&gs_psm[PSMZ16].pgs, 64, 64);
	gs_psm_set2(&gs_psm[PSMZ16S].pgs, 64, 64);

	gs_psm[PSMCT24].msk = 0x3f;
	gs_psm[PSMZ24].msk  = 0x3f;
	gs_psm[PSMT8H].msk  = 0xc0;
	gs_psm[PSMT4HL].msk = 0x40;
	gs_psm[PSMT4HH].msk = 0x80;

	gs_psm[PSMZ32].depth  = 1;
	gs_psm[PSMZ24].depth  = 1;
	gs_psm[PSMZ16].depth  = 1;
	gs_psm[PSMZ16S].depth = 1;

	gs_psm[PSMCT24].fmsk  = 0x00FFFFFF;
	gs_psm[PSGPU24].fmsk  = 0x00FFFFFF;
	gs_psm[PSMCT16].fmsk  = 0x80F8F8F8;
	gs_psm[PSMCT16S].fmsk = 0x80F8F8F8;
	gs_psm[PSMT8H].fmsk   = 0xFF000000;
	gs_psm[PSMT4HL].fmsk  = 0x0F000000;
	gs_psm[PSMT4HH].fmsk  = 0xF0000000;
	gs_psm[PSMZ24].fmsk   = 0x00FFFFFF;
	gs_psm[PSMZ16].fmsk   = 0x80F8F8F8;
	gs_psm[PSMZ16S].fmsk  = 0x80F8F8F8;
}

/* Block numbers here are right for colour formats only */
static u32 gs_psm_bn(u32 bp, u32 bw, u32 psm, int x, int y)
{
	gs_offset o = gs_offset_make(&gs_psm[psm].info, bp, bw, 0);
	return gs_offset_bn(&o, x, y);
}

u32 gs_psm_start_block(u32 bp, u32 bw, u32 psm, int l, int t)
{
	const gs_int2 pgs = gs_psm[psm].pgs;
	u32 result = gs_psm_bn(bp, bw, psm, l, t);
	if ((l & (pgs.x - 1)) == 0 && (t & (pgs.y - 1)) == 0)
		result &= ~(u32)(GS_BLOCKS_PER_PAGE - 1);
	return result;
}

u32 gs_psm_end_block(u32 bp, u32 bw, u32 psm, int r, int b)
{
	const gs_int2 pgs = gs_psm[psm].pgs;
	u32 result = gs_psm_bn(bp, bw, psm, r - 1, b - 1);
	/* Z formats' real last block is lower */
	if ((r & (pgs.x - 1)) == 0 && (b & (pgs.y - 1)) == 0)
		result = ((result + GS_BLOCKS_PER_PAGE - 1) & ~(u32)(GS_BLOCKS_PER_PAGE - 1)) - 1;
	return result;
}

u32 gs_psm_end_block_unwrapped(u32 bp, u32 bw, u32 psm, int r, int b)
{
	const u32 result = gs_psm_end_block(bp, bw, psm, r, b);
	return result < bp ? result + GS_MAX_BLOCKS : result;
}

int gs_psm_has_overlap(u32 src_bp, u32 src_bw, u32 src_psm, const int* src_rect,
	u32 dst_bp, u32 dst_bw, u32 dst_psm, const int* dst_rect)
{
	const u32 pmask = GS_BLOCKS_PER_PAGE - 1;
	u32 src_start = gs_psm_start_block(src_bp, src_bw, src_psm, src_rect[0], src_rect[1]) & ~pmask;
	u32 dst_start = gs_psm_start_block(dst_bp, dst_bw, dst_psm, dst_rect[0], dst_rect[1]) & ~pmask;
	u32 src_end   = (gs_psm_end_block_unwrapped(src_bp, src_bw, src_psm, src_rect[2], src_rect[3]) + 1 + pmask) & ~pmask;
	u32 dst_end   = (gs_psm_end_block_unwrapped(dst_bp, dst_bw, dst_psm, dst_rect[2], dst_rect[3]) + 1 + pmask) & ~pmask;

	if (dst_end > GS_MAX_BLOCKS && src_end < dst_start)
	{
		src_start += GS_MAX_BLOCKS;
		src_end   += GS_MAX_BLOCKS;
	}
	else if (src_end > GS_MAX_BLOCKS && dst_end < src_start)
	{
		dst_start += GS_MAX_BLOCKS;
		dst_end   += GS_MAX_BLOCKS;
	}
	return src_start < dst_end && dst_start < src_end;
}
