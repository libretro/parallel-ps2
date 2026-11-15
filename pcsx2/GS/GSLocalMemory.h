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

#include <stddef.h>

#include <retro_inline.h>

#include "GSTables.h"
#include "GSOffset.h"
#include "GSPsm.h"
#include "GSRegs.h"
#include "Renderers/Common/GSFlatMap.h"
#include "GSLocalMemoryMaps.h"

/*
 * The GS's 4MB of local memory, in C: the memory (mapped four times over,
 * so a run off the end lands back at the start, as on the console), the
 * maps made of it, and the palette paletted texels are read through. Its
 * pixels and texels are read and written one at a time here, by address
 * (from the swizzle, GSOffset.h) or by place; gs_lm_pixel has each
 * format's readers and writers. The C++ GSLocalMemory below derives from
 * it and keeps the transfers that move a rectangle at a time with SIMD.
 */

#define GS_LOCAL_MEMORY_SIZE (1024 * 1024 * 4)

typedef struct gs_local_memory
{
	u8* m_vm8;
	/* u64 key -> GSPixelOffset4* / GSPage2TileMap*, in GSFlatMaps: made
	 * once each, kept until the local memory goes */
	gs_flat_map_t m_po4map;
	gs_flat_map_t m_p2tmap;
	/* the palette as read out (GSClut.h's buff32): 8- and 4-bit texels */
	const u32* m_clut32;
} gs_local_memory;

/* A format's readers and writers, of one pixel or texel: by place (x, y in
 * a buffer at bp, bw) or by the address its swizzle gives */
typedef u32 (*gs_lm_read_pixel_fn)(const gs_local_memory* mem, int x, int y, u32 bp, u32 bw);
typedef u32 (*gs_lm_read_pixel_addr_fn)(const gs_local_memory* mem, u32 addr);
typedef void (*gs_lm_write_pixel_fn)(gs_local_memory* mem, int x, int y, u32 c, u32 bp, u32 bw);
typedef void (*gs_lm_write_pixel_addr_fn)(gs_local_memory* mem, u32 addr, u32 c);
typedef u32 (*gs_lm_read_texel_fn)(const gs_local_memory* mem, int x, int y, const GIFRegTEX0* TEX0,
	const GIFRegTEXA* TEXA);
typedef u32 (*gs_lm_read_texel_addr_fn)(const gs_local_memory* mem, u32 addr, const GIFRegTEXA* TEXA);

typedef struct gs_lm_pixel_fns
{
	gs_lm_read_pixel_fn rp;        /* a pixel as stored */
	gs_lm_read_pixel_addr_fn rpa;  /* the same, by address */
	gs_lm_write_pixel_fn wp;
	gs_lm_write_pixel_addr_fn wpa;
	gs_lm_read_texel_fn rt;        /* a texel as sampled: 32-bit colour */
	gs_lm_read_texel_addr_fn rta;
	gs_lm_write_pixel_addr_fn wfa; /* a 32-bit colour written as a frame */
} gs_lm_pixel_fns;

#ifdef __cplusplus
extern "C" {
#endif

/* Each of the 64 formats' (the formats the GS has none of read and write
 * as 32 bits) */
extern const gs_lm_pixel_fns gs_lm_pixel[64];

/* The memory, zeroed, and the maps empty; zero if it cannot be had */
int gs_local_memory_init(gs_local_memory* mem);
void gs_local_memory_deinit(gs_local_memory* mem);

/* A rectangle (left, top, right, bottom) of the buffer off names read as
 * 32-bit texels into dst: whole blocks by the transfers, the edges texel
 * by texel */
void gs_local_memory_read_texture(gs_local_memory* mem, const gs_offset* off, const int r[4], u8* dst, int dstpitch,
	const GIFRegTEXA* TEXA);

/* Whether each edge of r falls on a page of psm: a byte mask of the four,
 * four bits each, as SIMD compares give (0xffff: all) */
u32 gs_lm_page_aligned_mask(u32 psm, const int r[4]);
/* The rectangle (left, top, right, bottom) of the page offset_bp is in,
 * counted in pages from base_bp in a buffer bw wide */
void gs_lm_rect_for_page_offset(u32 base_bp, u32 offset_bp, u32 bw, u32 psm, int out[4]);

/* The 4MB mapped repeat times in a row (GS.cpp) */
void* GSAllocateWrappedMemory(size_t size, size_t repeat);
void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat);

/* What the C side asks of the C++ transfers (GSLocalMemoryMultiISA.cpp):
 * this CPU's readers and writers of rectangles put in the format table,
 * and a rectangle of whole blocks read */
void gs_local_memory_populate_transfers(void);
void gs_local_memory_read_texture_blocks(gs_local_memory* mem, const gs_offset* off, const int r[4], u8* dst,
	int dstpitch, const GIFRegTEXA* TEXA);

#ifdef __cplusplus
}
#endif

/* ---- one pixel, by address ---- */

#define GS_LM_VM32(mem) ((u32*)(mem)->m_vm8)
#define GS_LM_VM16(mem) ((u16*)(mem)->m_vm8)

static INLINE u32 gs_lm_read_pixel32(const gs_local_memory* mem, u32 addr) { return GS_LM_VM32(mem)[addr]; }
static INLINE u32 gs_lm_read_pixel24(const gs_local_memory* mem, u32 addr) { return GS_LM_VM32(mem)[addr] & 0x00ffffff; }
static INLINE u32 gs_lm_read_pixel16(const gs_local_memory* mem, u32 addr) { return (u32)GS_LM_VM16(mem)[addr]; }
static INLINE u32 gs_lm_read_pixel8(const gs_local_memory* mem, u32 addr) { return (u32)mem->m_vm8[addr]; }
static INLINE u32 gs_lm_read_pixel4(const gs_local_memory* mem, u32 addr)
{
	return (mem->m_vm8[addr >> 1] >> ((addr & 1) << 2)) & 0x0f;
}
static INLINE u32 gs_lm_read_pixel8h(const gs_local_memory* mem, u32 addr) { return GS_LM_VM32(mem)[addr] >> 24; }
static INLINE u32 gs_lm_read_pixel4hl(const gs_local_memory* mem, u32 addr) { return (GS_LM_VM32(mem)[addr] >> 24) & 0x0f; }
static INLINE u32 gs_lm_read_pixel4hh(const gs_local_memory* mem, u32 addr) { return (GS_LM_VM32(mem)[addr] >> 28) & 0x0f; }

