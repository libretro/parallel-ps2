/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GS_HW_DRAW_CONFIG_H
#define GS_HW_DRAW_CONFIG_H

/* What a HW draw is drawn with (GSHWDrawConfig): its targets, vertices
 * and indices, the keys its shaders, sampler, depth test, colour mask and
 * blend are set up by, its constant buffers, and the passes after it.
 * One layout for C and C++: C++ names it and its parts as GSHWDrawConfig
 * and its members (GSDevice.h), with their constructors and helpers, its
 * enum classes and GSVectors where C has their underlying types and
 * gs_rect.h's. */

#include <string.h>

#include <boolean.h>

#include "common/Pcsx2Defs.h"
#include "../../GSRegs.h"
#include "../../gs_rect.h"
#include "GSTextureBase.h"

#ifdef __cplusplus
#include "../../GSVector.h"
#include "GSShaderEnums.h"
#endif

union gs_vertex;

/* How the vertex shader expands a primitive (GSHWDrawConfig::VSExpand) */
#define GS_HW_VS_EXPAND_NONE 0
#define GS_HW_VS_EXPAND_POINT 1
#define GS_HW_VS_EXPAND_LINE 2
#define GS_HW_VS_EXPAND_SPRITE 3

#ifdef __cplusplus
enum class gs_hw_vs_expand : u8
{
	None,
	Point,
	Line,
	Sprite
};
#endif

#ifdef __cplusplus
#define GS_HW_ENUM(cxx, c) cxx
#else
#define GS_HW_ENUM(cxx, c) c
#endif

#pragma pack(push, 1)
typedef struct gs_hw_vs_selector
{
	union
	{
		struct
		{
			u8 fst : 1;
			u8 tme : 1;
			u8 iip : 1;
			u8 point_size : 1; /* points expanded without the VS expanding them */
			GS_HW_ENUM(gs_hw_vs_expand, u8) expand : 2;
			u8 _free : 2;
		};
		u8 key;
	};
#ifdef __cplusplus
	gs_hw_vs_selector() : key(0) {}
	gs_hw_vs_selector(u8 k) : key(k) {}

	/* whether the fixed index buffer is drawn with */
	__fi bool UseExpandIndexBuffer() const { return (expand == gs_hw_vs_expand::Point || expand == gs_hw_vs_expand::Sprite); }
#endif
} gs_hw_vs_selector;
#pragma pack(pop)

