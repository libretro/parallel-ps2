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

#include "GLProgram.h"

static GLuint s_last_program_id = 0;

void gs_gl_program_init(gs_gl_program* p)
{
	int i;
	p->program_id         = 0;
	p->vertex_shader_id   = 0;
	p->fragment_shader_id = 0;
	p->pending            = 0;
	p->uniform_count      = 0;
	for (i = 0; i <= GS_GL_PROGRAM_MAX_UNIFORMS; i++)
		p->uniforms[i] = -1;
}

/* The info log of shader or program object id, malloc'd, or NULL when
 * there is none worth printing. */
static char* gs_gl_info_log(GLuint id, int program)
{
	GLint info_log_length = 0;
	char* info_log;
	if (program)
		glGetProgramiv(id, GL_INFO_LOG_LENGTH, &info_log_length);
	else
		glGetShaderiv(id, GL_INFO_LOG_LENGTH, &info_log_length);
	/* A log without warnings is still a line break: one is none. */
	if (info_log_length <= 1)
		return NULL;
	info_log = (char*)malloc((size_t)info_log_length + 1);
	if (!info_log)
		return NULL;
	if (program)
		glGetProgramInfoLog(id, info_log_length, &info_log_length, info_log);
	else
		glGetShaderInfoLog(id, info_log_length, &info_log_length, info_log);
	info_log[info_log_length < 0 ? 0 : info_log_length] = 0;
	return info_log;
}

GLuint gs_gl_program_compile_shader(GLenum type, const char* source, size_t length)
{
	GLuint id = glCreateShader(type);
	const GLchar* src = source;
	GLint len         = (GLint)length;
	GLint status      = GL_FALSE;
	char* info_log;

	glShaderSource(id, 1, &src, &len);
	glCompileShader(id);
	glGetShaderiv(id, GL_COMPILE_STATUS, &status);

	info_log = gs_gl_info_log(id, 0);
	if (status == GL_FALSE || info_log)
	{
		if (status == GL_TRUE)
			log_cb(RETRO_LOG_WARN, "Shader compiled with warnings:\n%s\n", info_log);
		else
			log_cb(RETRO_LOG_ERROR, "Shader failed to compile:\n%s\n", info_log ? info_log : "");
		free(info_log);
		if (status == GL_FALSE)
		{
			glDeleteShader(id);
			return 0;
		}
	}

	return id;
}

void gs_gl_program_reset_last(void)
{
	s_last_program_id = 0;
}

int gs_gl_program_compile(gs_gl_program* p, const char* vertex_shader, size_t vertex_length,
	const char* fragment_shader, size_t fragment_length)
{
	if (vertex_length)
	{
		p->vertex_shader_id = gs_gl_program_compile_shader(GL_VERTEX_SHADER, vertex_shader, vertex_length);
		if (p->vertex_shader_id == 0)
			return 0;
	}

	if (fragment_length)
	{
		p->fragment_shader_id = gs_gl_program_compile_shader(GL_FRAGMENT_SHADER, fragment_shader, fragment_length);
		if (p->fragment_shader_id == 0)
			return 0;
	}

	p->program_id = glCreateProgram();
	if (p->vertex_shader_id != 0)
		glAttachShader(p->program_id, p->vertex_shader_id);
	if (p->fragment_shader_id != 0)
		glAttachShader(p->program_id, p->fragment_shader_id);
	return 1;
}

int gs_gl_program_create_from_binary(gs_gl_program* p, const void* data, u32 data_length, u32 data_format)
{
	GLuint prog = glCreateProgram();
	GLint link_status;

	glProgramBinary(prog, (GLenum)data_format, data, (GLsizei)data_length);
	glGetProgramiv(prog, GL_LINK_STATUS, &link_status);
	if (link_status != GL_TRUE)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create GL program from binary: status %d\n", link_status);
		glDeleteProgram(prog);
		return 0;
	}

	p->program_id = prog;
	return 1;
}

