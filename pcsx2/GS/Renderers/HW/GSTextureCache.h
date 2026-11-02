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

#include <new>
#include <cstddef>
#include <functional>
#include <utility> /* std::pair */
#include <limits>

#include "../Common/GSRenderer.h"
#include "GSObjectPool.h"
#include "../Common/GSFastList.h"
#include "../Common/GSDirtyRect.h"
#include "../Common/GSFlatMap.h"
#include "../Common/GSStableMap.h"

class GSHwHack;

// Only for debugging. Reads back every target to local memory after drawing, effectively
// disabling caching between draws.
//#define DISABLE_HW_TEXTURE_CACHE

class GSTextureCache
{
public:
	friend GSHwHack;

	enum
	{
		RenderTarget,
		DepthStencil
	};

	constexpr static u32 MAX_BP = 0x3fff;

	constexpr static bool CheckOverlap(const u32 a_bp, const u32 a_bp_end, const u32 b_bp, const u32 b_bp_end) noexcept
	{
		const bool valid = a_bp <= a_bp_end && b_bp <= b_bp_end;
		const bool overlap = a_bp <= b_bp_end && a_bp_end >= b_bp;
		return valid && overlap;
	}

	struct SourceRegion
	{
		u64 bits;

		bool HasX() const { return static_cast<u32>(bits) != 0; }
		bool HasY() const { return static_cast<u32>(bits >> 32) != 0; }
		bool HasEither() const { return (bits != 0); }

		void SetX(s32 min, s32 max) { bits |= (static_cast<u64>(static_cast<u16>(min)) | (static_cast<u64>(static_cast<u16>(max) << 16))); }
		void SetY(s32 min, s32 max) { bits |= ((static_cast<u64>(static_cast<u16>(min)) << 32) | (static_cast<u64>(static_cast<u16>(max)) << 48)); }

		s32 GetMinX() const { return static_cast<s16>(bits); }
		s32 GetMaxX() const { return static_cast<s16>((bits >> 16) & 0xFFFFu); }
		s32 GetMinY() const { return static_cast<s16>((bits >> 32) & 0xFFFFu); }
		s32 GetMaxY() const { return static_cast<s16>(bits >> 48); }

		s32 GetWidth() const { return (GetMaxX() - GetMinX()); }
		s32 GetHeight() const { return (GetMaxY() - GetMinY()); }

		/// Returns true if the area of the region exceeds the TW/TH size (i.e. "fixed tex0").
		bool IsFixedTEX0(GIFRegTEX0 TEX0) const;
		bool IsFixedTEX0(int tw, int th) const;
		bool IsFixedTEX0W(int tw) const;
		bool IsFixedTEX0H(int th) const;

		/// Returns the size that the region occupies.
		GSVector2i GetSize(int tw, int th) const;

		/// Returns the rectangle relative to the texture base pointer that the region occupies.
		GSVector4i GetRect(int tw, int th) const;

		/// When TW/TH is less than the extents covered by the region ("fixed tex0"), returns the offset
		/// which should be applied to any coordinates to relocate them to the actual region.
		GSVector4i GetOffset(int tw, int th) const;

		/// Reduces the range of texels relative to the specified mipmap level.
		SourceRegion AdjustForMipmap(u32 level) const;

		/// Adjusts the texture base pointer and block width relative to the region.
		void AdjustTEX0(GIFRegTEX0* TEX0) const;

		/// Creates a new source region based on the CLAMP register.
		static SourceRegion Create(GIFRegTEX0 TEX0, GIFRegCLAMP CLAMP);
	};

	using HashType = u64;

	struct HashCacheKey
	{
		HashType TEX0Hash, CLUTHash;
		GIFRegTEX0 TEX0;
		GIFRegTEXA TEXA;
		u32 region_width;
		u32 region_height;

		HashCacheKey();

		static HashCacheKey Create(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, const u32* clut, const GSVector2i* lod, SourceRegion region);

		HashCacheKey WithRemovedCLUTHash() const;
		void RemoveCLUTHash();

		__fi bool operator==(const HashCacheKey& e) const { return std::memcmp(this, &e, sizeof(*this)) == 0; }
		__fi bool operator!=(const HashCacheKey& e) const { return std::memcmp(this, &e, sizeof(*this)) != 0; }
		__fi bool operator<(const HashCacheKey& e) const { return std::memcmp(this, &e, sizeof(*this)) < 0; }
	};

	struct HashCacheKeyHash
	{
		u64 operator()(const HashCacheKey& key) const;
	};

	struct HashCacheEntry
	{
		GSTexture* texture;
		u32 refcount;
		u16 age;
		std::pair<u8, u8> alpha_minmax;
		bool valid_alpha_minmax;
		bool is_replacement;
	};


	class Surface : public GSAlignedClass<32>
	{
	protected:
		Surface();
		~Surface();

