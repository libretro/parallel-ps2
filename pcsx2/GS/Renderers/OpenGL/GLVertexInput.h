/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#ifndef GS_GL_VERTEX_INPUT_H
#define GS_GL_VERTEX_INPUT_H

#include "common/Pcsx2Defs.h"
#include "glad.h"
#include "GLStreamBuffer.h"

/* The vertex layouts the device draws with (GSVertexPT1, GSVertex) */
#define GS_GL_VERTEX_PT1_SIZE 32
#define GS_GL_VERTEX_SIZE     32
/* Quads the expansion index buffer covers (GSDevice::EXPAND_BUFFER_SIZE) */
#define GS_GL_EXPAND_QUADS 16383

/* The device's vertex arrays: the one it draws with and, where vertices
 * are expanded in the vertex shader, the one with the expansion indices.
 * Zeroed is none. */
typedef struct gs_gl_vertex_input
{
	GLuint vao;
	GLuint expand_vao;
	GLuint expand_ibo;
} gs_gl_vertex_input;

#ifdef __cplusplus
extern "C" {
#endif

/* Makes the drawing vertex array and binds it (the device then makes its
 * stream buffers on it) */
void gs_gl_vertex_input_begin(gs_gl_vertex_input* vi);

/* Binds the vertex and index streams to it and sets the vertex layout;
 * with expand, makes the expansion array and indices too, and with
 * ssbo_vertices binds the vertex stream once as storage buffer 2. Zero if
 * the indices could not be made. */
int gs_gl_vertex_input_finish(gs_gl_vertex_input* vi, const gs_gl_stream_buffer* vertices,
	const gs_gl_stream_buffer* indices, int expand, int ssbo_vertices);

/* Deletes the arrays and indices it made; vi is none after */
void gs_gl_vertex_input_destroy(gs_gl_vertex_input* vi);

/* The expansion indices: six a quad (two triangles) for quads quads */
void gs_gl_expand_indices(u16* out, u32 quads);

#ifdef __cplusplus
}
#endif

#endif
