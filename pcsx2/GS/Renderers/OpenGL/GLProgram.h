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

#ifndef GS_GL_PROGRAM_H
#define GS_GL_PROGRAM_H

#include "common/Pcsx2Defs.h"
#include "glad.h"
#include "GS/Renderers/Common/GSArray.h"

/* The most uniforms a program registers (the present pass has nine). A
 * registration past them logs and gets the spare slot, whose location is
 * none: setting it does nothing. */
#define GS_GL_PROGRAM_MAX_UNIFORMS 15

/* A GL program and the shaders it is built from. Plain data: a copy moves
 * it, and a zeroed one is not usable until gs_gl_program_init. */
typedef struct gs_gl_program
{
	GLuint program_id;
	GLuint vertex_shader_id;
	GLuint fragment_shader_id;
	u8 pending;       /* compiling in the driver (gs_gl_program_poll) */
	u8 uniform_count;
	/* Locations in registration order; the last is the spare, always -1 */
	GLint uniforms[GS_GL_PROGRAM_MAX_UNIFORMS + 1];
} gs_gl_program;

#ifdef __cplusplus
extern "C" {
#endif

/* No program, no uniforms */
void gs_gl_program_init(gs_gl_program* p);

/* A compiled shader of type from source (length bytes), or 0. */
GLuint gs_gl_program_compile_shader(GLenum type, const char* source, size_t length);

/* Forget which program was bound last (a new context binds none) */
void gs_gl_program_reset_last(void);

/* Compiles the stages given (a zero length skips one) and attaches them to
 * a new program; gs_gl_program_link finishes it. Zero if one failed. */
int gs_gl_program_compile(gs_gl_program* p, const char* vertex_shader, size_t vertex_length,
	const char* fragment_shader, size_t fragment_length);

/* The program from a binary gs_gl_program_get_binary gave. Zero if the
 * driver did not take it. */
int gs_gl_program_create_from_binary(gs_gl_program* p, const void* data, u32 data_length, u32 data_format);

/* The program's binary into out_data (a byte array, grown as needed) and
 * its format. Zero if the driver had none. */
int gs_gl_program_get_binary(const gs_gl_program* p, gs_array_t* out_data, u32* out_data_format);
void gs_gl_program_set_binary_retrievable_hint(gs_gl_program* p);

/* Links what gs_gl_program_compile attached; the shaders go. Zero, and no
 * program, if it failed. */
int gs_gl_program_link(gs_gl_program* p);

/* Compiles and links without waiting (KHR_parallel_shader_compile):
 * gs_gl_program_poll is 0 while the driver works, 1 once the program is
 * ready, -1 if it failed (the program is then none). */
int gs_gl_program_start_compile(gs_gl_program* p, const char* vertex_shader, size_t vertex_length,
	const char* fragment_shader, size_t fragment_length);
int gs_gl_program_poll(gs_gl_program* p);

/* Makes it the current program; nothing if it already is */
void gs_gl_program_bind(const gs_gl_program* p);

/* Deletes the program and its shaders and forgets its uniforms */
void gs_gl_program_destroy(gs_gl_program* p);

/* The index the uniform called name is set by */
int gs_gl_program_register_uniform(gs_gl_program* p, const char* name);

/* Sets uniform index (as registered) when the program has it */
void gs_gl_program_uniform1ui(const gs_gl_program* p, int index, u32 x);
void gs_gl_program_uniform3ui(const gs_gl_program* p, int index, u32 x, u32 y, u32 z);
void gs_gl_program_uniform4ui(const gs_gl_program* p, int index, u32 x, u32 y, u32 z, u32 w);
void gs_gl_program_uniform1i(const gs_gl_program* p, int index, s32 x);
void gs_gl_program_uniform2i(const gs_gl_program* p, int index, s32 x, s32 y);
void gs_gl_program_uniform1f(const gs_gl_program* p, int index, float x);
void gs_gl_program_uniform2iv(const gs_gl_program* p, int index, const s32* v);
void gs_gl_program_uniform4iv(const gs_gl_program* p, int index, const s32* v);
void gs_gl_program_uniform2fv(const gs_gl_program* p, int index, const float* v);
void gs_gl_program_uniform4fv(const gs_gl_program* p, int index, const float* v);

#ifdef __cplusplus
}