static INLINE u32 gs_lm_read_frame24(const gs_local_memory* mem, u32 addr)
{
	return 0x80000000 | (GS_LM_VM32(mem)[addr] & 0xffffff);
}
static INLINE u32 gs_lm_read_frame16(const gs_local_memory* mem, u32 addr)
{
	const u32 c = (u32)GS_LM_VM16(mem)[addr];
	return ((c & 0x8000) << 16) | ((c & 0x7c00) << 9) | ((c & 0x03e0) << 6) | ((c & 0x001f) << 3);
}

static INLINE void gs_lm_write_pixel32(gs_local_memory* mem, u32 addr, u32 c) { GS_LM_VM32(mem)[addr] = c; }
static INLINE void gs_lm_write24_at(u32* p, u32 c) { *p = (*p & 0xff000000) | (c & 0x00ffffff); }
static INLINE void gs_lm_write_pixel24(gs_local_memory* mem, u32 addr, u32 c) { gs_lm_write24_at(GS_LM_VM32(mem) + addr, c); }
static INLINE void gs_lm_write_pixel16(gs_local_memory* mem, u32 addr, u32 c) { GS_LM_VM16(mem)[addr] = (u16)c; }
static INLINE void gs_lm_write_pixel8(gs_local_memory* mem, u32 addr, u32 c) { mem->m_vm8[addr] = (u8)c; }
static INLINE void gs_lm_write_pixel4(gs_local_memory* mem, u32 addr, u32 c)
{
	const int shift = (addr & 1) << 2;
	addr >>= 1;
	mem->m_vm8[addr] = (u8)((mem->m_vm8[addr] & (0xf0 >> shift)) | ((c & 0x0f) << shift));
}
static INLINE void gs_lm_write8h_at(u32* p, u32 c) { *p = (*p & 0x00ffffff) | (c << 24); }
static INLINE void gs_lm_write4hl_at(u32* p, u32 c) { *p = (*p & 0xf0ffffff) | ((c & 0x0f) << 24); }
static INLINE void gs_lm_write4hh_at(u32* p, u32 c) { *p = (*p & 0x0fffffff) | ((c & 0x0f) << 28); }
static INLINE void gs_lm_write_pixel8h(gs_local_memory* mem, u32 addr, u32 c) { gs_lm_write8h_at(GS_LM_VM32(mem) + addr, c); }
static INLINE void gs_lm_write_pixel4hl(gs_local_memory* mem, u32 addr, u32 c) { gs_lm_write4hl_at(GS_LM_VM32(mem) + addr, c); }
static INLINE void gs_lm_write_pixel4hh(gs_local_memory* mem, u32 addr, u32 c) { gs_lm_write4hh_at(GS_LM_VM32(mem) + addr, c); }
static INLINE void gs_lm_write_frame16(gs_local_memory* mem, u32 addr, u32 c)
{
	const u32 rb = c & 0x00f800f8;
	const u32 ga = c & 0x8000f800;
	gs_lm_write_pixel16(mem, addr, (ga >> 16) | (rb >> 9) | (ga >> 6) | (rb >> 3));
}

/* ---- one texel, by address: as the GS samples it, 32-bit colour ---- */

static INLINE u32 gs_lm_expand24(u32 c, const GIFRegTEXA* TEXA)
{
	return (((!TEXA->AEM || (c & 0xffffff)) ? TEXA->TA0 : 0) << 24) | (c & 0xffffff);
}
static INLINE u32 gs_lm_expand16(u16 c, const GIFRegTEXA* TEXA)
{
	return (((c & 0x8000) ? TEXA->TA1 : (!TEXA->AEM || c) ? TEXA->TA0 : 0) << 24)
		| ((c & 0x7c00) << 9)
		| ((c & 0x03e0) << 6)
		| ((c & 0x001f) << 3);
}

static INLINE u32 gs_lm_read_texel32(const gs_local_memory* mem, u32 addr, const GIFRegTEXA* TEXA)
{
	(void)TEXA;
	return GS_LM_VM32(mem)[addr];
}
static INLINE u32 gs_lm_read_texel24(const gs_local_memory* mem, u32 addr, const GIFRegTEXA* TEXA)
{
	return gs_lm_expand24(GS_LM_VM32(mem)[addr], TEXA);
}
static INLINE u32 gs_lm_read_texel16(const gs_local_memory* mem, u32 addr, const GIFRegTEXA* TEXA)
{
	return gs_lm_expand16(GS_LM_VM16(mem)[addr], TEXA);
}
static INLINE u32 gs_lm_read_texel8(const gs_local_memory* mem, u32 addr, const GIFRegTEXA* TEXA)
{
	(void)TEXA;
	return mem->m_clut32[gs_lm_read_pixel8(mem, addr)];
}
static INLINE u32 gs_lm_read_texel4(const gs_local_memory* mem, u32 addr, const GIFRegTEXA* TEXA)
{
	(void)TEXA;
	return mem->m_clut32[gs_lm_read_pixel4(mem, addr)];
}
static INLINE u32 gs_lm_read_texel8h(const gs_local_memory* mem, u32 addr, const GIFRegTEXA* TEXA)
{
	(void)TEXA;
	return mem->m_clut32[gs_lm_read_pixel8h(mem, addr)];
}
static INLINE u32 gs_lm_read_texel4hl(const gs_local_memory* mem, u32 addr, const GIFRegTEXA* TEXA)
{
	(void)TEXA;
	return mem->m_clut32[gs_lm_read_pixel4hl(mem, addr)];
}
static INLINE u32 gs_lm_read_texel4hh(const gs_local_memory* mem, u32 addr, const GIFRegTEXA* TEXA)
{
	(void)TEXA;
	return mem->m_clut32[gs_lm_read_pixel4hh(mem, addr)];
}

