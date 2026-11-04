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

#pragma once

#include <stdlib.h>
#include <string.h>

#include "../../GSLocalMemory.h"
#include "GSArray.h"

union RGBAMask
{
	u32 _u32;
	struct
	{
		u32 r : 1;
		u32 g : 1;
		u32 b : 1;
		u32 a : 1;
	} c;
};

class GSDirtyRect
{
public:
	GSVector4i r;
	u32 psm;
	u32 bw;
	RGBAMask rgba;

	GSDirtyRect();
	GSDirtyRect(GSVector4i& r, u32 psm, u32 bw, RGBAMask rgba);
	GSVector4i GetDirtyRect(GIFRegTEX0 TEX0, bool align) const;
};

/* The rectangles of a target that local memory has newer data for, in
 * the order they were marked; a plain growable array, walked and indexed
 * by the texture cache. */
class GSDirtyRectList
{
	gs_array_t m_list;

public:
	GSDirtyRectList() { gs_array_init(&m_list, sizeof(GSDirtyRect)); }
	~GSDirtyRectList() { gs_array_free(&m_list); }
	/* (a target's clone, for a snapshot, keeps its own copy) */
	GSDirtyRectList(const GSDirtyRectList& other)
	{
		gs_array_init(&m_list, sizeof(GSDirtyRect));
		copy(other);
	}
	GSDirtyRectList& operator=(const GSDirtyRectList& other)
	{
		if (this != &other)
			copy(other);
		return *this;
	}

	__fi size_t size() const { return m_list.count; }
	__fi bool empty() const { return m_list.count == 0; }
	__fi void clear() { m_list.count = 0; }
	__fi GSDirtyRect* begin() { return GS_ARRAY_ITEMS(&m_list, GSDirtyRect); }
	__fi GSDirtyRect* end() { return begin() + m_list.count; }
	__fi const GSDirtyRect* begin() const { return GS_ARRAY_ITEMS(&m_list, const GSDirtyRect); }
	__fi const GSDirtyRect* end() const { return begin() + m_list.count; }
	__fi GSDirtyRect& operator[](size_t i) { return GS_ARRAY_AT(&m_list, GSDirtyRect, i); }
	__fi const GSDirtyRect& operator[](size_t i) const { return GS_ARRAY_AT(&m_list, const GSDirtyRect, i); }
	__fi void push_back(const GSDirtyRect& r)
	{
		GSDirtyRect* slot = static_cast<GSDirtyRect*>(gs_array_push(&m_list));
		if (!slot)
			abort();
		*slot = r;
	}
	__fi void erase(size_t i) { gs_array_erase(&m_list, i); }
	void copy(const GSDirtyRectList& other)
	{
		if (other.m_list.count && !gs_array_reserve(&m_list, other.m_list.count))
			abort();
		if (other.m_list.count)
			memcpy(m_list.items, other.m_list.items, other.m_list.count * sizeof(GSDirtyRect));
		m_list.count = other.m_list.count;
	}
	/* This list takes the other's rectangles; the other is left empty. */
	__fi void take(GSDirtyRectList& other)
	{
		gs_array_t mine = m_list;
		m_list = other.m_list;
		other.m_list = mine;
		other.m_list.count = 0;
	}

	GSVector4i GetTotalRect(GIFRegTEX0 TEX0, const GSVector2i& size) const;
	u32 GetDirtyChannels();
	GSVector4i GetDirtyRect(size_t index, GIFRegTEX0 TEX0, const GSVector4i& clamp, bool align) const;
};
