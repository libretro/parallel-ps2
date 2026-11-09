/* The D3D renderers' dynamic pixel shaders (tfx.fx's PS_DYNAMIC): a pixel
 * selector packed into PsKey, and the macros of the shader that reads it.
 * D3D11 and D3D12 pack it the same, from the one list. */
#pragma once

#include "GS/Renderers/Common/GSDevice.h"

#include <cstdio>
#include <string>

/* The pixel selector fields as tfx.fx's macros, each in the bits after the
 * last and none across a word: the dynamic pixel shader's PsKey. */
#define D3D_PS_FIELDS(X) \
	X(PS_FST, fst, 1) X(PS_WMS, wms, 2) X(PS_WMT, wmt, 2) X(PS_ADJS, adjs, 1) X(PS_ADJT, adjt, 1) \
	X(PS_AEM_FMT, aem_fmt, 2) X(PS_AEM, aem, 1) X(PS_TFX, tfx, 3) X(PS_TCC, tcc, 1) X(PS_DATE, date, 3) \
	X(PS_ATST, atst, 3) X(PS_AFAIL, afail, 3) X(PS_FOG, fog, 1) X(PS_IIP, iip, 1) X(PS_BLEND_HW, blend_hw, 3) \
	X(PS_A_MASKED, a_masked, 1) X(PS_FBA, fba, 1) X(PS_FBMASK, fbmask, 1) X(PS_LTF, ltf, 1) \
	X(PS_TCOFFSETHACK, tcoffsethack, 1) X(PS_POINT_SAMPLER, point_sampler, 1) X(PS_REGION_RECT, region_rect, 1) \
	X(PS_SHUFFLE, shuffle, 1) X(PS_SHUFFLE_SAME, shuffle_same, 1) X(PS_PROCESS_BA, process_ba, 2) \
	X(PS_PROCESS_RG, process_rg, 2) X(PS_SHUFFLE_ACROSS, shuffle_across, 1) X(PS_READ16_SRC, real16src, 1) \
	X(PS_CHANNEL_FETCH, channel, 3) X(PS_TALES_OF_ABYSS_HLE, tales_of_abyss_hle, 1) \
	X(PS_URBAN_CHAOS_HLE, urban_chaos_hle, 1) X(PS_DST_FMT, dst_fmt, 2) X(PS_DEPTH_FMT, depth_fmt, 2) \
	X(PS_PAL_FMT, pal_fmt, 2) X(PS_COLCLIP_HW, colclip_hw, 1) X(PS_RTA_CORRECTION, rta_correction, 1) \
	X(PS_RTA_SRC_CORRECTION, rta_source_correction, 1) X(PS_COLCLIP, colclip, 1) X(PS_BLEND_A, blend_a, 2) \
	X(PS_BLEND_B, blend_b, 2) X(PS_BLEND_C, blend_c, 2) X(PS_BLEND_D, blend_d, 2) X(PS_BLEND_MIX, blend_mix, 2) \
	X(PS_ROUND_INV, round_inv, 1) X(PS_FIXED_ONE_A, fixed_one_a, 1) X(PS_PABE, pabe, 1) X(PS_DITHER, dither, 2) \
	X(PS_DITHER_ADJUST, dither_adjust, 1) X(PS_ZCLAMP, zclamp, 1) X(PS_SCANMSK, scanmsk, 2) \
	X(PS_SAMPLE_MAP, sample_map, 1) X(PS_NATIVE_TAPS, native_taps, 1) X(PS_AUTOMATIC_LOD, automatic_lod, 1) \
	X(PS_MANUAL_LOD, manual_lod, 1) X(PS_TEX_IS_FB, tex_is_fb, 1) X(PS_NO_COLOR, no_color, 1) \
	X(PS_NO_COLOR1, no_color1, 1)

/* The interfaces a dynamic pixel shader can have: D3DDynamicIndex's. */
#define D3D_DYNAMIC_PS_COUNT 6u

/* The next field's place: in the word it starts in, else the next. */
static inline u32 D3DKeyPlace(u32* at, u32 bits)
{
	u32 place;
	if ((*at & 31u) + bits > 32u)
		*at = (*at + 31u) & ~31u;
	place = *at;
	*at += bits;
	return place;
}

static inline void D3DDynamicKey(const GSHWDrawConfig::PSSelector& sel, u32* key)
{
	u32 at = 0, place;
	key[0] = key[1] = key[2] = key[3] = 0;
#define D3D_PACK(name, field, bits) \
	place = D3DKeyPlace(&at, bits); \
	key[place >> 5] |= (static_cast<u32>(sel.field) & ((1u << bits) - 1u)) << (place & 31u);
	D3D_PS_FIELDS(D3D_PACK)
#undef D3D_PACK
}

/* The interface a selector's dynamic pixel shader has: the flat colour,
 * and the destination alpha test's first pass or primitive ID test. */
static inline u32 D3DDynamicIndex(const GSHWDrawConfig::PSSelector& sel)
{
	const u32 date = (sel.date == 1 || sel.date == 2) ? 1u : (sel.date == 3) ? 2u : 0u;
	return sel.iip * 3u + date;
}

template <typename SM>
static inline void D3DPixelShaderMacros(SM& sm, const GSHWDrawConfig::PSSelector& sel)
{
	sm.AddMacro("PIXEL_SHADER", 1);
#define D3D_MACRO(name, field, bits) sm.AddMacro(#name, static_cast<int>(sel.field));
	D3D_PS_FIELDS(D3D_MACRO)
#undef D3D_MACRO
}

/* The dynamic pixel shader of interface index: every field read from
 * PsKey but what shapes the interface; the second output and the depth
 * output are kept (a draw that does without them leaves them unread). */
template <typename SM>
static inline void D3DDynamicMacros(SM& sm, u32 index)
{
	const u32 date = index % 3u;
	char buf[48];
	u32 at = 0, place;
	sm.AddMacro("PIXEL_SHADER", 1);
	sm.AddMacro("PS_DYNAMIC", 1);
	sm.AddMacro("PS_IF_IIP", static_cast<int>(index / 3u));
	sm.AddMacro("PS_IF_DATE_INIT", date == 1);
	sm.AddMacro("PS_IF_PRIMID", date != 0);
	sm.AddMacro("PS_IF_NO_COLOR", 0);
	sm.AddMacro("PS_IF_NO_COLOR1", 0);
	sm.AddMacro("PS_IF_ZCLAMP", date != 1);
#define D3D_DYN(name, field, bits) \
	place = D3DKeyPlace(&at, bits); \
	snprintf(buf, sizeof(buf), "((PsKey[%u] >> %uu) & %uu)", place >> 5, place & 31u, (1u << bits) - 1u); \
	sm.AddMacro(#name, std::string(buf));
	D3D_PS_FIELDS(D3D_DYN)
#undef D3D_DYN
}
