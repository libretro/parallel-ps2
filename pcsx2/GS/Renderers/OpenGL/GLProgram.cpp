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

#include "common/Pcsx2Defs.h"
#include <stdlib.h>

#include "GLProgram.h"

static u32 s_last_program_id = 0;

GLProgram::GLProgram()
{
	gs_array_init(&m_uniform_locations, sizeof(GLint));
}

GLProgram::GLProgram(GLProgram&& prog)
{
	m_program_id              = prog.m_program_id;
	prog.m_program_id         = 0;
	m_pending                 = prog.m_pending;
	prog.m_pending            = false;
	m_vertex_shader_id        = prog.m_vertex_shader_id;
	prog.m_vertex_shader_id   = 0;
	m_fragment_shader_id      = prog.m_fragment_shader_id;
	prog.m_fragment_shader_id = 0;
	m_uniform_locations       = prog.m_uniform_locations;
	gs_array_init(&prog.m_uniform_locations, sizeof(GLint));
}

GLProgram::~GLProgram()
{
	Destroy();
	gs_array_free(&m_uniform_locations);
}

/* The info log of shader or program object id, malloc'd, or NULL when
 * there is none worth printing. */
static char* GLProgramInfoLog(GLuint id, bool program)
{
	GLint info_log_length = 0;
	char* info_log;
	if (program)
		glGetProgramiv(id, GL_INFO_LOG_LENGTH, &info_log_length);
	else
		glGetShaderiv(id, GL_INFO_LOG_LENGTH, &info_log_length);
	// Log will create a new line when there are no warnings so let's set a minimum log length of 1.
	if (info_log_length <= 1)
		return nullptr;
	info_log = static_cast<char*>(malloc(static_cast<size_t>(info_log_length) + 1));
	if (!info_log)
		return nullptr;
	if (program)
		glGetProgramInfoLog(id, info_log_length, &info_log_length, info_log);
	else
		glGetShaderInfoLog(id, info_log_length, &info_log_length, info_log);
	info_log[info_log_length < 0 ? 0 : info_log_length] = 0;
	return info_log;
}

GLuint GLProgram::CompileShader(GLenum type, const char* source, size_t length)
{
	GLuint id = glCreateShader(type);

	const GLchar* sources[1] = {source};
	GLint source_lengths[1] = {static_cast<GLint>(length)};
	glShaderSource(id, 1, sources, source_lengths);
	glCompileShader(id);

	GLint status = GL_FALSE;
	glGetShaderiv(id, GL_COMPILE_STATUS, &status);

	char* info_log = GLProgramInfoLog(id, false);
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

void GLProgram::ResetLastProgram()
{
	s_last_program_id = 0;
}

bool GLProgram::Compile(const char* vertex_shader, size_t vertex_length, const char* fragment_shader, size_t fragment_length)
{
	if (vertex_length)
	{
		m_vertex_shader_id = CompileShader(GL_VERTEX_SHADER, vertex_shader, vertex_length);
		if (m_vertex_shader_id == 0)
			return false;
	}

	if (fragment_length)
	{
		m_fragment_shader_id = CompileShader(GL_FRAGMENT_SHADER, fragment_shader, fragment_length);
		if (m_fragment_shader_id == 0)
			return false;
	}

	m_program_id = glCreateProgram();
	if (m_vertex_shader_id != 0)
		glAttachShader(m_program_id, m_vertex_shader_id);
	if (m_fragment_shader_id != 0)
		glAttachShader(m_program_id, m_fragment_shader_id);
	return true;
}

bool GLProgram::CreateFromBinary(const void* data, u32 data_length, u32 data_format)
{
	GLuint prog = glCreateProgram();
	glProgramBinary(prog, static_cast<GLenum>(data_format), data, data_length);

	GLint link_status;
	glGetProgramiv(prog, GL_LINK_STATUS, &link_status);
	if (link_status != GL_TRUE)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create GL program from binary: status %d\n", link_status);
		glDeleteProgram(prog);
		return false;
	}

	m_program_id = prog;
	return true;
}

/* The program's binary, into out_data (a byte array, grown as needed). */
bool GLProgram::GetBinary(gs_array_t* out_data, u32* out_data_format)
{
	GLint binary_size = 0;
	glGetProgramiv(m_program_id, GL_PROGRAM_BINARY_LENGTH, &binary_size);
	if (binary_size == 0)
	{
		log_cb(RETRO_LOG_WARN, "glGetProgramiv(GL_PROGRAM_BINARY_LENGTH) returned 0\n");
		return false;
	}

	GLenum format = 0;
	if (!gs_array_reserve(out_data, static_cast<size_t>(binary_size)))
		return false;
	out_data->count = static_cast<size_t>(binary_size);
	glGetProgramBinary(m_program_id, binary_size, &binary_size, &format, out_data->items);
	if (binary_size == 0)
	{
		log_cb(RETRO_LOG_WARN, "glGetProgramBinary() failed\n");
		return false;
	}
	else if (static_cast<size_t>(binary_size) != out_data->count)
	{
		/* (never more than asked for: the driver got that much room) */
		log_cb(RETRO_LOG_WARN, "Size changed from %zu to %d after glGetProgramBinary()\n", out_data->count, binary_size);
		out_data->count = static_cast<size_t>(binary_size);
	}

	*out_data_format = static_cast<u32>(format);
	return true;
}

