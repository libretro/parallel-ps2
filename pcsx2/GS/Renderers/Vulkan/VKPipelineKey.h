/*  PCSX2 - PS2 Emulator for PCs
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 */

#pragma once

/* What a TFX pipeline's shaders are made of, from its selector: the
 * module (VKSpirvPatch.h), the specialization constants (VKSC_*, so
 * VKShaders.inc is included first), and the selector as a key.
 *
 * The key: the fields the shaders (shaders/tfx.glsl) do not read, given
 * the others, zeroed. Two selectors that differ only there make the same
 * shader, so they are one pipeline, built once, listed once and warmed
 * once. Each rule names what tfx.glsl reads the field under. A field is
 * zeroed only where no path of the shader reads it; what the rest of the
 * renderer reads (the render pass, barriers, the feedback loop) comes
 * from the draw's own selector, never from the key.
 *
 * tests/vkpipekey holds all of this to the SPIR-V the core is built with:
 * for selectors at random, the shader specialized for the selector and
 * for its key run the same instructions; the module that reads no target
 * is only chosen for a shader that does not read it, and so on. */

#include "GS/Renderers/Common/GSDevice.h"
#include "VKSpirvPatch.h"

static inline void VKCanonicalPS(GSHWDrawConfig::PSSelector& ps)
{
	/* How the texture is read (ps_color): not at all, a channel fetch, a
	 * depth texture sampled, a colour texture sampled. */
	const bool needs_tex     = ps.tfx != 4;
	const bool channel       = needs_tex && ps.channel >= 1 && ps.channel <= 6;
	const bool depth         = needs_tex && !channel && ps.depth_fmt != 0;
	const bool color         = needs_tex && !channel && !depth;
	/* the shuffle that reads T back (ps_color) */
	const bool shuffle_read  = ps.shuffle && !ps.real16src && !ps.shuffle_same;
	const bool sw_blend      = ps.blend_a || ps.blend_b || ps.blend_d;
	const bool dither_on     = ps.dither == 1 || ps.dither == 2;
	/* sample_color's one-tap path: sample_c(st), no wrapping */
	const bool one_tap       = color && !ps.ltf && ps.aem_fmt == 0 && ps.pal_fmt == 0 && !ps.region_rect
		&& ps.wms < 2 && ps.wmt < 2 && !ps.sample_map;
	/* sample_c's coordinate scaling and LOD: read by sample_color but for
	 * the sample map, where it fetches texels itself */
	const bool scaled_sample = color && !ps.sample_map && !ps.tex_is_fb && !ps.region_rect;

	if (!needs_tex)
	{
		/* T is zero; tfx() passes the colour through, alpha and all */
		ps.tcc = 0;
		ps.tex_is_fb = 0;
		ps.depth_fmt = 0;
	}
	/* (7 reads as none) */
	if (!channel)
		ps.channel = 0;
	if (!depth && !color)
	{
		/* sample_depth and sample_color only */
		ps.wms = ps.wmt = 0;
		ps.region_rect = 0;
		ps.aem_fmt = 0;
		ps.pal_fmt = 0;
		ps.urban_chaos_hle = 0;
		ps.tales_of_abyss_hle = 0;
		/* the coordinates, read for sampling only */
		ps.fst = 0;
	}
	if (!depth)
	{
		ps.urban_chaos_hle = 0;
		ps.tales_of_abyss_hle = 0;
	}
	if (!color)
	{
		/* sample_color only */
		ps.ltf = 0;
		ps.tcoffsethack = 0;
		ps.sample_map = 0;
		ps.native_taps = 0;
		ps.rta_source_correction = 0;
	}
	if (!scaled_sample)
		ps.adjs = ps.adjt = ps.automatic_lod = ps.manual_lod = 0;
	else if (ps.automatic_lod)
		ps.manual_lod = 0;
	if (!ps.ltf)
		ps.native_taps = 0;
	if (one_tap)
		ps.wms = ps.wmt = 0; /* (both under 2: no wrap, whichever) */
	/* AEM: the expansion of a 24- or 16-bit texture's alpha, and the
	 * shuffle's */
	if (((!depth && !color) || (ps.aem_fmt != 1 && ps.aem_fmt != 2)) && !shuffle_read)
		ps.aem = 0;
	/* the shuffle's own; across only where neither of the two before it
	 * in main() is taken, RG only across */
	if (!ps.shuffle)
		ps.shuffle_same = ps.real16src = ps.shuffle_across = ps.process_ba = ps.process_rg = 0;
	if (ps.shuffle_same || ps.real16src)
		ps.shuffle_across = 0;
	if (!ps.shuffle_across)
		ps.process_rg = 0;
	/* SW_AD_TO_HW, the alpha mask's only reader */
	if (ps.blend_c != 1)
		ps.a_masked = 0;
	/* the destination alpha test that reads the target */
	if (ps.date < 5)
		ps.write_rg = 0;
	if (!sw_blend)
		ps.pabe = 0;
	if (!dither_on)
		ps.dither_adjust = 0;
	/* ps_color_clamp_wrap's (dither 3 is told from none only there) and
	 * ps_dither's */
	if (!sw_blend && !dither_on && !ps.fbmask)
		ps.colclip = ps.blend_mix = ps.round_inv = ps.dither = 0;
	if (!(ps.scanmsk & 2))
		ps.scanmsk = 0;
}

