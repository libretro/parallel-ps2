/* SPDX-License-Identifier: LGPL-3.0+ */

#include <stdio.h>
#include <string.h>

#include <libretro.h>

#include "GLShaderSource.h"

#define GL_CAT(out, s) (gs_str_cat(out, s) != 0)

static bool gl_header(gs_str_t* out, const gs_gl_shader_caps* caps, const char* entry, GLenum type, const char* macro)
{
	bool ok = true;
	gs_str_clear(out);

	if (caps->gles)
	{
		ok &= GL_CAT(out, GLAD_GL_ES_VERSION_3_2 ? "#version 320 es\n" : "#version 310 es\n");

		/* What 3.20 has in core, 3.10 takes from extensions: the shader
		 * interface blocks, and gl_PrimitiveID in a fragment shader. */
		if (!GLAD_GL_ES_VERSION_3_2)
		{
			if (GLAD_GL_EXT_shader_io_blocks)
				ok &= GL_CAT(out, "#extension GL_EXT_shader_io_blocks : require\n");
			else
				ok &= GL_CAT(out, "#extension GL_OES_shader_io_blocks : require\n");
			if (type == GL_FRAGMENT_SHADER && caps->primitive_id)
				ok &= GL_CAT(out, GLAD_GL_EXT_geometry_shader ? "#extension GL_EXT_geometry_shader : require\n"
				                                              : "#extension GL_OES_geometry_shader : require\n");
		}
		if (caps->dual_source_blend && type == GL_FRAGMENT_SHADER)
			ok &= GL_CAT(out, "#extension GL_EXT_blend_func_extended : require\n");
		if (caps->framebuffer_fetch && type == GL_FRAGMENT_SHADER)
			ok &= GL_CAT(out, "#extension GL_EXT_shader_framebuffer_fetch : require\n");

		/* ES has no default float precision in a fragment shader, and PS2
		 * depth and colour need all of it everywhere. */
		ok &= GL_CAT(out, "precision highp float;\n");
		ok &= GL_CAT(out, "precision highp int;\n");
		ok &= GL_CAT(out, "precision highp sampler2D;\n");
	}
	else
	{
		/* (Intel's driver takes no readonly qualifier with GLSL 3.30) */
		if (caps->vs_expand && GLAD_GL_VERSION_4_3)
			ok &= GL_CAT(out, "#version 430 core\n");
		else
		{
			ok &= GL_CAT(out, "#version 330 core\n");
			ok &= GL_CAT(out, "#extension GL_ARB_shading_language_420pack : require\n");
			if (GLAD_GL_ARB_gpu_shader5)
				ok &= GL_CAT(out, "#extension GL_ARB_gpu_shader5 : require\n");
			if (caps->vs_expand)
				ok &= GL_CAT(out, "#extension GL_ARB_shader_storage_buffer_object: require\n");
		}

		if (GLAD_GL_ARB_shader_draw_parameters)
			ok &= GL_CAT(out, "#extension GL_ARB_shader_draw_parameters : require\n");
		if (caps->framebuffer_fetch && GLAD_GL_EXT_shader_framebuffer_fetch)
			ok &= GL_CAT(out, "#extension GL_EXT_shader_framebuffer_fetch : require\n");
	}

	ok &= GL_CAT(out, caps->dual_source_blend ? "#define HAS_DUAL_SOURCE 1\n" : "#define HAS_DUAL_SOURCE 0\n");
	ok &= GL_CAT(out, caps->framebuffer_fetch ? "#define HAS_FRAMEBUFFER_FETCH 1\n" : "#define HAS_FRAMEBUFFER_FETCH 0\n");
	ok &= GL_CAT(out, caps->clip_control ? "#define HAS_CLIP_CONTROL 1\n" : "#define HAS_CLIP_CONTROL 0\n");

	/* (several shaders to a file) */
	switch (type)
	{
		case GL_VERTEX_SHADER:
			ok &= GL_CAT(out, "#define VERTEX_SHADER 1\n");
			break;
		case GL_GEOMETRY_SHADER:
			ok &= GL_CAT(out, "#define GEOMETRY_SHADER 1\n");
			break;
		case GL_FRAGMENT_SHADER:
			ok &= GL_CAT(out, "#define FRAGMENT_SHADER 1\n");
			break;
		default:
			break;
	}

	/* The entry point named main (and not main as main: the recursive
	 * macro breaks some Intel drivers) */
	if (strcmp(entry, "main") != 0)
	{
		ok &= GL_CAT(out, "#define ");
		ok &= GL_CAT(out, entry);
		ok &= GL_CAT(out, " main\n");
	}

	if (macro)
		ok &= GL_CAT(out, macro);

	return ok;
}

