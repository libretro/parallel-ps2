/* SPDX-License-Identifier: LGPL-3.0+ */
/* GSDeviceOGL.cpp's GLSL as it was (the header, the selectors' sources,
 * the dynamic key and program), the class renamed RefGL: what
 * GLShaderSource.c is held to. */

#include "reference.h"

bool RefGL::GetShaderSource(gs_str_t* out, const char* entry, GLenum type, const char* glsl_h_code, const char* macro)
{
	return GenGlslHeader(out, entry, type, macro) && gs_str_cat(out, glsl_h_code);
}

bool RefGL::GenGlslHeader(gs_str_t* out, const char* entry, GLenum type, const char* macro)
{
	gs_str_t* const header = out;
	bool ok = true;
	gs_str_clear(header);

	if (m_is_gles)
	{
		ok &= gs_str_cat(header, GLAD_GL_ES_VERSION_3_2 ? "#version 320 es\n" : "#version 310 es\n") != 0;

		// What 3.20 has in core, 3.10 takes from extensions: the SHADER
		// interface blocks, and gl_PrimitiveID in a fragment shader.
		if (!GLAD_GL_ES_VERSION_3_2)
		{
			if (GLAD_GL_EXT_shader_io_blocks)
				ok &= gs_str_cat(header, "#extension GL_EXT_shader_io_blocks : require\n") != 0;
			else
				ok &= gs_str_cat(header, "#extension GL_OES_shader_io_blocks : require\n") != 0;
			if (type == GL_FRAGMENT_SHADER && m_features.primitive_id)
				ok &= gs_str_cat(header, GLAD_GL_EXT_geometry_shader ? "#extension GL_EXT_geometry_shader : require\n" :
				                                                      "#extension GL_OES_geometry_shader : require\n") != 0;
		}
		if (m_features.dual_source_blend && type == GL_FRAGMENT_SHADER)
			ok &= gs_str_cat(header, "#extension GL_EXT_blend_func_extended : require\n") != 0;
		if (m_features.framebuffer_fetch && type == GL_FRAGMENT_SHADER)
			ok &= gs_str_cat(header, "#extension GL_EXT_shader_framebuffer_fetch : require\n") != 0;

		// ES has no default float precision in a fragment shader, and PS2
		// depth and colour need all of it everywhere.
		ok &= gs_str_cat(header, "precision highp float;\n") != 0;
		ok &= gs_str_cat(header, "precision highp int;\n") != 0;
		ok &= gs_str_cat(header, "precision highp sampler2D;\n") != 0;
	}
	else
	{
		// Intel's GL driver doesn't like the readonly qualifier with 3.3 GLSL.
		if (m_features.vs_expand && GLAD_GL_VERSION_4_3)
		{
			ok &= gs_str_cat(header, "#version 430 core\n") != 0;
		}
		else
		{
			ok &= gs_str_cat(header, "#version 330 core\n") != 0;
			ok &= gs_str_cat(header, "#extension GL_ARB_shading_language_420pack : require\n") != 0;
			if (GLAD_GL_ARB_gpu_shader5)
				ok &= gs_str_cat(header, "#extension GL_ARB_gpu_shader5 : require\n") != 0;
			if (m_features.vs_expand)
				ok &= gs_str_cat(header, "#extension GL_ARB_shader_storage_buffer_object: require\n") != 0;
		}

		if (GLAD_GL_ARB_shader_draw_parameters)
			ok &= gs_str_cat(header, "#extension GL_ARB_shader_draw_parameters : require\n") != 0;
		if (m_features.framebuffer_fetch && GLAD_GL_EXT_shader_framebuffer_fetch)
			ok &= gs_str_cat(header, "#extension GL_EXT_shader_framebuffer_fetch : require\n") != 0;
	}

	if (m_features.dual_source_blend)
		ok &= gs_str_cat(header, "#define HAS_DUAL_SOURCE 1\n") != 0;
	else
		ok &= gs_str_cat(header, "#define HAS_DUAL_SOURCE 0\n") != 0;

	if (m_features.framebuffer_fetch)
		ok &= gs_str_cat(header, "#define HAS_FRAMEBUFFER_FETCH 1\n") != 0;
	else
		ok &= gs_str_cat(header, "#define HAS_FRAMEBUFFER_FETCH 0\n") != 0;

	if (m_features.clip_control)
		ok &= gs_str_cat(header, "#define HAS_CLIP_CONTROL 1\n") != 0;
	else
		ok &= gs_str_cat(header, "#define HAS_CLIP_CONTROL 0\n") != 0;

	// Allow to puts several shader in 1 files
	switch (type)
	{
		case GL_VERTEX_SHADER:
			ok &= gs_str_cat(header, "#define VERTEX_SHADER 1\n") != 0;
			break;
		case GL_GEOMETRY_SHADER:
			ok &= gs_str_cat(header, "#define GEOMETRY_SHADER 1\n") != 0;
			break;
		case GL_FRAGMENT_SHADER:
			ok &= gs_str_cat(header, "#define FRAGMENT_SHADER 1\n") != 0;
			break;
		default:
			break;
	}

	// Don't remove this, the recursive macro breaks some Intel drivers.
	if (strcmp(entry, "main") != 0)
	{
		// Select the entry point ie the main function
		ok &= gs_str_cat(header, "#define ") != 0;
		ok &= gs_str_cat(header, entry) != 0;
		ok &= gs_str_cat(header, " main\n") != 0;
	}

	if (macro)
		ok &= gs_str_cat(header, macro) != 0;

	return ok;
}

