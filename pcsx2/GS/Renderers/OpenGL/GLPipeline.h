/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GL_PIPELINE_H
#define GL_PIPELINE_H

/* The OpenGL renderer's fixed-function state, from a HW draw's keys
 * (GSHWDrawConfig.h): samplers, depth and stencil tests, blending and the
 * colour mask, each set through the state cache (GLState.h) so that only
 * what changes reaches the driver. */

#include <boolean.h>

#include "common/Pcsx2Defs.h"
#include "glad.h"
#include "GS/Renderers/Common/GSHWDrawConfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A depth and stencil test: off by default, depth ALWAYS without a
 * write, stencil keeping */
typedef struct gs_gl_depth_stencil
{
	bool depth_enable;
	bool depth_mask;
	bool stencil_enable;
	GLenum depth_func;
	GLenum stencil_func;
	GLenum stencil_op; /* on a pass (stencil and depth) */
} gs_gl_depth_stencil;

void gs_gl_depth_stencil_init(gs_gl_depth_stencil* d);
/* The tests of a draw's depth key: ZTST and ZWE; DATE in the stencil */
void gs_gl_depth_stencil_from(gs_gl_depth_stencil* d, gs_hw_depth_selector sel);
void gs_gl_depth_stencil_apply(const gs_gl_depth_stencil* d);

/* A sampler of the key's filters, wraps and LOD clamp; max_anisotropy
 * (the setting) where the key asks for it */
GLuint gs_gl_sampler_create(gs_hw_sampler_selector sel, int max_anisotropy);

/* Blending on (the factors and op GL's) or off; the constant, 128 one,
 * where is_constant */
void gs_gl_set_blend(bool enable, GLenum src_factor, GLenum dst_factor, GLenum op, GLenum src_factor_alpha,
	GLenum dst_factor_alpha, bool is_constant, u8 constant);
/* A draw's blend, its factors (GSDevice's) and op made GL's */
void gs_gl_set_hw_blend(const gs_hw_blend_state* b, bool dual_source_blend);
void gs_gl_set_color_mask(gs_hw_color_mask m);

/* A blend factor (GSDevice's) as GL's: without dual source blending a SRC1
 * one, which the renderer keeps off such a device, is logged once and taken
 * as its SRC one */
GLenum gs_gl_blend_factor(u8 factor, bool dual_source_blend);
GLenum gs_gl_blend_op(u8 op);

#ifdef __cplusplus
}
#endif

#endif
