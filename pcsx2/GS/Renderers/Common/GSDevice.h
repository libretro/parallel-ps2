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

#include "GSRetireRing.h"

#include <array>

#include "common/HashCombine.h"
#include "common/WindowInfo.h"

#include "../../GS.h"
#include "../../GSAlignedClass.h"
#include "../../GSExtra.h"

#include "GSFastList.h"
#include "GSTexture.h"
#include "GSShaderEnums.h"
#include "GSVertex.h"
#include <memory>

enum class ShaderConvert
{
	COPY = 0,
	RGBA8_TO_16_BITS,
	DATM_1,
	DATM_0,
	DATM_1_RTA_CORRECTION,
	DATM_0_RTA_CORRECTION,
	COLCLIP_INIT,
	COLCLIP_RESOLVE,
	RTA_CORRECTION,
	RTA_DECORRECTION,
	TRANSPARENCY_FILTER,
	FLOAT32_TO_16_BITS,
	FLOAT32_TO_32_BITS,
	FLOAT32_TO_RGBA8,
	FLOAT32_TO_RGB8,
	FLOAT16_TO_RGB5A1,
	RGBA8_TO_FLOAT32,
	RGBA8_TO_FLOAT24,
	RGBA8_TO_FLOAT16,
	RGB5A1_TO_FLOAT16,
	RGBA8_TO_FLOAT32_BILN,
	RGBA8_TO_FLOAT24_BILN,
	RGBA8_TO_FLOAT16_BILN,
	RGB5A1_TO_FLOAT16_BILN,
	FLOAT32_TO_FLOAT24,
	DEPTH_COPY,
	DOWNSAMPLE_COPY,
	RGBA_TO_8I,
	MOVE_TEXELS,
	CLUT_4,
	CLUT_8,
	YUV,
	Count
};

enum class SetDATM : u8
{
	DATM0 = 0U,
	DATM1,
	DATM0_RTA_CORRECTION,
	DATM1_RTA_CORRECTION
};

enum class ShaderInterlace
{
	WEAVE = 0,
	BOB = 1,
	BLEND = 2,
	MAD_BUFFER = 3,
	MAD_RECONSTRUCT = 4,
	Count
};

static inline bool HasDepthOutput(ShaderConvert shader)
{
	switch (shader)
	{
		case ShaderConvert::RGBA8_TO_FLOAT32:
		case ShaderConvert::RGBA8_TO_FLOAT24:
		case ShaderConvert::RGBA8_TO_FLOAT16:
		case ShaderConvert::RGB5A1_TO_FLOAT16:
		case ShaderConvert::RGBA8_TO_FLOAT32_BILN:
		case ShaderConvert::RGBA8_TO_FLOAT24_BILN:
		case ShaderConvert::RGBA8_TO_FLOAT16_BILN:
		case ShaderConvert::RGB5A1_TO_FLOAT16_BILN:
		case ShaderConvert::FLOAT32_TO_FLOAT24:
		case ShaderConvert::DEPTH_COPY:
			return true;
		default:
			break;
	}
	return false;
}

static inline u32 ShaderConvertWriteMask(ShaderConvert shader)
{
	switch (shader)
	{
		case ShaderConvert::FLOAT32_TO_RGB8:
			return 0x7;
		default:
			break;
	}
	return 0xf;
}

enum class PresentShader
{
	COPY = 0,
	Count
};

/// Get the name of a shader
/// (Can't put methods on an enum class)
int SetDATMShader(SetDATM datm);
const char* shaderName(ShaderConvert value);

enum ChannelFetch
{
	ChannelFetch_NONE  = 0,
	ChannelFetch_RED   = 1,
	ChannelFetch_GREEN = 2,
	ChannelFetch_BLUE  = 3,
	ChannelFetch_ALPHA = 4,
	ChannelFetch_RGB   = 5,
	ChannelFetch_GXBY  = 6
};

/* HW blend_hw shader bit values (m_conf.ps.blend_hw / blend_multi_pass.blend_hw) */
#define HW_BLEND_SRC_ONE_DST_FACTOR      1 /* Use the dest color as blend factor, Cs is set to 1. */
#define HW_BLEND_SRC_ALPHA_DST_FACTOR    2 /* Use the dest color as blend factor, Cs is set to (Alpha - 1). */
#define HW_BLEND_SRC_DOUBLE              3 /* Double source color. */
#define HW_BLEND_SRC_HALF_ONE_DST_FACTOR 4 /* Cs is set to 0.5, additionally divide As or Af by 2. */
#define HW_BLEND_SRC_INV_DST_BLEND_HALF  5 /* Halve the alpha then double the final result. */
#define HW_BLEND_INV_SRC_DST_BLEND_HALF  6 /* Halve the alpha then double the final result. */
#define HW_BLEND_BMIX1_ALPHA_HIGH_ONE    1 /* Blend formula replaced when alpha is higher than 1. */
#define HW_BLEND_BMIX1_SRC_HALF          2 /* Impossible blend always wrong on hw, divide Cs by 2. */
#define HW_BLEND_BMIX2_OVERFLOW          3 /* Blending Cs might overflow, try to compensate. */

struct alignas(16) DisplayConstantBuffer
{
	GSVector4 SourceRect; // +0,xyzw
	GSVector4 TargetRect; // +16,xyzw
	GSVector2 SourceSize; // +32,xy
	GSVector2 TargetSize; // +40,zw
	GSVector2 TargetResolution; // +48,xy
	GSVector2 RcpTargetResolution; // +56,zw
	GSVector2 SourceResolution; // +64,xy
	GSVector2 RcpSourceResolution; // +72,zw
	// +96

