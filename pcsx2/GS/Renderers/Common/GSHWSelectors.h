/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GS_HW_SELECTORS_H
#define GS_HW_SELECTORS_H

/* The keys a HW draw is set up by (GSHWDrawConfig's selectors): its
 * vertex and pixel shaders, its sampler, depth test, colour mask and
 * blend. One layout for C and C++: C++ names them as GSHWDrawConfig's
 * (GSDevice.h), with its constructors and helpers, and its enum classes
 * where C has their underlying types. */

#include "common/Pcsx2Defs.h"
#include "../../GSRegs.h"

#ifdef __cplusplus
#include "GSShaderEnums.h"
#endif

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

#ifdef __cplusplus
static_assert(sizeof(gs_hw_vs_selector) == 1, "gs_hw_vs_selector");
static_assert(sizeof(gs_hw_ps_selector) == 12, "gs_hw_ps_selector");
#endif

#undef GS_HW_ENUM

#endif
