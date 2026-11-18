/* SPDX-License-Identifier: LGPL-3.0+ */
/* GSHWDrawConfig as it was in GSDevice.h, in a namespace of its own:
 * what GSHWDrawConfig.h is held to. */
#ifndef GSHWCONFIG_OLD_H
#define GSHWCONFIG_OLD_H

namespace old
{
enum class SetDATM : u8
{
	DATM0 = 0U,
	DATM1,
	DATM0_RTA_CORRECTION,
	DATM1_RTA_CORRECTION
};

struct alignas(16) GSHWDrawConfig
{
	enum class Topology: u8
	{
		Point,
		Line,
		Triangle,
	};
	using VSExpand = gs_hw_vs_expand;
	using VSSelector = gs_hw_vs_selector;
	using PSSelector = gs_hw_ps_selector;
	struct PSSelectorHash
	{
		size_t operator()(const PSSelector& p) const
		{
			/* Equality is key_lo and key_hi alone: hash exactly those. */
			const u64 words[2] = {p.key_lo, p.key_hi};
			return gs_key_hash(words, sizeof(words));
		}
	};
#pragma pack(push, 1)
	struct SamplerSelector
	{
		union
		{
			struct
			{
				u8 tau      : 1;
				u8 tav      : 1;
				u8 biln     : 1;
				u8 triln    : 3;
				u8 aniso    : 1;
				u8 lodclamp : 1;
			};
			u8 key;
		};
		SamplerSelector(): key(0) {}
		SamplerSelector(u8 k): key(k) {}
		static SamplerSelector Point() { return SamplerSelector(); }
		static SamplerSelector Linear()
		{
			SamplerSelector out;
			out.biln = 1;
			return out;
		}

		/// Returns true if the effective minification filter is linear.
		__fi bool IsMinFilterLinear() const
		{
			// use the same filter as mag when mipmapping is off
			if (triln < static_cast<u8>(GS_MIN_FILTER::Nearest_Mipmap_Nearest))
				return biln;
			// Linear_Mipmap_Nearest or Linear_Mipmap_Linear
			return (triln >= static_cast<u8>(GS_MIN_FILTER::Linear_Mipmap_Nearest));
		}

		/// Returns true if the effective magnification filter is linear.
		__fi bool IsMagFilterLinear() const
		{
			// magnification uses biln regardless of mip mode (they're only used for minification)
			return biln;
		}

		/// Returns true if the effective mipmap filter is linear.
		__fi bool IsMipFilterLinear() const
		{
			return (triln == static_cast<u8>(GS_MIN_FILTER::Nearest_Mipmap_Linear) ||
					triln == static_cast<u8>(GS_MIN_FILTER::Linear_Mipmap_Linear));
		}

		/// Returns true if mipmaps should be used when filtering (i.e. LOD not clamped to zero).
		__fi bool UseMipmapFiltering() const
		{
			return (triln >= static_cast<u8>(GS_MIN_FILTER::Nearest_Mipmap_Nearest));
		}
	};
	struct DepthStencilSelector
	{
		union
		{
			struct
			{
				u8 ztst : 2;
				u8 zwe  : 1;
				u8 date : 1;
				u8 date_one : 1;

				u8 _free : 3;
			};
			u8 key;
		};
		DepthStencilSelector(): key(0) {}
		DepthStencilSelector(u8 k): key(k) {}
		static DepthStencilSelector NoDepth()
		{
			DepthStencilSelector out;
			out.ztst = ZTST_ALWAYS;
			return out;
		}
	};
	struct ColorMaskSelector
	{
		union
		{
			struct
			{
				u8 wr : 1;
				u8 wg : 1;
				u8 wb : 1;
				u8 wa : 1;

				u8 _free : 4;
			};
			struct
			{
				u8 wrgba : 4;
			};
			u8 key;
		};
		ColorMaskSelector(): key(0xF) {}
		ColorMaskSelector(u8 c): key(0) { wrgba = c; }
	};
#pragma pack(pop)
	struct alignas(16) VSConstantBuffer
	{
		GSVector2 vertex_scale;
		GSVector2 vertex_offset;
		GSVector2 texture_scale;
		GSVector2 texture_offset;
		GSVector2 point_size;
		GSVector2i max_depth;
		__fi VSConstantBuffer()
		{
			memset(this, 0, sizeof(*this));
		}
		__fi VSConstantBuffer(const VSConstantBuffer& other)
		{
			memcpy(this, &other, sizeof(*this));
		}
		__fi VSConstantBuffer& operator=(const VSConstantBuffer& other)
		{
			new (this) VSConstantBuffer(other);
			return *this;
		}
		__fi bool operator==(const VSConstantBuffer& other) const
		{
			return memcmp(this, &other, sizeof(*this)) == 0;
		}
		__fi bool operator!=(const VSConstantBuffer& other) const
		{
			return !(*this == other);
		}
		__fi bool Update(const VSConstantBuffer& other)
		{
			if (*this == other)
				return false;

			memcpy(this, &other, sizeof(*this));
			return true;
		}
	};
	struct alignas(16) PSConstantBuffer
	{
		GSVector4 FogColor_AREF;
		GSVector4 WH;
		GSVector4 TA_MaxDepth_Af;
		GSVector4i FbMask;