#pragma pack(push, 4)
typedef struct gs_hw_ps_selector
{
	/* (There are too many shader combinations: toggling between them
	 * costs, and they take memory.) */
	union
	{
		struct
		{
			/* Format */
			u32 aem_fmt   : 2;
			u32 pal_fmt   : 2;
			u32 dst_fmt   : 2; /* 0: 32-bit, 1: 24-bit, 2: 16-bit */
			u32 depth_fmt : 2; /* 0: none, 1: 32-bit, 2: 16-bit, 3: RGBA */
			/* Alpha extension/correction */
			u32 aem : 1;
			u32 fba : 1;
			/* Fog */
			u32 fog : 1;
			/* Flat/gouraud shading */
			u32 iip : 1;
			/* Pixel test */
			u32 date : 3;
			GS_HW_ENUM(GSShader::PS_ATST, u32) atst : 3;
			GS_HW_ENUM(GSShader::PS_AFAIL, u32) afail : 3;
			/* Colour sampling */
			u32 fst : 1;
			u32 tfx : 3;
			u32 tcc : 1;
			u32 wms : 2;
			u32 wmt : 2;
			u32 adjs : 1;
			u32 adjt : 1;
			u32 ltf : 1;
			/* Shuffle and fbmask effect */
			u32 shuffle  : 1;
			u32 shuffle_same : 1;
			u32 real16src: 1;
			u32 process_ba : 2;
			u32 process_rg : 2;
			u32 shuffle_across : 1;
			u32 write_rg : 1;
			u32 fbmask   : 1;

			/* Blend and colclip */
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
			u32 round_inv      : 1; /* blending inverts the value: rounding the other way */
			u32 pabe           : 1;
			u32 no_color       : 1; /* no colour output (depth only) */
			u32 no_color1      : 1; /* no second colour output (when not needed) */

			/* Other ways to fetch the texture */
			u32 channel : 3;

			/* Dithering */
			u32 dither : 2;
			u32 dither_adjust : 1;

			/* Depth clamp */
			u32 zclamp : 1;

			/* Hacks */
			u32 tcoffsethack : 1;
			u32 urban_chaos_hle : 1;
			u32 tales_of_abyss_hle : 1;
			u32 tex_is_fb : 1; /* Jak's shadows */
			u32 automatic_lod : 1;
			u32 manual_lod : 1;
			u32 point_sampler : 1;
			u32 region_rect : 1;

			/* Scan mask */
			u32 scanmsk : 2;

			/* An indexed source drawn at scale: texels picked at native resolution */
			u32 sample_map : 1;
			/* A bilinear read of a scaled target: the two taps a native texel
			 * apart, weighed as the native pixel weighs them */
			u32 native_taps : 1;
		};

		struct
		{
			u64 key_lo;
			u32 key_hi;
		};
	};
#ifdef __cplusplus
	__fi gs_hw_ps_selector() : key_lo(0), key_hi(0) {}

	__fi bool operator==(const gs_hw_ps_selector& rhs) const { return (key_lo == rhs.key_lo && key_hi == rhs.key_hi); }
	__fi bool operator!=(const gs_hw_ps_selector& rhs) const { return (key_lo != rhs.key_lo || key_hi != rhs.key_hi); }
	__fi bool operator<(const gs_hw_ps_selector& rhs) const { return (key_lo < rhs.key_lo || key_hi < rhs.key_hi); }

	__fi bool IsFeedbackLoop() const
	{
		const u32 sw_blend_bits = blend_a | blend_b | blend_d;
		const bool sw_blend_needs_rt = sw_blend_bits != 0 && ((sw_blend_bits | blend_c) & 1u);
		return tex_is_fb || fbmask || (date > 0 && date != 3) || sw_blend_needs_rt;
	}

	/* No colour out of the pixel shader, when every channel is masked:
	 * no software blending (it would declare the colour inout with
	 * fbfetch), no TEX_IS_FB (it needs a colour output) and no fbmask
	 * (nothing is written); DATE stays. */
	__fi void DisableColorOutput()
	{
		blend_a = blend_b = blend_c = blend_d = 0;
		tex_is_fb = 0;
		fbmask = 0;
		no_color = no_color1 = 1;
	}
#endif
} gs_hw_ps_selector;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct gs_hw_sampler_selector
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
#ifdef __cplusplus
	gs_hw_sampler_selector() : key(0) {}
	gs_hw_sampler_selector(u8 k) : key(k) {}
	static gs_hw_sampler_selector Point() { return gs_hw_sampler_selector(); }
	static gs_hw_sampler_selector Linear()
	{
		gs_hw_sampler_selector out;
		out.biln = 1;
		return out;
	}

	/* the minification filter's linear (without mipmapping, the
	 * magnification's) */
	__fi bool IsMinFilterLinear() const
	{
		if (triln < static_cast<u8>(GS_MIN_FILTER::Nearest_Mipmap_Nearest))
			return biln;
		return (triln >= static_cast<u8>(GS_MIN_FILTER::Linear_Mipmap_Nearest));
	}

	/* (the mip mode is minification's alone) */
	__fi bool IsMagFilterLinear() const { return biln; }

	__fi bool IsMipFilterLinear() const
	{
		return (triln == static_cast<u8>(GS_MIN_FILTER::Nearest_Mipmap_Linear) ||
				triln == static_cast<u8>(GS_MIN_FILTER::Linear_Mipmap_Linear));
	}

	/* mipmaps filtered (the LOD not clamped to zero) */
	__fi bool UseMipmapFiltering() const
	{
		return (triln >= static_cast<u8>(GS_MIN_FILTER::Nearest_Mipmap_Nearest));
	}
#endif
} gs_hw_sampler_selector;

typedef struct gs_hw_depth_selector
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
#ifdef __cplusplus
	gs_hw_depth_selector() : key(0) {}
	gs_hw_depth_selector(u8 k) : key(k) {}
	static gs_hw_depth_selector NoDepth()
	{
		gs_hw_depth_selector out;
		out.ztst = ZTST_ALWAYS;
		return out;
	}
#endif
} gs_hw_depth_selector;

typedef struct gs_hw_color_mask
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
#ifdef __cplusplus
	gs_hw_color_mask() : key(0xF) {}
	gs_hw_color_mask(u8 c) : key(0) { wrgba = c; }
#endif
} gs_hw_color_mask;
#pragma pack(pop)

/* GSVector2's and GSVector2i's layout */
typedef struct gs_hw_float2
{
	float x, y;
} gs_hw_float2;