	// assumes that sRect is normalized
	void SetSource(const GSVector4& sRect, const GSVector2i& sSize)
	{
		SourceRect = sRect;
		SourceResolution = GSVector2(static_cast<float>(sSize.x), static_cast<float>(sSize.y));
		RcpSourceResolution = GSVector2(1.0f) / SourceResolution;
		SourceSize = GSVector2((sRect.z - sRect.x) * SourceResolution.x, (sRect.w - sRect.y) * SourceResolution.y);
	}
	void SetTarget(const GSVector4& dRect, const GSVector2i& dSize)
	{
		TargetRect = dRect;
		TargetResolution = GSVector2(static_cast<float>(dSize.x), static_cast<float>(dSize.y));
		RcpTargetResolution = GSVector2(1.0f) / TargetResolution;
		TargetSize = GSVector2(dRect.z - dRect.x, dRect.w - dRect.y);
	}
};

struct alignas(16) MergeConstantBuffer
{
	GSVector4 BGColor;
	u32 EMODA;
	u32 EMODC;
	u32 DOFFSET;
	float ScaleFactor;
};

/* A local-memory move worked out on the targets (ps_move_texels), packed
 * so that it fits every backend's utility constants, 32 bytes on D3D11.
 * Pairs are x | y << 16, in texels of the staging texture the move reads:
 * the old words of the destination block, the source block, and the
 * remap, one 32-bit entry per destination element. flags is
 * scale | nibbles << 8 | entries << 16 | has_old << 24: the integer
 * scale, the nibbles of a destination element, the elements a word holds
 * (8 / nibbles), and whether the words keep the bits the move leaves. */
struct alignas(16) MoveTexelsConstants
{
	u32 dst_origin;
	u32 old_base;
	u32 src_base;
	u32 remap_base;
	u32 flags;
	u32 pad[3];
};

struct alignas(16) InterlaceConstantBuffer
{
	GSVector4 ZrH; // data passed to the shader
};

/* The two passes of a VR frame (GSDevice::VRCompose), as the constants of
 * ps_vr_range and ps_vr_eye in each backend's vr.glsl. Both read the
 * atlas VRCompose builds: the picture in rows [0, H), the bytes of the
 * GS's Z under it in rows [H, 2H), and the depth range in texel (0, 2H).
 *
 * VRRangeConstants: one level of the reduction of the depth to its range.
 * src is the region read (x, y, width, height), params the block of it
 * one output texel covers, the input (VR_RANGE_FROM_Z: Z bytes, else
 * ranges), and the texel holding last frame's range when VR_RANGE_SMOOTH
 * eases this one towards it; fparams the scale that takes Z to [0, 1]
 * and how far towards the new range a frame moves. */
#define VR_RANGE_FROM_Z 1
#define VR_RANGE_SMOOTH 2

struct alignas(16) VRRangeConstants
{
	GSVector4i src;
	GSVector4i params;
	GSVector4 fparams;
};

/* VREyeConstants: one eye, in the space of the window: x right, y up, the
 * window centred on -z at screen.z, screen.x by screen.y across. The eye
 * is at eye_pos, turned by eye_quat (x, y, z, w), and sees fov_tan (the
 * tangents to its left, right, up and down edges). depth holds the
 * inverse distances, in units of the window's, of the nearest and the
 * farthest depth, then the Z scale; screen.w caps the march's steps. */
struct alignas(16) VREyeConstants
{
	GSVector4 eye_pos;
	GSVector4 eye_quat;
	GSVector4 fov_tan;
	GSVector4 screen;
	GSVector4 depth;
};

class GSDevice;

/* What a backend that makes VR frames answers, beside gs_device_ops: the
 * two passes, drawn over dRect of dTex from sTex. NULL in a device that
 * has none, which shows the plain frame. */
struct gs_vr_ops
{
	void (*range)(GSDevice* dev, GSTexture* sTex, GSTexture* dTex, const GSVector4* dRect, const VRRangeConstants* cb);
	void (*eye)(GSDevice* dev, GSTexture* sTex, GSTexture* dTex, const GSVector4* dRect, const VREyeConstants* cb);
};

/* One VR frame: where the depth of the displayed picture is, and the two
 * eyes to draw. depth is NULL when there is none, and the eyes see a
 * flat window. depth_rect is the part of depth under picture_rect of the
 * picture (normalised; pixels), depth_encoded whether depth is already
 * the bytes of Z (a snapshot) or the depth buffer itself, z_scale what
 * takes those bytes to [0, 1] for the Z format, and eye_size the size of
 * one eye. */
struct GSVRFrame
{
	VREyeConstants eyes[2];
	GSVector4 depth_rect;
	GSVector4 picture_rect;
	GSTexture* depth;
	GSVector2i eye_size;
	float z_scale;
	bool depth_encoded;
};

enum HWBlendFlags
{
	// Flags to determine blending behavior
	BLEND_CD     = 0x1,    // Output is Cd, hw blend can handle it
	BLEND_HW1    = 0x2,    // Clear color blending (use directly the destination color as blending factor)
	BLEND_HW2    = 0x4,    // Clear color blending (use directly the destination color as blending factor)
	BLEND_HW3    = 0x8,    // Multiply Cs by (255/128) to compensate for wrong Ad/255 value, should be Ad/128
	BLEND_HW4    = 0x10,   // HW rendering is split in 2 passes
	BLEND_HW5    = 0x20,   // HW rendering is split in 2 passes
	BLEND_HW6    = 0x40,   // HW rendering is split in 2 passes
	BLEND_HW7    = 0x80,   // HW rendering is split in 2 passes
	BLEND_HW8    = 0x100,  // HW rendering is split in 2 passes
	BLEND_HW9    = 0x200,  // HW rendering is split in 2 passes
	BLEND_MIX1   = 0x400,  // Mix of hw and sw, do Cs*F or Cs*As in shader
	BLEND_MIX2   = 0x800,  // Mix of hw and sw, do Cs*(As + 1) or Cs*(F + 1) in shader
	BLEND_MIX3   = 0x1000, // Mix of hw and sw, do Cs*(1 - As) or Cs*(1 - F) in shader
	BLEND_ACCU   = 0x2000, // Allow to use a mix of SW and HW blending to keep the best of the 2 worlds
	BLEND_NO_REC = 0x4000, // Doesn't require sampling of the RT as a texture
	BLEND_A_MAX  = 0x8000, // Impossible blending uses coeff bigger than 1
};