int gs_gl_program_get_binary(const gs_gl_program* p, gs_array_t* out_data, u32* out_data_format)
{
	GLint binary_size = 0;
	GLenum format     = 0;

	glGetProgramiv(p->program_id, GL_PROGRAM_BINARY_LENGTH, &binary_size);
	if (binary_size == 0)
	{
		log_cb(RETRO_LOG_WARN, "glGetProgramiv(GL_PROGRAM_BINARY_LENGTH) returned 0\n");
		return 0;
	}

	if (!gs_array_reserve(out_data, (size_t)binary_size))
		return 0;
	out_data->count = (size_t)binary_size;
	glGetProgramBinary(p->program_id, binary_size, &binary_size, &format, out_data->items);
	if (binary_size == 0)
	{
		log_cb(RETRO_LOG_WARN, "glGetProgramBinary() failed\n");
		return 0;
	}
	else if ((size_t)binary_size != out_data->count)
	{
		/* (never more than asked for: the driver got that much room) */
		log_cb(RETRO_LOG_WARN, "Size changed from %lu to %d after glGetProgramBinary()\n",
			(unsigned long)out_data->count, binary_size);
		out_data->count = (size_t)binary_size;
	}

	*out_data_format = (u32)format;
	return 1;
}

void gs_gl_program_set_binary_retrievable_hint(gs_gl_program* p)
{
	glProgramParameteri(p->program_id, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
}

int gs_gl_program_start_compile(gs_gl_program* p, const char* vertex_shader, size_t vertex_length,
	const char* fragment_shader, size_t fragment_length)
{
	const GLchar* src;
	GLint len;
	p->vertex_shader_id   = glCreateShader(GL_VERTEX_SHADER);
	p->fragment_shader_id = glCreateShader(GL_FRAGMENT_SHADER);
	src = vertex_shader;
	len = (GLint)vertex_length;
	glShaderSource(p->vertex_shader_id, 1, &src, &len);
	glCompileShader(p->vertex_shader_id);
	src = fragment_shader;
	len = (GLint)fragment_length;
	glShaderSource(p->fragment_shader_id, 1, &src, &len);
	glCompileShader(p->fragment_shader_id);
	p->program_id = glCreateProgram();
	glAttachShader(p->program_id, p->vertex_shader_id);
	glAttachShader(p->program_id, p->fragment_shader_id);
	glLinkProgram(p->program_id);
	p->pending = 1;
	return 1;
}

int gs_gl_program_poll(gs_gl_program* p)
{
	GLint done   = GL_FALSE;
	GLint status = GL_FALSE;
	if (!p->pending)
		return p->program_id ? 1 : -1;
	glGetProgramiv(p->program_id, GL_COMPLETION_STATUS_KHR, &done);
	if (!done)
		return 0;
	p->pending = 0;
	glGetProgramiv(p->program_id, GL_LINK_STATUS, &status);
	if (status != GL_TRUE)
	{
		char* vs_log = gs_gl_info_log(p->vertex_shader_id, 0);
		char* fs_log = gs_gl_info_log(p->fragment_shader_id, 0);
		char* pr_log = gs_gl_info_log(p->program_id, 1);
		log_cb(RETRO_LOG_ERROR, "Program failed to compile or link:\n%s%s%s\n", vs_log ? vs_log : "",
			fs_log ? fs_log : "", pr_log ? pr_log : "");
		free(vs_log);
		free(fs_log);
		free(pr_log);
		gs_gl_program_destroy(p);
		return -1;
	}
	glDeleteShader(p->vertex_shader_id);
	glDeleteShader(p->fragment_shader_id);
	p->vertex_shader_id = p->fragment_shader_id = 0;
	return 1;
}

int gs_gl_program_link(gs_gl_program* p)
{
	GLint status = GL_FALSE;
	char* info_log;

	glLinkProgram(p->program_id);

	if (p->vertex_shader_id != 0)
		glDeleteShader(p->vertex_shader_id);
	p->vertex_shader_id = 0;
	if (p->fragment_shader_id != 0)
		glDeleteShader(p->fragment_shader_id);
	p->fragment_shader_id = 0;

	glGetProgramiv(p->program_id, GL_LINK_STATUS, &status);

	info_log = gs_gl_info_log(p->program_id, 1);
	if (status == GL_FALSE || info_log)
	{
		if (status == GL_TRUE)
			log_cb(RETRO_LOG_ERROR, "Program linked with warnings:\n%s\n", info_log);
		else
			log_cb(RETRO_LOG_ERROR, "Program failed to link:\n%s\n", info_log ? info_log : "");
		free(info_log);
		if (status == GL_FALSE)
		{
			glDeleteProgram(p->program_id);
			p->program_id = 0;
			return 0;
		}
	}

	return 1;
}

void gs_gl_program_bind(const gs_gl_program* p)
{
	if (s_last_program_id == p->program_id)
		return;

	glUseProgram(p->program_id);
	s_last_program_id = p->program_id;
}

void gs_gl_program_destroy(gs_gl_program* p)
{
	if (p->vertex_shader_id != 0)
	{
		glDeleteShader(p->vertex_shader_id);
		p->vertex_shader_id = 0;
	}
	if (p->fragment_shader_id != 0)
	{
		glDeleteShader(p->fragment_shader_id);
		p->fragment_shader_id = 0;
	}
	if (p->program_id != 0)
	{
		glDeleteProgram(p->program_id);
		p->program_id = 0;
	}

	p->uniform_count = 0;
	p->pending       = 0;
}

int gs_gl_program_register_uniform(gs_gl_program* p, const char* name)
{
	const int id = p->uniform_count;
	if (id >= GS_GL_PROGRAM_MAX_UNIFORMS)
	{
		log_cb(RETRO_LOG_ERROR, "GL program: no room for uniform %s\n", name);
		return GS_GL_PROGRAM_MAX_UNIFORMS;
	}
	p->uniforms[id]  = glGetUniformLocation(p->program_id, name);
	p->uniform_count = (u8)(id + 1);
	return id;
}

void gs_gl_program_uniform1ui(const gs_gl_program* p, int index, u32 x)
{
	const GLint location = p->uniforms[index];
	if (location >= 0)
		glUniform1ui(location, x);
}

void gs_gl_program_uniform3ui(const gs_gl_program* p, int index, u32 x, u32 y, u32 z)
{
	const GLint location = p->uniforms[index];
	if (location >= 0)
		glUniform3ui(location, x, y, z);
}

void gs_gl_program_uniform4ui(const gs_gl_program* p, int index, u32 x, u32 y, u32 z, u32 w)
{
	const GLint location = p->uniforms[index];
	if (location >= 0)
		glUniform4ui(location, x, y, z, w);
}

void gs_gl_program_uniform1i(const gs_gl_program* p, int index, s32 x)
{
	const GLint location = p->uniforms[index];
	if (location >= 0)
		glUniform1i(location, x);
}

void gs_gl_program_uniform2i(const gs_gl_program* p, int index, s32 x, s32 y)
{
	const GLint location = p->uniforms[index];
	if (location >= 0)
		glUniform2i(location, x, y);
}

void gs_gl_program_uniform1f(const gs_gl_program* p, int index, float x)
{
	const GLint location = p->uniforms[index];
	if (location >= 0)
		glUniform1f(location, x);
}

void gs_gl_program_uniform2iv(const gs_gl_program* p, int index, const s32* v)
{
	const GLint location = p->uniforms[index];
	if (location >= 0)
		glUniform2iv(location, 1, v);
}

void gs_gl_program_uniform4iv(const gs_gl_program* p, int index, const s32* v)
{
	const GLint location = p->uniforms[index];
	if (location >= 0)
		glUniform4iv(location, 1, v);
}

void gs_gl_program_uniform2fv(const gs_gl_program* p, int index, const float* v)
{
	const GLint location = p->uniforms[index];
	if (location >= 0)
		glUniform2fv(location, 1, v);
}

void gs_gl_program_uniform4fv(const gs_gl_program* p, int index, const float* v)
{
	const GLint location = p->uniforms[index];
	if (location >= 0)
		glUniform4fv(location, 1, v);
}