#ifdef __cplusplus
#define GS_HW_V2(n) GSVector2 n
#define GS_HW_V2I(n) GSVector2i n
#define GS_HW_V4(n) GSVector4 n
#define GS_HW_V4I(n) GSVector4i n
#define GS_HW_TEX(n) GSTexture* n
#define GS_HW_VERTS(n) const GSVertex* n
/* (what makes a constant buffer a plain block of bytes to C++ too) */
#define GS_HW_CB_OPS(t) \
	__fi t() { memset(this, 0, sizeof(*this)); } \
	__fi t(const t& other) { memcpy(this, &other, sizeof(*this)); } \
	__fi t& operator=(const t& other) \
	{ \
		memcpy(this, &other, sizeof(*this)); \
		return *this; \
	} \
	__fi bool operator==(const t& other) const { return memcmp(this, &other, sizeof(*this)) == 0; } \
	__fi bool operator!=(const t& other) const { return !(*this == other); } \
	__fi bool Update(const t& other) \
	{ \
		if (*this == other) \
			return false; \
		memcpy(this, &other, sizeof(*this)); \
		return true; \
	}
class GSTexture;
struct GSVertex;
#else
#define GS_HW_V2(n) gs_hw_float2 n
#define GS_HW_V2I(n) gs_point n
#define GS_HW_V4(n) gs_rectf n
#define GS_HW_V4I(n) gs_recti n
#define GS_HW_TEX(n) gs_texture* n
#define GS_HW_VERTS(n) const union gs_vertex* n
#endif

typedef struct PCSX2_ALIGN(16) gs_hw_vs_cb
{
	GS_HW_V2(vertex_scale);
	GS_HW_V2(vertex_offset);
	GS_HW_V2(texture_scale);
	GS_HW_V2(texture_offset);
	GS_HW_V2(point_size);
	GS_HW_V2I(max_depth);
#ifdef __cplusplus
	GS_HW_CB_OPS(gs_hw_vs_cb)
#endif
} gs_hw_vs_cb;

typedef struct PCSX2_ALIGN(16) gs_hw_ps_cb
{
	GS_HW_V4(FogColor_AREF);
	GS_HW_V4(WH);
	GS_HW_V4(TA_MaxDepth_Af);
	GS_HW_V4I(FbMask);

	GS_HW_V4(HalfTexel);
	GS_HW_V4(MinMax);
	GS_HW_V4(LODParams);
	GS_HW_V4(STRange);
	GS_HW_V4I(ChannelShuffle);
	GS_HW_V2(TCOffsetHack);
	GS_HW_V2(STScale);
	/* where a channel shuffle's fetch by position starts in its source:
	 * the page the texture begins on, scaled */
	GS_HW_V4(ChannelOffset);

	GS_HW_V4(DitherMatrix[4]);

	GS_HW_V4(ScaleFactor);
#ifdef __cplusplus
	GS_HW_CB_OPS(gs_hw_ps_cb)
#endif
} gs_hw_ps_cb;

typedef struct gs_hw_blend_state
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
#ifdef __cplusplus
	gs_hw_blend_state() : key(0) {}
	gs_hw_blend_state(bool enable_, u8 src_factor_, u8 dst_factor_, u8 op_,
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

	/* (blending does nothing with RGB masked) */
	bool IsEffective(gs_hw_color_mask colormask) const;
#endif
} gs_hw_blend_state;

/* The draw's topology (GSHWDrawConfig::Topology) */
#define GS_HW_TOPOLOGY_POINT 0
#define GS_HW_TOPOLOGY_LINE 1
#define GS_HW_TOPOLOGY_TRIANGLE 2

/* How destination alpha is tested (GSHWDrawConfig::DestinationAlphaMode) */
#define GS_HW_DATE_OFF 0
#define GS_HW_DATE_STENCIL 1        /* a read-only stencil */
#define GS_HW_DATE_STENCIL_ONE 2    /* a read-write stencil, the first write wins */
#define GS_HW_DATE_PRIMID 3         /* the primitive ID of the last pixel let through */
#define GS_HW_DATE_FULL 4           /* barriers, or ROV */

/* What is done with colclip's float target (GSHWDrawConfig::ColClipMode) */
#define GS_HW_COLCLIP_NO_MODIFY 0
#define GS_HW_COLCLIP_CONVERT_ONLY 1
#define GS_HW_COLCLIP_RESOLVE_ONLY 2
#define GS_HW_COLCLIP_CONVERT_AND_RESOLVE 3
#define GS_HW_COLCLIP_EARLY_RESOLVE 4