// Determines the HW blend function for DX11/OGL
struct HWBlend
{
	u16 flags;
	u8 op, src, dst;
};

struct alignas(16) GSHWDrawConfig
{
	enum class Topology: u8
	{
		Point,
		Line,
		Triangle,
	};
	enum class VSExpand: u8
	{
		None,
		Point,
		Line,
		Sprite
	};
#pragma pack(push, 1)
	struct VSSelector
	{
		union
		{
			struct
			{
				u8 fst : 1;
				u8 tme : 1;
				u8 iip : 1;
				u8 point_size : 1;		///< Set when points need to be expanded without VS expanding.
				VSExpand expand : 2;
				u8 _free : 2;
			};
			u8 key;
		};
		VSSelector(): key(0) {}
		VSSelector(u8 k): key(k) {}

		/// Returns true if the fixed index buffer should be used.
		__fi bool UseExpandIndexBuffer() const { return (expand == VSExpand::Point || expand == VSExpand::Sprite); }
	};
#pragma pack(pop)
#pragma pack(push, 4)
	struct PSSelector
	{
		// Performance note: there are too many shader combinations
		// It might hurt the performance due to frequent toggling worse it could consume
		// a lots of memory.
		union
		{
			struct
			{
				// Format
				u32 aem_fmt   : 2;
				u32 pal_fmt   : 2;
				u32 dst_fmt   : 2; // 0 → 32-bit, 1 → 24-bit, 2 → 16-bit
				u32 depth_fmt : 2; // 0 → None, 1 → 32-bit, 2 → 16-bit, 3 → RGBA
				// Alpha extension/Correction
				u32 aem : 1;
				u32 fba : 1;
				// Fog
				u32 fog : 1;
				// Flat/goround shading
				u32 iip : 1;
				// Pixel test
				u32 date : 3;
				GSShader::PS_ATST atst : 3;
				GSShader::PS_AFAIL afail : 3;
				// Color sampling
				u32 fst : 1; // Investigate to do it on the VS
				u32 tfx : 3;
				u32 tcc : 1;
				u32 wms : 2;
				u32 wmt : 2;
				u32 adjs : 1;
				u32 adjt : 1;
				u32 ltf : 1;
				// Shuffle and fbmask effect
				u32 shuffle  : 1;
				u32 shuffle_same : 1;
				u32 real16src: 1;
				u32 process_ba : 2;
				u32 process_rg : 2;
				u32 shuffle_across : 1;
				u32 write_rg : 1;
				u32 fbmask   : 1;

				// Blend and Colclip
				u32 blend_a        : 2;
				u32 blend_b        : 2;
				u32 blend_c        : 2;
				u32 blend_d        : 2;
				u32 fixed_one_a    : 1;
				u32 blend_hw       : 3;
				u32 a_masked       : 1;
				u32 colclip_hw     : 1;
				u32 rta_correction : 1;
				u32 rta_source_correction : 1;
				u32 colclip        : 1;
				u32 blend_mix      : 2;
				u32 round_inv      : 1; // Blending will invert the value, so rounding needs to go the other way
				u32 pabe           : 1;
				u32 no_color       : 1; // disables color output entirely (depth only)
				u32 no_color1      : 1; // disables second color output (when unnecessary)

				// Others ways to fetch the texture
				u32 channel : 3;

				// Dithering
				u32 dither : 2;
				u32 dither_adjust : 1;

				// Depth clamp
				u32 zclamp : 1;

				// Hack
				u32 tcoffsethack : 1;
				u32 urban_chaos_hle : 1;
				u32 tales_of_abyss_hle : 1;
				u32 tex_is_fb : 1; // Jak Shadows
				u32 automatic_lod : 1;
				u32 manual_lod : 1;
				u32 point_sampler : 1;
				u32 region_rect : 1;

				// Scan mask
				u32 scanmsk : 2;

				// Indexed source drawn at scale: pick texels at native resolution
				u32 sample_map : 1;
				// Bilinear read of a scaled target: the two taps a native texel apart, weighed as the native pixel weighs them
				u32 native_taps : 1;
			};

			struct
			{
				u64 key_lo;
				u32 key_hi;
			};
		};
		__fi PSSelector() : key_lo(0), key_hi(0) {}

		__fi bool operator==(const PSSelector& rhs) const { return (key_lo == rhs.key_lo && key_hi == rhs.key_hi); }
		__fi bool operator!=(const PSSelector& rhs) const { return (key_lo != rhs.key_lo || key_hi != rhs.key_hi); }
		__fi bool operator<(const PSSelector& rhs) const { return (key_lo < rhs.key_lo || key_hi < rhs.key_hi); }

		__fi bool IsFeedbackLoop() const
		{
			const u32 sw_blend_bits = blend_a | blend_b | blend_d;
			const bool sw_blend_needs_rt = sw_blend_bits != 0 && ((sw_blend_bits | blend_c) & 1u);
			return tex_is_fb || fbmask || (date > 0 && date != 3) || sw_blend_needs_rt;
		}

