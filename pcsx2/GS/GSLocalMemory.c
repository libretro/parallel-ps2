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

#include <stddef.h>
#include <string.h>

#include "GSLocalMemory.h"

/* ---- one pixel or texel by place: the format's swizzle, then by address ---- */

/* (name, swizzle, by-address reader) */
#define GS_LM_RP(name, swz, rd) \
	static u32 gs_lm_rp_##name(const gs_local_memory* mem, int x, int y, u32 bp, u32 bw) \
	{ \
		return rd(mem, gs_swizzle_pa(&swz, x, y, bp, bw)); \
	}
#define GS_LM_WP(name, swz, wr) \
	static void gs_lm_wp_##name(gs_local_memory* mem, int x, int y, u32 c, u32 bp, u32 bw) \
	{ \
		wr(mem, gs_swizzle_pa(&swz, x, y, bp, bw), c); \
	}
#define GS_LM_RT(name, swz, rd) \
	static u32 gs_lm_rt_##name(const gs_local_memory* mem, int x, int y, const GIFRegTEX0* TEX0, \
		const GIFRegTEXA* TEXA) \
	{ \
		return rd(mem, gs_swizzle_pa(&swz, x, y, TEX0->TBP0, TEX0->TBW), TEXA); \
	}

GS_LM_RP(32, gs_swizzle32, gs_lm_read_pixel32)
GS_LM_RP(24, gs_swizzle32, gs_lm_read_pixel24)
GS_LM_RP(16, gs_swizzle16, gs_lm_read_pixel16)
GS_LM_RP(16s, gs_swizzle16S, gs_lm_read_pixel16)
GS_LM_RP(8, gs_swizzle8, gs_lm_read_pixel8)
GS_LM_RP(4, gs_swizzle4, gs_lm_read_pixel4)
GS_LM_RP(8h, gs_swizzle32, gs_lm_read_pixel8h)
GS_LM_RP(4hl, gs_swizzle32, gs_lm_read_pixel4hl)
GS_LM_RP(4hh, gs_swizzle32, gs_lm_read_pixel4hh)
GS_LM_RP(32z, gs_swizzle32Z, gs_lm_read_pixel32)
GS_LM_RP(24z, gs_swizzle32Z, gs_lm_read_pixel24)
GS_LM_RP(16z, gs_swizzle16Z, gs_lm_read_pixel16)
GS_LM_RP(16sz, gs_swizzle16SZ, gs_lm_read_pixel16)

GS_LM_WP(32, gs_swizzle32, gs_lm_write_pixel32)
GS_LM_WP(24, gs_swizzle32, gs_lm_write_pixel24)
GS_LM_WP(16, gs_swizzle16, gs_lm_write_pixel16)
GS_LM_WP(16s, gs_swizzle16S, gs_lm_write_pixel16)
GS_LM_WP(8, gs_swizzle8, gs_lm_write_pixel8)
GS_LM_WP(4, gs_swizzle4, gs_lm_write_pixel4)
GS_LM_WP(8h, gs_swizzle32, gs_lm_write_pixel8h)
GS_LM_WP(4hl, gs_swizzle32, gs_lm_write_pixel4hl)
GS_LM_WP(4hh, gs_swizzle32, gs_lm_write_pixel4hh)
GS_LM_WP(32z, gs_swizzle32Z, gs_lm_write_pixel32)
GS_LM_WP(24z, gs_swizzle32Z, gs_lm_write_pixel24)
GS_LM_WP(16z, gs_swizzle16Z, gs_lm_write_pixel16)
GS_LM_WP(16sz, gs_swizzle16SZ, gs_lm_write_pixel16)

