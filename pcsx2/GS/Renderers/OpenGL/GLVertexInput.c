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

#include <stdlib.h>

#include "GLState.h"
#include "GLVertexInput.h"

static void gs_gl_vertex_input_bind(GLuint vao)
{
	glBindVertexArray(vao);
	g_gl_state.vao = vao;
}

void gs_gl_vertex_input_begin(gs_gl_vertex_input* vi)
{
	vi->vao        = 0;
	vi->expand_vao = 0;
	vi->expand_ibo = 0;
	glGenVertexArrays(1, &vi->vao);
	gs_gl_vertex_input_bind(vi->vao);
}

void gs_gl_expand_indices(u16* out, u32 quads)
{
	u32 i;
	for (i = 0; i < quads; i++)
	{
		const u16 base = (u16)(i * 4);
		*(out++) = (u16)(base + 0);
		*(out++) = (u16)(base + 1);
		*(out++) = (u16)(base + 2);
		*(out++) = (u16)(base + 1);
		*(out++) = (u16)(base + 2);
		*(out++) = (u16)(base + 3);
	}
}

int gs_gl_vertex_input_finish(gs_gl_vertex_input* vi, const gs_gl_stream_buffer* vertices,
	const gs_gl_stream_buffer* indices, int expand, int ssbo_vertices)
{
	const GLsizeiptr expand_size = (GLsizeiptr)(GS_GL_EXPAND_QUADS * 6 * sizeof(u16));
	GLuint i;
	u16* data;

	glBindBuffer(vertices->target, vertices->buffer_id);
	glBindBuffer(indices->target, indices->buffer_id);

	for (i = 0; i < 8; i++)
		glEnableVertexAttribArray(i);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, GS_GL_VERTEX_PT1_SIZE, (const GLvoid*)0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, GS_GL_VERTEX_PT1_SIZE, (const GLvoid*)16);
	glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, GS_GL_VERTEX_SIZE, (const GLvoid*)8);
	glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, GS_GL_VERTEX_SIZE, (const GLvoid*)12);
	glVertexAttribIPointer(4, 2, GL_UNSIGNED_SHORT, GS_GL_VERTEX_SIZE, (const GLvoid*)16);
	glVertexAttribIPointer(5, 1, GL_UNSIGNED_INT, GS_GL_VERTEX_SIZE, (const GLvoid*)20);
	glVertexAttribIPointer(6, 2, GL_UNSIGNED_SHORT, GS_GL_VERTEX_SIZE, (const GLvoid*)24);
	glVertexAttribPointer(7, 4, GL_UNSIGNED_BYTE, GL_TRUE, GS_GL_VERTEX_SIZE, (const GLvoid*)28);

	if (!expand)
		return 1;

	glGenVertexArrays(1, &vi->expand_vao);
	gs_gl_vertex_input_bind(vi->expand_vao);

	/* Still need the vertex buffer bound: uploads happen to GL_ARRAY_BUFFER */
	glBindBuffer(vertices->target, vertices->buffer_id);

	data = (u16*)malloc((size_t)expand_size);
	if (!data)
		return 0;
	gs_gl_expand_indices(data, GS_GL_EXPAND_QUADS);
	glGenBuffers(1, &vi->expand_ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vi->expand_ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, expand_size, data, GL_STATIC_DRAW);
	free(data);

	/* bound once when the shader has gl_BaseVertexARB */
	if (ssbo_vertices)
		glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, vertices->buffer_id, 0, vertices->size);
	return 1;
}

void gs_gl_vertex_input_destroy(gs_gl_vertex_input* vi)
{
	gs_gl_vertex_input_bind(0);
	if (vi->expand_vao != 0)
		glDeleteVertexArrays(1, &vi->expand_vao);
	if (vi->vao != 0)
		glDeleteVertexArrays(1, &vi->vao);
	if (vi->expand_ibo != 0)
		glDeleteBuffers(1, &vi->expand_ibo);
	vi->vao        = 0;
	vi->expand_vao = 0;
	vi->expand_ibo = 0;
}
