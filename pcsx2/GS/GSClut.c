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

#include <stdlib.h>
#include <string.h>

#include <memalign.h>

#include "GSClut.h"
#include "GSOffset.h"
#include "GSPsm.h"
#include "gs_vector.h"

/* The CLUT buffer, 512 16-bit entries and the 256 high halves of 32-bit
 * ones (the second half repeats the first, as local memory wraps), then
 * the palette as read */
#define GS_CLUT_BYTES 2048
#define GS_CLUT_ALLOC (GS_CLUT_BYTES + 256 * 4)

/* Interleaves pairs: a, b, c, d to their 16- or 32-bit transposes */
#define GS_CLUT_SW16(a, b, c, d) do { \
	gs_vec4i e_ = (a), f_ = (c); \
	(a) = gs_v4i_upl16v(e_, (b)); \
	(c) = gs_v4i_uph16v(e_, (b)); \
	(b) = gs_v4i_upl16v(f_, (d)); \
	(d) = gs_v4i_uph16v(f_, (d)); \
} while (0)
#define GS_CLUT_SW32(a, b, c, d) do { \
	gs_vec4i e_ = (a), f_ = (c); \
	(a) = gs_v4i_upl32v(e_, (b)); \
	(c) = gs_v4i_uph32v(e_, (b)); \
	(b) = gs_v4i_upl32v(f_, (d)); \
	(d) = gs_v4i_uph32v(f_, (d)); \
} while (0)

void gs_clut_init(gs_clut* c)
{
	memset(c, 0, sizeof(*c));
	c->clut = (u16*)memalign_alloc(64, GS_CLUT_ALLOC);
	if (!c->clut)
		abort();
	memset(c->clut, 0, GS_CLUT_ALLOC);
	c->buff32      = (u32*)((u8*)c->clut + GS_CLUT_BYTES);
	c->write.dirty = 1;
	c->read.dirty  = 1;
}

void gs_clut_deinit(gs_clut* c)
{
	memalign_free(c->clut);
	c->clut   = NULL;
	c->buff32 = NULL;
}

void gs_clut_reset(gs_clut* c)
{
	c->cbp[0] = c->cbp[1] = 0;
	memset(c->clut, 0, GS_CLUT_ALLOC);
	memset(&c->write, 0, sizeof(c->write));
	memset(&c->read, 0, sizeof(c->read));
	c->write.dirty = 1;
	c->read.dirty  = 1;
}

int gs_clut_invalidate_range(gs_clut* c, u32 start_block, u32 end_block, int is_draw)
{
	GIFRegTEX0 next;

	if (c->write.dirty & 2)
		return c->write.dirty != 0;

	next.U64 = c->write.next_tex0;
	/* a write wrapping past the end of memory (Star Wars Battlefront 2) */
	if ((end_block & 0xFFE0) < (start_block & 0xFFE0))
	{
		if ((next.CBP + 3u) <= end_block)
			next.CBP += 0x4000;
		end_block += 0x4000;
	}
	if ((next.CBP + 3u) >= start_block && end_block >= next.CBP)
		c->write.dirty |= is_draw ? 2 : 1;

	return c->write.dirty != 0;
}

int gs_clut_can_load(gs_clut* c, const GIFRegTEX0* TEX0, int update_cbp)
{
	if ((TEX0->PSM & 0x7) < 3)
		return 0;

	switch (TEX0->CLD)
	{
		case 0:
		case 6: /* FFX2 menu */
		case 7: /* Ford Mustang Racing, Bouken Jidai Katsugeki Goemon */
			return 0;
		case 1:
			break;
		case 2:
			if (update_cbp)
				c->cbp[0] = TEX0->CBP;
			break;
		case 3:
			if (update_cbp)
				c->cbp[1] = TEX0->CBP;
			break;
		case 4:
			if (c->cbp[0] == TEX0->CBP)
				return 0;
			if (update_cbp)
				c->cbp[0] = TEX0->CBP;
			break;
		case 5:
			if (c->cbp[1] == TEX0->CBP)
				return 0;
			if (update_cbp)
				c->cbp[1] = TEX0->CBP;
			break;
		default:
			break;
	}
	return 1;
}

/* CSA, CSM, CPSM and CBP */
#define GS_CLUT_TEX0_MASK 0x1FFFFFE000000000ull

