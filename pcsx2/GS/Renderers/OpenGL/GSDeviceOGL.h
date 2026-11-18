/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef GS_DEVICE_OGL_H
#define GS_DEVICE_OGL_H

/* The OpenGL device (gs_device_new_opengl): the frontend's GL or GLES
 * context, through the table in GSDeviceOGL.c */

#include <boolean.h>

#include "common/Pcsx2Defs.h"
#include "glad.h"
#include "GLStreamBuffer.h"
#include "GLVertexInput.h"
#include "GLProgramCache.h"
#include "GLProgram.h"
#include "GLShaderCache.h"
#include "GLPipeline.h"
#include "GS/Renderers/Common/GSString.h"
#include "GS/Renderers/Common/GSDeviceBase.h"
#include "GS/Renderers/Common/GSHWDrawConfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A draw program's key: its two selectors, compared bit for bit */
typedef struct PCSX2_ALIGN(16) gs_gl_program_selector
{
	gs_hw_ps_selector ps;
	gs_hw_vs_selector vs;
	u8 pad[3];
} gs_gl_program_selector;

typedef struct gs_device_gl
{
	gs_device base;

	bool is_gles;
	bool gl_loaded; /* the frontend's GL entry points */
	bool colclip_float;
	bool disable_download_pbo;
	/* llvmpipe: a texture one scene still in flight samples and a later
	 * one renders to reads back stale (gs_gl_device_finish_before_cpu_read) */
	bool cpu_reads_need_finish;

	GLuint fbo;       /* frame buffer container */
	GLuint fbo_read;  /* only for reading */
	GLuint fbo_write; /* only for writing */

	gs_gl_stream_buffer texture_upload_buffer;
	gs_gl_stream_buffer vertex_stream_buffer;
	gs_gl_stream_buffer index_stream_buffer;
	gs_gl_vertex_input vi;
	GLenum draw_topology;

	gs_gl_stream_buffer vertex_uniform_stream_buffer;
	gs_gl_stream_buffer fragment_uniform_stream_buffer;
	GLint uniform_buffer_alignment;

	gs_gl_program merge_ps[2];
	gs_gl_program interlace_ps[GS_SHADER_INTERLACE_COUNT];

	struct
	{
		gs_str_t vs; /* its source, the vertex shader of several passes */
		gs_gl_program ps[GS_SHADER_CONVERT_COUNT];
		GLuint ln; /* sampler objects */
		GLuint pt;
		gs_gl_depth_stencil dss;
		gs_gl_depth_stencil dss_write;
	} convert;

	gs_gl_program present;
	gs_gl_program vr[2]; /* ps_vr_range, ps_vr_eye */
	/* What present_rect drew into the frontend's framebuffer, which is not
	 * the merge target's size when it is the VR eyes */
	gs_point present_size;

	struct
	{
		gs_gl_depth_stencil dss;
		gs_gl_program primid_ps[4];
	} date;

	GLuint ps_ss[1 << 8];
	gs_gl_depth_stencil om_dss[1 << 5];
	/* The draw programs by selector, compared bit for bit: a pool made with
	 * the device, so a lookup reads one slot and a new selector's program
	 * is never allocated as the game draws. */
	gs_gl_program_cache programs;
	/* The draws of one frame come in runs that share a program; the last
	 * selector spares the map a walk for every draw. */
	gs_gl_program_selector last_program_selector;
	gs_gl_program* last_program;
	gs_gl_shader_cache shader_cache;

	/* No program is compiled on the draw path. The dynamic programs (by
	 * flat colour, vertex expansion and alpha test class: they read the
	 * selectors from dynamic_key) are built as the device is created; a
	 * selector without a program of its own draws with one while the
	 * driver's threads compile its own (KHR_parallel_shader_compile), and
	 * without those threads stays drawn with one. */
	gs_gl_program dynamic[2][4][3];
	u32 dynamic_key[8];
	bool dynamic_key_valid;
	bool dynamic_ready;
	bool parallel_compile;
	u8 test_programs; /* LRPS2_TEST_GL_PROGRAMS: 1 sync, 2 dynamic */
	u32 programs_built_here;
	u32 programs_built_ahead;
	u32 programs_async;
	u32 dynamic_draws;

	GLuint palette_ss;

	gs_hw_vs_cb vs_cb_cache;
	gs_hw_ps_cb ps_cb_cache;

	/* The sources of the program being made, kept at the largest */
	gs_str_t src_vs;
	gs_str_t src_ps;
} gs_device_gl;

#ifdef __cplusplus
}
#endif

#endif