/* The vertex shader's: the coordinates without a texture, the point size
 * but for points drawn as points. */
static inline void VKCanonicalVS(GSHWDrawConfig::VSSelector& vs, u32 topology)
{
	if (!vs.tme)
		vs.fst = 0;
	if (vs.expand != GSHWDrawConfig::VSExpand::None || topology != static_cast<u32>(GSHWDrawConfig::Topology::Point))
		vs.point_size = 0;
}

/* Whether a draw's fragment shader reads its target: as tfx.glsl calls
 * sample_from_rt(), the module that reads it as nothing otherwise. */
static inline bool VKTFXReadsRT(const GSHWDrawConfig::PSSelector& sel)
{
	const bool sw_blend          = sel.blend_a || sel.blend_b || sel.blend_d;
	const bool sw_blend_needs_rt = sw_blend &&
		(sel.blend_a == 1 || sel.blend_b == 1 || sel.blend_c == 1 || sel.blend_d == 1);
	const bool sw_ad_to_hw       = sel.blend_c == 1 && sel.a_masked;
	return sel.tex_is_fb || sel.fbmask || sw_blend_needs_rt || sw_ad_to_hw || sel.date >= 5;
}

/* What of the fragment module's interface a draw does without: the second
 * output when it does not blend with it (or the device cannot), the
 * primitive ID outside the destination alpha tests 1-3, the depth output
 * when it does not clamp depth (or is the alpha test's first pass, which
 * never wrote it), the flat colour when it interpolates. */
static inline unsigned VKTFXFragmentDrop(bool dual_source_blend, const GSHWDrawConfig::PSSelector& sel)
{
	unsigned drop = 0;
	if (sel.no_color || sel.no_color1 || !dual_source_blend)
		drop |= VKSPV_DROP_COLOR1;
	if (sel.date < 1 || sel.date > 3)
		drop |= VKSPV_DROP_PRIMID;
	if (!sel.zclamp || sel.date == 1 || sel.date == 2)
		drop |= VKSPV_DROP_FRAGDEPTH;
	if (sel.iip)
		drop |= VKSPV_DROP_FLAT;
	return drop;
}

/* The selector fields, as tfx.glsl's specialization constants. */
static inline void VKTFXSpecVS(GSHWDrawConfig::VSSelector sel, u32* v)
{
	static_assert(VKSC_VS_COUNT == 4, "a VS_ constant of tfx.glsl has no value here");
	v[VKSC_VS_TME]         = sel.tme;
	v[VKSC_VS_FST]         = sel.fst;
	v[VKSC_VS_POINT_SIZE]  = sel.point_size;
	v[VKSC_VS_EXPAND_MODE] = static_cast<u32>(sel.expand);
}