	public:
		GSTexture* m_texture = nullptr;
		GIFRegTEX0 m_TEX0 = {};
		GIFRegTEXA m_TEXA = {};
		GSVector2i m_unscaled_size = {};
		float m_scale = 0.0f;
		int m_age = 0;
		u32 m_end_block = MAX_BP; // Hint of the surface area.
		bool m_32_bits_fmt = false; // Allow to detect the casting of 32 bits as 16 bits texture
		bool m_was_dst_matched = false;
		bool m_shared_texture = false;

		__fi GSTexture* GetTexture() const { return m_texture; }
		__fi int GetUnscaledWidth() const { return m_unscaled_size.x; }
		__fi int GetUnscaledHeight() const { return m_unscaled_size.y; }
		__fi const GSVector2i& GetUnscaledSize() const { return m_unscaled_size; }
		__fi GSVector4i GetUnscaledRect() const { return GSVector4i::loadh(m_unscaled_size); }
		__fi float GetScale() const { return m_scale; }

		/// Returns true if the target wraps around the end of GS memory.
		bool Wraps() const { return (m_end_block < m_TEX0.TBP0); }

		/// Returns the end block for the target, but doesn't wrap at 0x3FFF.
		/// Can be used for overlap tests.
		u32 UnwrappedEndBlock() const { return (m_end_block + (Wraps() ? GS_MAX_BLOCKS : 0)); }

		bool Inside(u32 bp, u32 bw, u32 psm, const GSVector4i& rect);
		bool Overlaps(u32 bp, u32 bw, u32 psm, const GSVector4i& rect);
	};

	struct PaletteKey
	{
		const u32* clut;
		u16 pal;
	};

	/* Palettes outlive a draw but a game that animates its CLUT makes
	 * them steadily; the map's cap is MAX_SIZE per map, two maps. */
	static constexpr u32 PALETTE_POOL_SIZE = 512;

	class Palette
	{
	public:
		static constexpr u32 ALIVE = 0x4c415021u; /* 'PAL!' */
		u32 m_alive = ALIVE;

		/* What shared_ptr did, as a number. A palette is owned by the map
		 * it is in and referenced by every Source using it; the map drops
		 * the ones nobody references when it grows past MAX_SIZE, which
		 * is the use_count() <= 1 test it replaces. AddRef and Release
		 * are the only things that touch it, and the GS is one thread, so
		 * it is a plain u32 and not an atomic. */
		u32 m_refs = 0;

		__fi void AddRef() { m_refs++; }
		__fi void Release()
		{
			if (m_refs)
				m_refs--;
		}

		static void* operator new(size_t size);
		static void operator delete(void* p);

	private:
	private:
		/* Sixteen or two hundred and fifty-six entries, in the object.
		 * It was a 64-byte or 1 KB allocation per palette, made in the
		 * constructor and freed in the destructor, for something a game
		 * that animates its CLUT churns constantly.
		 *
		 * alignas because the allocation it replaced was memalign_alloc's
		 * at 64, and PaletteKeyEqual compares two of these with
		 * GSVector4i::compare64, which casts to a vector pointer and
		 * indexes it - an aligned SSE load. Inside the object without
		 * this it sat at offset eight, and every comparison was a
		 * general protection fault. */
		alignas(GS_OBJECT_POOL_ALIGN) u32 m_clut[256];
		GSTexture* m_tex_palette;
		u16 m_pal;
		std::pair<u8, u8> m_alpha_minmax;

	public:
		Palette(const u32* clut, u16 pal, bool need_gs_texture);
		~Palette();

		__fi std::pair<u8, u8> GetAlphaMinMax() const { return m_alpha_minmax; }
		std::pair<u8, u8> GetAlphaMinMax(u8 min_index, u8 max_index) const;

		// Disable copy constructor and copy operator
		Palette(const Palette&) = delete;
		Palette& operator=(const Palette&) = delete;

		// Disable move constructor and move operator
		Palette(const Palette&&) = delete;
		Palette& operator=(const Palette&&) = delete;

		GSTexture* GetPaletteGSTexture();

		PaletteKey GetPaletteKey();

		void InitializeTexture();
	};

	struct PaletteKeyHash
	{
		// Calculate hash
		u64 operator()(const PaletteKey& key) const;
	};

	struct PaletteKeyEqual
	{
		// Compare pal value and clut contents
		bool operator()(const PaletteKey& lhs, const PaletteKey& rhs) const;
	};

	/* Targets live longer than sources and there are far fewer of them -
	 * a render target and a depth buffer per buffer in flight, plus what
	 * the game switches between - but they are still made and destroyed
	 * while a game runs. */
	static constexpr u32 TARGET_POOL_SIZE = 128;

	class Target : public Surface
	{
	public:
		/* Out of a pool, as Source is. */
		static void* operator new(size_t size);
		static void operator delete(void* p);

