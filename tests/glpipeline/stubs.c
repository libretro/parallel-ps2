/* A GL driver for the pipeline test: each entry point the pipeline state
 * calls logs its name and arguments, the log lines too. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <libretro.h>

#include "glad.h"
#include "stubs.h"

u32 g_log[STUB_LOG_MAX];
u32 g_log_len = 0;
static GLuint s_next_sampler = 0;

static void w(u32 v)
{
	if (g_log_len < STUB_LOG_MAX)
		g_log[g_log_len] = v;
	g_log_len++;
}

static void wf(float f)
{
	u32 v;
	memcpy(&v, &f, sizeof(v));
	w(v);
}

void stubs_reset(void)
{
	g_log_len      = 0;
	s_next_sampler = 0;
}

static void test_log(enum retro_log_level level, const char* fmt, ...)
{
	char buf[512];
	size_t i;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	w(0x1000u | (u32)level);
	for (i = 0; buf[i]; i++)
		w((u8)buf[i]);
}
retro_log_printf_t log_cb = test_log;

static void APIENTRY s_Enable(GLenum c) { w(1); w(c); }
static void APIENTRY s_Disable(GLenum c) { w(2); w(c); }
static void APIENTRY s_DepthFunc(GLenum f) { w(3); w(f); }
static void APIENTRY s_DepthMask(GLboolean m) { w(4); w(m); }
static void APIENTRY s_StencilFunc(GLenum f, GLint r, GLuint m) { w(5); w(f); w((u32)r); w(m); }
static void APIENTRY s_StencilOp(GLenum a, GLenum b, GLenum c) { w(6); w(a); w(b); w(c); }
static void APIENTRY s_CreateSamplers(GLsizei n, GLuint* s)
{
	GLsizei i;
	w(7);
	w((u32)n);
	for (i = 0; i < n; i++)
		s[i] = ++s_next_sampler;
}
static void APIENTRY s_SamplerParameteri(GLuint s, GLenum p, GLint v) { w(8); w(s); w(p); w((u32)v); }
static void APIENTRY s_SamplerParameterf(GLuint s, GLenum p, GLfloat v) { w(9); w(s); w(p); wf(v); }
static void APIENTRY s_BlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { w(10); wf(r); wf(g); wf(b); wf(a); }
static void APIENTRY s_BlendEquationSeparate(GLenum c, GLenum a) { w(11); w(c); w(a); }
static void APIENTRY s_BlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { w(12); w(a); w(b); w(c); w(d); }
static void APIENTRY s_ColorMaski(GLuint i, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	w(13); w(i); w(r); w(g); w(b); w(a);
}

PFNGLENABLEPROC glad_glEnable                                 = s_Enable;
PFNGLDISABLEPROC glad_glDisable                               = s_Disable;
PFNGLDEPTHFUNCPROC glad_glDepthFunc                           = s_DepthFunc;
PFNGLDEPTHMASKPROC glad_glDepthMask                           = s_DepthMask;
PFNGLSTENCILFUNCPROC glad_glStencilFunc                       = s_StencilFunc;
PFNGLSTENCILOPPROC glad_glStencilOp                           = s_StencilOp;
PFNGLCREATESAMPLERSPROC glad_glCreateSamplers                 = s_CreateSamplers;
PFNGLSAMPLERPARAMETERIPROC glad_glSamplerParameteri           = s_SamplerParameteri;
PFNGLSAMPLERPARAMETERFPROC glad_glSamplerParameterf           = s_SamplerParameterf;
PFNGLBLENDCOLORPROC glad_glBlendColor                         = s_BlendColor;
PFNGLBLENDEQUATIONSEPARATEPROC glad_glBlendEquationSeparate   = s_BlendEquationSeparate;
PFNGLBLENDFUNCSEPARATEPROC glad_glBlendFuncSeparate           = s_BlendFuncSeparate;
PFNGLCOLORMASKIPROC glad_glColorMaski                         = s_ColorMaski;
int GLAD_GL_ARB_texture_filter_anisotropic                    = 0;
int GLAD_GL_EXT_texture_filter_anisotropic                    = 0;