GS_LM_RT(32, gs_swizzle32, gs_lm_read_texel32)
GS_LM_RT(24, gs_swizzle32, gs_lm_read_texel24)
GS_LM_RT(16, gs_swizzle16, gs_lm_read_texel16)
GS_LM_RT(16s, gs_swizzle16S, gs_lm_read_texel16)
GS_LM_RT(8, gs_swizzle8, gs_lm_read_texel8)
GS_LM_RT(4, gs_swizzle4, gs_lm_read_texel4)
GS_LM_RT(8h, gs_swizzle32, gs_lm_read_texel8h)
GS_LM_RT(4hl, gs_swizzle32, gs_lm_read_texel4hl)
GS_LM_RT(4hh, gs_swizzle32, gs_lm_read_texel4hh)
GS_LM_RT(32z, gs_swizzle32Z, gs_lm_read_texel32)
GS_LM_RT(24z, gs_swizzle32Z, gs_lm_read_texel24)
GS_LM_RT(16z, gs_swizzle16Z, gs_lm_read_texel16)
GS_LM_RT(16sz, gs_swizzle16SZ, gs_lm_read_texel16)

/* (the by-address ones as plain functions: the table takes their address) */
#define GS_LM_RPA(name) \
	static u32 gs_lm_rpa_##name(const gs_local_memory* mem, u32 addr) { return gs_lm_read_pixel##name(mem, addr); }
#define GS_LM_WPA(name) \
	static void gs_lm_wpa_##name(gs_local_memory* mem, u32 addr, u32 c) { gs_lm_write_pixel##name(mem, addr, c); }
#define GS_LM_RTA(name) \
	static u32 gs_lm_rta_##name(const gs_local_memory* mem, u32 addr, const GIFRegTEXA* TEXA) \
	{ \
		return gs_lm_read_texel##name(mem, addr, TEXA); \
	}

GS_LM_RPA(32) GS_LM_RPA(24) GS_LM_RPA(16) GS_LM_RPA(8) GS_LM_RPA(4) GS_LM_RPA(8h) GS_LM_RPA(4hl) GS_LM_RPA(4hh)
GS_LM_WPA(32) GS_LM_WPA(24) GS_LM_WPA(16) GS_LM_WPA(8) GS_LM_WPA(4) GS_LM_WPA(8h) GS_LM_WPA(4hl) GS_LM_WPA(4hh)
GS_LM_RTA(32) GS_LM_RTA(24) GS_LM_RTA(16) GS_LM_RTA(8) GS_LM_RTA(4) GS_LM_RTA(8h) GS_LM_RTA(4hl) GS_LM_RTA(4hh)

static void gs_lm_wfa_16(gs_local_memory* mem, u32 addr, u32 c) { gs_lm_write_frame16(mem, addr, c); }