/* The program as the device holds it: moved, never copied, destroyed with
 * it. */
class GLProgram
{
public:
	GLProgram() { gs_gl_program_init(&m_p); }
	GLProgram(const GLProgram&) = delete;
	GLProgram(GLProgram&& prog)
	{
		m_p = prog.m_p;
		gs_gl_program_init(&prog.m_p);
	}
	~GLProgram() { gs_gl_program_destroy(&m_p); }

	GLProgram& operator=(const GLProgram&) = delete;
	GLProgram& operator=(GLProgram&& prog)
	{
		gs_gl_program_destroy(&m_p);
		m_p = prog.m_p;
		gs_gl_program_init(&prog.m_p);
		return *this;
	}

	static void ResetLastProgram() { gs_gl_program_reset_last(); }

	bool Compile(const char* vs, size_t vs_length, const char* fs, size_t fs_length) { return gs_gl_program_compile(&m_p, vs, vs_length, fs, fs_length) != 0; }
	bool CreateFromBinary(const void* data, u32 data_length, u32 data_format) { return gs_gl_program_create_from_binary(&m_p, data, data_length, data_format) != 0; }
	bool GetBinary(gs_array_t* out_data, u32* out_data_format) const { return gs_gl_program_get_binary(&m_p, out_data, out_data_format) != 0; }
	void SetBinaryRetrievableHint() { gs_gl_program_set_binary_retrievable_hint(&m_p); }
	bool Link() { return gs_gl_program_link(&m_p) != 0; }
	bool StartCompile(const char* vs, size_t vs_length, const char* fs, size_t fs_length) { return gs_gl_program_start_compile(&m_p, vs, vs_length, fs, fs_length) != 0; }
	int Poll() { return gs_gl_program_poll(&m_p); }
	bool IsPending() const { return m_p.pending != 0; }
	bool IsValid() const { return m_p.program_id != 0; }
	void Bind() const { gs_gl_program_bind(&m_p); }
	void Destroy() { gs_gl_program_destroy(&m_p); }

	int RegisterUniform(const char* name) { return gs_gl_program_register_uniform(&m_p, name); }
	void Uniform1ui(int index, u32 x) const { gs_gl_program_uniform1ui(&m_p, index, x); }
	void Uniform3ui(int index, u32 x, u32 y, u32 z) const { gs_gl_program_uniform3ui(&m_p, index, x, y, z); }
	void Uniform4ui(int index, u32 x, u32 y, u32 z, u32 w) const { gs_gl_program_uniform4ui(&m_p, index, x, y, z, w); }
	void Uniform1i(int index, s32 x) const { gs_gl_program_uniform1i(&m_p, index, x); }
	void Uniform2i(int index, s32 x, s32 y) const { gs_gl_program_uniform2i(&m_p, index, x, y); }
	void Uniform1f(int index, float x) const { gs_gl_program_uniform1f(&m_p, index, x); }
	void Uniform2iv(int index, const s32* v) const { gs_gl_program_uniform2iv(&m_p, index, v); }
	void Uniform4iv(int index, const s32* v) const { gs_gl_program_uniform4iv(&m_p, index, v); }
	void Uniform2fv(int index, const float* v) const { gs_gl_program_uniform2fv(&m_p, index, v); }
	void Uniform4fv(int index, const float* v) const { gs_gl_program_uniform4fv(&m_p, index, v); }

	bool operator==(const GLProgram& rhs) const { return m_p.program_id == rhs.m_p.program_id; }
	bool operator!=(const GLProgram& rhs) const { return m_p.program_id != rhs.m_p.program_id; }

private:
	gs_gl_program m_p;
};
#endif

#endif