#ifdef __cplusplus

#include "GSVector.h"
#include "GSClut.h"
#include "MultiISA.h"

class GSOffset;

/* The C swizzle (GSOffset.h), with the block number and pixel address of
 * a pixel at a given bp and bw */
class GSSwizzleInfo : public gs_swizzle_info
{
public:
	GSSwizzleInfo() = default;
	constexpr GSSwizzleInfo(const gs_swizzle_info& s) : gs_swizzle_info(s) {}

	u8 pageShiftX() const { return page_shift_x; }
	u8 pageShiftY() const { return page_shift_y; }

	u32 bn(int x, int y, u32 bp, u32 bw) const;
	u32 pa(int x, int y, u32 bp, u32 bw) const;
};

/* A buffer in local memory (GSOffset.h's gs_offset), with loops over its
 * pixels, blocks and pages */
class GSOffset : public gs_offset
{
public:
	GSOffset() = default;
	GSOffset(const gs_offset& o) : gs_offset(o) {}
	GSOffset(const gs_swizzle_info& s, u32 bp, u32 bw, u32 psm)
		: gs_offset(gs_offset_make(&s, bp, bw, psm))
	{
	}
	/// Help the optimizer by using this method instead of GSLocalMemory::GetOffset when the PSM is known
	static GSOffset fromKnownPSM(u32 bp, u32 bw, GS_PSM psm);

	u32 bp()  const { return gs_offset::bp; }
	u32 bw()  const { return gs_offset_bw(this); }
	u32 psm() const { return gs_offset::psm; }
	int blockShiftX() const { return swz.block_shift_x; }
	int blockShiftY() const { return swz.block_shift_y; }

	/// Block numbers in a scan, x then y
	class BNHelper
	{
		gs_bn_helper h;

	public:
		BNHelper(const GSOffset& off, int x, int y) { gs_bn_init(&h, &off, x, y); }
		int blkX() const { return h.blk_x; }
		int blkY() const { return h.blk_y; }
		void nextBlockX() { gs_bn_next_x(&h); }
		void nextBlockY() { gs_bn_next_y(&h); }
		u32 valueNoWrap() const { return gs_bn_value_nowrap(&h); }
		u32 value() const { return gs_bn_value(&h); }
	};

	u32 bn(int x, int y) const { return gs_offset_bn(this, x, y); }
	u32 bnNoWrap(int x, int y) const { return gs_offset_bn_nowrap(this, x, y); }
	BNHelper bnMulti(int x, int y) const { return BNHelper(*this, x, y); }

	static bool isAligned(const GSVector4i& r, const GSVector2i& mask)
	{
		return gs_rect_is_aligned(r.left, r.top, r.right, r.bottom, mask.x, mask.y) != 0;
	}
	bool isBlockAligned(const GSVector4i& r) const
	{
		return gs_rect_is_aligned(r.left, r.top, r.right, r.bottom, swz.block_mask_x, swz.block_mask_y) != 0;
	}
	bool isPageAligned(const GSVector4i& r) const
	{
		return gs_rect_is_aligned(r.left, r.top, r.right, r.bottom, swz.page_mask_x, swz.page_mask_y) != 0;
	}

	/// Loop over all the blocks in the given rect, calling `fn` on each
	template <typename Fn>
	void loopBlocks(const GSVector4i& rect, Fn&& fn) const
	{
		gs_bn_helper h;
		int right  = (rect.right + swz.block_mask_x) >> swz.block_shift_x;
		int bottom = (rect.bottom + swz.block_mask_y) >> swz.block_shift_y;

		for (gs_bn_init(&h, this, rect.left, rect.top); h.blk_y < bottom; gs_bn_next_y(&h))
			for (; h.blk_x < right; gs_bn_next_x(&h))
				fn(gs_bn_value(&h));
	}

	int pixelAddressZeroXRaw(int y) const { return gs_offset_pa_zero_x(this, y); }

	/// Pixel addresses along one row
	class PAHelper
	{
		gs_pa_helper h;

	public:
		PAHelper() = default;
		PAHelper(const GSOffset& off, int x, int y) { gs_pa_init(&h, &off, x, y); }
		u32 value(int x) const { return gs_pa_value(&h, x); }
	};

	u32 pa(int x, int y) const { return gs_offset_pa(this, x, y); }
	PAHelper paMulti(int x, int y) const { return PAHelper(*this, x, y); }

	/// Loop over the pixels in the given rectangle
	/// Fn should be void(*)(VM*, Src*)
	template <typename VM, typename Src, typename Fn>
	void loopPixels(const GSVector4i& r, VM* RESTRICT vm, Src* RESTRICT px, int pitch, Fn&& fn) const
	{
		px -= r.left;

		for (int y = r.top; y < r.bottom; y++, px = reinterpret_cast<Src*>(reinterpret_cast<u8*>(px) + pitch))
		{
			gs_pa_helper pa;
			gs_pa_init(&pa, this, 0, y);
			for (int x = r.left; x < r.right; x++)
				fn(&vm[gs_pa_value(&pa, x)], px + x);
		}
	}