		const int m_type = 0;
		int m_alpha_max = 0;
		int m_alpha_min = 0;
		bool m_alpha_range = false;

		// Valid alpha means "we have rendered to the alpha channel of this target".
		// A false value means that the alpha in local memory is still valid/up-to-date.
		bool m_valid_alpha_low = false;
		bool m_valid_alpha_high = false;
		bool m_valid_rgb = false;
		bool m_rt_alpha_scale = false;
		bool m_downscaled = false;
		int m_last_draw = 0;

		bool m_is_frame = false;
		bool m_used = false;
		/* The last draw wrote the target at its own width, pixel for pixel,
		 * so its texture holds the pages where that width puts them; a
		 * shuffle's emulation lays them out as the picture it moves. */
		bool m_drawn_at_width = false;
		/* Read as a texture at its own width: under the Normal half-pixel
		 * offset, a draw to it moves half a native pixel (see SetupIA). */
		bool m_half_pixel_shift = false;
		GSDirtyRectList m_dirty;
		GSVector4i m_valid{};
		GSVector4i m_drawn_since_read{};
		int readbacks_since_draw = 0;

	public:
		Target(GIFRegTEX0 TEX0, int type, const GSVector2i& unscaled_size, float scale, GSTexture* texture);
		~Target();

		static Target* Create(GIFRegTEX0 TEX0, int w, int h, float scale, int type, bool clear);

		__fi bool HasValidAlpha() const { return (m_valid_alpha_low | m_valid_alpha_high); }
		bool HasValidBitsForFormat(u32 psm, bool req_color, bool req_alpha);

		void ResizeDrawn(const GSVector4i& rect);
		void UpdateDrawn(const GSVector4i& rect, bool can_resize = true);
		void ResizeValidity(const GSVector4i& rect);
		void UpdateValidity(const GSVector4i& rect, bool can_resize = true);

		void ScaleRTAlpha();
		void UnscaleRTAlpha();

		void Update(bool cannot_scale = false);

		/// Updates the target, if the dirty area intersects with the specified rectangle.
		void UpdateIfDirtyIntersects(const GSVector4i& rc);

		/// Updates the valid alpha flag, based on PSM and fbmsk.
		void UpdateValidChannels(u32 psm, u32 fbmsk);

		/// Resizes target texture, DOES NOT RESCALE.
		bool ResizeTexture(int new_unscaled_width, int new_unscaled_height, bool recycle_old = true);

		/* A target as this one is, on a texture of its own with a GPU copy
		 * of this one's; nullptr when no texture can be had. Not in any
		 * list, and not counted in the cache's target memory. */
		Target* Clone() const;
	};

	/* How many sources exist at once, and so how many the pool holds.
	 * A frame uses a few dozen; past this the allocator is used as
	 * before, so the number is a budget, not a limit. */
	static constexpr u32 SOURCE_POOL_SIZE = 384;

	class Source : public Surface
	{
	public:
		/* A source is freed in two places - SourceMap::RemoveAt, for one
		 * that is in the map, and InvalidateTemporarySource, for the one
		 * that never is - and a crash in the second, in free, says the
		 * block was already gone or its header was overwritten. This
		 * tells the two apart at the moment it happens instead of
		 * afterwards: set on construction, cleared on destruction, and
		 * checked before anything in the destructor touches the heap. */
		static constexpr u32 ALIVE = 0x5253434eu; /* 'SRCN' */
		u32 m_alive = ALIVE;

	private:
		/* Three rectangles, in the object. They were a 48-byte allocation
		 * made on the first Write of every source and freed in the
		 * destructor - two trips through the allocator per source, in a
		 * draw, for something this size. */
		struct
		{
			alignas(16) GSVector4i rect[3];
			u32 count;
		} m_write = {};

		void PreloadLevel(int level);

		void Write(const GSVector4i& r, int layer, const GSOffset& off);
		void Flush(u32 count, int layer, const GSOffset& off);