bool gs_gl_shader_source(gs_str_t* out, const gs_gl_shader_caps* caps, const char* entry, GLenum type,
	const char* code, const char* macro)
{
	return gl_header(out, caps, entry, type, macro) && (!code || GL_CAT(out, code));
}

/* "#define name value\n" onto macro */
static bool gl_macro(gs_str_t* macro, const char* name, int value)
{
	return GL_CAT(macro, "#define ") && GL_CAT(macro, name) && GL_CAT(macro, " ") && gs_str_cat_int(macro, value)
		&& GL_CAT(macro, "\n");
}

bool gs_gl_vs_source(gs_str_t* out, const gs_gl_shader_caps* caps, gs_hw_vs_selector sel, const char* code)
{
	gs_str_t macro;
	bool ok;

	log_cb(RETRO_LOG_INFO, "Compiling new vertex shader with selector 0x%X\n", (unsigned)sel.key);

	/* (the macros first, then the header made and the two joined) */
	gs_str_init(&macro);
	ok = gl_macro(&macro, "VS_FST", sel.fst) && gl_macro(&macro, "VS_IIP", sel.iip)
		&& gl_macro(&macro, "VS_POINT_SIZE", sel.point_size) && gl_macro(&macro, "VS_EXPAND", (int)sel.expand)
		&& gs_gl_shader_source(out, caps, "vs_main", GL_VERTEX_SHADER, code, GS_STR(&macro));
	gs_str_free(&macro);
	return ok;
}

/* The pixel selector's fields as tfx_fs.glsl's macros, each in the bits
 * after the last and none across a word: the dynamic pixel shader's PsKey
 * (gl_key_place) */
#define GL_PS_FIELDS(X) \
	X(PS_FST, fst, 1) X(PS_WMS, wms, 2) X(PS_WMT, wmt, 2) X(PS_ADJS, adjs, 1) X(PS_ADJT, adjt, 1) \
	X(PS_AEM_FMT, aem_fmt, 2) X(PS_PAL_FMT, pal_fmt, 2) X(PS_DST_FMT, dst_fmt, 2) X(PS_DEPTH_FMT, depth_fmt, 2) \
	X(PS_CHANNEL_FETCH, channel, 3) X(PS_URBAN_CHAOS_HLE, urban_chaos_hle, 1) \
	X(PS_TALES_OF_ABYSS_HLE, tales_of_abyss_hle, 1) X(PS_TEX_IS_FB, tex_is_fb, 1) X(PS_AEM, aem, 1) \
	X(PS_TFX, tfx, 3) X(PS_TCC, tcc, 1) X(PS_ATST, atst, 3) X(PS_AFAIL, afail, 3) X(PS_FOG, fog, 1) \
	X(PS_BLEND_HW, blend_hw, 3) X(PS_A_MASKED, a_masked, 1) X(PS_FBA, fba, 1) X(PS_LTF, ltf, 1) \
	X(PS_AUTOMATIC_LOD, automatic_lod, 1) X(PS_MANUAL_LOD, manual_lod, 1) X(PS_COLCLIP, colclip, 1) \
	X(PS_DATE, date, 3) X(PS_TCOFFSETHACK, tcoffsethack, 1) X(PS_REGION_RECT, region_rect, 1) \
	X(PS_SAMPLE_MAP, sample_map, 1) X(PS_NATIVE_TAPS, native_taps, 1) X(PS_BLEND_A, blend_a, 2) \
	X(PS_BLEND_B, blend_b, 2) X(PS_BLEND_C, blend_c, 2) X(PS_BLEND_D, blend_d, 2) X(PS_IIP, iip, 1) \
	X(PS_SHUFFLE, shuffle, 1) X(PS_SHUFFLE_SAME, shuffle_same, 1) X(PS_PROCESS_BA, process_ba, 2) \
	X(PS_PROCESS_RG, process_rg, 2) X(PS_SHUFFLE_ACROSS, shuffle_across, 1) X(PS_READ16_SRC, real16src, 1) \
	X(PS_WRITE_RG, write_rg, 1) X(PS_FBMASK, fbmask, 1) X(PS_COLCLIP_HW, colclip_hw, 1) \
	X(PS_RTA_CORRECTION, rta_correction, 1) X(PS_RTA_SRC_CORRECTION, rta_source_correction, 1) \
	X(PS_DITHER, dither, 2) X(PS_DITHER_ADJUST, dither_adjust, 1) X(PS_ZCLAMP, zclamp, 1) \
	X(PS_BLEND_MIX, blend_mix, 2) X(PS_ROUND_INV, round_inv, 1) X(PS_FIXED_ONE_A, fixed_one_a, 1) \
	X(PS_PABE, pabe, 1) X(PS_SCANMSK, scanmsk, 2) X(PS_NO_COLOR, no_color, 1) X(PS_NO_COLOR1, no_color1, 1)

