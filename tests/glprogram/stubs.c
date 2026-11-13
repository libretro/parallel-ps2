/* A GL driver for the program tests: every entry point GLProgram.c calls
 * is a function here that folds its name and arguments into one trace
 * hash, and answers queries from a seeded generator of its own, so a run
 * is a fixed sequence of calls that one number stands for. The messages
 * logged go into the trace too. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "glad.h"
#include "common/Pcsx2Defs.h"
#include "stubs.h"

u32 g_trace = 2166136261u;
u32 g_calls = 0;
u32 g_program_deletes = 0;
int g_echo  = 0;
static u32 s_rng = 12345u;
static GLuint s_next_id = 0;

void tr(u32 v)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		g_trace ^= (v >> (i * 8)) & 0xFF;
		g_trace *= 16777619u;
	}
}

static void tr_bytes(const void* data, size_t n)
{
	const unsigned char* b = (const unsigned char*)data;
	size_t i;
	tr((u32)n);
	for (i = 0; i < n; i++)
	{
		g_trace ^= b[i];
		g_trace *= 16777619u;
	}
}

static void tr_str(const char* s)
{
	tr_bytes(s, strlen(s));
}

static void tr_f(float f)
{
	u32 v;
	memcpy(&v, &f, sizeof(v));
	tr(v);
}

static void call(u32 fn)
{
	g_calls++;
	tr(0xC0DE0000u | fn);
}

static u32 drv(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

void stubs_reset(void)
{
	g_trace   = 2166136261u;
	g_calls   = 0;
	s_rng     = 12345u;
	s_next_id = 0;
}

static void RETRO_CALLCONV test_log(enum retro_log_level level, const char* fmt, ...)
{
	char buf[512];
	size_t n;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	/* (paths read alike on every host) */
	for (n = 0; buf[n]; n++)
		if (buf[n] == '\\')
			buf[n] = '/';
	tr(0x106u);
	tr((u32)level);
	tr_str(buf);
	if (g_echo)
		fputs(buf, stdout);
}
retro_log_printf_t log_cb = test_log;

static GLint log_length(void)
{
	static const GLint lengths[4] = {0, 1, 2, 17};
	return lengths[drv() & 3];
}

static GLuint APIENTRY s_CreateShader(GLenum type)
{
	call(1);
	tr(type);
	return ++s_next_id;
}

static void APIENTRY s_ShaderSource(GLuint id, GLsizei count, const GLchar* const* srcs, const GLint* lens)
{
	call(2);
	tr(id);
	tr((u32)count);
	tr_bytes(srcs[0], (size_t)lens[0]);
}

static void APIENTRY s_CompileShader(GLuint id)
{
	call(3);
	tr(id);
}

static void APIENTRY s_GetShaderiv(GLuint id, GLenum pname, GLint* out)
{
	call(4);
	tr(id);
	tr(pname);
	if (pname == GL_COMPILE_STATUS)
		*out = (drv() & 3) ? GL_TRUE : GL_FALSE;
	else if (pname == GL_INFO_LOG_LENGTH)
		*out = log_length();
	else
		*out = 0;
}

static void info_log(GLuint id, GLsizei size, GLsizei* length, GLchar* buf)
{
	GLsizei i;
	tr(id);
	tr((u32)size);
	for (i = 0; i + 1 < size; i++)
		buf[i] = (GLchar)('a' + (id + (GLuint)i) % 26);
	if (size > 0)
		buf[size - 1] = 0;
	*length = size > 0 ? size - 1 : 0;
}

static void APIENTRY s_GetShaderInfoLog(GLuint id, GLsizei size, GLsizei* length, GLchar* buf)
{
	call(5);
	info_log(id, size, length, buf);
}

static void APIENTRY s_GetProgramInfoLog(GLuint id, GLsizei size, GLsizei* length, GLchar* buf)
{
	call(6);
	info_log(id, size, length, buf);
}

static void APIENTRY s_GetProgramiv(GLuint id, GLenum pname, GLint* out)
{
	static const GLint binary_lengths[4] = {0, 64, 100, 100};
	call(7);
	tr(id);
	tr(pname);
	if (pname == GL_LINK_STATUS)
		*out = (drv() & 3) ? GL_TRUE : GL_FALSE;
	else if (pname == GL_COMPLETION_STATUS_KHR)
		*out = (drv() % 3) ? GL_TRUE : GL_FALSE;
	else if (pname == GL_PROGRAM_BINARY_LENGTH)
		*out = binary_lengths[drv() & 3];
	else if (pname == GL_INFO_LOG_LENGTH)
		*out = log_length();
	else
		*out = 0;
}

static void APIENTRY s_DeleteShader(GLuint id)
{
	call(8);
	tr(id);
}

static GLuint APIENTRY s_CreateProgram(void)
{
	call(9);
	return ++s_next_id;
}

static void APIENTRY s_AttachShader(GLuint prog, GLuint shader)
{
	call(10);
	tr(prog);
	tr(shader);
}

static void APIENTRY s_ProgramBinary(GLuint prog, GLenum format, const void* data, GLsizei length)
{
	call(11);
	tr(prog);
	tr(format);
	tr_bytes(data, (size_t)length);
}

static void APIENTRY s_DeleteProgram(GLuint id)
{
	call(12);
	tr(id);
	g_program_deletes++;
}