/* "#define name value\n" onto macro. */
static bool GSDeviceOGLAppendShaderMacro(gs_str_t* macro, const char* name, int value)
{
	return gs_str_cat(macro, "#define ") && gs_str_cat(macro, name) && gs_str_cat(macro, " ")
		&& gs_str_cat_int(macro, value) && gs_str_cat(macro, "\n");
}

bool RefGL::GetVSSource(gs_str_t* out, RefVSSelector sel)
{
	log_cb(RETRO_LOG_INFO, "Compiling new vertex shader with selector 0x%" PRIX64 "\n", (uint64_t)sel.key);

	/* (the macros first, where the header will be, then the header is
	 * made elsewhere and the two joined: a second string) */
	gs_str_t macro;
	bool ok;
	gs_str_init(&macro);
	ok = GSDeviceOGLAppendShaderMacro(&macro, "VS_FST", sel.fst)
		&& GSDeviceOGLAppendShaderMacro(&macro, "VS_IIP", sel.iip)
		&& GSDeviceOGLAppendShaderMacro(&macro, "VS_POINT_SIZE", sel.point_size)
		&& GSDeviceOGLAppendShaderMacro(&macro, "VS_EXPAND", (int)sel.expand)
		&& GetShaderSource(out, "vs_main", GL_VERTEX_SHADER, tfx_vs_glsl_shader_raw, GS_STR(&macro));
	gs_str_free(&macro);
	return ok;
}

/* The pixel selector fields as tfx_fs.glsl's macros, each in the bits
 * after the last and none across a word: the dynamic pixel shader's PsKey
 * (GLKeyPlace). */
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

/* The next field's place: in the word it starts in, else the next. */
static u32 GLKeyPlace(u32* at, u32 bits)
{
	u32 place;
	if ((*at & 31u) + bits > 32u)
		*at = (*at + 31u) & ~31u;
	place = *at;
	*at += bits;
	return place;
}

/* The dynamic shaders' key: the pixel selector (PsKey), then the vertex
 * selector's texture coordinates and point size (VsKey). */
static void GLDynamicKey(const RefPSSelector& sel, RefVSSelector vs, u32* key)
{
	u32 at = 0, place;
	memset(key, 0, 8 * sizeof(u32));
#define GL_PACK(name, field, bits) \
	place = GLKeyPlace(&at, bits); \
	key[place >> 5] |= (static_cast<u32>(sel.field) & ((1u << bits) - 1u)) << (place & 31u);
	GL_PS_FIELDS(GL_PACK)
#undef GL_PACK
	key[4] = vs.fst | (vs.point_size << 1);
}