		/// Disables color output from the pixel shader, this is done when all channels are masked.
		__fi void DisableColorOutput()
		{
			// remove software blending, since this will cause the color to be declared inout with fbfetch.
			blend_a = blend_b = blend_c = blend_d = 0;

			// TEX_IS_FB relies on us having a color output to begin with.
			tex_is_fb = 0;

			// no point having fbmask, since we're not writing. DATE has to stay.
			fbmask = 0;

			// disable both outputs.
			no_color = no_color1 = 1;
		}
	};
#pragma pack(pop)
	struct PSSelectorHash
	{
		std::size_t operator()(const PSSelector& p) const
		{
			std::size_t h = 0;
			HashCombine(h, p.key_lo, p.key_hi);
			return h;
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
			return BitEqual(*this, other);
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
			return BitEqual(*this, other);
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
	const std::vector<size_t>* drawlist; ///< For reducing barriers on sprites
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

struct gs_device_ops;

class GSDevice : public GSAlignedClass<32>
{
public:
	enum class PresentResult
	{
		OK,
		FrameSkipped,
		DeviceLost
	};

	enum class DebugMessageCategory
	{
		Cache,
		Reg,
		Debug,
		Message,
		Performance
	};

	// clang-format off
	struct FeatureSupport
	{
		bool broken_point_sampler : 1; ///< Issue with AMD cards, see tfx shader for details
		bool vs_expand            : 1; ///< Supports expanding points/lines/sprites in the vertex shader
		bool primitive_id         : 1; ///< Supports primitive ID for use with prim tracking destination alpha algorithm
		bool texture_barrier      : 1; ///< Supports sampling rt and hopefully texture barrier
		bool provoking_vertex_last: 1; ///< Supports using the last vertex in a primitive as the value for flat shading.
		bool point_expand         : 1; ///< Supports point expansion in hardware.
		bool line_expand          : 1; ///< Supports line expansion in hardware.
		bool prefer_new_textures  : 1; ///< Allocate textures up to the pool size before reusing them, to avoid render pass restarts.
		bool dxt_textures         : 1; ///< Supports DXTn texture compression, i.e. S3TC and BC1-3.
		bool bptc_textures        : 1; ///< Supports BC6/7 texture compression.
		bool framebuffer_fetch    : 1; ///< Can sample from the framebuffer without texture barriers.
		bool clip_control         : 1; ///< Can use 0..1 depth range instead of -1..1.
		bool stencil_buffer       : 1; ///< Supports stencil buffer, and can use for DATE.
		bool test_and_sample_depth: 1; ///< Supports concurrently binding the depth-stencil buffer for sampling and depth testing.
		bool dual_source_blend    : 1; ///< Supports a second fragment output (SRC1) as a hardware blend factor.
		bool colclip_hw           : 1; ///< Has a render target format that holds a 16-bit integer per channel exactly, for the colour clip accumulation.
		FeatureSupport()
		{
			memset(this, 0, sizeof(*this));
			// Every backend but OpenGL ES has both.
			dual_source_blend = true;
			colclip_hw = true;
		}
	};

	struct MultiStretchRect
	{
		GSVector4 src_rect;
		GSVector4 dst_rect;
		GSTexture* src;
		bool linear;
		GSHWDrawConfig::ColorMaskSelector wmask; // 0xf for all channels by default
	};

	struct TextureRecycleDeleter
	{
		void operator()(GSTexture* const tex);
	};
	using RecycledTexture = std::unique_ptr<GSTexture, TextureRecycleDeleter>;

	enum BlendFactor : u8
	{
		// HW blend factors
		SRC_COLOR,   INV_SRC_COLOR,   DST_COLOR,  INV_DST_COLOR,
		SRC1_COLOR,  INV_SRC1_COLOR,  SRC_ALPHA,  INV_SRC_ALPHA,
		DST_ALPHA,   INV_DST_ALPHA,   SRC1_ALPHA, INV_SRC1_ALPHA,
		CONST_COLOR, INV_CONST_COLOR, CONST_ONE,  CONST_ZERO,
	};
	enum BlendOp : u8
	{
		// HW blend operations
		OP_ADD, OP_SUBTRACT, OP_REV_SUBTRACT
	};
	// clang-format on

private:
	std::array<FastList<GSTexture*>, 2> m_pool; // [texture, target]

	static const HWBlend m_blendMap[81];

protected:
	static constexpr int NUM_INTERLACE_SHADERS = 5;
	/* How many the pool may hold. The ages that used to sit beside these
	 * are gone: a pooled texture is not freed for being old, only for
	 * putting the pool over one of its two ceilings, so in steady state
	 * nothing is freed at all. */
	static constexpr u32 MAX_POOLED_TARGETS = 300;
	static constexpr u32 MAX_POOLED_TEXTURES = 300;

	/* What the pool may hold, in bytes, from what the console has rather
	 * than from a number someone picked.
	 *
	 * A count bounds nothing. A target at 4x is 4096x3584, 58 MB, and
	 * three hundred of those is seventeen gigabytes the pool sits on
	 * instead of handing back - which is what "Failed to allocate device
	 * memory for 4096x3584 texture" is: not a card too small for the
	 * game, a pool holding every size the game has ever used.
	 *
	 * The PS2 has four megabytes of GS memory, and every render target,
	 * depth buffer and texture the game has at any moment lives in those
	 * four megabytes. That is the real bound, and the host copy of it is
	 * the same thing multiplied by the upscale squared: 64 MB at 4x, 16
	 * at 2x, 4 at native. The pool holds a few times the live set so
	 * that a target returned this frame and wanted the next is still
	 * there, and nothing beyond that.
	 *
	 * For comparison, paraLLEl-GS allocates one buffer of exactly
	 * GSLocalMemory::m_vmsize and renders out of it; its video memory is
	 * bounded by construction, because it never makes a host texture per
	 * target at all. GSdx has to, but the count of them is still the
	 * console's, not three hundred. */
	static constexpr u32 POOL_LIVE_SETS_TARGETS  = 4;
	static constexpr u32 POOL_LIVE_SETS_TEXTURES = 2;

	/* pool_idx 0 is textures, 1 is targets. */
	/* The upscale the pool's textures are at, which is not the option's
	 * value for the software renderer. */
	float PoolScale() const;
	u64 PoolByteBudget(u32 pool_idx) const;



	u64 m_pool_memory_usage[2] = {};
	static constexpr u32 EXPAND_BUFFER_SIZE = sizeof(u16) * 16383 * 6;

	WindowInfo m_window_info;

	GSTexture* m_merge = nullptr;
	GSTexture* m_weavebob = nullptr;
	GSTexture* m_blend = nullptr;
	GSTexture* m_mad = nullptr;
	/* The merge that last carried each field, which the weave and bob read
	 * the other field's lines from. Never handed to the frontend. */
	GSTexture* m_weave_field[2];
	/* Textures that have been handed to the frontend by reference (the
	 * merge/interlace chain the present registers) cannot be destroyed
	 * the moment a resize replaces them: the frontend samples the
	 * registered image in its own submissions and may replay it for
	 * pause or screenshots, none of which our fences observe. They are
	 * retired here instead (GSRetireRing.h) and freed once the frontend
	 * is done with them. */
	/* How many presents a retired texture is held for when nothing better
	 * is known - a guess at how long the frontend might still reference
	 * it, which a frontend that caches frames for longer would outlive.
	 * Where the frontend tells us, SyncIndexWaited below is used instead
	 * and this does not apply. */
	static constexpr u32 RETIRED_PRESENT_MIN_AGE = 8;
	struct gs_retire_ring m_retired_present = {};
	u32 m_present_age = 0;
	/* What the frontend has actually told us, where it does. A backend
	 * that hands textures over per sync index calls SyncIndexWaited right
	 * after its wait_sync_index returns; once one full trip of the
	 * frontend's slots has been waited through since a texture was
	 * retired, the frontend has demonstrably finished with it, and no
	 * guess at a frame count is involved. */
	u32 m_sync_waits = 0;
	u32 m_sync_slots = 0;
	GSTexture* m_target_tmp = nullptr;
	GSTexture* m_current = nullptr;
	GSTexture* m_colclip_rt = nullptr; ///< Temp hw colclip texture

	/* VR (VRCompose). The output is handed to the frontend like the merge
	 * target, and retired like it; the rest is the device's own. */
	GSTexture* m_vr_out;
	GSTexture* m_vr_atlas;
	GSTexture* m_vr_reduce[2];
	GSTexture* m_vr_range[2];
	u32 m_vr_range_cur;
	u32 m_vr_depth_missing; /* Frames in a row VRCompose had no depth for */

	struct
	{
		u32 start, count;
	} m_vertex = {};
	struct
	{
		u32 start, count;
	} m_index = {};
	unsigned int m_frame = 0; // for ageing the pool
	bool m_rbswapped = false;
	FeatureSupport m_features;

	void AcquireWindow();

	GSTexture* CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format);
	GSTexture* FetchSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format, bool clear, bool prefer_reuse);

	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear);
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb);