	/// The pages a rect touches (gs_page_looper)
	class PageLooper : public gs_page_looper
	{
	public:
		PageLooper() = default;
		PageLooper(const gs_page_looper& l) : gs_page_looper(l) {}
		/// Loop over pages, fn can return `false` to break the loop
		/// Fn: bool(*)(u32)
		template <typename Fn>
		void loopPagesWithBreak(const Fn& fn) const
		{
			gs_page_rows rows;
			u32 start, end;
			gs_page_rows_init(&rows, this);

			if (unlikely(slow))
			{
				u32 touched[GS_MAX_PAGES / 32] = {};
				while (gs_page_rows_next(&rows, &start, &end))
				{
					for (u32 pos = start; pos < end; pos++)
					{
						u32 page = pos % GS_MAX_PAGES;
						u32 idx = page / 32;
						u32 mask = 1 << (page % 32);
						if (touched[idx] & mask)
							continue;
						if (!fn(page))
							return;
						touched[idx] |= mask;
					}
				}
			}
			else
			{
				while (gs_page_rows_next(&rows, &start, &end))
					for (u32 pos = start; pos < end; pos++)
						if (!fn(pos % GS_MAX_PAGES))
							return;
			}
		}

		/// Loop over pages, calling `fn` on each one with no option to break
		/// Fn: void(*)(u32)
		template <typename Fn>
		void loopPages(const Fn& fn) const
		{
			loopPagesWithBreak([&fn](u32 page) { fn(page); return true; });
		}
	};

	/// Get an object for looping over the pages in the given rect
	PageLooper pageLooperForRect(const GSVector4i& rect) const
	{
		PageLooper out;
		gs_offset_page_looper(&out, this, rect.left, rect.top, rect.right, rect.bottom);
		return out;
	}

	/// Loop over all the pages in the given rect, calling `fn` on each
	template <typename Fn>
	void loopPages(const GSVector4i& rect, const Fn& fn) const
	{
		pageLooperForRect(rect).loopPages(fn);
	}

	/// The same buffer, its shape taken from `swz`, which the compiler
	/// can fold where the swizzle is known
	GSOffset assertSizesMatch(const gs_swizzle_info& s) const
	{
		GSOffset o = *this;
		o.swz.page_mask_x    = s.page_mask_x;
		o.swz.page_mask_y    = s.page_mask_y;
		o.swz.block_mask_x   = s.block_mask_x;
		o.swz.block_mask_y   = s.block_mask_y;
		o.swz.pixel_row_mask = s.pixel_row_mask;
		o.swz.page_shift_x   = s.page_shift_x;
		o.swz.page_shift_y   = s.page_shift_y;
		o.swz.block_shift_x  = s.block_shift_x;
		o.swz.block_shift_y  = s.block_shift_y;
		return o;
	}
};

inline u32 GSSwizzleInfo::bn(int x, int y, u32 bp, u32 bw) const
{
	return gs_swizzle_bn(this, x, y, bp, bw);
}

inline u32 GSSwizzleInfo::pa(int x, int y, u32 bp, u32 bw) const
{
	return gs_swizzle_pa(this, x, y, bp, bw);
}

class GSLocalMemory;
MULTI_ISA_DEF(class GSLocalMemoryFunctions;)
MULTI_ISA_DEF(void GSLocalMemoryPopulateFunctions();)

/* The C local memory (above), with the C++ renderers' names, the palette
 * it reads paletted texels through, and the SIMD transfers per format */
class GSLocalMemory final : public gs_local_memory, public GSAlignedClass<32>
{
	MULTI_ISA_FRIEND(GSLocalMemoryFunctions)

public:
	typedef u32 (*pixelAddress)(int x, int y, u32 bp, u32 bw);
	typedef void (*writeImage)(GSLocalMemory& mem, int& tx, int& ty, const u8* src, int len, GIFRegBITBLTBUF& BITBLTBUF, GIFRegTRXPOS& TRXPOS, GIFRegTRXREG& TRXREG);
	typedef void (*readImage)(const GSLocalMemory& mem, int& tx, int& ty, u8* dst, int len, GIFRegBITBLTBUF& BITBLTBUF, GIFRegTRXPOS& TRXPOS, GIFRegTRXREG& TRXREG);
	typedef void (*readTexture)(GSLocalMemory& mem, const GSOffset& off, const GSVector4i& r, u8* dst, int dstpitch, const GIFRegTEXA& TEXA);
	typedef void (*readTextureBlock)(const GSLocalMemory& mem, u32 bp, u8* dst, int dstpitch, const GIFRegTEXA& TEXA);

	enum PSM_FMT
	{
		PSM_FMT_32,
		PSM_FMT_24,
		PSM_FMT_16
	};

	/* A mode's attributes (GSPsm.h's gs_psm), and its transfers of
	 * rectangles (one pixel at a time: gs_lm_pixel) */
	struct alignas(128) psm_t : gs_psm_attr
	{
		writeImage wi;
		readImage ri;
		readTexture rtx, rtxP;
		readTextureBlock rtxb, rtxbP;
	};

	static psm_t m_psm[64];
	static readImage m_readImageX;

	static constexpr int m_vmsize = GS_LOCAL_MEMORY_SIZE;

	GSClut m_clut;

public:
	static constexpr GSSwizzleInfo swizzle32{gs_swizzle32};
	static constexpr GSSwizzleInfo swizzle32Z{gs_swizzle32Z};
	static constexpr GSSwizzleInfo swizzle16{gs_swizzle16};
	static constexpr GSSwizzleInfo swizzle16S{gs_swizzle16S};
	static constexpr GSSwizzleInfo swizzle16Z{gs_swizzle16Z};
	static constexpr GSSwizzleInfo swizzle16SZ{gs_swizzle16SZ};
	static constexpr GSSwizzleInfo swizzle8{gs_swizzle8};
	static constexpr GSSwizzleInfo swizzle4{gs_swizzle4};

protected:
	__forceinline static u32 Expand24To32(u32 c, const GIFRegTEXA& TEXA) { return gs_lm_expand24(c, &TEXA); }
	__forceinline static u32 Expand16To32(u16 c, const GIFRegTEXA& TEXA) { return gs_lm_expand16(c, &TEXA); }

	// TODO

	friend class GSClut;

