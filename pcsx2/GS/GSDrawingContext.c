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

#include <math.h>
#include <string.h>

#include "GSDrawingContext.h"

/* The furthest texel a run of coordinates tl..br reaches under the wrap
 * mode, for a texture of limit + 1 texels; past 1024 the hardware masks
 * the size down to one texel (but for region clamp) */
static int gs_find_max(int tl, int br, int limit, int wm, int minuv, int maxuv)
{
	int uv = br;

	if (limit > 1024)
		limit = 0;

	if (wm == CLAMP_CLAMP)
	{
		if (uv > limit)
			uv = limit;
	}
	else if (wm == CLAMP_REPEAT)
	{
		if (tl < 0 || uv > limit)
			uv = limit; /* wraps around */
	}
	else if (wm == CLAMP_REGION_CLAMP)
	{
		if (uv < minuv)
			uv = minuv;
		if (uv > maxuv)
			uv = maxuv;
	}
	else if (wm == CLAMP_REGION_REPEAT)
	{
		/* Taken & mask | fix: the mask is MINUV within the texture's own
		 * size even when MAXUV moves it out; br is the bound when it is
		 * below the mask, since a larger value may lie between tl and br */
		minuv &= limit;
		if (tl < 0)
			uv = minuv | maxuv;
		else
			uv = (uv < minuv ? uv : minuv) | maxuv;
	}

	return uv;
}

/* The smallest log2 size, down to 8, holding the inclusive coordinate */
static int gs_reduce(int uv, int size)
{
	while (size > 3 && (1 << (size - 1)) > uv)
		size--;
	return size;
}

/* The log2 size, up to 1024, grown to hold it */
static int gs_extend(int uv, int size)
{
	while (size < 10 && (1 << size) <= uv)
		size++;
	return size;
}

/* A float to int as cvttps2dq does it: out of range or NaN is INT_MIN */
static int gs_ftoi(float f)
{
	if (!(f >= -2147483648.0f && f < 2147483648.0f))
		return (int)0x80000000u;
	return (int)f;
}

void gs_ctx_scissor(const GIFRegSCISSOR* scissor, const GIFRegXYOFFSET* xyoffset,
	int in[4], int cull[4], int xyof[4])
{
	/* the register's far edges are inclusive */
	const int x0 = (int)scissor->SCAX0, y0 = (int)scissor->SCAY0;
	const int x1 = (int)scissor->SCAX1, y1 = (int)scissor->SCAY1;

	in[0] = x0;
	in[1] = y0;
	in[2] = x1 + 1;
	in[3] = y1 + 1;

	cull[0] = x0 << 4;
	cull[1] = y0 << 4;
	cull[2] = x1 << 4;
	cull[3] = y1 << 4;

	/* at least one pixel a scanline is filled, so subpixels round down */
	xyof[0] = (int)xyoffset->OFX;
	xyof[1] = (int)xyoffset->OFY;
	xyof[2] = (int)(xyoffset->OFX - 15u);
	xyof[3] = (int)(xyoffset->OFY - 15u);
}

GIFRegTEX0 gs_ctx_size_fixed_tex0(const GIFRegTEX0* tex0, const GIFRegCLAMP* clamp,
	const float st[4], int linear, int mipmap)
{
	GIFRegTEX0 res = *tex0;
	int tw = (int)tex0->TW;
	int th = (int)tex0->TH;
	int wms, wmt, u0, v0, u1, v1, uvx, uvy;

	if (mipmap)
	{
		res.TW = tw > 10 ? 0 : tw;
		res.TH = th > 10 ? 0 : th;
		return res;
	}

	wms = (int)clamp->WMS;
	wmt = (int)clamp->WMT;

	/* bilinear reaches half a texel either side */
	u0 = gs_ftoi((float)floor(st[0] - (linear ? 0.5f : 0.0f)));
	v0 = gs_ftoi((float)floor(st[1] - (linear ? 0.5f : 0.0f)));
	u1 = gs_ftoi((float)ceil(st[2] + (linear ? 0.5f : 0.0f)));
	v1 = gs_ftoi((float)ceil(st[3] + (linear ? 0.5f : 0.0f)));

	uvx = gs_find_max(u0, u1, (1 << tw) - 1, wms, (int)clamp->MINU, (int)clamp->MAXU);
	uvy = gs_find_max(v0, v1, (1 << th) - 1, wmt, (int)clamp->MINV, (int)clamp->MAXV);

	/* Smaller ones only make more texture cache entries for the memory
	 * they save */
	if (tw + th >= 19)
	{
		tw = gs_reduce(uvx, tw);
		th = gs_reduce(uvy, th);
	}

	if (wms == CLAMP_REGION_CLAMP || wms == CLAMP_REGION_REPEAT)
		tw = gs_extend(uvx, tw);
	if (wmt == CLAMP_REGION_CLAMP || wmt == CLAMP_REGION_REPEAT)
		th = gs_extend(uvy, th);

	res.TW = tw > 10 ? 0 : tw;
	res.TH = th > 10 ? 0 : th;
	return res;
}