static int gs_clut_write_dirty(gs_clut* c, const GIFRegTEX0* TEX0, const GIFRegTEXCLUT* TEXCLUT)
{
	int dirty = c->write.dirty != 0;

	if (((c->write.TEX0.U64 ^ TEX0->U64) & GS_CLUT_TEX0_MASK)
		|| gs_psm[c->write.TEX0.PSM].pal != gs_psm[TEX0->PSM].pal)
		dirty = 1;
	else if (TEX0->CSM == 1 && (TEXCLUT->U32[0] ^ c->write.TEXCLUT.U32[0]))
		dirty = 1;

	if (!dirty)
	{
		c->write.TEX0.U64    = TEX0->U64;
		c->write.TEXCLUT.U64 = TEXCLUT->U64;
	}
	return dirty;
}

static int gs_clut_read_dirty(gs_clut* c, const GIFRegTEX0* TEX0, const GIFRegTEXA* TEXA)
{
	/* TA0 and AEM for 24 bits, and TA1 for 16 */
	const u64 texa24_mask = 0x80FFull;
	const u64 texa16_mask = 0xFF000080FFull;
	int dirty = c->read.dirty;

	if (((c->read.TEX0.U64 ^ TEX0->U64) & GS_CLUT_TEX0_MASK)
		|| gs_psm[c->read.TEX0.PSM].pal != gs_psm[TEX0->PSM].pal)
		dirty = 1;
	else if (TEX0->CPSM == PSMCT24 && ((c->read.TEXA.U64 ^ TEXA->U64) & texa24_mask))
		dirty = 1;
	else if (TEX0->CPSM >= PSMCT16 && ((c->read.TEXA.U64 ^ TEXA->U64) & texa16_mask))
		dirty = 1;

	if (!dirty)
	{
		c->read.TEX0.U64 = TEX0->U64;
		c->read.TEXA.U64 = TEXA->U64;
	}
	return dirty;
}

int gs_clut_write_test(gs_clut* c, const GIFRegTEX0* TEX0, const GIFRegTEXCLUT* TEXCLUT)
{
	/* An index format first: CBP0/1 do not change for another, which
	 * breaks games (Corvette NTSC) */
	if ((TEX0->PSM & 0x7) < 3)
		return 0;
	if (!gs_clut_can_load(c, TEX0, 1))
		return 0;
	return gs_clut_write_dirty(c, TEX0, TEXCLUT);
}

/* ---- loading: CSM1, a block laid out as the CLUT's own order ---- */

/* 16 32-bit entries, one block's worth, low halves then high */
static void gs_clut_write_t32_i4(const u32* src, u16* clut)
{
	const gs_vec4i* s = (const gs_vec4i*)src;
	gs_vec4i* d = (gs_vec4i*)clut;
	gs_vec4i v0 = s[0], v1 = s[1], v2 = s[2], v3 = s[3];

	GS_CLUT_SW16(v0, v1, v2, v3);
	GS_CLUT_SW32(v0, v1, v2, v3);
	GS_CLUT_SW16(v0, v2, v1, v3);

	d[0]  = v0;
	d[1]  = v2;
	d[32] = v1;
	d[33] = v3;
}

static void gs_clut_write_t32_i8(const u32* src, u16* clut, unsigned offset)
{
	unsigned i;
	/* from CSA on: the entries below it stay */
	for (i = offset; i < 16; i++)
	{
		const unsigned off = i << 4;
		const unsigned s = gs_clut_table_t32i8[off & 0x70] | (off & 0x80);
		gs_clut_write_t32_i4(&src[s], &clut[off]);
	}
}

static void gs_clut_write_t16_i8(const u16* src, u16* clut)
{
	/* two blocks */
	const gs_vec4i* s = (const gs_vec4i*)src;
	gs_vec4i* d = (gs_vec4i*)clut;
	int i;

	for (i = 0; i < 32; i += 4)
	{
		gs_vec4i v0 = s[i + 0], v1 = s[i + 1], v2 = s[i + 2], v3 = s[i + 3];

		GS_CLUT_SW16(v0, v1, v2, v3);
		GS_CLUT_SW32(v0, v1, v2, v3);
		GS_CLUT_SW16(v0, v2, v1, v3);

		d[i + 0] = v0;
		d[i + 1] = v2;
		d[i + 2] = v1;
		d[i + 3] = v3;
	}
}

static void gs_clut_write_t16_i4(const u16* src, u16* clut)
{
	int i;
	for (i = 0; i < 16; i++)
		clut[i] = src[gs_clut_table_t16i4[i]];
}