	//

public:
	GSLocalMemory()
		: m_clut(this)
	{
		gs_local_memory_init(this);
		m_clut32 = m_clut;
	}
	~GSLocalMemory() { gs_local_memory_deinit(this); }
	GSLocalMemory(const GSLocalMemory&) = delete;
	GSLocalMemory& operator=(const GSLocalMemory&) = delete;

	__forceinline u8* vm8() const { return m_vm8; }
	__forceinline u16* vm16() const { return reinterpret_cast<u16*>(m_vm8); }
	__forceinline u32* vm32() const { return reinterpret_cast<u32*>(m_vm8); }

	GSOffset GetOffset(u32 bp, u32 bw, u32 psm) const
	{
		return GSOffset(m_psm[psm].info, bp, bw, psm);
	}
	GSPixelOffset4* GetPixelOffset4(const GIFRegFRAME& FRAME, const GIFRegZBUF& ZBUF)
	{
		return gs_pixel_offset4_get(&m_po4map, &FRAME, &ZBUF);
	}
	GSPage2TileMap* GetPage2TileMap(const GIFRegTEX0& TEX0) { return gs_page2tile_map_get(&m_p2tmap, &TEX0); }
	static bool HasOverlap(u32 src_bp, u32 src_bw, u32 src_psm, GSVector4i src_rect, u32 dst_bp, u32 dst_bw, u32 dst_psm, GSVector4i dst_rect)
	{
		const int s[4] = {src_rect.x, src_rect.y, src_rect.z, src_rect.w};
		const int d[4] = {dst_rect.x, dst_rect.y, dst_rect.z, dst_rect.w};
		return gs_psm_has_overlap(src_bp, src_bw, src_psm, s, dst_bp, dst_bw, dst_psm, d) != 0;
	}
	static u32 IsPageAlignedMasked(u32 psm, const GSVector4i& rc)
	{
		const int r[4] = {rc.x, rc.y, rc.z, rc.w};
		return gs_lm_page_aligned_mask(psm, r);
	}
	static bool IsPageAligned(u32 psm, const GSVector4i& rc) { return IsPageAlignedMasked(psm, rc) == 0xffff; }
	static u32 GetStartBlockAddress(u32 bp, u32 bw, u32 psm, GSVector4i rect)
	{
		return gs_psm_start_block(bp, bw, psm, rect.x, rect.y);
	}
	static u32 GetEndBlockAddress(u32 bp, u32 bw, u32 psm, GSVector4i rect)
	{
		return gs_psm_end_block(bp, bw, psm, rect.z, rect.w);
	}
	static u32 GetUnwrappedEndBlockAddress(u32 bp, u32 bw, u32 psm, GSVector4i rect)
	{
		return gs_psm_end_block_unwrapped(bp, bw, psm, rect.z, rect.w);
	}
	static GSVector4i GetRectForPageOffset(u32 base_bp, u32 offset_bp, u32 bw, u32 psm)
	{
		int r[4];
		gs_lm_rect_for_page_offset(base_bp, offset_bp, bw, psm, r);
		return GSVector4i(r[0], r[1], r[2], r[3]);
	}

	// address

	static u32 BlockNumber32(int x, int y, u32 bp, u32 bw)
	{
		return swizzle32.bn(x, y, bp, bw);
	}

	static u32 BlockNumber16(int x, int y, u32 bp, u32 bw)
	{
		return swizzle16.bn(x, y, bp, bw);
	}

	static u32 BlockNumber16S(int x, int y, u32 bp, u32 bw)
	{
		return swizzle16S.bn(x, y, bp, bw);
	}

	static u32 BlockNumber8(int x, int y, u32 bp, u32 bw)
	{
		return swizzle8.bn(x, y, bp, bw);
	}

	static u32 BlockNumber4(int x, int y, u32 bp, u32 bw)
	{
		return swizzle4.bn(x, y, bp, bw);
	}

	static u32 BlockNumber32Z(int x, int y, u32 bp, u32 bw)
	{
		return swizzle32Z.bn(x, y, bp, bw);
	}

	static u32 BlockNumber16Z(int x, int y, u32 bp, u32 bw)
	{
		return swizzle16Z.bn(x, y, bp, bw);
	}

	static u32 BlockNumber16SZ(int x, int y, u32 bp, u32 bw)
	{
		return swizzle16SZ.bn(x, y, bp, bw);
	}

	u8* BlockPtr(u32 bp) const
	{
		return &m_vm8[(bp % GS_MAX_BLOCKS) << 8];
	}

	u8* BlockPtr32(int x, int y, u32 bp, u32 bw) const
	{
		return &m_vm8[BlockNumber32(x, y, bp, bw) << 8];
	}

	u8* BlockPtr16(int x, int y, u32 bp, u32 bw) const
	{
		return &m_vm8[BlockNumber16(x, y, bp, bw) << 8];
	}

	u8* BlockPtr16S(int x, int y, u32 bp, u32 bw) const
	{
		return &m_vm8[BlockNumber16S(x, y, bp, bw) << 8];
	}

	u8* BlockPtr8(int x, int y, u32 bp, u32 bw) const
	{
		return &m_vm8[BlockNumber8(x, y, bp, bw) << 8];
	}

	u8* BlockPtr4(int x, int y, u32 bp, u32 bw) const
	{
		return &m_vm8[BlockNumber4(x, y, bp, bw) << 8];
	}

	u8* BlockPtr32Z(int x, int y, u32 bp, u32 bw) const
	{
		return &m_vm8[BlockNumber32Z(x, y, bp, bw) << 8];
	}

	u8* BlockPtr16Z(int x, int y, u32 bp, u32 bw) const
	{
		return &m_vm8[BlockNumber16Z(x, y, bp, bw) << 8];
	}

	u8* BlockPtr16SZ(int x, int y, u32 bp, u32 bw) const
	{
		return &m_vm8[BlockNumber16SZ(x, y, bp, bw) << 8];
	}

	static __forceinline u32 PixelAddress32(int x, int y, u32 bp, u32 bw)
	{
		return swizzle32.pa(x, y, bp, bw);
	}

