/* The C++ side's view of gs_gl_state, for main.c to compare */
#include <stddef.h>

#include "GS/Renderers/OpenGL/GLState.h"

#define GL_STATE_FIELDS(X) X(vao) X(fbo) X(viewport) X(scissor) X(point_size) \
	X(line_width) X(blend) X(eq_RGB) X(f_sRGB) X(f_dRGB) X(f_sA) X(f_dA) X(bf) \
	X(wrgba) X(depth) X(depth_func) X(depth_mask) X(stencil) X(stencil_func) \
	X(stencil_pass) X(ps_ss) X(rt) X(ds) X(tex_unit) X(tex_handle)
#define GL_STATE_OFFSET(f) offsetof(gs_gl_state, f),

extern "C" const size_t gl_state_cxx_layout[] = {GL_STATE_FIELDS(GL_STATE_OFFSET) sizeof(gs_gl_state)};
extern "C" const size_t gl_state_cxx_count = sizeof(gl_state_cxx_layout) / sizeof(gl_state_cxx_layout[0]);