/* The block at CBP in a buffer one page wide */
static const u8* gs_clut_block(const u8* vm, const gs_swizzle_info* swz, u32 cbp)
{
	return vm + ((size_t)gs_swizzle_bn(swz, 0, 0, cbp, 1) << 8);
}

/* ---- loading: CSM2, a row of the buffer from (COU * 16, COV) ---- */

static void gs_clut_write_csm2(gs_clut* c, const u8* vm, const GIFRegTEX0* TEX0,
	const GIFRegTEXCLUT* TEXCLUT, int n)
{
	gs_offset off;
	gs_pa_helper pa;
	int i;

	if (TEX0->CPSM == PSMCT32 || TEX0->CPSM == PSMCT24)
	{
		const u32* vm32 = (const u32*)vm;
		u16* clut = c->clut + ((TEX0->CSA & 15) << 4);
		off = gs_offset_make(&gs_swizzle32, TEX0->CBP, TEXCLUT->CBW, PSMCT32);
		gs_pa_init(&pa, &off, TEXCLUT->COU << 4, TEXCLUT->COV);
		for (i = 0; i < n; i++)
		{
			const u32 v = vm32[gs_pa_value(&pa, i)];
			clut[i]       = (u16)(v & 0xffff);
			clut[i + 256] = (u16)(v >> 16);
		}
	}
	else
	{
		const u16* vm16 = (const u16*)vm;
		u16* clut = c->clut + (TEX0->CSA << 4);
		off = gs_offset_make(TEX0->CPSM == PSMCT16S ? &gs_swizzle16S : &gs_swizzle16,
			TEX0->CBP, TEXCLUT->CBW, TEX0->CPSM);
		gs_pa_init(&pa, &off, TEXCLUT->COU << 4, TEXCLUT->COV);
		for (i = 0; i < n; i++)
			clut[i] = vm16[gs_pa_value(&pa, i)];
	}
}

void gs_clut_write(gs_clut* c, const u8* vm, const GIFRegTEX0* TEX0, const GIFRegTEXCLUT* TEXCLUT)
{
	/* the GS reads the low three bits of PSM for 8 or 4 bits */
	const unsigned psm  = TEX0->PSM;
	const unsigned cpsm = TEX0->CPSM;
	const int eight = (psm & 7) == 3, four = (psm & 7) == 4;

	c->write.TEX0    = *TEX0;
	c->write.TEXCLUT = *TEXCLUT;
	c->read.dirty    = 1;
	c->write.dirty   = 0;

	if (TEX0->CSM == 0)
	{
		if (cpsm == PSMCT32 || cpsm == PSMCT24) /* 24 undocumented (KH?) */
		{
			const u32* src = (const u32*)gs_clut_block(vm, &gs_swizzle32, TEX0->CBP);
			if (eight)
				gs_clut_write_t32_i8(src, c->clut, TEX0->CSA & 15);
			else if (four)
				gs_clut_write_t32_i4(src, c->clut + ((TEX0->CSA & 15) << 4));
		}
		else if (cpsm == PSMCT16 || cpsm == PSMCT16S)
		{
			const u16* src = (const u16*)gs_clut_block(vm,
				cpsm == PSMCT16S ? &gs_swizzle16S : &gs_swizzle16, TEX0->CBP);
			if (eight)
				gs_clut_write_t16_i8(src, c->clut + (TEX0->CSA << 4));
			else if (four)
				gs_clut_write_t16_i4(src, c->clut + (TEX0->CSA << 4));
		}
	}
	else if (cpsm == PSMCT32 || cpsm == PSMCT24 || cpsm == PSMCT16 || cpsm == PSMCT16S)
	{
		/* (untested on hardware) */
		if (psm == PSMT8 || psm == PSMT8H)
			gs_clut_write_csm2(c, vm, TEX0, TEXCLUT, 256);
		else if (psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH)
			gs_clut_write_csm2(c, vm, TEX0, TEXCLUT, 16);
	}
}

/* ---- reading out ---- */

/* 16 entries from the low and high halves */
static void gs_clut_read_t32_i4(const u16* clut, u32* dst)
{
	const gs_vec4i* s = (const gs_vec4i*)clut;
	gs_vec4i* d = (gs_vec4i*)dst;
	gs_vec4i v0 = s[0], v1 = s[1], v2 = s[32], v3 = s[33];

	GS_CLUT_SW16(v0, v2, v1, v3);

	d[0] = v0;
	d[1] = v1;
	d[2] = v2;
	d[3] = v3;
}