	static __forceinline u32 PixelAddress16(int x, int y, u32 bp, u32 bw)
	{
		return swizzle16.pa(x, y, bp, bw);
	}

	static __forceinline u32 PixelAddress16S(int x, int y, u32 bp, u32 bw)
	{
		return swizzle16S.pa(x, y, bp, bw);
	}

	static __forceinline u32 PixelAddress8(int x, int y, u32 bp, u32 bw)
	{
		return swizzle8.pa(x, y, bp, bw);
	}

	static __forceinline u32 PixelAddress4(int x, int y, u32 bp, u32 bw)
	{
		return swizzle4.pa(x, y, bp, bw);
	}

	static __forceinline u32 PixelAddress32Z(int x, int y, u32 bp, u32 bw)
	{
		return swizzle32Z.pa(x, y, bp, bw);
	}

	static __forceinline u32 PixelAddress16Z(int x, int y, u32 bp, u32 bw)
	{
		return swizzle16Z.pa(x, y, bp, bw);
	}

	static __forceinline u32 PixelAddress16SZ(int x, int y, u32 bp, u32 bw)
	{
		return swizzle16SZ.pa(x, y, bp, bw);
	}

	// pixel R/W

	__forceinline u32 ReadPixel32(u32 addr) const
	{
		return gs_lm_read_pixel32(this, addr);
	}

	__forceinline u32 ReadPixel24(u32 addr) const
	{
		return gs_lm_read_pixel24(this, addr);
	}

	__forceinline u32 ReadPixel16(u32 addr) const
	{
		return gs_lm_read_pixel16(this, addr);
	}

	__forceinline u32 ReadPixel8(u32 addr) const
	{
		return gs_lm_read_pixel8(this, addr);
	}

	__forceinline u32 ReadPixel4(u32 addr) const
	{
		return gs_lm_read_pixel4(this, addr);
	}

	__forceinline u32 ReadPixel8H(u32 addr) const
	{
		return gs_lm_read_pixel8h(this, addr);
	}

	__forceinline u32 ReadPixel4HL(u32 addr) const
	{
		return gs_lm_read_pixel4hl(this, addr);
	}

	__forceinline u32 ReadPixel4HH(u32 addr) const
	{
		return gs_lm_read_pixel4hh(this, addr);
	}

	__forceinline u32 ReadFrame24(u32 addr) const
	{
		return gs_lm_read_frame24(this, addr);
	}

	__forceinline u32 ReadFrame16(u32 addr) const
	{
		return gs_lm_read_frame16(this, addr);
	}

	__forceinline u32 ReadPixel32(int x, int y, u32 bp, u32 bw) const
	{
		return ReadPixel32(PixelAddress32(x, y, bp, bw));
	}

	__forceinline u32 ReadPixel24(int x, int y, u32 bp, u32 bw) const
	{
		return ReadPixel24(PixelAddress32(x, y, bp, bw));
	}

	__forceinline u32 ReadPixel16(int x, int y, u32 bp, u32 bw) const
	{
		return ReadPixel16(PixelAddress16(x, y, bp, bw));
	}

	__forceinline u32 ReadPixel16S(int x, int y, u32 bp, u32 bw) const
	{
		return ReadPixel16(PixelAddress16S(x, y, bp, bw));
	}

	__forceinline u32 ReadPixel8(int x, int y, u32 bp, u32 bw) const
	{
		return ReadPixel8(PixelAddress8(x, y, bp, bw));
	}

	__forceinline u32 ReadPixel4(int x, int y, u32 bp, u32 bw) const
	{
		return ReadPixel4(PixelAddress4(x, y, bp, bw));
	}

	__forceinline u32 ReadPixel8H(int x, int y, u32 bp, u32 bw) const
	{
		return ReadPixel8H(PixelAddress32(x, y, bp, bw));
	}

	__forceinline u32 ReadPixel4HL(int x, int y, u32 bp, u32 bw) const
	{
		return ReadPixel4HL(PixelAddress32(x, y, bp, bw));
	}

	__forceinline u32 ReadPixel4HH(int x, int y, u32 bp, u32 bw) const
	{
		return ReadPixel4HH(PixelAddress32(x, y, bp, bw));
	}

	__forceinline u32 ReadPixel32Z(int x, int y, u32 bp, u32 bw) const
	{
		return ReadPixel32(PixelAddress32Z(x, y, bp, bw));
	}

	__forceinline u32 ReadPixel24Z(int x, int y, u32 bp, u32 bw) const
	{
		return ReadPixel24(PixelAddress32Z(x, y, bp, bw));
	}

	__forceinline u32 ReadPixel16Z(int x, int y, u32 bp, u32 bw) const
	{
		return ReadPixel16(PixelAddress16Z(x, y, bp, bw));
	}

	__forceinline u32 ReadPixel16SZ(int x, int y, u32 bp, u32 bw) const
	{
		return ReadPixel16(PixelAddress16SZ(x, y, bp, bw));
	}

	__forceinline u32 ReadFrame24(int x, int y, u32 bp, u32 bw) const
	{
		return ReadFrame24(PixelAddress32(x, y, bp, bw));
	}

	__forceinline u32 ReadFrame16(int x, int y, u32 bp, u32 bw) const
	{
		return ReadFrame16(PixelAddress16(x, y, bp, bw));
	}

	__forceinline u32 ReadFrame16S(int x, int y, u32 bp, u32 bw) const
	{
		return ReadFrame16(PixelAddress16S(x, y, bp, bw));
	}

	__forceinline u32 ReadFrame24Z(int x, int y, u32 bp, u32 bw) const
	{
		return ReadFrame24(PixelAddress32Z(x, y, bp, bw));
	}

	__forceinline u32 ReadFrame16Z(int x, int y, u32 bp, u32 bw) const
	{
		return ReadFrame16(PixelAddress16Z(x, y, bp, bw));
	}