	public:
		HashCacheEntry* m_from_hash_cache = nullptr;
		/* A reference, taken with AddRef and given back in the
		 * destructor and wherever it is replaced. */
		Palette* m_palette_obj = nullptr;
		/* One bit per block, 32 blocks to a page, in the object rather
		 * than a 2 KB allocation made the first time a source is written
		 * to and freed with it. m_valid_init says whether it has been
		 * cleared yet, which is what the pointer being null used to say. */
		u32 m_valid[GS_MAX_PAGES];
		bool m_valid_init = false;
		GSTexture* m_palette = nullptr;
		GSVector4i m_valid_rect = {};
		GSVector2i m_lod = {};
		SourceRegion m_region = {};
		u8 m_valid_hashes = 0;
		u8 m_complete_layers = 0;
		bool m_target = false;
		bool m_target_direct = false;
		// Indexed view of a target, converted at the target's scale.
		bool m_scaled_indexed = false;
		bool m_repeating = false;
		bool m_valid_alpha_minmax = false;
		std::pair<u8, u8> m_alpha_minmax = {0u, 255u};
		GSPage2TileMap* m_p2t = nullptr;
		// Keep a trace of the target origin. There is no guarantee that pointer will
		// still be valid on future. However it ought to be good when the source is created
		// so it can be used to access un-converted data for the current draw call.
		Target* m_from_target = nullptr;
		GIFRegTEX0 m_from_target_TEX0 = {}; // TEX0 of the target texture, if any, else equal to texture TEX0
		GIFRegTEX0 m_layer_TEX0[7] = {}; // Detect already loaded value
		HashType m_layer_hash[7] = {};
		// Keep a GSTextureCache::SourceMap::m_map iterator to allow fast erase
		// Deliberately not initialized to save cycles.
		u16 m_erase_it[GS_MAX_PAGES];
		GSOffset::PageLooper m_pages;
		/* Its place in SourceMap::m_surfaces. */
		u32 m_surface_index = 0;

	public:
		/* Out of a pool fixed at startup, not the allocator: a source is
		 * made and destroyed inside a draw, and a 3.5 KB allocation per
		 * draw is both a frame-time cost and the thing that puts a heap
		 * block header next to it to be corrupted. Falls back to the
		 * allocator only if more than SOURCE_POOL_SIZE are live. */
		static void* operator new(size_t size);
		static void operator delete(void* p);

		Source(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA);
		virtual ~Source();

		__fi bool CanPreload() const { return CanPreloadTextureSize(m_TEX0.TW, m_TEX0.TH); }
		__fi bool IsFromTarget() const { return m_target; }
		bool IsPaletteFormat() const;

		__fi const SourceRegion& GetRegion() const { return m_region; }
		__fi GSVector2i GetRegionSize() const { return m_region.GetSize(m_unscaled_size.x, m_unscaled_size.y); }
		__fi GSVector4i GetRegionRect() const { return m_region.GetRect(m_unscaled_size.x, m_unscaled_size.y); }
		__fi const std::pair<u8, u8> GetAlphaMinMax() const { return m_alpha_minmax; }

		void SetPages();

		void Update(const GSVector4i& rect, int layer = 0);
		void UpdateLayer(const GIFRegTEX0& TEX0, const GSVector4i& rect, int layer = 0);

		bool ClutMatch(const PaletteKey& palette_key);
	};

	class PaletteMap
	{
	private:
		/* When a map passes this, the palettes nothing is using are
		 * dropped. It was 65535, and the two maps reserved that many
		 * buckets outright - a megabyte of pointers, touched at random by
		 * every lookup, for a count no game comes near: a few hundred
		 * palettes is a lot, and a game that animates its CLUT can only
		 * hold as many as it uses at once. The reserve is gone with it;
		 * the maps grow into what they need. */
		static const u16 MAX_SIZE = 1024; // Max size of each map.

		/* Two GSStableMaps, the first for 16-entry palettes and the second
		 * for 256: the CLUT's contents (64 or 1024 bytes, a copy in the
		 * node, compared whole) -> Palette*. */
		gs_stable_map_t m_maps[2];

	public:
		PaletteMap();
		~PaletteMap();

		// Retrieves a shared pointer to a valid Palette from m_maps or creates a new one adding it to the data structure
		/* Both return a reference the caller owns and must Release. */
		Palette* LookupPalette(u16 pal, bool need_gs_texture);
		Palette* LookupPalette(const u32* clut, u16 pal, bool need_gs_texture);

		void Clear(); // Clears m_maps, thus deletes Palette objects
	};

	class SourceMap
	{
	public:
		/* Every source, in no order: m_surfaces[i] holds
		 * m_surface_index i, and a source taken out has the last one
		 * moved into its place, so a walk downwards may take out the
		 * source it is at and go on. Room for as many as there have been
		 * at once; grown, rarely, by doubling. */
		Source** m_surfaces = nullptr;
		u32 m_surfaces_count = 0;
		u32 m_surfaces_capacity = 0;
		std::array<FastList<Source*>, GS_MAX_PAGES> m_map;

		~SourceMap();

		void Add(Source* s, const GIFRegTEX0& TEX0);
		void SwapTexture(GSTexture* old_tex, GSTexture* new_tex);
		void RemoveAll();
		void RemoveAt(Source* s);
		void Unlist(Source* s);
	};

	struct TargetHeightElem
	{
		union
		{
			u32 bits;

			struct
			{
				u32 bp : 14;
				u32 fbw : 6;
				u32 psm : 6;
				u32 pad : 6;
			};
		};

		s32 width;
		s32 height;
		u32 age;
	};

