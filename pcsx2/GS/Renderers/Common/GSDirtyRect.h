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

#ifndef GS_DIRTY_RECT_H
#define GS_DIRTY_RECT_H

#include <stdlib.h>
#include <string.h>

#include "../../GSRegs.h"
#include "../../GSPsm.h"
#include "../../gs_vector.h"

typedef union RGBAMask
{
	u32 _u32;
	struct
	{
		u32 r : 1;
		u32 g : 1;
		u32 b : 1;
		u32 a : 1;
	} c;
} RGBAMask;

/* A rectangle of a target that local memory has newer data for, in the
 * format and width it was written with, and the channels written. The
 * rectangle is the same 16 bytes either way; C++ takes it as GSVector4i. */
typedef struct gs_dirty_rect
{
#ifdef __cplusplus
	GSVector4i r;
#else
	gs_vec4i r;
#endif
	u32 psm;
	u32 bw;
	RGBAMask rgba;
} gs_dirty_rect;

#ifdef __cplusplus
extern "C" {
#endif

/* The rectangle in tex_psm's pixels, aligned out to its blocks if asked */
gs_vec4i gs_dirty_rect_get(const gs_dirty_rect* d, u32 tex_psm, int align);
/* The block-aligned union of n of them, within w x h */
gs_vec4i gs_dirty_total_rect(const gs_dirty_rect* d, size_t n, u32 tex_psm, int w, int h);
/* The channels any of n of them wrote */
u32 gs_dirty_channels(const gs_dirty_rect* d, size_t n);
/* One, aligned out to tex_psm's blocks if asked, within clamp */
gs_vec4i gs_dirty_rect_clamped(const gs_dirty_rect* d, u32 tex_psm, gs_vec4i clamp, int align);

#ifdef __cplusplus
}

#include "GSArray.h"

struct GSDirtyRect : gs_dirty_rect
{
	GSDirtyRect()
	{
		r = GSVector4i(gs_v4i_zero());
		psm = PSMCT32;
		bw = 1;
		rgba._u32 = 0;
	}
	GSDirtyRect(const GSVector4i& r_, u32 psm_, u32 bw_, RGBAMask rgba_)
	{
		r = r_;
		psm = psm_;
		bw = bw_;
		rgba = rgba_;
	}
	GSVector4i GetDirtyRect(GIFRegTEX0 TEX0, bool align) const
	{
		return GSVector4i(gs_dirty_rect_get(this, TEX0.PSM, align));
	}
};

/* The rectangles of a target that local memory has newer data for, in
 * the order they were marked, walked and indexed by the texture cache.
 * The first INLINE_COUNT live in the list itself (targets come from a
 * pool, so a new one marked dirty allocates nothing); past them they
 * move to the heap. */
class GSDirtyRectList
{
	enum { INLINE_COUNT = 16 };

	GSDirtyRect* m_items;
	size_t m_count;
	size_t m_capacity;
	/* (bytes, not GSDirtyRects: nothing to construct) */
	alignas(16) unsigned char m_inline[INLINE_COUNT * sizeof(GSDirtyRect)];

	__fi GSDirtyRect* inline_items() { return reinterpret_cast<GSDirtyRect*>(m_inline); }
	__fi bool on_heap() const { return m_items != reinterpret_cast<const GSDirtyRect*>(m_inline); }
	void reserve(size_t n)
	{
		size_t capacity = m_capacity;
		GSDirtyRect* grown;
		if (n <= capacity)
			return;
		while (capacity < n)
			capacity *= 2;
		grown = static_cast<GSDirtyRect*>(malloc(capacity * sizeof(GSDirtyRect)));
		if (!grown)
			abort();
		memcpy(static_cast<void*>(grown), m_items, m_count * sizeof(GSDirtyRect));
		if (on_heap())
			free(m_items);
		m_items = grown;
		m_capacity = capacity;
	}
	__fi void reset()
	{
		m_items = inline_items();
		m_count = 0;
		m_capacity = INLINE_COUNT;
	}

public:
	GSDirtyRectList() { reset(); }
	~GSDirtyRectList()
	{
		if (on_heap())
			free(m_items);
	}
	/* (a target's clone, for a snapshot, keeps its own copy) */
	GSDirtyRectList(const GSDirtyRectList& other)
	{
		reset();
		copy(other);
	}
	GSDirtyRectList& operator=(const GSDirtyRectList& other)
	{
		if (this != &other)
			copy(other);
		return *this;
	}

	__fi size_t size() const { return m_count; }
	__fi bool empty() const { return m_count == 0; }
	__fi void clear() { m_count = 0; }
	__fi GSDirtyRect* begin() { return m_items; }
	__fi GSDirtyRect* end() { return m_items + m_count; }
	__fi const GSDirtyRect* begin() const { return m_items; }
	__fi const GSDirtyRect* end() const { return m_items + m_count; }
	__fi GSDirtyRect& operator[](size_t i) { return m_items[i]; }
	__fi const GSDirtyRect& operator[](size_t i) const { return m_items[i]; }
	__fi void push_back(const GSDirtyRect& r)
	{
		if (m_count == m_capacity)
			reserve(m_count + 1);
		m_items[m_count++] = r;
	}
	__fi void erase(size_t i)
	{
		memmove(static_cast<void*>(m_items + i), m_items + i + 1, (m_count - i - 1) * sizeof(GSDirtyRect));
		m_count--;
	}
	void copy(const GSDirtyRectList& other)
	{
		reserve(other.m_count);
		if (other.m_count)
			memcpy(static_cast<void*>(m_items), other.m_items, other.m_count * sizeof(GSDirtyRect));
		m_count = other.m_count;
	}
	/* This list takes the other's rectangles; the other is left empty. A
	 * heap block moves across rather than being copied, and this list's
	 * own goes to the other. */
	void take(GSDirtyRectList& other)
	{
		if (!other.on_heap())
		{
			copy(other);
			other.m_count = 0;
			return;
		}
		GSDirtyRect* const theirs = other.m_items;
		const size_t theirs_count = other.m_count;
		const size_t theirs_capacity = other.m_capacity;
		if (on_heap())
		{
			other.m_items = m_items;
			other.m_capacity = m_capacity;
		}
		else
			other.reset();
		other.m_count = 0;
		m_items = theirs;
		m_count = theirs_count;
		m_capacity = theirs_capacity;
	}

	GSVector4i GetTotalRect(GIFRegTEX0 TEX0, const GSVector2i& size) const
	{
		return GSVector4i(gs_dirty_total_rect(begin(), m_count, TEX0.PSM, size.x, size.y));
	}
	u32 GetDirtyChannels() const { return gs_dirty_channels(begin(), m_count); }
	GSVector4i GetDirtyRect(size_t index, GIFRegTEX0 TEX0, const GSVector4i& clamp, bool align) const
	{
		return GSVector4i(gs_dirty_rect_clamped(&(*this)[index], TEX0.PSM, clamp, align));
	}
};

#endif

#endif