public:
	GSDevice();
	~GSDevice();

	/* The table this device answers through, and how one is disposed of
	 * now that the destructor is not virtual (gs_device_ops, below). */
	const struct gs_device_ops* m_ops = nullptr;
	/* Set by a device that makes VR frames, NULL otherwise. */
	const struct gs_vr_ops* m_vr_ops;
	/* The Z of a depth buffer as bytes (VRSnapshotDepth), kept for the
	 * frame buffer it was drawn with once the game draws another with it;
	 * NULL until one is taken. */
	GSTexture* m_vr_snapshot;
	void Free();

	/// Generates a fixed index buffer for expanding points and sprites. Buffer is assumed to be at least EXPAND_BUFFER_SIZE in size.
	static void GenerateExpansionIndexBuffer(void* buffer);

	/// Returns the maximum number of mipmap levels for a given texture size.
	static int GetMipmapLevelsForSize(int width, int height);

	__fi FeatureSupport Features() const { return m_features; }

	__fi s32 GetWindowWidth() const { return static_cast<s32>(m_window_info.surface_width); }
	__fi s32 GetWindowHeight() const { return static_cast<s32>(m_window_info.surface_height); }
	__fi GSVector2i GetWindowSize() const { return GSVector2i(static_cast<s32>(m_window_info.surface_width), static_cast<s32>(m_window_info.surface_height)); }
	__fi GSTexture* GetCurrent() const { return m_current; }

	GSTexture* GetColorClipTexture() const { return m_colclip_rt; }

	void SetColorClipTexture(GSTexture* tex) { m_colclip_rt = tex; }

	void Recycle(GSTexture* t);

	/* What GSDevice itself does; a device's own version calls the Base one
	 * where it used to call GSDevice::X(). */
	bool CreateBase();
	void DestroyBase();
	void DrawMultiStretchRectsBase(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader);

	bool Create();
	void Destroy();

	void ResetAPIState();
	void RestoreAPIState();

	/// Returns the graphics API used by this device.
	RenderAPI GetRenderAPI() const;

	/// Returns false if the window was completely occluded. If frame_skip is set, the frame won't be
	/// displayed, but the GPU command queue will still be flushed.
	PresentResult BeginPresent(bool frame_skip);

	/// Presents the frame to the display.
	void EndPresent();

	void ClearRenderTarget(GSTexture* t, u32 c);
	void ClearDepth(GSTexture* t, float d);
	void InvalidateRenderTarget(GSTexture* t);

	GSTexture* CreateRenderTarget(int w, int h, GSTexture::Format format, bool clear = true);
	GSTexture* CreateDepthStencil(int w, int h, GSTexture::Format format, bool clear = true);
	GSTexture* CreateTexture(int w, int h, int mipmap_levels, GSTexture::Format format, bool prefer_reuse = false);

	GSDownloadTexturePtr CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format);

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY);
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader = ShaderConvert::COPY, bool linear = true);
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha, ShaderConvert shader = ShaderConvert::COPY);

	void StretchRect(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader = ShaderConvert::COPY, bool linear = true);

	/// Performs a screen blit for display. If dTex is null, it assumes you are writing to the system framebuffer/swap chain.
	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect);

	/// Same as doing StretchRect for each item, except tries to batch together rectangles in as few draws as possible.
	/// The provided list should be sorted by texture, the implementations only check if it's the same as the last.
	void DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader = ShaderConvert::COPY);

	/// Sorts a MultiStretchRect list for optimal batching.
	static void SortMultiStretchRects(MultiStretchRect* rects, u32 num_rects);

	/// Updates a GPU CLUT texture from a source texture.
	void UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize, u32 samples);

	/// Converts a colour format to an indexed format texture. offsetX/offsetY
	/// are page-aligned, unscaled pixel offsets into the source target.
	void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM);

	/// The page of a 32-bit source that a page-aligned pixel offset lands on.
	static u32 IndexedConversionPageOffset(u32 offsetX, u32 offsetY, u32 SBW)
	{
		return (offsetY / 32) * (pcsx2_max_u(SBW, 64u) / 64) + (offsetX / 64);
	}

	/// Uses box downsampling to resize a texture.
	void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect);

	/// Rebuilds the words of dTex in dRect from the staging texture sTex
	/// (see MoveTexelsConstants). False when the device has no such pass.
	bool MoveTexels(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const MoveTexelsConstants& cb);

	void RenderHW(GSHWDrawConfig& config);

	void ClearSamplerCache();

	/* Creates the target sizes a PS2 game actually uses, at the current
	 * upscale, and puts them in the pool - so the first draw allocates
	 * nothing either. Called once the device is fully up, by
	 * OpenGSDevice, not from the device's own Create. */
	void PrewarmPool();

	void ClearCurrent();

	/* VR (GSVR.h). VRCompose draws both eyes of frame from the picture,
	 * side by side, left first, and returns the texture they are in, or
	 * NULL when it could not. VRBlank returns that texture black at size
	 * (both eyes), for a blank frame. VRSnapshotDepth keeps the Z of a
	 * depth buffer as bytes in m_vr_snapshot. */
	GSTexture* VRCompose(GSTexture* picture, const GSVRFrame& frame);
	GSTexture* VRBlank(const GSVector2i& eye_size);
	bool VRSnapshotDepth(GSTexture* depth);
	void VRRelease();

	void Merge(GSTexture* sTex[3], GSVector4* sRect, GSVector4* dRect, const GSVector2i& fs, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c);
	void Interlace(const GSVector2i& ds, int field, int mode, float yoffset, float block, bool half_fields);

	bool ResizeRenderTarget(GSTexture** t, int w, int h, bool preserve_contents, bool recycle, bool defer_destroy = false);
	void RetirePresentTexture(GSTexture* t);
	/* Called by a backend immediately after wait_sync_index returns;
	 * `slots` is how many indices the frontend cycles through. */
	__fi void SyncIndexWaited(u32 slots)
	{
		m_sync_slots = slots;
		m_sync_waits++;
	}
	/* Called by a backend that has just handed the frontend `handed` for a
	 * sync index, with `released`, what that index carried the time before
	 * and the wait on it has made ours again. Every texture that reaches
	 * PresentRect is drawn over in full each frame it is presented - the
	 * merge, the deinterlacers' outputs, the VR eyes - so the one that
	 * held `handed` takes `released` in its place, and the next frame
	 * draws into a texture the frontend has let go of. False when `handed`
	 * is none of them; the backend keeps nothing for the index then. */
	__fi bool SwapPresentTexture(GSTexture* handed, GSTexture* released)
	{
		GSTexture** owner;
		if (handed == m_merge)
			owner = &m_merge;
		else if (handed == m_weavebob)
			owner = &m_weavebob;
		else if (handed == m_blend)
			owner = &m_blend;
		else if (handed == m_vr_out)
			owner = &m_vr_out;
		else
			return false;
		*owner = released;
		if (m_current == handed)
			m_current = released;
		return true;
	}

	bool IsRBSwapped() { return m_rbswapped; }

	void AgePool();
	void PurgePool();

	__fi static constexpr bool IsDualSourceBlendFactor(u8 factor)
	{
		return (factor == SRC1_ALPHA || factor == INV_SRC1_ALPHA || factor == SRC1_COLOR || factor == INV_SRC1_COLOR);
	}
	__fi static constexpr bool IsConstantBlendFactor(u16 factor)
	{
		return (factor == CONST_COLOR || factor == INV_CONST_COLOR);
	}

	// Convert the GS blend equations to HW blend factors/ops
	// Index is computed as ((((A * 3 + B) * 3) + C) * 3) + D. A, B, C, D taken from ALPHA register.
	__ri static HWBlend GetBlend(u32 index) { return m_blendMap[index]; }
	__ri static u16 GetBlendFlags(u32 index) { return m_blendMap[index].flags; }
};