	struct SurfaceOffsetKeyElem
	{
		u32 psm;
		u32 bp;
		u32 bw;
		GSVector4i rect;
	};

	struct SurfaceOffsetKey
	{
		std::array<SurfaceOffsetKeyElem, 2> elems;  // A and B elems.
	};

	struct SurfaceOffset
	{
		bool is_valid;
		GSVector4i b2a_offset;  // B to A offset in B coords.
	};

protected:
	PaletteMap m_palette_map;
	SourceMap m_src;
	u64 m_source_memory_usage = 0;
	/* HashCacheKey -> HashCacheEntry, in a GSStableMap: sources point at
	 * their entries, which stay put. */
	gs_stable_map_t m_hash_cache;
	/* The palette each paletted texture in the hash cache was first
	 * expanded with, by its key without one (HashCacheKey -> HashType),
	 * in a GSFlatMap. An 8-bit texture expanded to 32 bits is four times
	 * its size, sixteen for 4-bit, and one drawn with several palettes is
	 * that once for each: Ridge Racer V's intro has 27 textures, 7 MB of
	 * indices, as 370 MB of expansions. The second palette a texture is
	 * seen with makes it an index texture, shared by every palette and
	 * read through them on the GPU, as GPU palette conversion does for
	 * all of them; a texture only ever drawn with one stays expanded.
	 * Forgotten all at once when full: a texture drawn on is expanded
	 * once more, and indexed at its next palette again. */
	gs_flat_map_t m_palette_seen;
	static constexpr size_t PALETTE_SEEN_MAX = 1024;
	/* Where did this number come from? A game called Corvette draws its
	 * background FMVs with a ton of 17x17 tiles, about 600 texture uploads
	 * a frame; 800 is an upper bound with a bit of a buffer. What the hash
	 * cache is purged down to each frame. */
	static constexpr u32 MAX_HASH_CACHE_SIZE = 800;
	bool m_auto_index = true;
	u64 m_hash_cache_memory_usage = 0;
	u64 m_hash_cache_replacement_memory_usage = 0;

	FastList<Target*> m_dst[2];
	FastList<TargetHeightElem> m_target_heights;

	/* Targets as savestates taken in this instance left them, by the
	 * state's key (SnapshotTargets), each slot replaced oldest first. */
	enum { TARGET_SNAPSHOTS = 2 };
	FastList<Target*> m_snap[TARGET_SNAPSHOTS][2];
	u64 m_snap_key[TARGET_SNAPSHOTS] = {};
	bool m_snap_used[TARGET_SNAPSHOTS] = {};
	u32 m_snap_next = 0;
	bool m_snap_keep = false; /* RemoveAll leaves the snapshots */
	void DiscardSnapshot(u32 slot);
	u64 m_target_memory_usage = 0;
	/* The usage, in MB, the over-budget warning last named. */
	u64 m_budget_warned_mb = 0;

	/* What the live targets may hold, from the console: every render
	 * target and depth buffer a game has at once lives in the PS2's four
	 * megabytes of GS memory, so the host copy of all of them is that
	 * times the upscale squared. Twice that, because a game double
	 * buffers and because GSdx extends target heights past what the
	 * game asked for.
	 *
	 * Ageing alone does not bound this: a target is kept for sixty
	 * frames, and a game that makes large ones quickly - Tomb Raider
	 * Legend's RAD camera at 4x asks for 4096x3584, 58 MB each - has
	 * gigabytes of them live before the first one is a second old. Over
	 * the budget, the targets that go are those GSTargetBudget.c picks:
	 * never the ones in use, which hold what the game reads next. */
	static constexpr u32 TARGET_LIVE_SETS = 2;
	void EnforceTargetBudget();

public:
	u64 TargetByteBudget() const;

private:

	int m_expected_src_bp = -1;
	int m_remembered_src_bp = -1;
	int m_expected_dst_bp = -1;
	int m_remembered_dst_bp = -1;

	/* The remaps MoveInTargets has built. A game moves the same regions
	 * the same way every frame, so a remap is kept for the move and the
	 * targets it was built for, and uploaded once. */
	struct MoveRemap
	{
		GSVector4i dst_rect;         // destination target pixels it rebuilds
		GSVector4i src_rect;         // source target pixels it reads
		GSTexture* texture;          // one 32-bit entry per destination element, RGBA8
		u32 move[12];                // SBP SBW SPSM sx sy DBP DBW DPSM dx dy w h
		u32 src_tex0[3];             // the source target's TBP0 TBW PSM
		u32 dst_tex0[3];             // the destination target's
		u32 last_use;
		u8 src_type;
		u8 nibbles;                  // of a destination element
		u8 entries;                  // destination elements per word
		bool has_old;                // some words keep bits the move leaves
		bool writes_rgb;
		bool writes_alpha;
	};
	static constexpr size_t MAX_MOVE_REMAPS = 256;
	std::vector<MoveRemap> m_move_remaps;
	u32 m_move_remap_clock = 0;

