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

#include <limits.h>

#include "GSDirtyRect.h"

gs_vec4i gs_dirty_rect_get(const gs_dirty_rect* d, u32 tex_psm, int align)
{
	const gs_int2* src = &gs_psm[d->psm].bs;
	union gs_v4i_view r;

	if (d->psm != tex_psm)
	{
		/* scaled block for block into the other format's */
		const gs_int2* dst = &gs_psm[tex_psm].bs;
		union gs_v4i_view cur;
		cur.v = d->r;
		r.rect.left   = (cur.rect.left   * dst->x) / src->x;
		r.rect.top    = (cur.rect.top    * dst->y) / src->y;
		r.rect.right  = (cur.rect.right  * dst->x) / src->x;
		r.rect.bottom = (cur.rect.bottom * dst->y) / src->y;
	}
	else
		r.v = d->r;

	return align ? gs_v4i_ralign_outside(r.v, src) : r.v;
}

gs_vec4i gs_dirty_total_rect(const gs_dirty_rect* d, size_t n, u32 tex_psm, int w, int h)
{
	gs_vec4i r;
	int size[2];
	size_t i;

	if (!n)
		return gs_v4i_zero();

	r = gs_v4i_set4(INT_MAX, INT_MAX, 0, 0);
	for (i = 0; i < n; i++)
		r = gs_v4i_runion(r, gs_dirty_rect_get(&d[i], tex_psm, 1));

	size[0] = w;
	size[1] = h;
	return gs_v4i_rintersect(gs_v4i_ralign_outside(r, &gs_psm[tex_psm].bs), gs_v4i_loadh(size));
}

u32 gs_dirty_channels(const gs_dirty_rect* d, size_t n)
{
	u32 channels = 0;
	size_t i;
	for (i = 0; i < n; i++)
		channels |= d[i].rgba._u32;
	return channels;
}

gs_vec4i gs_dirty_rect_clamped(const gs_dirty_rect* d, u32 tex_psm, gs_vec4i clamp, int align)
{
	gs_vec4i r = gs_dirty_rect_get(d, tex_psm, align);
	if (align)
		r = gs_v4i_ralign_outside(r, &gs_psm[tex_psm].bs);
	return gs_v4i_rintersect(r, clamp);
}