template <>
struct std::hash<GSHWDrawConfig::PSSelector> : public GSHWDrawConfig::PSSelectorHash {};

/* The open device, or NULL. A plain pointer: a device is disposed of with
 * Free(), through its table, since its destructor is not virtual. */
/* What the five devices - D3D11, D3D12, Vulkan, OpenGL and the software
 * one - answer differently, as a table of plain functions with the device
 * as the first argument. This is what GSDevice's virtual functions used
 * to be. Each device fills one in its own .cpp and sets m_ops in its
 * constructor; GSDevice's member functions of the same names call through
 * it, so nothing that calls StretchRect() or RenderHW() on a device had
 * to change. A NULL entry means what GSDevice does by default.
 * GS_DEVICE_OPS_DEFINE below writes the table for a device that has a
 * function of its own for every entry, which is all but the software one. */
struct gs_device_ops
{
	void       (*free)(GSDevice* dev);
	bool       (*create)(GSDevice* dev);
	void       (*destroy)(GSDevice* dev);
	void       (*reset_api_state)(GSDevice* dev);
	void       (*restore_api_state)(GSDevice* dev);
	RenderAPI  (*get_render_api)(const GSDevice* dev);
	int        (*begin_present)(GSDevice* dev, bool frame_skip);
	void       (*end_present)(GSDevice* dev);
	GSTexture* (*create_surface)(GSDevice* dev, GSTexture::Type type, int width, int height, int levels, GSTexture::Format format);
	GSDownloadTexture* (*create_download_texture)(GSDevice* dev, u32 width, u32 height, GSTexture::Format format);
	void       (*do_merge)(GSDevice* dev, GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE* PMODE, const GSRegEXTBUF* EXTBUF, u32 c, bool linear);
	void       (*do_interlace)(GSDevice* dev, GSTexture* sTex, const GSVector4* sRect, GSTexture* dTex, const GSVector4* dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer* cb);
	void       (*copy_rect)(GSDevice* dev, GSTexture* sTex, GSTexture* dTex, const GSVector4i* r, u32 destX, u32 destY);
	void       (*stretch_rect)(GSDevice* dev, GSTexture* sTex, const GSVector4* sRect, GSTexture* dTex, const GSVector4* dRect, ShaderConvert shader, bool linear);
	void       (*stretch_rect_mask)(GSDevice* dev, GSTexture* sTex, const GSVector4* sRect, GSTexture* dTex, const GSVector4* dRect, bool red, bool green, bool blue, bool alpha, ShaderConvert shader);
	void       (*present_rect)(GSDevice* dev, GSTexture* sTex, const GSVector4* sRect, GSTexture* dTex, const GSVector4* dRect);
	void       (*draw_multi_stretch_rects)(GSDevice* dev, const GSDevice::MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader);
	void       (*update_clut_texture)(GSDevice* dev, GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize, u32 samples);
	void       (*convert_to_indexed_texture)(GSDevice* dev, GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM);
	void       (*filtered_downsample_texture)(GSDevice* dev, GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i* clamp_min, const GSVector4* dRect);
	void       (*move_texels)(GSDevice* dev, GSTexture* sTex, GSTexture* dTex, const GSVector4* dRect, const MoveTexelsConstants* cb);
	void       (*render_hw)(GSDevice* dev, GSHWDrawConfig* config);
	void       (*clear_sampler_cache)(GSDevice* dev);
};