/* An offset past the last sixteen clamps rather than wraps (the dirt in
 * GTA San Andreas goes transparent otherwise) */
static void gs_clut_read_t32_i8(const u16* clut, u32* dst, int offset)
{
	int i;
	for (i = 0; i < 256; i += 16)
		gs_clut_read_t32_i4(&clut[i + offset < 240 ? i + offset : 240], &dst[i]);
}

/* 16-bit colours to 32, alpha from TEXA */
static void gs_clut_expand16(const u16* src, u32* dst, int w, const GIFRegTEXA* TEXA)
{
	const gs_vec4i rm  = gs_v4i_set32(0x0000001f);
	const gs_vec4i gm  = gs_v4i_set32(0x000003e0);
	const gs_vec4i bm  = gs_v4i_set32(0x00007c00);
	const gs_vec4i ta0 = gs_v4i_set32((int)(TEXA->TA0 << 24));
	const gs_vec4i ta1 = gs_v4i_set32((int)(TEXA->TA1 << 24));
	const gs_vec4i zero = gs_v4i_zero();
	const gs_vec4i* s = (const gs_vec4i*)src;
	gs_vec4i* d = (gs_vec4i*)dst;
	int i, half;

	for (i = 0; i < (w >> 3); i++)
	{
		const gs_vec4i v = s[i];
		for (half = 0; half < 2; half++)
		{
			const gs_vec4i x = half ? gs_v4i_uph16v(v, v) : gs_v4i_upl16v(v, v);
			gs_vec4i a = gs_v4i_blend8(ta0, ta1, GS_V4I_SRA16(x, 15));
			gs_vec4i rgb = gs_v4i_or(gs_v4i_or(
				GS_V4I_SLL32(gs_v4i_and(x, rm), 3),
				GS_V4I_SLL32(gs_v4i_and(x, gm), 6)),
				GS_V4I_SLL32(gs_v4i_and(x, bm), 9));
			/* AEM: black is transparent */
			if (TEXA->AEM)
				a = gs_v4i_andnot(gs_v4i_eq32(x, zero), a);
			d[i * 2 + half] = gs_v4i_or(rgb, a);
		}
	}
}

int gs_clut_read32(gs_clut* c, const GIFRegTEX0* TEX0, const GIFRegTEXA* TEXA)
{
	const unsigned psm = TEX0->PSM;
	const int four = psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH;
	const int eight = psm == PSMT8 || psm == PSMT8H;

	if (!gs_clut_read_dirty(c, TEX0, TEXA))
		return 0;

	c->read.TEX0   = *TEX0;
	c->read.TEXA   = *TEXA;
	c->read.dirty  = 0;
	c->read.adirty = 1;

	if (TEX0->CPSM == PSMCT32 || TEX0->CPSM == PSMCT24)
	{
		if (eight)
			gs_clut_read_t32_i8(c->clut, c->buff32, (TEX0->CSA & 15) << 4);
		else if (four)
			gs_clut_read_t32_i4(c->clut + ((TEX0->CSA & 15) << 4), c->buff32);
	}
	else if (TEX0->CPSM == PSMCT16 || TEX0->CPSM == PSMCT16S)
	{
		if (eight || four)
			gs_clut_expand16(c->clut + (TEX0->CSA << 4), c->buff32, eight ? 256 : 16, TEXA);
	}
	return 1;
}

void gs_clut_alpha_minmax(gs_clut* c, int* amin, int* amax)
{
	if (c->read.adirty)
	{
		c->read.adirty = 0;

		if (gs_psm[c->read.TEX0.CPSM].trbpp == 24 && c->read.TEXA.AEM == 0)
			c->read.amin = c->read.amax = (int)c->read.TEXA.TA0;
		else
		{
			const int n = gs_psm[c->read.TEX0.PSM].pal == 256 ? 256 : 16;
			u32 lo = 255, hi = 0;
			int i;
			for (i = 0; i < n; i++)
			{
				const u32 a = c->buff32[i] >> 24;
				if (a < lo)
					lo = a;
				if (a > hi)
					hi = a;
			}
			c->read.amin = (int)lo;
			c->read.amax = (int)hi;
		}
	}
	*amin = c->read.amin;
	*amax = c->read.amax;
}