static inline void VKTFXSpecPS(const GSHWDrawConfig::PSSelector& sel, u32* v)
{
	static_assert(VKSC_PS_COUNT == 54, "a PS_ constant of tfx.glsl has no value here");
	v[VKSC_PS_FST]                = sel.fst;
	v[VKSC_PS_WMS]                = sel.wms;
	v[VKSC_PS_WMT]                = sel.wmt;
	v[VKSC_PS_ADJS]               = sel.adjs;
	v[VKSC_PS_ADJT]               = sel.adjt;
	v[VKSC_PS_AEM_FMT]            = sel.aem_fmt;
	v[VKSC_PS_PAL_FMT]            = sel.pal_fmt;
	v[VKSC_PS_DST_FMT]            = sel.dst_fmt;
	v[VKSC_PS_DEPTH_FMT]          = sel.depth_fmt;
	v[VKSC_PS_CHANNEL_FETCH]      = sel.channel;
	v[VKSC_PS_URBAN_CHAOS_HLE]    = sel.urban_chaos_hle;
	v[VKSC_PS_TALES_OF_ABYSS_HLE] = sel.tales_of_abyss_hle;
	v[VKSC_PS_AEM]                = sel.aem;
	v[VKSC_PS_TFX]                = sel.tfx;
	v[VKSC_PS_TCC]                = sel.tcc;
	v[VKSC_PS_ATST]               = static_cast<u32>(sel.atst);
	v[VKSC_PS_AFAIL]              = static_cast<u32>(sel.afail);
	v[VKSC_PS_FOG]                = sel.fog;
	v[VKSC_PS_BLEND_HW]           = sel.blend_hw;
	v[VKSC_PS_A_MASKED]           = sel.a_masked;
	v[VKSC_PS_FBA]                = sel.fba;
	v[VKSC_PS_LTF]                = sel.ltf;
	v[VKSC_PS_AUTOMATIC_LOD]      = sel.automatic_lod;
	v[VKSC_PS_MANUAL_LOD]         = sel.manual_lod;
	v[VKSC_PS_COLCLIP]            = sel.colclip;
	v[VKSC_PS_DATE]               = sel.date;
	v[VKSC_PS_TCOFFSETHACK]       = sel.tcoffsethack;
	v[VKSC_PS_REGION_RECT]        = sel.region_rect;
	v[VKSC_PS_BLEND_A]            = sel.blend_a;
	v[VKSC_PS_BLEND_B]            = sel.blend_b;
	v[VKSC_PS_BLEND_C]            = sel.blend_c;
	v[VKSC_PS_BLEND_D]            = sel.blend_d;
	v[VKSC_PS_BLEND_MIX]          = sel.blend_mix;
	v[VKSC_PS_ROUND_INV]          = sel.round_inv;
	v[VKSC_PS_FIXED_ONE_A]        = sel.fixed_one_a;
	v[VKSC_PS_SHUFFLE]            = sel.shuffle;
	v[VKSC_PS_SHUFFLE_SAME]       = sel.shuffle_same;
	v[VKSC_PS_PROCESS_BA]         = sel.process_ba;
	v[VKSC_PS_PROCESS_RG]         = sel.process_rg;
	v[VKSC_PS_SHUFFLE_ACROSS]     = sel.shuffle_across;
	v[VKSC_PS_READ16_SRC]         = sel.real16src;
	v[VKSC_PS_WRITE_RG]           = sel.write_rg;
	v[VKSC_PS_FBMASK]             = sel.fbmask;
	v[VKSC_PS_COLCLIP_HW]         = sel.colclip_hw;
	v[VKSC_PS_RTA_CORRECTION]     = sel.rta_correction;
	v[VKSC_PS_RTA_SRC_CORRECTION] = sel.rta_source_correction;
	v[VKSC_PS_DITHER]             = sel.dither;
	v[VKSC_PS_DITHER_ADJUST]      = sel.dither_adjust;
	v[VKSC_PS_PABE]               = sel.pabe;
	v[VKSC_PS_SCANMSK]            = sel.scanmsk;
	v[VKSC_PS_SAMPLE_MAP]         = sel.sample_map;
	v[VKSC_PS_NATIVE_TAPS]        = sel.native_taps;
	v[VKSC_PS_TEX_IS_FB]          = sel.tex_is_fb;
	v[VKSC_PS_NO_COLOR]           = sel.no_color;
}

/* The selector of a pipeline's dynamic sibling (the fragment module's
 * DYNAMIC build, which reads the selector from push constants): the one
 * pipeline every selector with the same fixed state and the same module
 * draws with until its own is built. What it keeps is what the pipeline's
 * state and its module are made from: the destination alpha test (the
 * render pass and blend of its first pass, the primitive ID), the colour
 * clip render pass, the outputs, the flat colour and the depth clamp;
 * and FBMASK stands for whether the module reads the target. */
static inline void VKDynamicPS(GSHWDrawConfig::PSSelector& ps)
{
	GSHWDrawConfig::PSSelector d;
	d.date       = ps.date;
	d.colclip_hw = ps.colclip_hw;
	d.no_color   = ps.no_color;
	d.no_color1  = ps.no_color1;
	d.iip        = ps.iip;
	d.zclamp     = ps.zclamp;
	d.fbmask     = VKTFXReadsRT(ps);
	ps = d;
}

/* The key the dynamic sibling reads the selector from: each field where
 * vkspv_tfx_fs_key_at puts it. */
static inline void VKTFXDynamicKey(const GSHWDrawConfig::PSSelector& sel, u32* key)
{
	u32 v[VKSC_PS_COUNT];
	u32 i;
	VKTFXSpecPS(sel, v);
	key[0] = key[1] = key[2] = key[3] = 0;
	for (i = 0; i < VKSC_PS_COUNT; i++)
		key[vkspv_tfx_fs_key_at[i] >> 5] |= (v[i] & ((1u << vkspv_tfx_fs_key_bits[i]) - 1u)) << (vkspv_tfx_fs_key_at[i] & 31u);
}