extern GSDevice* g_gs_device;

#define GS_DEVICE_CALL(fn, ...) (m_ops->fn(this, ##__VA_ARGS__))

__forceinline_odr void GSDevice::Free()                { m_ops->free(this); }
__forceinline_odr bool GSDevice::Create()              { return m_ops->create ? GS_DEVICE_CALL(create) : CreateBase(); }
__forceinline_odr void GSDevice::Destroy()             { if (m_ops->destroy) GS_DEVICE_CALL(destroy); else DestroyBase(); }
__forceinline_odr void GSDevice::ResetAPIState()       { if (m_ops->reset_api_state) GS_DEVICE_CALL(reset_api_state); }
__forceinline_odr void GSDevice::RestoreAPIState()     { if (m_ops->restore_api_state) GS_DEVICE_CALL(restore_api_state); }
__forceinline_odr RenderAPI GSDevice::GetRenderAPI() const { return m_ops->get_render_api(this); }
__forceinline_odr GSDevice::PresentResult GSDevice::BeginPresent(bool frame_skip) { return static_cast<PresentResult>(GS_DEVICE_CALL(begin_present, frame_skip)); }
__forceinline_odr void GSDevice::EndPresent()          { GS_DEVICE_CALL(end_present); }
__forceinline_odr GSTexture* GSDevice::CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{ return GS_DEVICE_CALL(create_surface, type, width, height, levels, format); }
__forceinline_odr GSDownloadTexturePtr GSDevice::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{ return GSDownloadTexturePtr(GS_DEVICE_CALL(create_download_texture, width, height, format)); }
__forceinline_odr void GSDevice::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear)
{ GS_DEVICE_CALL(do_merge, sTex, sRect, dTex, dRect, &PMODE, &EXTBUF, c, linear); }
__forceinline_odr void GSDevice::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb)
{ GS_DEVICE_CALL(do_interlace, sTex, &sRect, dTex, &dRect, shader, linear, &cb); }
__forceinline_odr void GSDevice::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{ GS_DEVICE_CALL(copy_rect, sTex, dTex, &r, destX, destY); }
__forceinline_odr void GSDevice::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader, bool linear)
{ GS_DEVICE_CALL(stretch_rect, sTex, &sRect, dTex, &dRect, shader, linear); }
__forceinline_odr void GSDevice::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha, ShaderConvert shader)
{ GS_DEVICE_CALL(stretch_rect_mask, sTex, &sRect, dTex, &dRect, red, green, blue, alpha, shader); }
__forceinline_odr void GSDevice::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect)
{ GS_DEVICE_CALL(present_rect, sTex, &sRect, dTex, &dRect); }
__forceinline_odr void GSDevice::DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader)
{
	if (m_ops->draw_multi_stretch_rects)
		GS_DEVICE_CALL(draw_multi_stretch_rects, rects, num_rects, dTex, shader);
	else
		DrawMultiStretchRectsBase(rects, num_rects, dTex, shader);
}
__forceinline_odr void GSDevice::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize, u32 samples)
{ GS_DEVICE_CALL(update_clut_texture, sTex, sScale, offsetX, offsetY, dTex, dOffset, dSize, samples); }
__forceinline_odr void GSDevice::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{ GS_DEVICE_CALL(convert_to_indexed_texture, sTex, sScale, offsetX, offsetY, SBW, SPSM, dTex, DBW, DPSM); }
__forceinline_odr void GSDevice::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect)
{ GS_DEVICE_CALL(filtered_downsample_texture, sTex, dTex, downsample_factor, &clamp_min, &dRect); }
__forceinline_odr bool GSDevice::MoveTexels(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const MoveTexelsConstants& cb)
{
	if (!m_ops->move_texels)
		return false;
	GS_DEVICE_CALL(move_texels, sTex, dTex, &dRect, &cb);
	return true;
}
__forceinline_odr void GSDevice::RenderHW(GSHWDrawConfig& config) { GS_DEVICE_CALL(render_hw, &config); }
__forceinline_odr void GSDevice::ClearSamplerCache() { if (m_ops->clear_sampler_cache) GS_DEVICE_CALL(clear_sampler_cache); }

#undef GS_DEVICE_CALL

/* The table of a device that has its own version of everything, written
 * from the class name: GS_DEVICE_OPS_DEFINE(GSDevice11, d3d11) makes
 * s_d3d11_device_ops out of GSDevice11's member functions. The software
 * device writes its table out by hand, having no version of some. */
