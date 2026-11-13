/* The GL device's state cache (GS/Renderers/OpenGL/GLState.c): the C and
 * C++ halves see the same layout (xlayout skips the OpenGL headers, which
 * need glad), and clearing it gives a new context's state - the point
 * size and line width included, which a device made again used to
 * inherit from the last one. */
#include <stddef.h>
#include <stdio.h>

#include "GS/Renderers/OpenGL/GLState.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define GL_STATE_FIELDS(X) X(vao) X(fbo) X(viewport) X(scissor) X(point_size) \
	X(line_width) X(blend) X(eq_RGB) X(f_sRGB) X(f_dRGB) X(f_sA) X(f_dA) X(bf) \
	X(wrgba) X(depth) X(depth_func) X(depth_mask) X(stencil) X(stencil_func) \
	X(stencil_pass) X(ps_ss) X(rt) X(ds) X(tex_unit) X(tex_handle)
#define GL_STATE_OFFSET(f) offsetof(gs_gl_state, f),

static const size_t c_layout[] = {GL_STATE_FIELDS(GL_STATE_OFFSET) sizeof(gs_gl_state)};
extern const size_t gl_state_cxx_layout[];
extern const size_t gl_state_cxx_count;

int main(void)
{
	size_t i;

	CHECK(gl_state_cxx_count == sizeof(c_layout) / sizeof(c_layout[0]));
	for (i = 0; i < gl_state_cxx_count && i < sizeof(c_layout) / sizeof(c_layout[0]); i++)
		if (c_layout[i] != gl_state_cxx_layout[i])
		{
			printf("  field %u: C at %u, C++ at %u\n", (unsigned)i,
				(unsigned)c_layout[i], (unsigned)gl_state_cxx_layout[i]);
			fails++;
		}

	g_gl_state.point_size = 1;
	g_gl_state.line_width = 3.0f;
	g_gl_state.vao = 7;
	g_gl_state.tex_unit[5] = 9;
	gs_gl_state_clear();
	CHECK(g_gl_state.point_size == 0);
	CHECK(g_gl_state.line_width == 1.0f);
	CHECK(g_gl_state.vao == 0 && g_gl_state.tex_unit[5] == 0 && g_gl_state.rt == NULL);
	CHECK(g_gl_state.viewport[0] == 1 && g_gl_state.viewport[1] == 1);
	CHECK(g_gl_state.scissor[0] == 0 && g_gl_state.scissor[2] == 1 && g_gl_state.scissor[3] == 1);
	CHECK(g_gl_state.eq_RGB == GL_FUNC_ADD && g_gl_state.f_sRGB == GL_ONE && g_gl_state.f_dRGB == GL_ZERO);
	CHECK(g_gl_state.f_sA == GL_ONE && g_gl_state.f_dA == GL_ZERO && g_gl_state.wrgba == 0xF);
	CHECK(g_gl_state.depth_func == GL_LESS && g_gl_state.stencil_func == GL_ALWAYS
		&& g_gl_state.stencil_pass == GL_KEEP);

	if (fails)
	{
		printf("glstate: %d failed\n", fails);
		return 1;
	}
	printf("glstate: ok\n");
	return 0;
}