		GSVector4 HalfTexel;
		GSVector4 MinMax;
		GSVector4 LODParams;
		GSVector4 STRange;
		GSVector4i ChannelShuffle;
		GSVector2 TCOffsetHack;
		GSVector2 STScale;
		// Where a channel shuffle's fetch by position starts in its source: the page the texture begins on, scaled.
		GSVector4 ChannelOffset;

		GSVector4 DitherMatrix[4];

		GSVector4 ScaleFactor;

		__fi PSConstantBuffer()
		{
			memset(this, 0, sizeof(*this));
		}
		__fi PSConstantBuffer(const PSConstantBuffer& other)
		{
			memcpy(this, &other, sizeof(*this));
		}
		__fi PSConstantBuffer& operator=(const PSConstantBuffer& other)
		{
			new (this) PSConstantBuffer(other);
			return *this;
		}
		__fi bool operator==(const PSConstantBuffer& other) const
		{
			return memcmp(this, &other, sizeof(*this)) == 0;
		}
		__fi bool operator!=(const PSConstantBuffer& other) const
		{
			return !(*this == other);
		}
		__fi bool Update(const PSConstantBuffer& other)
		{
			if (*this == other)
				return false;

			memcpy(this, &other, sizeof(*this));
			return true;
		}
	};
	struct BlendState
	{
		union
		{
			struct
			{
				u8 enable : 1;
				u8 constant_enable : 1;
				u8 op : 6;
				u8 src_factor : 4;
				u8 dst_factor : 4;
				u8 src_factor_alpha : 4;
				u8 dst_factor_alpha : 4;
				u8 constant;
			};
			u32 key;
		};
		BlendState(): key(0) {}
		BlendState(bool enable_, u8 src_factor_, u8 dst_factor_, u8 op_,
			u8 src_alpha_factor_, u8 dst_alpha_factor_, bool constant_enable_, u8 constant_)
			: key(0)
		{
			enable = enable_;
			constant_enable = constant_enable_;
			src_factor = src_factor_;
			dst_factor = dst_factor_;
			op = op_;
			src_factor_alpha = src_alpha_factor_;
			dst_factor_alpha = dst_alpha_factor_;
			constant = constant_;
		}

		// Blending has no effect if RGB is masked.
		bool IsEffective(ColorMaskSelector colormask) const;
	};
	enum class DestinationAlphaMode : u8
	{
		Off,            ///< No destination alpha test
		Stencil,        ///< Emulate using read-only stencil
		StencilOne,     ///< Emulate using read-write stencil (first write wins)
		PrimIDTracking, ///< Emulate by tracking the primitive ID of the last pixel allowed through
		Full,           ///< Full emulation (using barriers / ROV)
	};

	enum class ColClipMode : u8
	{
		NoModify = 0,
		ConvertOnly = 1,
		ResolveOnly = 2,
		ConvertAndResolve = 3,
		EarlyResolve = 4
	};

	GSTexture* rt;        ///< Render target
	GSTexture* ds;        ///< Depth stencil
	GSTexture* tex;       ///< Source texture
	GSTexture* pal;       ///< Palette texture
	const GSVertex* verts;///< Vertices to draw
	const u16* indices;   ///< Indices to draw
	u32 nverts;           ///< Number of vertices
	u32 nindices;         ///< Number of indices
	u32 indices_per_prim; ///< Number of indices that make up one primitive
	const u32* drawlist; ///< For reducing barriers on sprites: a sprite count a run, drawlist_count runs
	u32 drawlist_count;
	GSVector4i scissor; ///< Scissor rect
	GSVector4i drawarea; ///< Area in the framebuffer which will be modified.
	Topology topology;  ///< Draw topology

	alignas(8) PSSelector ps;
	VSSelector vs;

	BlendState blend;
	SamplerSelector sampler;
	ColorMaskSelector colormask;
	DepthStencilSelector depth;

	bool require_one_barrier;  ///< Require texture barrier before draw (also used to requst an rt copy if texture barrier isn't supported)
	bool require_full_barrier; ///< Require texture barrier between all prims

	DestinationAlphaMode destination_alpha;
	SetDATM datm : 2;
	bool line_expand : 1;

	struct AlphaPass
	{
		alignas(8) PSSelector ps;
		bool enable;
		ColorMaskSelector colormask;
		DepthStencilSelector depth;
		float ps_aref;
	};

	AlphaPass alpha_second_pass;

	struct BlendMultiPass
	{
		BlendState blend;
		u8 blend_hw;
		u8 dither;
		bool enable;
	};

	BlendMultiPass blend_multi_pass;

	VSConstantBuffer cb_vs;
	PSConstantBuffer cb_ps;

	// These are here as they need to be preserved between draws, and the state clear only does up to the constant buffers.
	ColClipMode colclip_mode;
	GIFRegFRAME colclip_frame;
	GSVector4i colclip_update_area; ///< Area in the framebuffer which the hw colclip will modify;
};

} // namespace old

#endif