#define GS_DEVICE_OPS_THUNKS(klass, tag) \
struct klass##_ops_access { \
	static void       tag##_free(GSDevice* d) { delete static_cast<klass*>(d); } \
	static bool       tag##_create(GSDevice* d) { return static_cast<klass*>(d)->Create(); } \
	static void       tag##_destroy(GSDevice* d) { static_cast<klass*>(d)->Destroy(); } \
	static void       tag##_reset_api_state(GSDevice* d) { static_cast<klass*>(d)->ResetAPIState(); } \
	static void       tag##_restore_api_state(GSDevice* d) { static_cast<klass*>(d)->RestoreAPIState(); } \
	static RenderAPI  tag##_get_render_api(const GSDevice* d) { return static_cast<const klass*>(d)->GetRenderAPI(); } \
	static int        tag##_begin_present(GSDevice* d, bool frame_skip) { return static_cast<int>(static_cast<klass*>(d)->BeginPresent(frame_skip)); } \
	static void       tag##_end_present(GSDevice* d) { static_cast<klass*>(d)->EndPresent(); } \
	static GSTexture* tag##_create_surface(GSDevice* d, GSTexture::Type type, int w, int h, int levels, GSTexture::Format format) { return static_cast<klass*>(d)->CreateSurface(type, w, h, levels, format); } \
	static GSDownloadTexture* tag##_create_download_texture(GSDevice* d, u32 w, u32 h, GSTexture::Format format) { return static_cast<klass*>(d)->CreateDownloadTexture(w, h, format).release(); } \
	static void       tag##_do_merge(GSDevice* d, GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE* PMODE, const GSRegEXTBUF* EXTBUF, u32 c, bool linear) { static_cast<klass*>(d)->DoMerge(sTex, sRect, dTex, dRect, *PMODE, *EXTBUF, c, linear); } \
	static void       tag##_do_interlace(GSDevice* d, GSTexture* sTex, const GSVector4* sRect, GSTexture* dTex, const GSVector4* dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer* cb) { static_cast<klass*>(d)->DoInterlace(sTex, *sRect, dTex, *dRect, shader, linear, *cb); } \
	static void       tag##_copy_rect(GSDevice* d, GSTexture* sTex, GSTexture* dTex, const GSVector4i* r, u32 destX, u32 destY) { static_cast<klass*>(d)->CopyRect(sTex, dTex, *r, destX, destY); } \
	static void       tag##_stretch_rect(GSDevice* d, GSTexture* sTex, const GSVector4* sRect, GSTexture* dTex, const GSVector4* dRect, ShaderConvert shader, bool linear) { static_cast<klass*>(d)->StretchRect(sTex, *sRect, dTex, *dRect, shader, linear); } \
	static void       tag##_stretch_rect_mask(GSDevice* d, GSTexture* sTex, const GSVector4* sRect, GSTexture* dTex, const GSVector4* dRect, bool red, bool green, bool blue, bool alpha, ShaderConvert shader) { static_cast<klass*>(d)->StretchRect(sTex, *sRect, dTex, *dRect, red, green, blue, alpha, shader); } \
	static void       tag##_present_rect(GSDevice* d, GSTexture* sTex, const GSVector4* sRect, GSTexture* dTex, const GSVector4* dRect) { static_cast<klass*>(d)->PresentRect(sTex, *sRect, dTex, *dRect); } \
	static void       tag##_draw_multi_stretch_rects(GSDevice* d, const GSDevice::MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader) { static_cast<klass*>(d)->DrawMultiStretchRects(rects, num_rects, dTex, shader); } \
	static void       tag##_update_clut_texture(GSDevice* d, GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize, u32 samples) { static_cast<klass*>(d)->UpdateCLUTTexture(sTex, sScale, offsetX, offsetY, dTex, dOffset, dSize, samples); } \
	static void       tag##_convert_to_indexed_texture(GSDevice* d, GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM) { static_cast<klass*>(d)->ConvertToIndexedTexture(sTex, sScale, offsetX, offsetY, SBW, SPSM, dTex, DBW, DPSM); } \
	static void       tag##_filtered_downsample_texture(GSDevice* d, GSTexture* sTex, GSTexture* dTex, u32 factor, const GSVector2i* clamp_min, const GSVector4* dRect) { static_cast<klass*>(d)->FilteredDownsampleTexture(sTex, dTex, factor, *clamp_min, *dRect); } \
	static void       tag##_move_texels(GSDevice* d, GSTexture* sTex, GSTexture* dTex, const GSVector4* dRect, const MoveTexelsConstants* cb) { static_cast<klass*>(d)->MoveTexels(sTex, dTex, *dRect, *cb); } \
	static void       tag##_render_hw(GSDevice* d, GSHWDrawConfig* config) { static_cast<klass*>(d)->RenderHW(*config); } \
	static void       tag##_clear_sampler_cache(GSDevice* d) { static_cast<klass*>(d)->ClearSamplerCache(); } \
};

#define GS_DEVICE_OPS_TABLE(klass, tag) \
	static const struct gs_device_ops s_##tag##_device_ops = { \
		klass##_ops_access::tag##_free, klass##_ops_access::tag##_create, \
		klass##_ops_access::tag##_destroy, klass##_ops_access::tag##_reset_api_state, \
		klass##_ops_access::tag##_restore_api_state, klass##_ops_access::tag##_get_render_api, \
		klass##_ops_access::tag##_begin_present, klass##_ops_access::tag##_end_present, \
		klass##_ops_access::tag##_create_surface, klass##_ops_access::tag##_create_download_texture, \
		klass##_ops_access::tag##_do_merge, klass##_ops_access::tag##_do_interlace, \
		klass##_ops_access::tag##_copy_rect, klass##_ops_access::tag##_stretch_rect, \
		klass##_ops_access::tag##_stretch_rect_mask, klass##_ops_access::tag##_present_rect, \
		klass##_ops_access::tag##_draw_multi_stretch_rects, klass##_ops_access::tag##_update_clut_texture, \
		klass##_ops_access::tag##_convert_to_indexed_texture, \
		klass##_ops_access::tag##_filtered_downsample_texture, \
		klass##_ops_access::tag##_move_texels, \
		klass##_ops_access::tag##_render_hw, klass##_ops_access::tag##_clear_sampler_cache }

#define GS_DEVICE_OPS_DEFINE(klass, tag) GS_DEVICE_OPS_THUNKS(klass, tag) GS_DEVICE_OPS_TABLE(klass, tag)