#ifdef __cplusplus
enum class gs_hw_topology : u8
{
	Point,
	Line,
	Triangle,
};

enum class gs_hw_dest_alpha_mode : u8
{
	Off,            ///< No destination alpha test
	Stencil,        ///< Emulate using read-only stencil
	StencilOne,     ///< Emulate using read-write stencil (first write wins)
	PrimIDTracking, ///< Emulate by tracking the primitive ID of the last pixel allowed through
	Full,           ///< Full emulation (using barriers / ROV)
};

enum class gs_hw_colclip_mode : u8
{
	NoModify = 0,
	ConvertOnly = 1,
	ResolveOnly = 2,
	ConvertAndResolve = 3,
	EarlyResolve = 4
};

enum class SetDATM : u8
{
	DATM0 = 0U,
	DATM1,
	DATM0_RTA_CORRECTION,
	DATM1_RTA_CORRECTION
};
#endif

typedef struct gs_hw_alpha_pass
{
	PCSX2_ALIGN(8) gs_hw_ps_selector ps;
	bool enable;
	gs_hw_color_mask colormask;
	gs_hw_depth_selector depth;
	float ps_aref;
} gs_hw_alpha_pass;

typedef struct gs_hw_blend_multi_pass
{
	gs_hw_blend_state blend;
	u8 blend_hw;
	u8 dither;
	bool enable;
} gs_hw_blend_multi_pass;

typedef struct PCSX2_ALIGN(16) gs_hw_draw_config
{
	GS_HW_TEX(rt);              /* render target */
	GS_HW_TEX(ds);              /* depth stencil */
	GS_HW_TEX(tex);             /* source texture */
	GS_HW_TEX(pal);             /* palette texture */
	GS_HW_VERTS(verts);
	const u16* indices;
	u32 nverts;
	u32 nindices;
	u32 indices_per_prim;       /* indices a primitive takes */
	const u32* drawlist;        /* sprites a run, drawlist_count runs: fewer barriers */
	u32 drawlist_count;
	GS_HW_V4I(scissor);
	GS_HW_V4I(drawarea);        /* what of the framebuffer the draw changes */
	GS_HW_ENUM(gs_hw_topology, u8) topology;

	PCSX2_ALIGN(8) gs_hw_ps_selector ps;
	gs_hw_vs_selector vs;

	gs_hw_blend_state blend;
	gs_hw_sampler_selector sampler;
	gs_hw_color_mask colormask;
	gs_hw_depth_selector depth;

	bool require_one_barrier;   /* a texture barrier before the draw (an RT copy without them) */
	bool require_full_barrier;  /* one between every primitive */

	GS_HW_ENUM(gs_hw_dest_alpha_mode, u8) destination_alpha;
	GS_HW_ENUM(SetDATM, u8) datm : 2;
	bool line_expand : 1;

	gs_hw_alpha_pass alpha_second_pass;
	gs_hw_blend_multi_pass blend_multi_pass;

	gs_hw_vs_cb cb_vs;
	gs_hw_ps_cb cb_ps;

	/* (kept between draws: the state clear stops at the constant buffers) */
	GS_HW_ENUM(gs_hw_colclip_mode, u8) colclip_mode;
	GIFRegFRAME colclip_frame;
	GS_HW_V4I(colclip_update_area); /* what of the framebuffer the colclip changes */
} gs_hw_draw_config;

#ifdef __cplusplus
static_assert(sizeof(gs_hw_vs_selector) == 1, "gs_hw_vs_selector");
static_assert(sizeof(gs_hw_ps_selector) == 12, "gs_hw_ps_selector");
static_assert(sizeof(GSVector2) == sizeof(gs_hw_float2) && alignof(GSVector2) == alignof(gs_hw_float2), "GSVector2");
static_assert(sizeof(GSVector2i) == sizeof(gs_point) && alignof(GSVector2i) == alignof(gs_point), "GSVector2i");
static_assert(sizeof(GSVector4) == sizeof(gs_rectf) && alignof(GSVector4) == alignof(gs_rectf), "GSVector4");
static_assert(sizeof(GSVector4i) == sizeof(gs_recti) && alignof(GSVector4i) == alignof(gs_recti), "GSVector4i");
#undef GS_HW_CB_OPS
#endif

#undef GS_HW_V2
#undef GS_HW_V2I
#undef GS_HW_V4
#undef GS_HW_V4I
#undef GS_HW_TEX
#undef GS_HW_VERTS
#undef GS_HW_ENUM

#endif