void GLProgram::SetBinaryRetrievableHint()
{
	glProgramParameteri(m_program_id, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
}

void GLProgram::BindAttribute(GLuint index, const char* name)
{
	glBindAttribLocation(m_program_id, index, name);
}

void GLProgram::BindDefaultAttributes()
{
	BindAttribute(0, "a_position");
	BindAttribute(1, "a_texcoord");
	BindAttribute(2, "a_color");
}

void GLProgram::BindFragData(GLuint index, const char* name)
{
	glBindFragDataLocation(m_program_id, index, name);
}

void GLProgram::BindFragDataIndexed(GLuint color_number, const char* name)
{
	if (GLAD_GL_VERSION_3_3 || GLAD_GL_ARB_blend_func_extended)
	{
		glBindFragDataLocationIndexed(m_program_id, color_number, 0, name);
		return;
	}
	else if (GLAD_GL_EXT_blend_func_extended)
	{
		glBindFragDataLocationIndexedEXT(m_program_id, color_number, 0, name);
		return;
	}

	log_cb(RETRO_LOG_ERROR, "BindFragDataIndexed() called without ARB or EXT extension, we'll probably crash.\n");
	glBindFragDataLocationIndexed(m_program_id, color_number, 0, name);
}

bool GLProgram::StartCompile(const char* vertex_shader, size_t vertex_length, const char* fragment_shader, size_t fragment_length)
{
	const GLchar* src;
	GLint len;
	m_vertex_shader_id   = glCreateShader(GL_VERTEX_SHADER);
	m_fragment_shader_id = glCreateShader(GL_FRAGMENT_SHADER);
	src = vertex_shader;
	len = static_cast<GLint>(vertex_length);
	glShaderSource(m_vertex_shader_id, 1, &src, &len);
	glCompileShader(m_vertex_shader_id);
	src = fragment_shader;
	len = static_cast<GLint>(fragment_length);
	glShaderSource(m_fragment_shader_id, 1, &src, &len);
	glCompileShader(m_fragment_shader_id);
	m_program_id = glCreateProgram();
	glAttachShader(m_program_id, m_vertex_shader_id);
	glAttachShader(m_program_id, m_fragment_shader_id);
	glLinkProgram(m_program_id);
	m_pending = true;
	return true;
}

int GLProgram::Poll()
{
	GLint done = GL_FALSE, status = GL_FALSE;
	if (!m_pending)
		return m_program_id ? 1 : -1;
	glGetProgramiv(m_program_id, GL_COMPLETION_STATUS_KHR, &done);
	if (!done)
		return 0;
	m_pending = false;
	glGetProgramiv(m_program_id, GL_LINK_STATUS, &status);
	if (status != GL_TRUE)
	{
		char* info_log = GLProgramInfoLog(m_vertex_shader_id, false);
		char* fs_log   = GLProgramInfoLog(m_fragment_shader_id, false);
		char* pr_log   = GLProgramInfoLog(m_program_id, true);
		log_cb(RETRO_LOG_ERROR, "Program failed to compile or link:\n%s%s%s\n", info_log ? info_log : "",
			fs_log ? fs_log : "", pr_log ? pr_log : "");
		free(info_log);
		free(fs_log);
		free(pr_log);
		Destroy();
		return -1;
	}
	glDeleteShader(m_vertex_shader_id);
	glDeleteShader(m_fragment_shader_id);
	m_vertex_shader_id = m_fragment_shader_id = 0;
	return 1;
}

bool GLProgram::Link()
{
	glLinkProgram(m_program_id);

	if (m_vertex_shader_id != 0)
		glDeleteShader(m_vertex_shader_id);
	m_vertex_shader_id = 0;
	if (m_fragment_shader_id != 0)
		glDeleteShader(m_fragment_shader_id);
	m_fragment_shader_id = 0;

	GLint status = GL_FALSE;
	glGetProgramiv(m_program_id, GL_LINK_STATUS, &status);

	char* info_log = GLProgramInfoLog(m_program_id, true);
	if (status == GL_FALSE || info_log)
	{
		if (status == GL_TRUE)
			log_cb(RETRO_LOG_ERROR, "Program linked with warnings:\n%s\n", info_log);
		else
			log_cb(RETRO_LOG_ERROR, "Program failed to link:\n%s\n", info_log ? info_log : "");
		free(info_log);
		if (status == GL_FALSE)
		{
			glDeleteProgram(m_program_id);
			m_program_id = 0;
			return false;
		}
	}

	return true;
}

void GLProgram::Bind() const
{
	if (s_last_program_id == m_program_id)
		return;

	glUseProgram(m_program_id);
	s_last_program_id = m_program_id;
}

void GLProgram::Destroy()
{
	if (m_vertex_shader_id != 0)
	{
		glDeleteShader(m_vertex_shader_id);
		m_vertex_shader_id = 0;
	}
	if (m_fragment_shader_id != 0)
	{
		glDeleteShader(m_fragment_shader_id);
		m_fragment_shader_id = 0;
	}
	if (m_program_id != 0)
	{
		glDeleteProgram(m_program_id);
		m_program_id = 0;
	}

	m_uniform_locations.count = 0;
	m_pending = false;
}

int GLProgram::RegisterUniform(const char* name)
{
	const int id = static_cast<int>(m_uniform_locations.count);
	GLint* slot = static_cast<GLint*>(gs_array_push(&m_uniform_locations));
	if (!slot)
		abort();
	*slot = glGetUniformLocation(m_program_id, name);
	return id;
}

void GLProgram::Uniform1ui(int index, u32 x) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform1ui(location, x);
}