bool RefGL::GetPSSource(gs_str_t* out, const RefPSSelector& sel)
{
	log_cb(RETRO_LOG_INFO, "Compiling new pixel shader with selector 0x%" PRIX64 "%08X\n", (uint64_t)sel.key_hi, (unsigned)sel.key_lo);

	gs_str_t macro;
	bool ok = true;
	gs_str_init(&macro);
#define GL_MACRO(name, field, bits) ok = ok && GSDeviceOGLAppendShaderMacro(&macro, #name, static_cast<int>(sel.field));
	GL_PS_FIELDS(GL_MACRO)
#undef GL_MACRO
	ok = ok && GetShaderSource(out, "ps_main", GL_FRAGMENT_SHADER, tfx_fs_glsl_shader_raw, GS_STR(&macro));
	gs_str_free(&macro);
	return ok;
}

/* The dynamic program of a flat colour (iip), a vertex expansion and an
 * alpha test class (GLDynamicDate): every field read from the key but
 * what shapes the interface. The second output and the depth output are
 * kept but in the alpha test's first pass, which writes neither. */
bool RefGL::BuildDynamicProgram(GLProgram* prog, u32 iip, u32 expand, u32 date)
{
	gs_str_t macro;
	char buf[64];
	u32 at = 0, place;
	bool ok;
	gs_str_init(&macro);
	ok = GSDeviceOGLAppendShaderMacro(&macro, "VS_DYNAMIC", 1)
		&& GSDeviceOGLAppendShaderMacro(&macro, "VS_IIP", static_cast<int>(iip))
		&& GSDeviceOGLAppendShaderMacro(&macro, "VS_EXPAND", static_cast<int>(expand))
		&& GetShaderSource(&m_src_vs, "vs_main", GL_VERTEX_SHADER, tfx_vs_glsl_shader_raw, GS_STR(&macro));
	gs_str_clear(&macro);
	ok = ok && GSDeviceOGLAppendShaderMacro(&macro, "PS_DYNAMIC", 1)
		&& GSDeviceOGLAppendShaderMacro(&macro, "IF_IIP", static_cast<int>(iip))
		&& GSDeviceOGLAppendShaderMacro(&macro, "IF_PRIMID", date != 0)
		&& GSDeviceOGLAppendShaderMacro(&macro, "IF_DATE3", date == 2)
		&& GSDeviceOGLAppendShaderMacro(&macro, "IF_ZCLAMP", date != 1);
#define GL_DYN(name, field, bits) \
	place = GLKeyPlace(&at, bits); \
	snprintf(buf, sizeof(buf), "#define " #name " int((PsKey[%u] >> %uu) & %uu)\n", place >> 5, place & 31u, (1u << bits) - 1u); \
	ok = ok && gs_str_cat(&macro, buf);
	GL_PS_FIELDS(GL_DYN)
#undef GL_DYN
	ok = ok && GetShaderSource(&m_src_ps, "ps_main", GL_FRAGMENT_SHADER, tfx_fs_glsl_shader_raw, GS_STR(&macro))
		&& GetProgram(prog, &m_src_vs, &m_src_ps);
	gs_str_free(&macro);
	return ok;
}

/* The alpha test class a dynamic program is built for: none, its first
 * pass (1 and 2) or the primitive ID test (3). */
static u32 GLDynamicDate(u32 date)
{
	return (date == 1 || date == 2) ? 1u : (date == 3) ? 2u : 0u;
}


/* (the test's: the file's own functions, by name) */
void RefDynamicKey(const RefPSSelector& sel, RefVSSelector vs, u32* key) { GLDynamicKey(sel, vs, key); }
u32 RefDynamicDate(u32 date) { return GLDynamicDate(date); }
