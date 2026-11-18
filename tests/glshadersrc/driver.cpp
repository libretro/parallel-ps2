/* SPDX-License-Identifier: LGPL-3.0+ */
/* The OpenGL renderer's GLSL (GS/Renderers/OpenGL/GLShaderSource.c) held
 * to the C++ that wrote it (reference.cpp, kept as it was): the header
 * for every context (GLES 3.1 and 3.2, GL 3.3 and 4.3, each extension
 * there or not, each feature on or off), the sources of random vertex
 * and pixel selectors, the dynamic key and the dynamic programs, compared
 * as strings. The selectors are the old C++ ones on the reference's side
 * and the C ones on the other, the same bytes: a field moved between
 * them shows in its macro. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reference.h"
#include "GS/Renderers/OpenGL/GLShaderSource.h"

extern "C" {
static void quiet(enum retro_log_level, const char*, ...) {}
retro_log_printf_t log_cb = quiet;
}

const char* tfx_vs_glsl_shader_raw = "// tfx_vs\nvoid vs_main() {}\n";
const char* tfx_fs_glsl_shader_raw = "// tfx_fs\nvoid ps_main() {}\n";

static u32 s_rng = 0xa54ff53au;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

static long s_checks;

static bool same(const char* what, int n, gs_str_t* a, gs_str_t* b)
{
	s_checks++;
	if (strcmp(GS_STR(a), GS_STR(b)) == 0)
		return true;
	printf("%s %d differs:\n--- reference\n%s\n--- C\n%s\n", what, n, GS_STR(a), GS_STR(b));
	return false;
}

int main(void)
{
	static const char* const entries[] = {"main", "vs_main", "ps_main", "ps_convert_rgba8_16bits", "ps_vr_eye"};
	static const GLenum types[] = {GL_VERTEX_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER, GL_COMPUTE_SHADER};
	RefGL ref;
	gs_str_t a, b, a2, b2;
	int n, k;

	gs_str_init(&a);
	gs_str_init(&b);
	gs_str_init(&a2);
	gs_str_init(&b2);
	gs_str_init(&ref.m_src_vs);
	gs_str_init(&ref.m_src_ps);

	for (n = 0; n < 20000; n++)
	{
		const u32 f = rnd();
		gs_gl_shader_caps caps;
		RefPSSelector rps;
		RefVSSelector rvs;
		gs_hw_ps_selector ps;
		gs_hw_vs_selector vs;
		u32 ka[8], kb[8];
		GLProgram prog;

		/* the context */
		GLAD_GL_ES_VERSION_3_2              = (f >> 0) & 1;
		GLAD_GL_EXT_shader_io_blocks         = (f >> 1) & 1;
		GLAD_GL_EXT_geometry_shader          = (f >> 2) & 1;
		GLAD_GL_VERSION_4_3                  = (f >> 3) & 1;
		GLAD_GL_ARB_gpu_shader5              = (f >> 4) & 1;
		GLAD_GL_ARB_shader_draw_parameters   = (f >> 5) & 1;
		GLAD_GL_EXT_shader_framebuffer_fetch = (f >> 6) & 1;
		ref.m_is_gles                        = (f >> 7) & 1;
		memset(&ref.m_features, 0, sizeof(ref.m_features));
		ref.m_features.vs_expand         = (f >> 8) & 1;
		ref.m_features.primitive_id      = (f >> 9) & 1;
		ref.m_features.dual_source_blend = (f >> 10) & 1;
		ref.m_features.framebuffer_fetch = (f >> 11) & 1;
		ref.m_features.clip_control      = (f >> 12) & 1;
		caps.gles              = ref.m_is_gles;
		caps.vs_expand         = ref.m_features.vs_expand;
		caps.primitive_id      = ref.m_features.primitive_id;
		caps.dual_source_blend = ref.m_features.dual_source_blend;
		caps.framebuffer_fetch = ref.m_features.framebuffer_fetch;
		caps.clip_control      = ref.m_features.clip_control;

		/* a header, with a macro or none */
		{
			const char* entry = entries[rnd() % 5];
			const GLenum type = types[rnd() % 4];
			const char* macro = (rnd() & 1) ? "#define X 1\n" : NULL;
			ref.GetShaderSource(&a, entry, type, "// code\n", macro);
			gs_gl_shader_source(&b, &caps, entry, type, "// code\n", macro);
			if (!same("header", n, &a, &b))
				return 1;
		}

		/* the selectors: the same random bytes in each */
		{
			u32 words[3] = {rnd(), rnd(), rnd()};
			u8 v = (u8)rnd();
			memcpy(&rps, words, sizeof(words));
			memcpy(&ps, words, sizeof(words));
			memcpy(&rvs, &v, 1);
			memcpy(&vs, &v, 1);
		}
		ref.GetVSSource(&a, rvs);
		gs_gl_vs_source(&b, &caps, vs, tfx_vs_glsl_shader_raw);
		if (!same("vertex shader", n, &a, &b))
			return 1;
		ref.GetPSSource(&a, rps);
		gs_gl_ps_source(&b, &caps, &ps, tfx_fs_glsl_shader_raw);
		if (!same("pixel shader", n, &a, &b))
			return 1;

		RefDynamicKey(rps, rvs, ka);
		gs_gl_dynamic_key(&ps, vs, kb);
		s_checks++;
		if (memcmp(ka, kb, sizeof(ka)))
		{
			printf("dynamic key %d differs\n", n);
			return 1;
		}

		/* a dynamic program */
		{
			const u32 iip = rnd() & 1, expand = rnd() & 3, date = rnd() % 3;
			ref.BuildDynamicProgram(&prog, iip, expand, date);
			gs_gl_dynamic_sources(&a2, &b2, &caps, iip, expand, date, tfx_vs_glsl_shader_raw, tfx_fs_glsl_shader_raw);
			if (!same("dynamic vertex shader", n, &ref.m_src_vs, &a2) || !same("dynamic pixel shader", n, &ref.m_src_ps, &b2))
				return 1;
		}
	}

	for (k = 0; k < 16; k++)
	{
		s_checks++;
		if (RefDynamicDate((u32)k) != gs_gl_dynamic_date((u32)k))
		{
			printf("dynamic date %d differs\n", k);
			return 1;
		}
	}

	gs_str_free(&a);
	gs_str_free(&b);
	gs_str_free(&a2);
	gs_str_free(&b2);
	gs_str_free(&ref.m_src_vs);
	gs_str_free(&ref.m_src_ps);
	printf("glshadersrc: %ld checks, 0 differed\n", s_checks);
	return 0;
}