	void ClearMoveRemaps();
	MoveRemap* BuildMoveRemap(const u32 move[12], Target* src, Target* dst);

	/* When the cache passes this it is emptied outright. It was 65535,
	 * and the constructor reserved that many buckets - half a megabyte of
	 * pointers, touched at random by every lookup, for entries that are
	 * one per pair of overlapping surfaces. The reserve is gone; the map
	 * grows into what it needs, and 4096 is still far more than a frame
	 * asks for. */
	constexpr static size_t S_SURFACE_OFFSET_CACHE_MAX_SIZE = 4096;
	/* SurfaceOffsetKey -> SurfaceOffset in a GSFlatMap, the key packed
	 * into fourteen words (SurfaceOffsetCacheKey: the elements' psm, bp,
	 * bw and rect, no padding to compare), the offset into five. */
	gs_flat_map_t m_surface_offset_cache;

	Source* m_temporary_source = nullptr; // invalidated after the draw

	GSDownloadTexturePtr m_color_download_texture;
	GSDownloadTexturePtr m_uint16_download_texture;
	GSDownloadTexturePtr m_uint32_download_texture;

	Source* CreateSource(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, Target* t, bool half_right, int x_offset, int y_offset, const GSVector2i* lod, const GSVector4i* src_range, GSTexture* gpu_clut, SourceRegion region, int page_gather);
	static int ReadEndX(u32 bw, u32 psm, const GSVector4i& r);
	static int PagesInTarget(const Target* t, u32 bp, u32 bw, u32 psm, const GSVector4i& r);

	bool PreloadTarget(GIFRegTEX0 TEX0, const GSVector2i& size, const GSVector2i& valid_size, bool is_frame,
		bool preload, bool preserve_target, const GSVector4i draw_rect, Target* dst, GSTextureCache::Source* src = nullptr);

	// Returns scaled texture size.
	static GSVector2i ScaleRenderTargetSize(const GSVector2i& sz, float scale);

	/// Expands a target when the block pointer for a display framebuffer is within another target, but the read offset
	/// plus the height is larger than the current size of the target.
	void ScaleTargetForDisplay(Target* t, const GIFRegTEX0& dispfb, int real_w, int real_h);

	/// Resizes the download texture if needed.
	bool PrepareDownloadTexture(u32 width, u32 height, GSTexture::Format format, GSDownloadTexturePtr* tex, u32 extent_w = 0, u32 extent_h = 0);

	/* A readback with its copy recorded and not yet waited for. */
	struct PendingRead
	{
		GSDownloadTexture* dl;
		GIFRegTEX0 TEX0;
		GSVector4i tr;
		GSVector4i drc;
		u32 write_mask;
	};

	/* Records the copy of r back from t. With own given, a download
	 * texture this cache holds is used the first time in shared_used
	 * and own after that, so the copies of one batch do not share one. */
	bool ReadQueue(Target* t, const GSVector4i& r, GSDownloadTexturePtr* own, u32* shared_used, PendingRead* out);
	/* Waits for the copy, unless done, and writes it to local memory. */
	bool ReadStore(const PendingRead& p);

	/* The reads one InvalidateLocalMem takes: every copy recorded, then
	 * one wait and the stores in the same order. A read retires its
	 * drawn area when queued, as the loop deciding the next read needs
	 * to see it; a store that fails puts the area back. */
	static constexpr u32 READ_BATCH_MAX = 8;
	struct ReadBatch
	{
		PendingRead pending[READ_BATCH_MAX];
		Target* target[READ_BATCH_MAX];
		GSVector4i drawn_before[READ_BATCH_MAX];
		u32 count;
		u32 shared_used;
	};
	GSDownloadTexturePtr m_read_batch_download[READ_BATCH_MAX];
	bool ReadBatchQueue(ReadBatch* b, Target* t, const GSVector4i& r);
	void ReadBatchStore(ReadBatch* b);
	void InvalidateLocalMemReads(const GSOffset& off, const GSVector4i& r, bool full_flush, ReadBatch* b);

	HashCacheEntry* LookupHashCache(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, bool& paltex, const u32* clut, const GSVector2i* lod, SourceRegion region);
	HashCacheEntry* FindHashCache(const HashCacheKey& key) const;
	HashCacheEntry* InsertHashCache(const HashCacheKey& key, const HashCacheEntry& entry);
	void RemoveFromHashCache(HashCacheEntry* e);
	void AgeHashCache();

	static void PreloadTexture(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, SourceRegion region, GSLocalMemory& mem, bool paltex, GSTexture* tex, u32 level, std::pair<u8, u8>* alpha_minmax);
	static HashType HashTexture(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, SourceRegion region);

