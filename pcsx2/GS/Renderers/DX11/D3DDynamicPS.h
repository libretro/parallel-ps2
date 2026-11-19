/* The D3D renderers' dynamic pixel shaders (tfx.fx's PS_DYNAMIC): a pixel
 * selector packed into PsKey, and the macros of the shader that reads it.
 * D3D11 and D3D12 pack it the same, from the one list. */
#ifndef GS_D3D_DYNAMIC_PS_H
#define GS_D3D_DYNAMIC_PS_H

#include <boolean.h>
#include <retro_inline.h>

#include "common/Pcsx2Defs.h"
#include "GS/Renderers/Common/GSHWDrawConfig.h"
#include "D3D.h"

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
static INLINE u32 gs_d3d_key_place(u32* at, u32 bits)
{
	u32 place;
	if ((*at & 31u) + bits > 32u)
		*at = (*at + 31u) & ~31u;
	place = *at;
	*at += bits;
	return place;
}

static INLINE void gs_d3d_dynamic_key(const gs_hw_ps_selector* sel, u32* key)
{
	u32 at = 0, place;
	key[0] = key[1] = key[2] = key[3] = 0;
#define D3D_PACK(name, field, bits) \
	place = gs_d3d_key_place(&at, bits); \
	key[place >> 5] |= ((u32)(sel->field) & ((1u << bits) - 1u)) << (place & 31u);
	D3D_PS_FIELDS(D3D_PACK)
#undef D3D_PACK
}

/* The interface a selector's dynamic pixel shader has: the flat colour,
 * and the destination alpha test's first pass or primitive ID test. */
static INLINE u32 gs_d3d_dynamic_index(const gs_hw_ps_selector* sel)
{
	const u32 date = (sel->date == 1 || sel->date == 2) ? 1u : (sel->date == 3) ? 2u : 0u;
	return sel->iip * 3u + date;
}

/* A shader's macros (D3D_SHADER_MACRO, NULL ended), with room for their
 * values: the shader model's for a feature level, then those added */
#define GS_D3D_MACROS_MAX 96
typedef struct gs_d3d_macros
{
	D3D_SHADER_MACRO list[GS_D3D_MACROS_MAX + 1];
	char values[GS_D3D_MACROS_MAX][48];
	unsigned count;
} gs_d3d_macros;

#ifdef __cplusplus
extern "C" {
#endif

void gs_d3d_macros_init(gs_d3d_macros* m, D3D_FEATURE_LEVEL fl);
void gs_d3d_macros_add(gs_d3d_macros* m, const char* name, const char* value);
void gs_d3d_macros_add_int(gs_d3d_macros* m, const char* name, int value);
/* The list, ended */
const D3D_SHADER_MACRO* gs_d3d_macros_list(gs_d3d_macros* m);
/* A pixel selector's tfx.fx macros */
void gs_d3d_ps_macros(gs_d3d_macros* m, const gs_hw_ps_selector* sel);
/* The dynamic pixel shader of interface index: every field read from
 * PsKey but what shapes the interface; the second output and the depth
 * output are kept (a draw that does without them leaves them unread). */
void gs_d3d_dynamic_macros(gs_d3d_macros* m, u32 index);

#ifdef __cplusplus
}
#endif

#endif