	__forceinline u32 ReadFrame16SZ(int x, int y, u32 bp, u32 bw) const
	{
		return ReadFrame16(PixelAddress16SZ(x, y, bp, bw));
	}

	__forceinline void WritePixel32(u32 addr, u32 c)
	{
		gs_lm_write_pixel32(this, addr, c);
	}

	__forceinline static void WritePixel24(u32* addr, u32 c)
	{
		gs_lm_write24_at(addr, c);
	}

	__forceinline void WritePixel24(u32 addr, u32 c)
	{
		gs_lm_write_pixel24(this, addr, c);
	}

	__forceinline void WritePixel16(u32 addr, u32 c)
	{
		gs_lm_write_pixel16(this, addr, c);
	}

	__forceinline void WritePixel8(u32 addr, u32 c)
	{
		gs_lm_write_pixel8(this, addr, c);
	}

	__forceinline void WritePixel4(u32 addr, u32 c)
	{
		gs_lm_write_pixel4(this, addr, c);
	}

	__forceinline static void WritePixel8H(u32* addr, u32 c)
	{
		gs_lm_write8h_at(addr, c);
	}

	__forceinline void WritePixel8H(u32 addr, u32 c)
	{
		gs_lm_write_pixel8h(this, addr, c);
	}

	__forceinline static void WritePixel4HL(u32* addr, u32 c)
	{
		gs_lm_write4hl_at(addr, c);
	}

	__forceinline void WritePixel4HL(u32 addr, u32 c)
	{
		gs_lm_write_pixel4hl(this, addr, c);
	}

	__forceinline static void WritePixel4HH(u32* addr, u32 c)
	{
		gs_lm_write4hh_at(addr, c);
	}

	__forceinline void WritePixel4HH(u32 addr, u32 c)
	{
		gs_lm_write_pixel4hh(this, addr, c);
	}

	__forceinline void WriteFrame16(u32 addr, u32 c)
	{
		gs_lm_write_frame16(this, addr, c);
	}

	__forceinline void WritePixel32(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WritePixel32(PixelAddress32(x, y, bp, bw), c);
	}

	__forceinline void WritePixel24(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WritePixel24(PixelAddress32(x, y, bp, bw), c);
	}

	__forceinline void WritePixel16(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WritePixel16(PixelAddress16(x, y, bp, bw), c);
	}

	__forceinline void WritePixel16S(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WritePixel16(PixelAddress16S(x, y, bp, bw), c);
	}

	__forceinline void WritePixel8(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WritePixel8(PixelAddress8(x, y, bp, bw), c);
	}

	__forceinline void WritePixel4(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WritePixel4(PixelAddress4(x, y, bp, bw), c);
	}

	__forceinline void WritePixel8H(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WritePixel8H(PixelAddress32(x, y, bp, bw), c);
	}

	__forceinline void WritePixel4HL(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WritePixel4HL(PixelAddress32(x, y, bp, bw), c);
	}

	__forceinline void WritePixel4HH(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WritePixel4HH(PixelAddress32(x, y, bp, bw), c);
	}

	__forceinline void WritePixel32Z(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WritePixel32(PixelAddress32Z(x, y, bp, bw), c);
	}

	__forceinline void WritePixel24Z(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WritePixel24(PixelAddress32Z(x, y, bp, bw), c);
	}

	__forceinline void WritePixel16Z(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WritePixel16(PixelAddress16Z(x, y, bp, bw), c);
	}

	__forceinline void WritePixel16SZ(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WritePixel16(PixelAddress16SZ(x, y, bp, bw), c);
	}

	__forceinline void WriteFrame16(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WriteFrame16(PixelAddress16(x, y, bp, bw), c);
	}

	__forceinline void WriteFrame16S(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WriteFrame16(PixelAddress16S(x, y, bp, bw), c);
	}

	__forceinline void WriteFrame16Z(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WriteFrame16(PixelAddress16Z(x, y, bp, bw), c);
	}

	__forceinline void WriteFrame16SZ(int x, int y, u32 c, u32 bp, u32 bw)
	{
		WriteFrame16(PixelAddress16SZ(x, y, bp, bw), c);
	}

	void WritePixel32(u8* RESTRICT src, u32 pitch, const GSOffset& off, const GSVector4i& r)
	{
		off.loopPixels(r, vm32(), (u32*)src, pitch, [&](u32* dst, u32* src) { *dst = *src; });
	}

	void WritePixel32(u8* RESTRICT src, u32 pitch, const GSOffset& off, const GSVector4i& r, u32 write_mask)
	{
		off.loopPixels(r, vm32(), (u32*)src, pitch, [&](u32* dst, u32* src) { *dst = (*dst & ~write_mask) | (*src & write_mask); });
	}

	void WritePixel24(u8* RESTRICT src, u32 pitch, const GSOffset& off, const GSVector4i& r)
	{
		off.loopPixels(r, vm32(), (u32*)src, pitch,
			[&](u32* dst, u32* src)
		{
			*dst = (*dst & 0xff000000) | (*src & 0x00ffffff);
		});
	}

	void WritePixel16(u8* RESTRICT src, u32 pitch, const GSOffset& off, const GSVector4i& r)
	{
		off.loopPixels(r, vm16(), (u16*)src, pitch, [&](u16* dst, u16* src) { *dst = *src; });
	}

	void WriteFrame16(u8* RESTRICT src, u32 pitch, const GSOffset& off, const GSVector4i& r)
	{
		off.loopPixels(r, vm16(), (u32*)src, pitch,
		[&](u16* dst, u32* src)
		{
			u32 rb = *src & 0x00f800f8;
			u32 ga = *src & 0x8000f800;

			*dst = (u16)((ga >> 16) | (rb >> 9) | (ga >> 6) | (rb >> 3));
		});
	}

	__forceinline u32 ReadTexel32(u32 addr, const GIFRegTEXA& TEXA) const
	{
		return gs_lm_read_texel32(this, addr, &TEXA);
	}

