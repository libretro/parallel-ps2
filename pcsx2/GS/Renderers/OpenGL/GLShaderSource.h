/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GL_SHADER_SOURCE_H
#define GL_SHADER_SOURCE_H

/* The OpenGL renderer's GLSL: each shader's source as the header its
 * context takes (version, extensions, what the driver has), the
 * selector's macros and the shader file's code. */

#include <boolean.h>

#include "common/Pcsx2Defs.h"
#include "glad.h"
#include "GS/Renderers/Common/GSString.h"
#include "GS/Renderers/Common/GSHWDrawConfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What of the context the header follows */
typedef struct gs_gl_shader_caps
{
	bool gles;
	bool vs_expand;
	bool primitive_id;
	bool dual_source_blend;
	bool framebuffer_fetch;
	bool clip_control;
} gs_gl_shader_caps;

/* The header for a shader of type (GL_*_SHADER) whose main is entry, then
 * macro (if any), then code (if any), into out */
bool gs_gl_shader_source(gs_str_t* out, const gs_gl_shader_caps* caps, const char* entry, GLenum type,
	const char* code, const char* macro);

/* The vertex and pixel shaders of the selectors, from the tfx files */
bool gs_gl_vs_source(gs_str_t* out, const gs_gl_shader_caps* caps, gs_hw_vs_selector sel, const char* code);
bool gs_gl_ps_source(gs_str_t* out, const gs_gl_shader_caps* caps, const gs_hw_ps_selector* sel, const char* code);

/* The dynamic shaders' key: the pixel selector packed as their PsKey, then
 * the vertex selector's texture coordinates and point size (VsKey) */
void gs_gl_dynamic_key(const gs_hw_ps_selector* sel, gs_hw_vs_selector vs, u32 key[8]);

/* The dynamic program of a flat colour (iip), a vertex expansion and an
 * alpha test class (gs_gl_dynamic_date): every field read from the key
 * but what shapes the interface */
bool gs_gl_dynamic_sources(gs_str_t* vs, gs_str_t* ps, const gs_gl_shader_caps* caps, u32 iip, u32 expand,
	u32 date, const char* vs_code, const char* ps_code);

/* The alpha test class a dynamic program is built for: none, its first
 * pass (1 and 2) or the primitive ID test (3) */
u32 gs_gl_dynamic_date(u32 date);

#ifdef __cplusplus
}
#endif

#endif