/* The next field's place: in the word it starts in, else the next */
static u32 gl_key_place(u32* at, u32 bits)
{
	u32 place;
	if ((*at & 31u) + bits > 32u)
		*at = (*at + 31u) & ~31u;
	place = *at;
	*at += bits;
	return place;
}

void gs_gl_dynamic_key(const gs_hw_ps_selector* sel, gs_hw_vs_selector vs, u32 key[8])
{
	u32 at = 0, place;
	memset(key, 0, 8 * sizeof(u32));
#define GL_PACK(name, field, bits) \
	place = gl_key_place(&at, bits); \
	key[place >> 5] |= ((u32)sel->field & ((1u << bits) - 1u)) << (place & 31u);
	GL_PS_FIELDS(GL_PACK)
#undef GL_PACK
	key[4] = (u32)vs.fst | ((u32)vs.point_size << 1);
}

bool gs_gl_ps_source(gs_str_t* out, const gs_gl_shader_caps* caps, const gs_hw_ps_selector* sel, const char* code)
{
	gs_str_t macro;
	bool ok = true;

	log_cb(RETRO_LOG_INFO, "Compiling new pixel shader with selector 0x%X%08X\n", (unsigned)sel->key_hi,
		(unsigned)sel->key_lo);

	gs_str_init(&macro);
#define GL_MACRO(name, field, bits) ok = ok && gl_macro(&macro, #name, (int)sel->field);
	GL_PS_FIELDS(GL_MACRO)
#undef GL_MACRO
	ok = ok && gs_gl_shader_source(out, caps, "ps_main", GL_FRAGMENT_SHADER, code, GS_STR(&macro));
	gs_str_free(&macro);
	return ok;
}

bool gs_gl_dynamic_sources(gs_str_t* vs, gs_str_t* ps, const gs_gl_shader_caps* caps, u32 iip, u32 expand,
	u32 date, const char* vs_code, const char* ps_code)
{
	gs_str_t macro;
	char buf[128]; /* (a line is under 64) */
	u32 at = 0, place;
	bool ok;

	gs_str_init(&macro);
	ok = gl_macro(&macro, "VS_DYNAMIC", 1) && gl_macro(&macro, "VS_IIP", (int)iip)
		&& gl_macro(&macro, "VS_EXPAND", (int)expand)
		&& gs_gl_shader_source(vs, caps, "vs_main", GL_VERTEX_SHADER, vs_code, GS_STR(&macro));
	gs_str_clear(&macro);
	ok = ok && gl_macro(&macro, "PS_DYNAMIC", 1) && gl_macro(&macro, "IF_IIP", (int)iip)
		&& gl_macro(&macro, "IF_PRIMID", date != 0) && gl_macro(&macro, "IF_DATE3", date == 2)
		&& gl_macro(&macro, "IF_ZCLAMP", date != 1);
#define GL_DYN(name, field, bits) \
	place = gl_key_place(&at, bits); \
	sprintf(buf, "#define " #name " int((PsKey[%u] >> %uu) & %uu)\n", (unsigned)(place >> 5), (unsigned)(place & 31u), \
		(unsigned)((1u << bits) - 1u)); \
	ok = ok && GL_CAT(&macro, buf);
	GL_PS_FIELDS(GL_DYN)
#undef GL_DYN
	ok = ok && gs_gl_shader_source(ps, caps, "ps_main", GL_FRAGMENT_SHADER, ps_code, GS_STR(&macro));
	gs_str_free(&macro);
	return ok;
}

u32 gs_gl_dynamic_date(u32 date)
{
	return (date == 1 || date == 2) ? 1u : (date == 3) ? 2u : 0u;
}