	__forceinline u32 ReadTexel24(u32 addr, const GIFRegTEXA& TEXA) const
	{
		return gs_lm_read_texel24(this, addr, &TEXA);
	}

	__forceinline u32 ReadTexel16(u32 addr, const GIFRegTEXA& TEXA) const
	{
		return gs_lm_read_texel16(this, addr, &TEXA);
	}

	__forceinline u32 ReadTexel8(u32 addr, const GIFRegTEXA& TEXA) const
	{
		return gs_lm_read_texel8(this, addr, &TEXA);
	}

	__forceinline u32 ReadTexel4(u32 addr, const GIFRegTEXA& TEXA) const
	{
		return gs_lm_read_texel4(this, addr, &TEXA);
	}

	__forceinline u32 ReadTexel8H(u32 addr, const GIFRegTEXA& TEXA) const
	{
		return gs_lm_read_texel8h(this, addr, &TEXA);
	}

	__forceinline u32 ReadTexel4HL(u32 addr, const GIFRegTEXA& TEXA) const
	{
		return gs_lm_read_texel4hl(this, addr, &TEXA);
	}

	__forceinline u32 ReadTexel4HH(u32 addr, const GIFRegTEXA& TEXA) const
	{
		return gs_lm_read_texel4hh(this, addr, &TEXA);
	}

	__forceinline u32 ReadTexel32(int x, int y, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) const
	{
		return ReadTexel32(PixelAddress32(x, y, TEX0.TBP0, TEX0.TBW), TEXA);
	}

	__forceinline u32 ReadTexel24(int x, int y, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) const
	{
		return ReadTexel24(PixelAddress32(x, y, TEX0.TBP0, TEX0.TBW), TEXA);
	}

	__forceinline u32 ReadTexel16(int x, int y, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) const
	{
		return ReadTexel16(PixelAddress16(x, y, TEX0.TBP0, TEX0.TBW), TEXA);
	}

	__forceinline u32 ReadTexel16S(int x, int y, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) const
	{
		return ReadTexel16(PixelAddress16S(x, y, TEX0.TBP0, TEX0.TBW), TEXA);
	}

	__forceinline u32 ReadTexel8(int x, int y, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) const
	{
		return ReadTexel8(PixelAddress8(x, y, TEX0.TBP0, TEX0.TBW), TEXA);
	}

	__forceinline u32 ReadTexel4(int x, int y, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) const
	{
		return ReadTexel4(PixelAddress4(x, y, TEX0.TBP0, TEX0.TBW), TEXA);
	}

	__forceinline u32 ReadTexel8H(int x, int y, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) const
	{
		return ReadTexel8H(PixelAddress32(x, y, TEX0.TBP0, TEX0.TBW), TEXA);
	}

	__forceinline u32 ReadTexel4HL(int x, int y, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) const
	{
		return ReadTexel4HL(PixelAddress32(x, y, TEX0.TBP0, TEX0.TBW), TEXA);
	}

	__forceinline u32 ReadTexel4HH(int x, int y, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) const
	{
		return ReadTexel4HH(PixelAddress32(x, y, TEX0.TBP0, TEX0.TBW), TEXA);
	}

	__forceinline u32 ReadTexel32Z(int x, int y, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) const
	{
		return ReadTexel32(PixelAddress32Z(x, y, TEX0.TBP0, TEX0.TBW), TEXA);
	}

	__forceinline u32 ReadTexel24Z(int x, int y, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) const
	{
		return ReadTexel24(PixelAddress32Z(x, y, TEX0.TBP0, TEX0.TBW), TEXA);
	}

	__forceinline u32 ReadTexel16Z(int x, int y, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) const
	{
		return ReadTexel16(PixelAddress16Z(x, y, TEX0.TBP0, TEX0.TBW), TEXA);
	}

	__forceinline u32 ReadTexel16SZ(int x, int y, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) const
	{
		return ReadTexel16(PixelAddress16SZ(x, y, TEX0.TBP0, TEX0.TBW), TEXA);
	}

	__forceinline void ReadImageX(int& tx, int& ty, u8* dst, int len, GIFRegBITBLTBUF& BITBLTBUF, GIFRegTRXPOS& TRXPOS, GIFRegTRXREG& TRXREG) const
	{
		m_readImageX(*this, tx, ty, dst, len, BITBLTBUF, TRXPOS, TRXREG);
	}

	void ReadTexture(const GSOffset& off, const GSVector4i& r, u8* dst, int dstpitch, const GIFRegTEXA& TEXA)
	{
		const int rc[4] = {r.left, r.top, r.right, r.bottom};
		gs_local_memory_read_texture(this, &off, rc, dst, dstpitch, &TEXA);
	}
};

inline GSOffset GSOffset::fromKnownPSM(u32 bp, u32 bw, GS_PSM psm)
{
	switch (psm)
	{
		case PSMT4:    return GSOffset(GSLocalMemory::swizzle4,    bp, bw, psm);
		case PSMT8:    return GSOffset(GSLocalMemory::swizzle8,    bp, bw, psm);
		case PSMZ16:   return GSOffset(GSLocalMemory::swizzle16Z,  bp, bw, psm);
		case PSMCT16S: return GSOffset(GSLocalMemory::swizzle16S,  bp, bw, psm);
		case PSMZ16S:  return GSOffset(GSLocalMemory::swizzle16SZ, bp, bw, psm);
		case PSMCT16:
		case PSGPU24:
			       return GSOffset(GSLocalMemory::swizzle16,   bp, bw, psm);
		case PSMZ32:
		case PSMZ24:
			       return GSOffset(GSLocalMemory::swizzle32Z,  bp, bw, psm);
		case PSMCT32:
		case PSMCT24:
		case PSMT8H:
		case PSMT4HL:
		case PSMT4HH:
			       break;
	}
	return GSOffset(GSLocalMemory::swizzle32, bp, bw, psm);
}
#endif