static void APIENTRY s_GetProgramBinary(GLuint prog, GLsizei size, GLsizei* length, GLenum* format, void* data)
{
	GLsizei i, n;
	u32 mode = drv() & 3;
	call(13);
	tr(prog);
	tr((u32)size);
	n = mode == 0 ? 0 : mode == 1 ? size - 8 : size;
	for (i = 0; i < n; i++)
		((unsigned char*)data)[i] = (unsigned char)(prog * 7 + (GLuint)i);
	*length = n;
	*format = 0x1234 + prog;
}

static void APIENTRY s_ProgramParameteri(GLuint prog, GLenum pname, GLint value)
{
	call(14);
	tr(prog);
	tr(pname);
	tr((u32)value);
}

static void APIENTRY s_LinkProgram(GLuint prog)
{
	call(15);
	tr(prog);
}

static void APIENTRY s_UseProgram(GLuint prog)
{
	call(16);
	tr(prog);
}

static GLint APIENTRY s_GetUniformLocation(GLuint prog, const GLchar* name)
{
	call(17);
	tr(prog);
	tr_str(name);
	return (drv() % 5) == 0 ? -1 : (GLint)(drv() & 63);
}

static void APIENTRY s_Uniform1ui(GLint l, GLuint x) { call(20); tr((u32)l); tr(x); }
static void APIENTRY s_Uniform3ui(GLint l, GLuint x, GLuint y, GLuint z) { call(21); tr((u32)l); tr(x); tr(y); tr(z); }
static void APIENTRY s_Uniform4ui(GLint l, GLuint x, GLuint y, GLuint z, GLuint w) { call(22); tr((u32)l); tr(x); tr(y); tr(z); tr(w); }
static void APIENTRY s_Uniform1i(GLint l, GLint x) { call(23); tr((u32)l); tr((u32)x); }
static void APIENTRY s_Uniform2i(GLint l, GLint x, GLint y) { call(24); tr((u32)l); tr((u32)x); tr((u32)y); }
static void APIENTRY s_Uniform1f(GLint l, GLfloat x) { call(25); tr((u32)l); tr_f(x); }
static void APIENTRY s_Uniform2iv(GLint l, GLsizei n, const GLint* v) { call(26); tr((u32)l); tr((u32)n); tr_bytes(v, 2 * sizeof(*v)); }
static void APIENTRY s_Uniform4iv(GLint l, GLsizei n, const GLint* v) { call(27); tr((u32)l); tr((u32)n); tr_bytes(v, 4 * sizeof(*v)); }
static void APIENTRY s_Uniform2fv(GLint l, GLsizei n, const GLfloat* v) { call(28); tr((u32)l); tr((u32)n); tr_bytes(v, 2 * sizeof(*v)); }
static void APIENTRY s_Uniform4fv(GLint l, GLsizei n, const GLfloat* v) { call(29); tr((u32)l); tr((u32)n); tr_bytes(v, 4 * sizeof(*v)); }

PFNGLCREATESHADERPROC glad_glCreateShader             = s_CreateShader;
PFNGLSHADERSOURCEPROC glad_glShaderSource             = s_ShaderSource;
PFNGLCOMPILESHADERPROC glad_glCompileShader           = s_CompileShader;
PFNGLGETSHADERIVPROC glad_glGetShaderiv               = s_GetShaderiv;
PFNGLGETSHADERINFOLOGPROC glad_glGetShaderInfoLog     = s_GetShaderInfoLog;
PFNGLGETPROGRAMINFOLOGPROC glad_glGetProgramInfoLog   = s_GetProgramInfoLog;
PFNGLGETPROGRAMIVPROC glad_glGetProgramiv             = s_GetProgramiv;
PFNGLDELETESHADERPROC glad_glDeleteShader             = s_DeleteShader;
PFNGLCREATEPROGRAMPROC glad_glCreateProgram           = s_CreateProgram;
PFNGLATTACHSHADERPROC glad_glAttachShader             = s_AttachShader;
PFNGLPROGRAMBINARYPROC glad_glProgramBinary           = s_ProgramBinary;
PFNGLDELETEPROGRAMPROC glad_glDeleteProgram           = s_DeleteProgram;
PFNGLGETPROGRAMBINARYPROC glad_glGetProgramBinary     = s_GetProgramBinary;
PFNGLPROGRAMPARAMETERIPROC glad_glProgramParameteri   = s_ProgramParameteri;
PFNGLLINKPROGRAMPROC glad_glLinkProgram               = s_LinkProgram;
PFNGLUSEPROGRAMPROC glad_glUseProgram                 = s_UseProgram;
PFNGLGETUNIFORMLOCATIONPROC glad_glGetUniformLocation = s_GetUniformLocation;
PFNGLUNIFORM1UIPROC glad_glUniform1ui                 = s_Uniform1ui;
PFNGLUNIFORM3UIPROC glad_glUniform3ui                 = s_Uniform3ui;
PFNGLUNIFORM4UIPROC glad_glUniform4ui                 = s_Uniform4ui;
PFNGLUNIFORM1IPROC glad_glUniform1i                   = s_Uniform1i;
PFNGLUNIFORM2IPROC glad_glUniform2i                   = s_Uniform2i;
PFNGLUNIFORM1FPROC glad_glUniform1f                   = s_Uniform1f;
PFNGLUNIFORM2IVPROC glad_glUniform2iv                 = s_Uniform2iv;
PFNGLUNIFORM4IVPROC glad_glUniform4iv                 = s_Uniform4iv;
PFNGLUNIFORM2FVPROC glad_glUniform2fv                 = s_Uniform2fv;
PFNGLUNIFORM4FVPROC glad_glUniform4fv                 = s_Uniform4fv;