	// TODO: virtual void Write(Source* s, const GSVector4i& r) = 0;
	// TODO: virtual void Write(Target* t, const GSVector4i& r) = 0;

	Source* CreateMergedSource(GIFRegTEX0 TEX0, GIFRegTEXA TEXA, SourceRegion region, float scale);

public:
	GSTextureCache();
	~GSTextureCache();

	__fi u64 GetHashCacheMemoryUsage() const { return m_hash_cache_memory_usage; }
	__fi u64 GetHashCacheReplacementMemoryUsage() const { return m_hash_cache_replacement_memory_usage; }
	__fi u64 GetTotalHashCacheMemoryUsage() const { return (m_hash_cache_memory_usage + m_hash_cache_replacement_memory_usage); }
	__fi u64 GetSourceMemoryUsage() const { return m_source_memory_usage; }
	__fi u64 GetTargetMemoryUsage() const { return m_target_memory_usage; }

	/// Writes the target back to local memory. Returns false when nothing was
	/// written, so callers do not retire a region that still needs reading.
	bool Read(Target* t, const GSVector4i& r);
	void Read(Source* t, const GSVector4i& r);
	void RemoveAll(bool sources, bool targets, bool hash_cache);
	void ReadbackAll();

	/* Copies every target, on the GPU, under `key`. */
	void SnapshotTargets(u64 key);
	/* The targets as SnapshotTargets(key) kept them, in place of all the
	 * cache holds (sources and targets, as RemoveAll); false, and nothing
	 * changed, when there is no such snapshot. */
	bool RestoreTargets(u64 key);
	static void AddDirtyRectTarget(Target* target, GSVector4i rect, u32 psm, u32 bw, RGBAMask rgba, bool req_linear = false);
	void ResizeTarget(Target* t, GSVector4i rect, u32 tbp, u32 psm, u32 tbw);
	bool RelayoutTarget(Target* t, u32 tbw);
	static bool FullRectDirty(Target* target, u32 rgba_mask);
	static bool FullRectDirty(Target* target);
	bool CanTranslate(u32 bp, u32 bw, u32 spsm, GSVector4i r, u32 dbp, u32 dpsm, u32 dbw);
	GSVector4i TranslateAlignedRectByPage(u32 tbp, u32 tebp, u32 tbw, u32 tpsm, u32 sbp, u32 spsm, u32 sbw, GSVector4i src_r, bool is_invalidation = false);
	GSVector4i TranslateAlignedRectByPage(Target* t, u32 sbp, u32 spsm, u32 sbw, GSVector4i src_r, bool is_invalidation = false);
	void DirtyRectByPage(u32 sbp, u32 spsm, u32 sbw, Target* t, GSVector4i src_r);
	void DirtyRectByPageOld(u32 sbp, u32 spsm, u32 sbw, Target* t, GSVector4i src_r);
	GSTexture* LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, const GSVector2i& size);
	/* Makes sure a palette for this CLUT exists, with its GS texture if
	 * asked for, and hands back the GS texture or NULL. The palette itself
	 * stays with the map; the callers only ever wanted the texture. */
	GSTexture* LookupPaletteObject(const u32* clut, u16 pal, bool need_gs_texture);

	Source* LookupSource(const bool is_color, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, const GIFRegCLAMP& CLAMP, const GSVector4i& r, const GSVector2i* lod, const bool possible_shuffle, const bool linear, const u32 frame_fbp = 0xFFFFFFFF, bool req_color = true, bool req_alpha = true);
	Source* LookupDepthSource(const bool is_depth, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, const GIFRegCLAMP& CLAMP, const GSVector4i& r, const bool possible_shuffle, const bool linear, const u32 frame_fbp = 0xFFFFFFFF, bool req_color = true, bool req_alpha = true, bool palette = false);


	Target* FindTargetOverlap(Target* target, int type, int psm);
	Target* LookupTarget(GIFRegTEX0 TEX0, const GSVector2i& size, float scale, int type, bool used = true, u32 fbmask = 0,
		bool is_frame = false, bool preload = GSConfig.PreloadFrameWithGSData, bool preserve_rgb = true, bool preserve_alpha = true,
		const GSVector4i draw_rc = GSVector4i::zero(), bool is_shuffle = false, bool possible_clear = false, bool preserve_scale = false,
		const Target* keep = nullptr);
	Target* CreateTarget(GIFRegTEX0 TEX0, const GSVector2i& size, const GSVector2i& valid_size,float scale, int type, bool used = true, u32 fbmask = 0,
		bool is_frame = false, bool preload = GSConfig.PreloadFrameWithGSData, bool preserve_target = true,
		const GSVector4i draw_rc = GSVector4i::zero(), GSTextureCache::Source* src = nullptr);
	Target* LookupDisplayTarget(GIFRegTEX0 TEX0, const GSVector2i& size, float scale, bool is_feedback);
	void SeedSubTargetFromEnclosing(Target* rt);
	void SyncSubTargetsForDisplay(Target* rt);

	/// Looks up a target in the cache, and only returns it if the BP/BW match exactly.
	Target* GetExactTarget(u32 BP, u32 BW, int type, u32 end_bp);
	Target* FindPageOwner(u32 BP, u32 BW, u32 PSM, int& x, int& y, int page_in_draw);
	bool HasTargetAt(u32 BP, u32 BW) const;
	Target* GetTargetWithSharedBits(u32 BP, u32 PSM) const;
	/* The depth buffer at BP, as it was drawn: the one the VR eyes see. */
	Target* FindDepthTarget(u32 BP) const;
	Target* FindOverlappingTarget(GSTextureCache::Target* target) const;
	Target* FindOverlappingTarget(u32 BP, u32 end_bp) const;
	Target* FindOverlappingTarget(u32 BP, u32 BW, u32 PSM, GSVector4i rc) const;

	GSVector2i GetTargetSize(u32 bp, u32 fbw, u32 psm, s32 min_width, s32 min_height, bool can_expand = true);
	bool HasTargetInHeightCache(u32 bp, u32 fbw, u32 psm, u32 max_age = std::numeric_limits<u32>::max(), bool move_front = true);
	bool Has32BitTarget(u32 bp);

	void InvalidateContainedTargets(u32 start_bp, u32 end_bp, u32 write_psm = PSMCT32);
	void InvalidateVideoMemType(int type, u32 bp, u32 write_psm = PSMCT32, u32 write_fbmsk = 0, bool dirty_only = false,
		const Target* keep = nullptr);
	void InvalidateVideoMemSubTarget(GSTextureCache::Target* rt);
	void InvalidateVideoMem(const GSOffset& off, const GSVector4i& r, bool target = true);
	void InvalidateLocalMem(const GSOffset& off, const GSVector4i& r, bool full_flush = false);

	/// Removes any sources which point to the specified target.
	void InvalidateSourcesFromTarget(const Target* t);

	/// Replaces a source's texture externally. Required for some CRC hacks.
	void ReplaceSourceTexture(Source* s, GSTexture* new_texture, float new_scale, const GSVector2i& new_unscaled_size,
		HashCacheEntry* hc_entry, bool new_texture_is_shared);

	/// Converts single color value to depth using the specified shader expression.
	static float ConvertColorToDepth(u32 c, ShaderConvert convert);

	/// Converts single depth value to colour using the specified shader expression.
	static u32 ConvertDepthToColor(float d, ShaderConvert convert);

	/// Copies RGB channels from depth target to a color target.
	bool CopyRGBFromDepthToColor(Target* dst, Target* depth_src);

	bool Move(u32 SBP, u32 SBW, u32 SPSM, int sx, int sy, u32 DBP, u32 DBW, u32 DPSM, int dx, int dy, int w, int h);
	/// A local-memory move worked out on the targets holding its source and
	/// destination, element by element and one sample at a time, as each
	/// sample's own memory would see it (see MoveTexelsConstants).
	bool MoveInTargets(u32 SBP, u32 SBW, u32 SPSM, int sx, int sy, u32 DBP, u32 DBW, u32 DPSM, int dx, int dy, int w, int h);
	bool ShuffleMove(u32 BP, u32 BW, u32 PSM, int sx, int sy, int dx, int dy, int w, int h);
	bool PageMove(u32 SBP, u32 DBP, u32 BW, u32 PSM, int sx, int sy, int dx, int dy, int w, int h);
	void CopyPages(Target* src, u32 sbw, u32 src_offset, Target* dst, u32 dbw, u32 dst_offset, u32 num_pages,
		ShaderConvert shader = ShaderConvert::COPY);

	void IncAge();

	void AttachPaletteToSource(Source* s, u16 pal, bool need_gs_texture, bool update_alpha_minmax);
	void AttachPaletteToSource(Source* s, GSTexture* gpu_clut);
	SurfaceOffset ComputeSurfaceOffset(const GSOffset& off, const GSVector4i& r, const Target* t);
	SurfaceOffset ComputeSurfaceOffset(const uint32_t bp, const uint32_t bw, const uint32_t psm, const GSVector4i& r, const Target* t);
	SurfaceOffset ComputeSurfaceOffset(const SurfaceOffsetKey& sok);

	/// Invalidates a temporary source, a partial copy only created from the current RT/DS for the current draw.
	void InvalidateTemporarySource();

	/// Injects a texture into the hash cache, by using GSTexture::Swap(), transitively applying to all sources. Ownership of tex is transferred.
	void InjectHashCacheTexture(const HashCacheKey& key, GSTexture* tex, const std::pair<u8, u8>& alpha_minmax);
};

extern std::unique_ptr<GSTextureCache> g_texture_cache;
