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

#include "GSDirtyRect.h"
extern "C" {
#include "GS/gs_vector.h"
}
#include <limits.h>

GSDirtyRect::GSDirtyRect() :
	r(GSVector4i(gs_v4i_zero())),
	psm(PSMCT32),
	bw(1),
	rgba({}),
	req_linear(false)
{
}

GSDirtyRect::GSDirtyRect(GSVector4i& r, u32 psm, u32 bw, RGBAMask rgba, bool req_linear) :
	r(r),
	psm(psm),
	bw(bw),
	rgba(rgba),
	req_linear(req_linear)
{
}

GSVector4i GSDirtyRect::GetDirtyRect(GIFRegTEX0 TEX0, bool align) const
{
	union gs_v4i_view _r;

	const GSVector2i& src = GSLocalMemory::m_psm[psm].bs;

	if (psm != TEX0.PSM)
	{
		const union gs_v4i_view& cur = reinterpret_cast<const union gs_v4i_view&>(r);
		const GSVector2i& dst = GSLocalMemory::m_psm[TEX0.PSM].bs;
		_r.rect.left   = (cur.rect.left   * dst.x) / src.x;
		_r.rect.top    = (cur.rect.top    * dst.y) / src.y;
		_r.rect.right  = (cur.rect.right  * dst.x) / src.x;
		_r.rect.bottom = (cur.rect.bottom * dst.y) / src.y;
	}
	else
	{
		_r.v = r;
	}

	return GSVector4i(align ? gs_v4i_ralign_outside(_r.v, &src) : _r.v);
}

GSVector4i GSDirtyRectList::GetTotalRect(GIFRegTEX0 TEX0, const GSVector2i& size) const
{
	if (!empty())
	{
		gs_vec4i r = gs_v4i_set4(INT_MAX, INT_MAX, 0, 0);

		for (auto& dirty_rect : *this)
		{
			r = gs_v4i_runion(r, dirty_rect.GetDirtyRect(TEX0, true));
		}

		const GSVector2i& bs = GSLocalMemory::m_psm[TEX0.PSM].bs;

		return GSVector4i(gs_v4i_rintersect(gs_v4i_ralign_outside(r, &bs),
		                                    gs_v4i_loadh(&size)));
	}

	return GSVector4i(gs_v4i_zero());
}

u32 GSDirtyRectList::GetDirtyChannels()
{
	u32 channels = 0;

	if (!empty())
	{
		for (auto& dirty_rect : *this)
		{
			channels |= dirty_rect.rgba._u32;
		}
	}

	return channels;
}

GSVector4i GSDirtyRectList::GetDirtyRect(size_t index, GIFRegTEX0 TEX0, const GSVector4i& clamp, bool align) const
{
	gs_vec4i r = (*this)[index].GetDirtyRect(TEX0, align);
	const GSVector2i& bs = GSLocalMemory::m_psm[TEX0.PSM].bs;
	if (align)
		r = gs_v4i_ralign_outside(r, &bs);

	return GSVector4i(gs_v4i_rintersect(r, clamp));
}