void GLProgram::Uniform2ui(int index, u32 x, u32 y) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform2ui(location, x, y);
}

void GLProgram::Uniform3ui(int index, u32 x, u32 y, u32 z) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform3ui(location, x, y, z);
}

void GLProgram::Uniform4ui(int index, u32 x, u32 y, u32 z, u32 w) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform4ui(location, x, y, z, w);
}

void GLProgram::Uniform1i(int index, s32 x) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform1i(location, x);
}

void GLProgram::Uniform2i(int index, s32 x, s32 y) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform2i(location, x, y);
}

void GLProgram::Uniform3i(int index, s32 x, s32 y, s32 z) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform3i(location, x, y, z);
}

void GLProgram::Uniform4i(int index, s32 x, s32 y, s32 z, s32 w) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform4i(location, x, y, z, w);
}

void GLProgram::Uniform1f(int index, float x) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform1f(location, x);
}

void GLProgram::Uniform2f(int index, float x, float y) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform2f(location, x, y);
}

void GLProgram::Uniform3f(int index, float x, float y, float z) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform3f(location, x, y, z);
}

void GLProgram::Uniform4f(int index, float x, float y, float z, float w) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform4f(location, x, y, z, w);
}

void GLProgram::Uniform2uiv(int index, const u32* v) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform2uiv(location, 1, v);
}

void GLProgram::Uniform3uiv(int index, const u32* v) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform3uiv(location, 1, v);
}

void GLProgram::Uniform4uiv(int index, const u32* v) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform4uiv(location, 1, v);
}

void GLProgram::Uniform2iv(int index, const s32* v) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform2iv(location, 1, v);
}

void GLProgram::Uniform3iv(int index, const s32* v) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform3iv(location, 1, v);
}

void GLProgram::Uniform4iv(int index, const s32* v) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform4iv(location, 1, v);
}

void GLProgram::Uniform2fv(int index, const float* v) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform2fv(location, 1, v);
}

void GLProgram::Uniform3fv(int index, const float* v) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform3fv(location, 1, v);
}

void GLProgram::Uniform4fv(int index, const float* v) const
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniform4fv(location, 1, v);
}

void GLProgram::UniformMatrix2fv(int index, const float* v)
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniformMatrix2fv(location, 1, GL_FALSE, v);
}

void GLProgram::UniformMatrix3fv(int index, const float* v)
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniformMatrix3fv(location, 1, GL_FALSE, v);
}

void GLProgram::UniformMatrix4fv(int index, const float* v)
{
	const GLint location = GS_ARRAY_AT(&m_uniform_locations, GLint, index);
	if (location >= 0)
		glUniformMatrix4fv(location, 1, GL_FALSE, v);
}

void GLProgram::BindUniformBlock(const char* name, u32 index)
{
	const GLint location = glGetUniformBlockIndex(m_program_id, name);
	if (location >= 0)
		glUniformBlockBinding(m_program_id, location, index);
}

GLProgram& GLProgram::operator=(GLProgram&& prog)
{
	Destroy();
	m_program_id = prog.m_program_id;
	prog.m_program_id = 0;
	m_pending = prog.m_pending;
	prog.m_pending = false;
	m_vertex_shader_id = prog.m_vertex_shader_id;
	prog.m_vertex_shader_id = 0;
	m_fragment_shader_id = prog.m_fragment_shader_id;
	prog.m_fragment_shader_id = 0;
	gs_array_free(&m_uniform_locations);
	m_uniform_locations = prog.m_uniform_locations;
	gs_array_init(&prog.m_uniform_locations, sizeof(GLint));
	return *this;
}
