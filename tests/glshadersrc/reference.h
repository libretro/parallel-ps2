/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GLSHADERSRC_REFERENCE_H
#define GLSHADERSRC_REFERENCE_H

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <libretro.h>

#include "common/Pcsx2Defs.h"
#include "glad.h"
#include "GS/Renderers/Common/GSString.h"
#include "GS/Renderers/Common/GSDeviceBase.h"
#include "GS/Renderers/Common/GSShaderEnums.h"
#include "GS/Renderers/Common/GSKeyHash.h"

/* GSHWDrawConfig's vertex and pixel selectors as they were */
struct RefSelectors
{
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
};

typedef RefSelectors::VSSelector RefVSSelector;
typedef RefSelectors::PSSelector RefPSSelector;

struct GLProgram
{
	int built;
};

/* GSDeviceOGL's part that writes GLSL */
class RefGL
{
public:
	bool m_is_gles = false;
	gs_device_features m_features;
	gs_str_t m_src_vs;
	gs_str_t m_src_ps;

	bool GetProgram(GLProgram* prog, gs_str_t*, gs_str_t*)
	{
		prog->built = 1;
		return true;
	}

	bool GetShaderSource(gs_str_t* out, const char* entry, GLenum type, const char* glsl_h_code, const char* macro = nullptr);
	bool GenGlslHeader(gs_str_t* out, const char* entry, GLenum type, const char* macro);
	bool GetVSSource(gs_str_t* out, RefVSSelector sel);
	bool GetPSSource(gs_str_t* out, const RefPSSelector& sel);
	bool BuildDynamicProgram(GLProgram* prog, u32 iip, u32 expand, u32 date);
};

/* The shader files the sources end in */
extern const char* tfx_vs_glsl_shader_raw;
extern const char* tfx_fs_glsl_shader_raw;

void RefDynamicKey(const RefPSSelector& sel, RefVSSelector vs, u32* key);
u32 RefDynamicDate(u32 date);

#endif