/* (rp, rpa, wp, wpa, rt, rta, wfa) */
#define GS_LM_FNS(rp, rpa, wp, wpa, rt, rta, wfa) \
	{gs_lm_rp_##rp, gs_lm_rpa_##rpa, gs_lm_wp_##wp, gs_lm_wpa_##wpa, gs_lm_rt_##rt, gs_lm_rta_##rta, wfa}
#define GS_LM_32 GS_LM_FNS(32, 32, 32, 32, 32, 32, gs_lm_wpa_32)

const gs_lm_pixel_fns gs_lm_pixel[64] = {
	GS_LM_FNS(32, 32, 32, 32, 32, 32, gs_lm_wpa_32), /*  0 PSMCT32 */
	GS_LM_FNS(24, 24, 24, 24, 24, 24, gs_lm_wpa_24), /*  1 PSMCT24 */
	GS_LM_FNS(16, 16, 16, 16, 16, 16, gs_lm_wfa_16), /*  2 PSMCT16 */
	GS_LM_32, /*  3 */
	GS_LM_32, /*  4 */
	GS_LM_32, /*  5 */
	GS_LM_32, /*  6 */
	GS_LM_32, /*  7 */
	GS_LM_32, /*  8 */
	GS_LM_32, /*  9 */
	GS_LM_FNS(16s, 16, 16s, 16, 16s, 16, gs_lm_wfa_16), /* 10 PSMCT16S */
	GS_LM_32, /* 11 */
	GS_LM_32, /* 12 */
	GS_LM_32, /* 13 */
	GS_LM_32, /* 14 */
	GS_LM_32, /* 15 */
	GS_LM_32, /* 16 */
	GS_LM_32, /* 17 */
	GS_LM_32, /* 18 */
	GS_LM_FNS(8, 8, 8, 8, 8, 8, gs_lm_wpa_32), /* 19 PSMT8 */
	GS_LM_FNS(4, 4, 4, 4, 4, 4, gs_lm_wpa_32), /* 20 PSMT4 */
	GS_LM_32, /* 21 */
	GS_LM_32, /* 22 */
	GS_LM_32, /* 23 */
	GS_LM_32, /* 24 */
	GS_LM_32, /* 25 */
	GS_LM_32, /* 26 */
	GS_LM_FNS(8h, 8h, 8h, 8h, 8h, 8h, gs_lm_wpa_32), /* 27 PSMT8H */
	GS_LM_32, /* 28 */
	GS_LM_32, /* 29 */
	GS_LM_32, /* 30 */
	GS_LM_32, /* 31 */
	GS_LM_32, /* 32 */
	GS_LM_32, /* 33 */
	GS_LM_32, /* 34 */
	GS_LM_32, /* 35 */
	GS_LM_FNS(4hl, 4hl, 4hl, 4hl, 4hl, 4hl, gs_lm_wpa_32), /* 36 PSMT4HL */
	GS_LM_32, /* 37 */
	GS_LM_32, /* 38 */
	GS_LM_32, /* 39 */
	GS_LM_32, /* 40 */
	GS_LM_32, /* 41 */
	GS_LM_32, /* 42 */
	GS_LM_32, /* 43 */
	GS_LM_FNS(4hh, 4hh, 4hh, 4hh, 4hh, 4hh, gs_lm_wpa_32), /* 44 PSMT4HH */
	GS_LM_32, /* 45 */
	GS_LM_32, /* 46 */
	GS_LM_32, /* 47 */
	GS_LM_FNS(32z, 32, 32z, 32, 32z, 32, gs_lm_wpa_32), /* 48 PSMZ32 */
	GS_LM_FNS(24z, 24, 24z, 24, 24z, 24, gs_lm_wpa_24), /* 49 PSMZ24 */
	GS_LM_FNS(16z, 16, 16z, 16, 16z, 16, gs_lm_wfa_16), /* 50 PSMZ16 */
	GS_LM_32, /* 51 */
	GS_LM_32, /* 52 */
	GS_LM_32, /* 53 */
	GS_LM_32, /* 54 */
	GS_LM_32, /* 55 */
	GS_LM_32, /* 56 */
	GS_LM_32, /* 57 */
	GS_LM_FNS(16sz, 16, 16sz, 16, 16sz, 16, gs_lm_wfa_16), /* 58 PSMZ16S */
	GS_LM_32, /* 59 */
	GS_LM_32, /* 60 */
	GS_LM_32, /* 61 */
	GS_LM_32, /* 62 */
	GS_LM_32, /* 63 */
};

/* ---- the memory ---- */

int gs_local_memory_init(gs_local_memory* mem)
{
	if (!gs_p2t_cache_init(&mem->m_p2tmap) || !gs_po4_pool_init(&mem->m_po4))
		return 0;
	mem->m_clut32 = NULL;
	gs_local_memory_populate_transfers();

	mem->m_vm8 = (u8*)GSAllocateWrappedMemory(GS_LOCAL_MEMORY_SIZE, 4);
	if (!mem->m_vm8)
		return 0;
	memset(mem->m_vm8, 0, GS_LOCAL_MEMORY_SIZE);
	return 1;
}

void gs_local_memory_deinit(gs_local_memory* mem)
{
	if (mem->m_vm8)
		GSFreeWrappedMemory(mem->m_vm8, GS_LOCAL_MEMORY_SIZE, 4);
	mem->m_vm8 = NULL;
	gs_po4_pool_free(&mem->m_po4);
	gs_p2t_cache_free(&mem->m_p2tmap);
}

/* ---- rectangles ---- */

u32 gs_lm_page_aligned_mask(u32 psm, const int r[4])
{
	const gs_int2 pgs = gs_psm[psm].pgs;
	const int mx = pgs.x - 1, my = pgs.y - 1;
	return ((r[0] & mx) ? 0u : 0x000fu) | ((r[1] & my) ? 0u : 0x00f0u)
		| ((r[2] & mx) ? 0u : 0x0f00u) | ((r[3] & my) ? 0u : 0xf000u);
}

void gs_lm_rect_for_page_offset(u32 base_bp, u32 offset_bp, u32 bw, u32 psm, int out[4])
{
	const u32 page_offset = (offset_bp - base_bp) >> 5;
	const gs_int2 pgs     = gs_psm[psm].pgs;
	const u32 valid_bw    = bw > 1 ? bw : 1;
	const int px          = (int)(page_offset % valid_bw);
	const int py          = (int)(page_offset / valid_bw);
	out[0] = pgs.x * px;
	out[1] = pgs.y * py;
	out[2] = pgs.x * px + pgs.x;
	out[3] = pgs.y * py + pgs.y;
}

/* texel by texel, rows top to bottom, of r (l, t, r, b) */
static u8* gs_lm_read_texels(const gs_local_memory* mem, gs_lm_read_texel_fn rt, const GIFRegTEX0* TEX0,
	const GIFRegTEXA* TEXA, int left, int top, int right, int bottom, int first, u8* dst, int dstpitch)
{
	int x, y, i;
	for (y = top; y < bottom; y++, dst += dstpitch)
		for (x = left, i = first; x < right; x++, i++)
			((u32*)dst)[i] = rt(mem, x, y, TEX0, TEXA);
	return dst;
}

void gs_local_memory_read_texture(gs_local_memory* mem, const gs_offset* off, const int r[4], u8* dst, int dstpitch,
	const GIFRegTEXA* TEXA)
{
	const gs_psm_attr* psm = &gs_psm[off->psm];
	const int bx = psm->bs.x, by = psm->bs.y;
	const int left = r[0], top = r[1], right = r[2], bottom = r[3];
	gs_lm_read_texel_fn rt;
	GIFRegTEX0 TEX0;
	int cr[4], aligned, empty, y;
	u8* crdst;

	if (right - left >= bx && bottom - top >= by && !(left & (bx - 1)) && !(top & (by - 1))
		&& !(right & (bx - 1)) && !(bottom & (by - 1)))
	{
		gs_local_memory_read_texture_blocks(mem, off, r, dst, dstpitch, TEXA);
		return;
	}

	rt = gs_lm_pixel[off->psm].rt;
	TEX0.U64  = 0;
	TEX0.TBP0 = (u32)off->bp;
	TEX0.TBW  = gs_offset_bw(off);
	TEX0.PSM  = (u32)off->psm;

	/* in to whole blocks: those the transfers read, the edges texel by
	 * texel */
	cr[0]   = (left + bx - 1) & ~(bx - 1);
	cr[1]   = (top + by - 1) & ~(by - 1);
	cr[2]   = right & ~(bx - 1);
	cr[3]   = bottom & ~(by - 1);
	empty   = !(cr[0] < cr[2] && cr[1] < cr[3]);
	aligned = (((size_t)(dst + (cr[0] - left) * sizeof(u32))) & 0xf) == 0;

	if (empty || !aligned)
	{
		gs_lm_read_texels(mem, rt, &TEX0, TEXA, left, top, right, bottom, 0, dst, dstpitch);
		return;
	}

	crdst = dst;
	dst   = gs_lm_read_texels(mem, rt, &TEX0, TEXA, left, top, right, cr[1], 0, dst, dstpitch);
	for (y = cr[1]; y < cr[3]; y++, dst += dstpitch)
	{
		gs_lm_read_texels(mem, rt, &TEX0, TEXA, left, y, cr[0], y + 1, 0, dst, dstpitch);
		gs_lm_read_texels(mem, rt, &TEX0, TEXA, cr[2], y, right, y + 1, cr[2] - left, dst, dstpitch);
	}
	gs_lm_read_texels(mem, rt, &TEX0, TEXA, left, cr[3], right, bottom, 0, dst, dstpitch);

	crdst += dstpitch * (cr[1] - top);
	crdst += sizeof(u32) * (cr[0] - left);
	gs_local_memory_read_texture_blocks(mem, off, cr, crdst, dstpitch, TEXA);
}
