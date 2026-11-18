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

#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <memalign.h>

#include "common/Pcsx2Defs.h"
#include "glad.h"

#include "Config.h"
#include "GS/GSBackend.h"
#include "GS/Renderers/Common/GSKeyHash.h"
#include "GLContext.h"
#include "GLDevice.h"
#include "GLShaderSource.h"
#include "GLState.h"
#include "GSDeviceOGL.h"
#include "GSTextureOGL.h"

#include "convert.glsl"
#include "vr.glsl"
#include "interlace.glsl"
#include "present.glsl"
#include "merge.glsl"
#include "tfx_fs.glsl"
#include "tfx_vs.glsl"

#define GL_DEFAULT_FRAMEBUFFER hw_render.get_current_framebuffer()

#define GL_VS_CB_INDEX 1
#define GL_PS_CB_INDEX 0

#define VERTEX_BUFFER_SIZE (32 * 1024 * 1024)
#define INDEX_BUFFER_SIZE (16 * 1024 * 1024)
#define VERTEX_UNIFORM_BUFFER_SIZE (8 * 1024 * 1024)
#define FRAGMENT_UNIFORM_BUFFER_SIZE (8 * 1024 * 1024)
#define TEXTURE_UPLOAD_BUFFER_SIZE (128 * 1024 * 1024)
/* Draw programs the cache holds before it starts over: more than a game
 * makes in a session; 640 KiB of programs and 512 KiB of slots */
#define PROGRAM_CACHE_SIZE 8192

#define GL_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* The vertex of the utility passes (C++: GSVertexPT1) */
typedef struct PCSX2_ALIGN(32) gs_gl_vertex_pt1
{
	float p[4];
	float t[2];
	char pad[4];
	u32 c;
} gs_gl_vertex_pt1;

typedef char gs_gl_vertex_pt1_size[(sizeof(gs_gl_vertex_pt1) == GS_GL_VERTEX_PT1_SIZE) ? 1 : -1];
typedef char gs_gl_program_selector_size[(sizeof(gs_gl_program_selector) == 16) ? 1 : -1];
typedef char gs_gl_expand_buffer_size[(GS_DEVICE_EXPAND_BUFFER_SIZE == GS_GL_EXPAND_QUADS * 6 * 2) ? 1 : -1];

#define GL_DEV(d) ((gs_device_gl*)(d))
#define GL_TEX(t) ((gs_texture_gl*)(t))
#define GL_TEX_ID(t) ((t) ? GL_TEX(t)->id : 0u)

/* ====================================================================
 * Entry points a context lacks
 * ==================================================================== */

static void APIENTRY gl_replace_scissor_indexed(GLuint index, GLint left, GLint bottom, GLsizei width, GLsizei height)
{
	(void)index;
	glScissor(left, bottom, width, height);
}

static void APIENTRY gl_replace_viewport_indexedf(GLuint index, GLfloat x, GLfloat y, GLfloat w, GLfloat h)
{
	(void)index;
	glViewport((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h);
}

static void APIENTRY gl_replace_texture_barrier(void)
{
}

/* Only colour attachment 0 is ever masked. */
static void APIENTRY gl_replace_color_maski(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	(void)index;
	glColorMask(r, g, b, a);
}

/* Whether the context advertises an extension GLAD has no flag for */
static bool gl_has_extension(const char* name)
{
	GLint count = 0;
	GLint i;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (i = 0; i < count; i++)
	{
		const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		if (ext && !strcmp(ext, name))
			return true;
	}
	return false;
}

/* Direct state access, where the context has none: through texture unit 7 */
static void APIENTRY gl_dsa_bind_texture_unit(GLuint unit, GLuint texture)
{
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, texture);
}

static void APIENTRY gl_dsa_create_textures(GLenum target, GLsizei n, GLuint* textures)
{
	(void)target;
	(void)n;
	glGenTextures(1, textures);
}

static void APIENTRY gl_dsa_texture_storage(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width,
	GLsizei height)
{
	gl_dsa_bind_texture_unit(7, texture);
	glTexStorage2D(GL_TEXTURE_2D, levels, internalformat, width, height);
}

static void APIENTRY gl_dsa_texture_sub_image(GLuint texture, GLint level, GLint xoffset, GLint yoffset,
	GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)
{
	gl_dsa_bind_texture_unit(7, texture);
	glTexSubImage2D(GL_TEXTURE_2D, level, xoffset, yoffset, width, height, format, type, pixels);
}

static void APIENTRY gl_dsa_compressed_texture_sub_image(GLuint texture, GLint level, GLint xoffset, GLint yoffset,
	GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* data)
{
	gl_dsa_bind_texture_unit(7, texture);
	glCompressedTexSubImage2D(GL_TEXTURE_2D, level, xoffset, yoffset, width, height, format, imageSize, data);
}

static void APIENTRY gl_dsa_get_texture_image(GLuint texture, GLint level, GLenum format, GLenum type,
	GLsizei bufSize, void* pixels)
{
	(void)bufSize;
	gl_dsa_bind_texture_unit(7, texture);
	glGetTexImage(GL_TEXTURE_2D, level, format, type, pixels);
}

static void APIENTRY gl_dsa_texture_parameteri(GLuint texture, GLenum pname, GLint param)
{
	gl_dsa_bind_texture_unit(7, texture);
	glTexParameteri(GL_TEXTURE_2D, pname, param);
}

static void APIENTRY gl_dsa_generate_texture_mipmap(GLuint texture)
{
	gl_dsa_bind_texture_unit(7, texture);
	glGenerateMipmap(GL_TEXTURE_2D);
}

static void APIENTRY gl_dsa_create_samplers(GLsizei n, GLuint* samplers)
{
	glGenSamplers(n, samplers);
}

static void gl_emulate_dsa(void)
{
	glBindTextureUnit             = gl_dsa_bind_texture_unit;
	glCreateTextures              = gl_dsa_create_textures;
	glTextureStorage2D            = gl_dsa_texture_storage;
	glTextureSubImage2D           = gl_dsa_texture_sub_image;
	glCompressedTextureSubImage2D = gl_dsa_compressed_texture_sub_image;
	glGetTextureImage             = gl_dsa_get_texture_image;
	glTextureParameteri           = gl_dsa_texture_parameteri;
	glGenerateTextureMipmap       = gl_dsa_generate_texture_mipmap;
	glCreateSamplers              = gl_dsa_create_samplers;
}

/* ====================================================================
 * The state cache
 * ==================================================================== */

static void gl_set_fbo(gs_device_gl* dev, GLuint fbo)
{
	(void)dev;
	if (g_gl_state.fbo != fbo)
	{
		g_gl_state.fbo = fbo;
		if (fbo)
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
		else
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GL_DEFAULT_FRAMEBUFFER);
	}
}

static void gl_attach_rt(gs_device_gl* dev, gs_texture* rt)
{
	(void)dev;
	if (g_gl_state.rt == (void*)rt)
		return;
	g_gl_state.rt = rt;
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, GL_TEX_ID(rt), 0);
}

static GLenum gl_ds_attachment(const gs_device_gl* dev)
{
	return dev->base.m_features.framebuffer_fetch ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT;
}

static void gl_attach_ds(gs_device_gl* dev, gs_texture* ds)
{
	if (g_gl_state.ds == (void*)ds)
		return;
	g_gl_state.ds = ds;
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, gl_ds_attachment(dev), GL_TEXTURE_2D, GL_TEX_ID(ds), 0);
}

static void gl_set_viewport(int w, int h)
{
	if (g_gl_state.viewport[0] != w || g_gl_state.viewport[1] != h)
	{
		g_gl_state.viewport[0] = w;
		g_gl_state.viewport[1] = h;
		glViewport(0, 0, w, h);
	}
}

static void gl_set_scissor(const gs_recti* r)
{
	if (g_gl_state.scissor[0] != r->x || g_gl_state.scissor[1] != r->y || g_gl_state.scissor[2] != r->z
		|| g_gl_state.scissor[3] != r->w)
	{
		g_gl_state.scissor[0] = r->x;
		g_gl_state.scissor[1] = r->y;
		g_gl_state.scissor[2] = r->z;
		g_gl_state.scissor[3] = r->w;
		glScissor(r->x, r->y, r->z - r->x, r->w - r->y);
	}
}

static void gl_set_vao(GLuint vao)
{
	if (g_gl_state.vao == vao)
		return;
	g_gl_state.vao = vao;
	glBindVertexArray(vao);
}

static void gl_set_vertex_buffer(gs_device_gl* dev, const void* vertices, u32 count)
{
	const u32 size = count * GS_GL_VERTEX_PT1_SIZE;
	const gs_gl_stream_map res = gs_gl_stream_buffer_map(&dev->vertex_stream_buffer, GS_GL_VERTEX_PT1_SIZE, size);
	memcpy(res.pointer, vertices, size);
	dev->base.m_vertex.start = res.index_aligned;
	dev->base.m_vertex.count = count;
	gs_gl_stream_buffer_unmap(&dev->vertex_stream_buffer, size);
}

static void gl_set_index_buffer(gs_device_gl* dev, const void* index, u32 count)
{
	const u32 size = count * (u32)sizeof(u16);
	const gs_gl_stream_map res = gs_gl_stream_buffer_map(&dev->index_stream_buffer, sizeof(u16), size);
	dev->base.m_index.start = res.index_aligned;
	dev->base.m_index.count = count;
	memcpy(res.pointer, index, size);
	gs_gl_stream_buffer_unmap(&dev->index_stream_buffer, size);
}

static void gl_set_shader_resource(int i, gs_texture* sr)
{
	const GLuint id = GL_TEX_ID(sr);
	if (g_gl_state.tex_unit[i] != id)
	{
		g_gl_state.tex_unit[i] = id;
		glBindTextureUnit((GLuint)i, id);
	}
}

static void gl_set_sampler(GLuint ss)
{
	if (g_gl_state.ps_ss != ss)
	{
		g_gl_state.ps_ss = ss;
		glBindSampler(0, ss);
	}
}

/* The default colour mask: every channel */
static void gl_set_color_mask_key(u8 wrgba)
{
	gs_hw_color_mask m;
	m.key = (u8)(wrgba & 0xF);
	gs_gl_set_color_mask(m);
}

static void gl_set_blend_off(void)
{
	gs_gl_set_blend(false, GL_ONE, GL_ZERO, GL_FUNC_ADD, GL_ONE, GL_ZERO, false, 0);
}

static void gl_draw_primitive(gs_device_gl* dev)
{
	glDrawArrays(dev->draw_topology, (GLint)dev->base.m_vertex.start, (GLsizei)dev->base.m_vertex.count);
}

static void gl_draw_indexed(gs_device_gl* dev, u32 offset, u32 count)
{
	glDrawElementsBaseVertex(dev->draw_topology, (GLsizei)count, GL_UNSIGNED_SHORT,
		(void*)(uintptr_t)((dev->base.m_index.start + offset) * sizeof(u16)), (GLint)dev->base.m_vertex.start);
}

/* ====================================================================
 * Clears
 * ==================================================================== */

static bool gl_same_size(const gs_texture* a, const gs_texture* b)
{
	return a->m_size.x == b->m_size.x && a->m_size.y == b->m_size.y;
}

static bool gl_is_integer_format(const gs_texture_gl* t)
{
	return t->int_format == GL_RED_INTEGER || t->int_format == GL_RGBA_INTEGER;
}

static bool gl_is_unsigned_format(const gs_texture_gl* t)
{
	return t->int_type == GL_UNSIGNED_BYTE || t->int_type == GL_UNSIGNED_SHORT || t->int_type == GL_UNSIGNED_INT;
}

static void gl_commit_clear(gs_device_gl* dev, gs_texture* t, bool use_write_fbo)
{
	if ((t->m_type != GS_TEXTYPE_RENDER_TARGET && t->m_type != GS_TEXTYPE_DEPTH_STENCIL)
		|| t->m_state == GS_TEXSTATE_DIRTY)
		return;

	if (use_write_fbo)
	{
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dev->fbo_write);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
			(t->m_type == GS_TEXTYPE_RENDER_TARGET) ? GL_TEX(t)->id : 0, 0);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, gl_ds_attachment(dev), GL_TEXTURE_2D,
			(t->m_type == GS_TEXTYPE_DEPTH_STENCIL) ? GL_TEX(t)->id : 0, 0);
	}
	else
	{
		gl_set_fbo(dev, dev->fbo);
		if (t->m_type == GS_TEXTYPE_DEPTH_STENCIL)
		{
			if (g_gl_state.rt && !gl_same_size((const gs_texture*)g_gl_state.rt, t))
				gl_attach_rt(dev, NULL);
			gl_attach_ds(dev, t);
		}
		else
		{
			if (g_gl_state.ds && !gl_same_size((const gs_texture*)g_gl_state.ds, t))
				gl_attach_ds(dev, NULL);
			gl_attach_rt(dev, t);
		}
	}
	if (t->m_state == GS_TEXSTATE_INVALIDATED)
	{
		if (GLAD_GL_VERSION_4_3)
		{
			const GLenum attachment =
				(t->m_type == GS_TEXTYPE_DEPTH_STENCIL) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0;
			glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &attachment);
		}
	}
	else
	{
		glDisable(GL_SCISSOR_TEST);

		if (t->m_type == GS_TEXTYPE_DEPTH_STENCIL)
		{
			const float d = t->m_clear_value.depth;
			if (g_gl_state.depth_mask)
				glClearBufferfv(GL_DEPTH, 0, &d);
			else
			{
				glDepthMask(true);
				glClearBufferfv(GL_DEPTH, 0, &d);
				glDepthMask(false);
			}
		}
		else
		{
			const u32 old_color_mask = g_gl_state.wrgba;
			const u32 c              = t->m_clear_value.color;
			/* (GSVector4::unorm8, whose bits the integer clears take) */
			float c_unorm[4];
			int i;
			gl_set_color_mask_key(0xF);
			for (i = 0; i < 4; i++)
				c_unorm[i] = (float)((c >> (i * 8)) & 0xFFu) * (1.0f / 255.0f);

			if (gl_is_integer_format(GL_TEX(t)))
			{
				GLuint bits[4];
				memcpy(bits, c_unorm, sizeof(bits));
				if (gl_is_unsigned_format(GL_TEX(t)))
					glClearBufferuiv(GL_COLOR, 0, bits);
				else
					glClearBufferiv(GL_COLOR, 0, (const GLint*)bits);
			}
			else
				glClearBufferfv(GL_COLOR, 0, c_unorm);
			gl_set_color_mask_key((u8)old_color_mask);
		}

		glEnable(GL_SCISSOR_TEST);
	}

	t->m_state = GS_TEXSTATE_DIRTY;

	if (use_write_fbo)
	{
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
			(t->m_type == GS_TEXTYPE_RENDER_TARGET) ? GL_COLOR_ATTACHMENT0 : gl_ds_attachment(dev), GL_TEXTURE_2D,
			0, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gl_state.fbo);
	}
}

static void gl_finish_before_cpu_read(const gs_device_gl* dev)
{
	if (dev->cpu_reads_need_finish)
		glFinish();
}

/* rt and ds bound, their clears done, the viewport theirs and the scissor
 * scissor (all of them without one) */
static void gl_set_render_targets(gs_device_gl* dev, gs_texture* rt, gs_texture* ds, const gs_recti* scissor)
{
	/* Split up to avoid unbind/bind calls when clearing. */
	gl_set_fbo(dev, dev->fbo);
	gl_attach_rt(dev, rt);
	gl_attach_ds(dev, ds);

	if (rt)
		gl_commit_clear(dev, rt, false);
	if (ds)
		gl_commit_clear(dev, ds, false);

	if (rt || ds)
	{
		const gs_texture* t = rt ? rt : ds;
		gs_recti whole;
		whole.x = 0;
		whole.y = 0;
		whole.z = t->m_size.x;
		whole.w = t->m_size.y;
		gl_set_viewport(t->m_size.x, t->m_size.y);
		gl_set_scissor(scissor ? scissor : &whole);
	}
}

static void gl_unbind_texture(gs_device_gl* dev, gs_texture* tex)
{
	if (g_gl_state.rt != (void*)tex && g_gl_state.ds != (void*)tex)
		return;

	gl_set_fbo(dev, dev->fbo);
	if (g_gl_state.rt == (void*)tex)
		gl_attach_rt(dev, NULL);
	if (g_gl_state.ds == (void*)tex)
		gl_attach_ds(dev, NULL);
}

/* ====================================================================
 * What the C textures ask of the device (GLDevice.h)
 * ==================================================================== */

#define GL_THE_DEV GL_DEV(g_gs_device_c)

void gs_gl_device_commit_clear(gs_texture* t, int use_write_fbo) { gl_commit_clear(GL_THE_DEV, t, use_write_fbo != 0); }
void gs_gl_device_unbind_texture(gs_texture* t) { gl_unbind_texture(GL_THE_DEV, t); }
gs_gl_stream_buffer* gs_gl_device_upload_buffer(void)
{
	gs_device_gl* dev = GL_THE_DEV;
	return dev->texture_upload_buffer.buffer_id ? &dev->texture_upload_buffer : NULL;
}
int gs_gl_device_colclip_float(void) { return GL_THE_DEV->colclip_float; }
int gs_gl_device_framebuffer_fetch(void) { return g_gs_device_c->m_features.framebuffer_fetch; }
int gs_gl_device_download_pbo_disabled(void) { return GL_THE_DEV->disable_download_pbo; }
/* Before the driver reads a texture on the CPU: a copy out of it or a
 * readback. llvmpipe asks whether a texture is in use scene by scene and
 * takes the first scene that has it: one that only samples it answers
 * "read", and a read waits for nothing, though a later scene still renders
 * to it. The read then sees the texture from before that draw. A finish
 * leaves no scene in flight; llvmpipe would have finished for the copy
 * anyway had it seen the writer. (Mesa's lp_setup_is_resource_referenced;
 * so in 25.2 and in main as of November 2026.) */
void gs_gl_device_finish_before_cpu_read(void) { gl_finish_before_cpu_read(GL_THE_DEV); }
GLuint gs_gl_device_fbo_read(void) { return GL_THE_DEV->fbo_read; }
GLuint gs_gl_device_default_framebuffer(void) { return GL_DEFAULT_FRAMEBUFFER; }

/* ====================================================================
 * Programs
 * ==================================================================== */

/* (what of the context the GLSL header follows) */
static gs_gl_shader_caps gl_shader_caps(const gs_device_gl* dev)
{
	gs_gl_shader_caps caps;
	caps.gles              = dev->is_gles;
	caps.vs_expand         = dev->base.m_features.vs_expand;
	caps.primitive_id      = dev->base.m_features.primitive_id;
	caps.dual_source_blend = dev->base.m_features.dual_source_blend;
	caps.framebuffer_fetch = dev->base.m_features.framebuffer_fetch;
	caps.clip_control      = dev->base.m_features.clip_control;
	return caps;
}

/* A shader's source into out: the GLSL header for entry and type, the
 * macros, then the shader file's code */
static bool gl_shader_source(const gs_device_gl* dev, gs_str_t* out, const char* entry, GLenum type,
	const char* glsl_h_code)
{
	const gs_gl_shader_caps caps = gl_shader_caps(dev);
	return gs_gl_shader_source(out, &caps, entry, type, glsl_h_code, NULL);
}

/* prog from the sources vs and ps, through the program cache */
static bool gl_get_program(gs_device_gl* dev, gs_gl_program* prog, const gs_str_t* vs, const gs_str_t* ps)
{
	return gs_gl_shader_cache_get_program(&dev->shader_cache, prog, GS_STR(vs), GS_STR_LEN(vs), GS_STR(ps),
		GS_STR_LEN(ps)) != 0;
}

static bool gl_build_dynamic_program(gs_device_gl* dev, gs_gl_program* prog, u32 iip, u32 expand, u32 date)
{
	const gs_gl_shader_caps caps = gl_shader_caps(dev);
	return gs_gl_dynamic_sources(&dev->src_vs, &dev->src_ps, &caps, iip, expand, date, tfx_vs_glsl_shader_raw,
		tfx_fs_glsl_shader_raw) && gl_get_program(dev, prog, &dev->src_vs, &dev->src_ps);
}

static void gl_write_to_stream_buffer(gs_gl_stream_buffer* sb, u32 index, u32 align, const void* data, u32 size)
{
	const gs_gl_stream_map res = gs_gl_stream_buffer_map(sb, align, size);
	memcpy(res.pointer, data, size);
	gs_gl_stream_buffer_unmap(sb, size);

	glBindBufferRange(GL_UNIFORM_BUFFER, index, sb->buffer_id, res.buffer_offset, size);
}

/* The dynamic programs' key, at binding 2 (cb_key). */
static void gl_write_dynamic_key(gs_device_gl* dev)
{
	gl_write_to_stream_buffer(&dev->fragment_uniform_stream_buffer, 2, (u32)dev->uniform_buffer_alignment,
		dev->dynamic_key, sizeof(dev->dynamic_key));
}

static void gl_setup_pipeline(gs_device_gl* dev, const gs_gl_program_selector* psel)
{
	const gs_gl_shader_caps caps = gl_shader_caps(dev);
	gs_gl_program* prog;
	size_t hash;
	if (dev->last_program && !memcmp(&dev->last_program_selector, psel, sizeof(*psel)))
	{
		gs_gl_program_bind(dev->last_program);
		return;
	}

	hash = gs_key_hash(psel, sizeof(*psel));
	prog = gs_gl_program_cache_find(&dev->programs, psel, sizeof(*psel), hash);
	if (!prog)
	{
		prog = gs_gl_program_cache_add(&dev->programs, psel, hash);
		if (!dev->dynamic_ready || dev->test_programs == 1)
		{
			/* (without the dynamic programs: built here, as before them) */
			if (gs_gl_vs_source(&dev->src_vs, &caps, psel->vs, tfx_vs_glsl_shader_raw)
				&& gs_gl_ps_source(&dev->src_ps, &caps, &psel->ps, tfx_fs_glsl_shader_raw))
				gl_get_program(dev, prog, &dev->src_vs, &dev->src_ps);
			dev->programs_built_here++;
		}
		/* Compiled and linked by the driver's threads while a dynamic
		 * program draws; without them it stays drawn with one. */
		else if (dev->parallel_compile && dev->test_programs != 2
			&& gs_gl_vs_source(&dev->src_vs, &caps, psel->vs, tfx_vs_glsl_shader_raw)
			&& gs_gl_ps_source(&dev->src_ps, &caps, &psel->ps, tfx_fs_glsl_shader_raw))
			gs_gl_program_start_compile(prog, GS_STR(&dev->src_vs), GS_STR_LEN(&dev->src_vs), GS_STR(&dev->src_ps),
				GS_STR_LEN(&dev->src_ps));
	}
	if (prog->pending && gs_gl_program_poll(prog) > 0)
		dev->programs_async++;

	if (!prog->program_id || prog->pending)
	{
		u32 key[8];
		const u32 expand = dev->base.m_features.vs_expand ? (u32)psel->vs.expand : 0u;
		gs_gl_dynamic_key(&psel->ps, psel->vs, key);
		if (!dev->dynamic_key_valid || memcmp(key, dev->dynamic_key, sizeof(key)))
		{
			memcpy(dev->dynamic_key, key, sizeof(key));
			dev->dynamic_key_valid = true;
			gl_write_dynamic_key(dev);
		}
		gs_gl_program_bind(&dev->dynamic[psel->vs.iip][expand][gs_gl_dynamic_date(psel->ps.date)]);
		dev->dynamic_draws++;
		dev->last_program = NULL;
		return;
	}

	dev->last_program_selector = *psel;
	dev->last_program          = prog;
	gs_gl_program_bind(prog);
}

static void gl_setup_om(gs_device_gl* dev, gs_hw_depth_selector dssel)
{
	gs_gl_depth_stencil_apply(&dev->om_dss[dssel.key]);
}

/* ====================================================================
 * Features
 * ==================================================================== */

static bool gl_check_features(gs_device_gl* dev, bool* buggy_pbo)
{
	gs_device_features* f = &dev->base.m_features;
	bool vendor_id_nvidia = false;
	bool nv_texture_barrier;
	bool buggy_vs_expand;
	const char* vendor   = (const char*)glGetString(GL_VENDOR);
	const char* renderer = (const char*)glGetString(GL_RENDERER);
	GLint major_gl       = 0;
	GLint minor_gl       = 0;
	GLint point_range[2];
	if (strstr(vendor, "NVIDIA Corporation"))
		vendor_id_nvidia = true;

	/* Mesa's software rasterizer, threaded, can read back a render target
	 * from before its last draw (gs_gl_device_finish_before_cpu_read). Its
	 * rasterizer runs on its own threads unless LP_NUM_THREADS=0. */
	dev->cpu_reads_need_finish = renderer && strstr(renderer, "llvmpipe");
	if (dev->cpu_reads_need_finish)
		log_cb(RETRO_LOG_INFO, "llvmpipe: finishing before texture copies and readbacks.\n");

	glGetIntegerv(GL_MAJOR_VERSION, &major_gl);
	glGetIntegerv(GL_MINOR_VERSION, &minor_gl);
	if (dev->is_gles)
	{
		/* GLSL ES 3.10 is the first to take layout(binding) on samplers and
		 * uniform blocks, which every shader here relies on. */
		if (!GLAD_GL_ES_VERSION_3_1)
		{
			log_cb(RETRO_LOG_ERROR, "OpenGL ES 3.1 is required. Only OpenGL ES %d.%d was found\n", major_gl, minor_gl);
			return false;
		}

		/* The vertex to fragment interface is a block, core from 3.2. */
		if (!GLAD_GL_ES_VERSION_3_2 && !GLAD_GL_EXT_shader_io_blocks && !GLAD_GL_OES_shader_io_blocks)
		{
			log_cb(RETRO_LOG_ERROR, "GL_EXT_shader_io_blocks is not supported, this is required for the OpenGL renderer on OpenGL ES 3.1.\n");
			return false;
		}
	}
	else
	{
		if (!GLAD_GL_VERSION_3_3)
		{
			log_cb(RETRO_LOG_ERROR, "OpenGL is not supported. Only OpenGL %d.%d\n was found\n", major_gl, minor_gl);
			return false;
		}

		if (!GLAD_GL_ARB_shading_language_420pack)
		{
			log_cb(RETRO_LOG_WARN, "GL_ARB_shading_language_420pack is not supported, this is required for the OpenGL renderer.\n");
			return false;
		}
	}

	if (!GLAD_GL_ARB_viewport_array)
	{
		glScissorIndexed   = gl_replace_scissor_indexed;
		glViewportIndexedf = gl_replace_viewport_indexedf;
		log_cb(RETRO_LOG_WARN, "GL_ARB_viewport_array is not supported! Function pointer will be replaced.\n");
	}

	/* GLAD's ES loader does not know NV_texture_barrier, though ES drivers
	 * (Mesa's among them) advertise it, so it is looked up by hand. */
	nv_texture_barrier = GLAD_GL_NV_texture_barrier;
	if (dev->is_gles && gl_has_extension("GL_NV_texture_barrier"))
	{
		glTextureBarrierNV = (PFNGLTEXTUREBARRIERNVPROC)hw_render.get_proc_address("glTextureBarrierNV");
		nv_texture_barrier = (glTextureBarrierNV != NULL);
	}

	if (!GLAD_GL_ARB_texture_barrier)
	{
		if (nv_texture_barrier)
			glTextureBarrier = glTextureBarrierNV;
		else
		{
			log_cb(RETRO_LOG_WARN, "GL_ARB_texture_barrier is not supported, blending will not be accurate.\n");
			glTextureBarrier = gl_replace_texture_barrier;
		}
	}

	if (!GLAD_GL_ARB_direct_state_access)
	{
		log_cb(RETRO_LOG_WARN, "GL_ARB_direct_state_access is not supported, this will reduce performance.\n");
		gl_emulate_dsa();
	}

	if (dev->is_gles)
	{
		/* Core only from ES 3.2. On 3.1 GLAD leaves the core pointers null and
		 * loads the suffixed ones the extensions name. */
		if (!glDrawElementsBaseVertex)
		{
			if (GLAD_GL_OES_draw_elements_base_vertex && glDrawElementsBaseVertexOES)
				glDrawElementsBaseVertex = glDrawElementsBaseVertexOES;
			else if (GLAD_GL_EXT_draw_elements_base_vertex && glDrawElementsBaseVertexEXT)
				glDrawElementsBaseVertex = glDrawElementsBaseVertexEXT;
			else
			{
				log_cb(RETRO_LOG_ERROR, "OpenGL ES: glDrawElementsBaseVertex is not supported, this is required for the OpenGL renderer.\n");
				return false;
			}
		}

		if (!glColorMaski)
		{
			if (GLAD_GL_OES_draw_buffers_indexed && glColorMaskiOES)
				glColorMaski = glColorMaskiOES;
			else if (GLAD_GL_EXT_draw_buffers_indexed && glColorMaskiEXT)
				glColorMaski = glColorMaskiEXT;
			else
				glColorMaski = gl_replace_color_maski;
		}

		/* The same entry point and enums as ARB_clip_control. */
		if (GLAD_GL_EXT_clip_control)
			glClipControl = glClipControlEXT;

		if (!glDebugMessageCallback && GLAD_GL_KHR_debug)
		{
			glDebugMessageCallback = glDebugMessageCallbackKHR;
			glDebugMessageControl  = glDebugMessageControlKHR;
		}
	}

	/* Don't use PBOs when we don't have ARB_buffer_storage, orphaning buffers
	 * probably ends up worse than just using the normal texture update
	 * routines and letting the driver take care of it. */
	*buggy_pbo = !GLAD_GL_VERSION_4_4 && !GLAD_GL_ARB_buffer_storage && !GLAD_GL_EXT_buffer_storage;
	if (*buggy_pbo)
		log_cb(RETRO_LOG_WARN, "Not using PBOs for texture uploads because buffer_storage is unavailable.\n");

	/* (no option sets DisableGLDownloadPBO; this was its default) */
	dev->disable_download_pbo = false;
	if (dev->disable_download_pbo)
		log_cb(RETRO_LOG_WARN, "Not using PBOs for texture downloads, this may reduce performance.\n");

	/* optional features based on context */
	f->broken_point_sampler = false;
	/* gl_PrimitiveID reaches a fragment shader in ES 3.2, or on 3.1 through
	 * the geometry shader extensions. */
	f->primitive_id = !dev->is_gles || GLAD_GL_ES_VERSION_3_2 || GLAD_GL_EXT_geometry_shader
		|| GLAD_GL_OES_geometry_shader;
	/* Desktop GL 3.3 has it in core; ES only through EXT_blend_func_extended. */
	f->dual_source_blend = !dev->is_gles || GLAD_GL_EXT_blend_func_extended;
	if (!f->dual_source_blend)
		log_cb(RETRO_LOG_WARN, "GL_EXT_blend_func_extended is not supported, blends that need it are done in the shader.\n");

	f->framebuffer_fetch = GLAD_GL_EXT_shader_framebuffer_fetch != 0;
	if (f->framebuffer_fetch && GSConfig.DisableFramebufferFetch)
	{
		log_cb(RETRO_LOG_WARN, "Framebuffer fetch was found but is disabled. This will reduce performance.\n");
		f->framebuffer_fetch = false;
	}

	if (GSConfig.OverrideTextureBarriers == 0)
		f->texture_barrier = f->framebuffer_fetch; /* Force Disabled */
	else if (GSConfig.OverrideTextureBarriers == 1)
		f->texture_barrier = true; /* Force Enabled */
	else
		f->texture_barrier = f->framebuffer_fetch || GLAD_GL_ARB_texture_barrier || nv_texture_barrier;
	if (!f->texture_barrier)
		log_cb(RETRO_LOG_WARN, "GL_ARB_texture_barrier is not supported, blending will not be accurate.\n");
	/* A real glTextureBarrier, not framebuffer fetch alone: it orders the
	 * texel fetches of a draw sampling its target against the writes
	 * before it, wherever they are. */
	f->texture_feedback = GSConfig.OverrideTextureBarriers != 0 && (GLAD_GL_ARB_texture_barrier || nv_texture_barrier);

	/* The colour clip accumulation needs a render target that holds a 16-bit
	 * integer per channel, and its blended sums, exactly. ES renders to
	 * UNORM16 only through EXT_texture_norm16; failing that, 32-bit float
	 * does the same through EXT_color_buffer_float (half float does not: 11
	 * bits of mantissa lose the low bits past 2047). With neither the clip
	 * is done in the shader. */
	dev->colclip_float = dev->is_gles && !GLAD_GL_EXT_texture_norm16 && GLAD_GL_EXT_color_buffer_float;
	f->colclip_hw      = !dev->is_gles || GLAD_GL_EXT_texture_norm16 || GLAD_GL_EXT_color_buffer_float;
	if (!f->colclip_hw)
		log_cb(RETRO_LOG_WARN, "Neither GL_EXT_texture_norm16 nor GL_EXT_color_buffer_float is supported, colour clipping is done in the shader.\n");

	f->provoking_vertex_last = true;
	f->dxt_textures          = GLAD_GL_EXT_texture_compression_s3tc != 0;
	f->bptc_textures = GLAD_GL_VERSION_4_2 || GLAD_GL_ARB_texture_compression_bptc || GLAD_GL_EXT_texture_compression_bptc;
	f->prefer_new_textures = false;
	f->clip_control        = GLAD_GL_ARB_clip_control || (dev->is_gles && GLAD_GL_EXT_clip_control);
	if (!f->clip_control)
		log_cb(RETRO_LOG_WARN, "GL_ARB_clip_control is not supported, this will cause rendering issues.\n");
	f->stencil_buffer        = true;
	f->test_and_sample_depth = f->texture_barrier;

	/* NVIDIA GPUs prior to Kepler appear to have broken vertex shader buffer
	 * loading. Use bindless textures (introduced in Kepler) to differentiate. */
	buggy_vs_expand = vendor_id_nvidia && (!GLAD_GL_ARB_bindless_texture && !GLAD_GL_NV_bindless_texture);
	if (buggy_vs_expand)
		log_cb(RETRO_LOG_WARN, "Disabling vertex shader expand due to broken NVIDIA driver.\n");

	if (GLAD_GL_ARB_shader_storage_buffer_object)
	{
		GLint max_vertex_ssbos = 0;
		glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &max_vertex_ssbos);
		log_cb(RETRO_LOG_INFO, "GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS: %d\n", max_vertex_ssbos);
		f->vs_expand = (!GSConfig.DisableVertexShaderExpand && !buggy_vs_expand && max_vertex_ssbos > 0
			&& GLAD_GL_ARB_gpu_shader5);
	}
	if (!f->vs_expand)
		log_cb(RETRO_LOG_WARN, "Vertex expansion is not supported. This will reduce performance.\n");

	point_range[0] = 0;
	point_range[1] = 0;
	glGetIntegerv(GL_ALIASED_POINT_SIZE_RANGE, point_range);
	f->point_expand = (point_range[0] <= GSConfig.UpscaleMultiplier && point_range[1] >= GSConfig.UpscaleMultiplier);
	f->line_expand  = false;

	log_cb(RETRO_LOG_INFO, "Using %s for point expansion, %s for line expansion and %s for sprite expansion.\n",
		f->point_expand ? "hardware" : (f->vs_expand ? "vertex expanding" : "UNSUPPORTED"),
		f->line_expand ? "hardware" : (f->vs_expand ? "vertex expanding" : "UNSUPPORTED"),
		f->vs_expand ? "vertex expanding" : "CPU");

	return true;
}

/* Used by OpenGL, so the same calling convention is required. */
static void APIENTRY gl_debug_message_callback(GLenum gl_source, GLenum gl_type, GLuint id, GLenum gl_severity,
	GLsizei gl_length, const GLchar* gl_message, const void* userParam)
{
	const int length     = gl_length >= 0 ? gl_length : (int)strlen(gl_message);
	const char* type;
	const char* severity = "";
	(void)userParam;
	switch (gl_type)
	{
		case GL_DEBUG_TYPE_ERROR_ARB:               type = "Error"; break;
		case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_ARB: type = "Deprecated bhv"; break;
		case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_ARB:  type = "Undefined bhv"; break;
		case GL_DEBUG_TYPE_PORTABILITY_ARB:         type = "Portability"; break;
		case GL_DEBUG_TYPE_PERFORMANCE_ARB:         type = "Perf"; break;
		case GL_DEBUG_TYPE_OTHER_ARB:               type = "Oth"; break;
		case GL_DEBUG_TYPE_PUSH_GROUP:              return; /* Don't print message injected by myself */
		case GL_DEBUG_TYPE_POP_GROUP:               return; /* Don't print message injected by myself */
		default:                                    type = "TTT"; break;
	}
	switch (gl_severity)
	{
		case GL_DEBUG_SEVERITY_HIGH_ARB:   severity = "High"; break;
		case GL_DEBUG_SEVERITY_MEDIUM_ARB: severity = "Mid"; break;
		case GL_DEBUG_SEVERITY_LOW_ARB:    severity = "Low"; break;
		default:
			if (id == 0xFEAD)
				severity = "Cache";
			else if (id == 0xB0B0)
				severity = "REG";
			else if (id == 0xD0D0)
				severity = "EXTRA";
			break;
	}

	/* Don't spam noisy information on the terminal */
	if (gl_severity != GL_DEBUG_SEVERITY_NOTIFICATION && gl_source != GL_DEBUG_SOURCE_APPLICATION)
		log_cb(RETRO_LOG_ERROR, "T:%s\tID:%d\tS:%s\t=> %.*s\n", type, gs_state_draw_number(), severity, length,
			gl_message);
}

/* ====================================================================
 * Creation
 * ==================================================================== */

static void gl_vr_range(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_rectf* dRect,
	const gs_vr_range_constants* cb);
static void gl_vr_eye(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_rectf* dRect,
	const gs_vr_eye_constants* cb);
static const struct gs_vr_ops s_opengl_vr_ops = {gl_vr_range, gl_vr_eye};

static GLuint gl_create_sampler(u8 key)
{
	gs_hw_sampler_selector sel;
	sel.key = key;
	return gs_gl_sampler_create(sel, GSConfig.MaxAnisotropy);
}

static bool gl_create_texture_fx(gs_device_gl* dev)
{
	u32 key;
	/* warning 1 sampler by image unit. So you cannot reuse ps_ss... */
	dev->palette_ss = gl_create_sampler(0);
	glBindSampler(1, dev->palette_ss);

	/* Enable all bits for stencil operations. Technically 1 bit is enough
	 * but buffer is polluted with noise. Clear will be limited to the
	 * mask. */
	glStencilMask(0xFF);
	for (key = 0; key < GL_ARRAY_COUNT(dev->om_dss); key++)
	{
		gs_hw_depth_selector sel;
		sel.key = (u8)key;
		gs_gl_depth_stencil_from(&dev->om_dss[key], sel);
	}

	/* Every dynamic program, as the game boots: then no draw waits for a
	 * program, and a new selector's own is compiled by the driver's
	 * threads (KHR_parallel_shader_compile) while a dynamic one draws.
	 * With the program cache this is a binary loaded each. */
	{
		u32 iip, expand, date;
		const u32 expands = dev->base.m_features.vs_expand ? 4u : 1u;
		const char* mode;
		dev->dynamic_ready = true;
		for (iip = 0; iip < 2; iip++)
			for (expand = 0; expand < expands; expand++)
				for (date = 0; date < 3; date++)
				{
					gs_gl_program* p = &dev->dynamic[iip][expand][date];
					/* (the primitive ID test only where there is one) */
					if (date && !dev->base.m_features.primitive_id)
						continue;
					if (gl_build_dynamic_program(dev, p, iip, expand, date) && p->program_id != 0)
						dev->programs_built_ahead++;
					else
						dev->dynamic_ready = false;
				}
		dev->parallel_compile = GLAD_GL_KHR_parallel_shader_compile || GLAD_GL_ARB_parallel_shader_compile;
		if (GLAD_GL_KHR_parallel_shader_compile)
			glMaxShaderCompilerThreadsKHR(0xFFFFFFFFu);
		else if (GLAD_GL_ARB_parallel_shader_compile)
			glMaxShaderCompilerThreadsARB(0xFFFFFFFFu);
		/* (tests/glnegotiate: "sync", every program built as it is first
		 * drawn with, here; "dynamic", every draw with a dynamic one) */
		mode               = getenv("LRPS2_TEST_GL_PROGRAMS");
		dev->test_programs = !mode ? 0 : !strcmp(mode, "sync") ? 1 : !strcmp(mode, "dynamic") ? 2 : 0;
		if (dev->dynamic_ready && !dev->parallel_compile)
			log_cb(RETRO_LOG_WARN, "OpenGL: no parallel shader compile; new draw states stay on the dynamic programs\n");
	}

	/* The draw programs' pool and slots, so that none is allocated as a
	 * game draws */
	gs_gl_program_cache_free(&dev->programs);
	if (!gs_gl_program_cache_init(&dev->programs, sizeof(gs_gl_program_selector), PROGRAM_CACHE_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "OpenGL: failed to allocate the program cache\n");
		return false;
	}

	gs_gl_program_reset_last();
	return true;
}

/* The uniforms each convert shader takes, by its name */
static void gl_register_convert_uniforms(gs_gl_program* p, int shader)
{
	switch (shader)
	{
		case GS_SHADER_CONVERT_RGBA_TO_8I:
			gs_gl_program_register_uniform(p, "SBW");
			gs_gl_program_register_uniform(p, "DBW");
			gs_gl_program_register_uniform(p, "ScaleFactor");
			gs_gl_program_register_uniform(p, "PageOffset");
			break;
		case GS_SHADER_CONVERT_YUV:
			gs_gl_program_register_uniform(p, "EMOD");
			break;
		case GS_SHADER_CONVERT_CLUT_4:
		case GS_SHADER_CONVERT_CLUT_8:
			gs_gl_program_register_uniform(p, "offset");
			gs_gl_program_register_uniform(p, "scale");
			gs_gl_program_register_uniform(p, "sample_at");
			break;
		case GS_SHADER_CONVERT_DOWNSAMPLE_COPY:
			gs_gl_program_register_uniform(p, "ClampMin");
			gs_gl_program_register_uniform(p, "DownsampleFactor");
			gs_gl_program_register_uniform(p, "Weight");
			break;
		case GS_SHADER_CONVERT_MOVE_TEXELS:
			gs_gl_program_register_uniform(p, "MoveA");
			gs_gl_program_register_uniform(p, "MoveB");
			break;
		default:
			break;
	}
}

/* A numbered shader's entry point (prefix and a digit) into out[32] */
static void gl_entry_name(char* out, const char* prefix, unsigned i)
{
	const size_t len = strlen(prefix);
	memcpy(out, prefix, len);
	out[len]     = (char)('0' + i);
	out[len + 1] = '\0';
}

static bool gl_dev_create(gs_device* d)
{
	gs_device_gl* dev = GL_DEV(d);
	bool buggy_pbo    = false;
	int gles          = 0;
	unsigned i;

	/* GL is a pain and needs the window super early to create the context. */
	gs_device_acquire_window(&dev->base);

	if (!gs_gl_context_load(&gles))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create any GL context\n");
		return false;
	}
	dev->gl_loaded = true;
	dev->is_gles   = gles != 0;

	if (!gl_check_features(dev, &buggy_pbo))
		return false;

	if (!GSConfig.DisableShaderCache)
	{
		if (!gs_gl_shader_cache_open(&dev->shader_cache, dev->is_gles, EmuFolders.Cache))
			log_cb(RETRO_LOG_WARN, "Shader cache failed to open.\n");
	}
	else
		log_cb(RETRO_LOG_INFO, "Not using shader cache.\n");

	/* because of fbo bindings below... */
	gs_gl_state_clear();

	/* Debug helper */
	if (GSConfig.UseDebugDevice && glDebugMessageCallback)
	{
		/* Useless info message on Nvidia driver */
		static const GLuint ids[] = {0x20004};
		glDebugMessageCallback(gl_debug_message_callback, NULL);
		glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, true);
		glDebugMessageControl(GL_DEBUG_SOURCE_API_ARB, GL_DEBUG_TYPE_OTHER_ARB, GL_DONT_CARE, GL_ARRAY_COUNT(ids), ids,
			false);
		glEnable(GL_DEBUG_OUTPUT);
	}

	/* Various object */
	{
		/* Always write to the first buffer */
		static const GLenum target[1] = {GL_COLOR_ATTACHMENT0};
		glGenFramebuffers(1, &dev->fbo);
		glGenFramebuffers(1, &dev->fbo_read);
		glGenFramebuffers(1, &dev->fbo_write);

		gl_set_fbo(dev, dev->fbo);
		glDrawBuffers(1, target);

		/* Always read from the first buffer */
		glBindFramebuffer(GL_READ_FRAMEBUFFER, dev->fbo_read);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, GL_DEFAULT_FRAMEBUFFER);
	}

	/* Vertex buffer state */
	{
		int streams;
		gs_gl_vertex_input_begin(&dev->vi);

		/* (each made whatever the others did) */
		streams = gs_gl_stream_buffer_create(&dev->vertex_stream_buffer, GL_ARRAY_BUFFER, VERTEX_BUFFER_SIZE) != 0;
		streams &= gs_gl_stream_buffer_create(&dev->index_stream_buffer, GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE) != 0;
		streams &= gs_gl_stream_buffer_create(&dev->vertex_uniform_stream_buffer, GL_UNIFORM_BUFFER,
			VERTEX_UNIFORM_BUFFER_SIZE) != 0;
		streams &= gs_gl_stream_buffer_create(&dev->fragment_uniform_stream_buffer, GL_UNIFORM_BUFFER,
			FRAGMENT_UNIFORM_BUFFER_SIZE) != 0;
		glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &dev->uniform_buffer_alignment);
		if (!streams)
		{
			log_cb(RETRO_LOG_ERROR, "GS: Failed to create vertex/index/uniform streaming buffers\n");
			return false;
		}

		if (!gs_gl_vertex_input_finish(&dev->vi, &dev->vertex_stream_buffer, &dev->index_stream_buffer,
				dev->base.m_features.vs_expand, GLAD_GL_ARB_shader_draw_parameters))
			return false;

		/* Force UBOs to be uploaded on first use. */
		memset(&dev->vs_cb_cache, 0xFF, sizeof(dev->vs_cb_cache));
		memset(&dev->ps_cb_cache, 0xFF, sizeof(dev->ps_cb_cache));
	}

	/* Pre Generate the different sampler object */
	for (i = 0; i < GL_ARRAY_COUNT(dev->ps_ss); i++)
		dev->ps_ss[i] = gl_create_sampler((u8)i);

	/* convert */
	{
		gs_hw_sampler_selector bilinear;
		if (!gl_shader_source(dev, &dev->convert.vs, "vs_main", GL_VERTEX_SHADER, convert_glsl_shader_raw))
			return false;

		for (i = 0; i < GL_ARRAY_COUNT(dev->convert.ps); i++)
		{
			if (!gl_shader_source(dev, &dev->src_ps, gs_shader_convert_name((int)i), GL_FRAGMENT_SHADER,
					convert_glsl_shader_raw)
				|| !gl_get_program(dev, &dev->convert.ps[i], &dev->convert.vs, &dev->src_ps))
				return false;
			gl_register_convert_uniforms(&dev->convert.ps[i], (int)i);
		}

		dev->convert.pt = dev->ps_ss[0];
		bilinear.key    = 0;
		bilinear.biln   = 1;
		dev->convert.ln = dev->ps_ss[bilinear.key];

		gs_gl_depth_stencil_init(&dev->convert.dss);
		gs_gl_depth_stencil_init(&dev->convert.dss_write);
		dev->convert.dss_write.depth_enable = true;
		dev->convert.dss_write.depth_func   = GL_ALWAYS;
		dev->convert.dss_write.depth_mask   = true;
	}

	/* VR. A device without the passes shows the plain frame, so them
	 * failing is no reason to fail the device. */
	dev->base.m_vr_ops = NULL;
	if (gl_shader_source(dev, &dev->src_ps, "ps_vr_range", GL_FRAGMENT_SHADER, vr_glsl_shader_raw)
		&& gl_get_program(dev, &dev->vr[0], &dev->convert.vs, &dev->src_ps)
		&& gl_shader_source(dev, &dev->src_ps, "ps_vr_eye", GL_FRAGMENT_SHADER, vr_glsl_shader_raw)
		&& gl_get_program(dev, &dev->vr[1], &dev->convert.vs, &dev->src_ps))
	{
		gs_gl_program_register_uniform(&dev->vr[0], "SrcRect");
		gs_gl_program_register_uniform(&dev->vr[0], "Params");
		gs_gl_program_register_uniform(&dev->vr[0], "FParams");
		gs_gl_program_register_uniform(&dev->vr[1], "EyePos");
		gs_gl_program_register_uniform(&dev->vr[1], "EyeQuat");
		gs_gl_program_register_uniform(&dev->vr[1], "FovTan");
		gs_gl_program_register_uniform(&dev->vr[1], "Screen");
		gs_gl_program_register_uniform(&dev->vr[1], "DepthMap");
		dev->base.m_vr_ops = &s_opengl_vr_ops;
	}
	else
		log_cb(RETRO_LOG_WARN, "OpenGL: the VR passes did not link, VR is off.\n");

	/* present */
	if (!gl_shader_source(dev, &dev->src_vs, "vs_main", GL_VERTEX_SHADER, present_glsl_shader_raw)
		|| !gl_shader_source(dev, &dev->src_ps, "ps_copy", GL_FRAGMENT_SHADER, present_glsl_shader_raw)
		|| !gl_get_program(dev, &dev->present, &dev->src_vs, &dev->src_ps))
		return false;
	/* (uniforms, saving a UBO no other shader needs) */
	gs_gl_program_register_uniform(&dev->present, "u_source_rect");
	gs_gl_program_register_uniform(&dev->present, "u_target_rect");
	gs_gl_program_register_uniform(&dev->present, "u_source_size");
	gs_gl_program_register_uniform(&dev->present, "u_target_size");
	gs_gl_program_register_uniform(&dev->present, "u_target_resolution");
	gs_gl_program_register_uniform(&dev->present, "u_rcp_target_resolution");
	gs_gl_program_register_uniform(&dev->present, "u_source_resolution");
	gs_gl_program_register_uniform(&dev->present, "u_rcp_source_resolution");
	gs_gl_program_register_uniform(&dev->present, "u_time");

	/* merge */
	for (i = 0; i < GL_ARRAY_COUNT(dev->merge_ps); i++)
	{
		char entry[32];
		gl_entry_name(entry, "ps_main", i);
		if (!gl_shader_source(dev, &dev->src_ps, entry, GL_FRAGMENT_SHADER, merge_glsl_shader_raw)
			|| !gl_get_program(dev, &dev->merge_ps[i], &dev->convert.vs, &dev->src_ps))
			return false;
		gs_gl_program_register_uniform(&dev->merge_ps[i], "BGColor");
	}

	/* interlace */
	for (i = 0; i < GL_ARRAY_COUNT(dev->interlace_ps); i++)
	{
		char entry[32];
		gl_entry_name(entry, "ps_main", i);
		if (!gl_shader_source(dev, &dev->src_ps, entry, GL_FRAGMENT_SHADER, interlace_glsl_shader_raw)
			|| !gl_get_program(dev, &dev->interlace_ps[i], &dev->convert.vs, &dev->src_ps))
			return false;
		gs_gl_program_register_uniform(&dev->interlace_ps[i], "ZrH");
	}

	/* rasterization configuration: ES has no polygon mode, and
	 * multisampling there follows the framebuffer */
	if (!dev->is_gles)
	{
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		glDisable(GL_MULTISAMPLE);
	}
	glDisable(GL_CULL_FACE);
	glEnable(GL_SCISSOR_TEST);
	glDisable(GL_DITHER);

	/* DATE */
	gs_gl_depth_stencil_init(&dev->date.dss);
	dev->date.dss.stencil_enable = true;
	dev->date.dss.stencil_func   = GL_ALWAYS;
	dev->date.dss.stencil_op     = GL_REPLACE;
	for (i = 0; i < GL_ARRAY_COUNT(dev->date.primid_ps); i++)
	{
		char entry[32];
		gl_entry_name(entry, "ps_stencil_image_init_", i);
		if (gl_shader_source(dev, &dev->src_ps, entry, GL_FRAGMENT_SHADER, convert_glsl_shader_raw))
			gl_get_program(dev, &dev->date.primid_ps[i], &dev->convert.vs, &dev->src_ps);
	}

	/* The DX depth convention: gl_Position.z in [0, 1] rather than [-1, 1],
	 * which loses less of a small GS Z */
	if (dev->base.m_features.clip_control)
		glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

	/* HW renderer shader */
	if (!gl_create_texture_fx(dev))
		return false;

	/* Pbo Pool allocation */
	if (!buggy_pbo)
	{
		if (gs_gl_stream_buffer_create(&dev->texture_upload_buffer, GL_PIXEL_UNPACK_BUFFER, TEXTURE_UPLOAD_BUFFER_SIZE))
		{
			/* Don't keep it bound, we'll re-bind when we need it. Otherwise
			 * non-PBO texture uploads break. Yay for global state. */
			gs_gl_stream_buffer_unbind(&dev->texture_upload_buffer);
		}
		else
			log_cb(RETRO_LOG_ERROR, "Failed to create texture upload buffer. Using slow path.\n");
	}

	return true;
}

static void gl_destroy_resources(gs_device_gl* dev)
{
	u32 i, j, k;
	if (dev->programs_built_here || dev->programs_built_ahead)
		log_cb(RETRO_LOG_INFO, "OpenGL programs: %u built on the render thread, %u as the device was created, %u compiled by the driver's threads; %u draws drawn with a dynamic program\n",
			dev->programs_built_here, dev->programs_built_ahead, dev->programs_async, dev->dynamic_draws);
	dev->programs_built_here = dev->programs_built_ahead = dev->programs_async = dev->dynamic_draws = 0;
	for (i = 0; i < 2; i++)
		for (j = 0; j < 4; j++)
			for (k = 0; k < 3; k++)
				gs_gl_program_destroy(&dev->dynamic[i][j][k]);
	dev->dynamic_ready = false;
	gs_gl_shader_cache_close(&dev->shader_cache);

	if (dev->palette_ss != 0)
		glDeleteSamplers(1, &dev->palette_ss);

	dev->last_program = NULL;
	gs_gl_program_cache_free(&dev->programs);

	if (dev->ps_ss[0] != 0)
		glDeleteSamplers(GL_ARRAY_COUNT(dev->ps_ss), dev->ps_ss);

	for (i = 0; i < GL_ARRAY_COUNT(dev->date.primid_ps); i++)
		gs_gl_program_destroy(&dev->date.primid_ps[i]);
	gs_gl_program_destroy(&dev->present);
	for (i = 0; i < GL_ARRAY_COUNT(dev->convert.ps); i++)
		gs_gl_program_destroy(&dev->convert.ps[i]);
	for (i = 0; i < GL_ARRAY_COUNT(dev->interlace_ps); i++)
		gs_gl_program_destroy(&dev->interlace_ps[i]);

	gs_gl_program_destroy(&dev->vr[0]);
	gs_gl_program_destroy(&dev->vr[1]);
	dev->base.m_vr_ops = NULL;

	for (i = 0; i < GL_ARRAY_COUNT(dev->merge_ps); i++)
		gs_gl_program_destroy(&dev->merge_ps[i]);

	gs_gl_stream_buffer_destroy(&dev->fragment_uniform_stream_buffer);
	gs_gl_stream_buffer_destroy(&dev->vertex_uniform_stream_buffer);

	gs_gl_vertex_input_destroy(&dev->vi);

	gs_gl_stream_buffer_destroy(&dev->index_stream_buffer);
	gs_gl_stream_buffer_destroy(&dev->vertex_stream_buffer);
	gs_gl_stream_buffer_destroy(&dev->texture_upload_buffer);

	if (dev->fbo != 0)
		glDeleteFramebuffers(1, &dev->fbo);
	if (dev->fbo_read != 0)
		glDeleteFramebuffers(1, &dev->fbo_read);
	if (dev->fbo_write != 0)
		glDeleteFramebuffers(1, &dev->fbo_write);
}

static void gl_dev_destroy(gs_device* d)
{
	gs_device_gl* dev = GL_DEV(d);
	gs_device_destroy_base(d);

	if (dev->gl_loaded)
	{
		gl_destroy_resources(dev);
		dev->gl_loaded = false;
	}
}

/* Every program of the device, for its init and free */
static void gl_for_each_program(gs_device_gl* dev, void (*fn)(gs_gl_program* p))
{
	u32 i, j, k;
	for (i = 0; i < GL_ARRAY_COUNT(dev->merge_ps); i++)
		fn(&dev->merge_ps[i]);
	for (i = 0; i < GL_ARRAY_COUNT(dev->interlace_ps); i++)
		fn(&dev->interlace_ps[i]);
	for (i = 0; i < GL_ARRAY_COUNT(dev->convert.ps); i++)
		fn(&dev->convert.ps[i]);
	fn(&dev->present);
	fn(&dev->vr[0]);
	fn(&dev->vr[1]);
	for (i = 0; i < GL_ARRAY_COUNT(dev->date.primid_ps); i++)
		fn(&dev->date.primid_ps[i]);
	for (i = 0; i < 2; i++)
		for (j = 0; j < 4; j++)
			for (k = 0; k < 3; k++)
				fn(&dev->dynamic[i][j][k]);
}

static void gl_dev_free(gs_device* d)
{
	gs_device_gl* dev = GL_DEV(d);
	gs_gl_program_cache_free(&dev->programs);
	gs_gl_shader_cache_free(&dev->shader_cache);
	gs_str_free(&dev->src_vs);
	gs_str_free(&dev->src_ps);
	gs_str_free(&dev->convert.vs);
	gl_for_each_program(dev, gs_gl_program_destroy);
	gs_device_deinit(d);
	memalign_free(dev);
}

static int gl_dev_get_render_api(const gs_device* d)
{
	(void)d;
	return GS_RENDER_API_OPENGL;
}

/* ====================================================================
 * Presenting
 * ==================================================================== */

static int gl_dev_begin_present(gs_device* d, bool frame_skip)
{
	gs_device_gl* dev = GL_DEV(d);
	gs_recti whole;
	if (frame_skip)
		return GS_PRESENT_FRAME_SKIPPED;

	gl_set_fbo(dev, 0);
	gl_set_color_mask_key(0xF);

	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_SCISSOR_TEST);

	whole.x = 0;
	whole.y = 0;
	whole.z = (int)dev->base.m_window_info.surface_width;
	whole.w = (int)dev->base.m_window_info.surface_height;
	gl_set_viewport(whole.z, whole.w);
	gl_set_scissor(&whole);

	return GS_PRESENT_OK;
}

static void gl_dev_end_present(gs_device* d)
{
	gs_device_gl* dev = GL_DEV(d);
	if (dev->base.m_current)
	{
		const int w = (dev->present_size.x > 0) ? dev->present_size.x : dev->base.m_current->m_size.x;
		const int h = (dev->present_size.x > 0) ? dev->present_size.y : dev->base.m_current->m_size.y;
		video_cb(RETRO_HW_FRAME_BUFFER_VALID, (unsigned)w, (unsigned)h, 0);
	}
	else
		video_cb(NULL, 0, 0, 0);
	dev->present_size.x = 0;
	dev->present_size.y = 0;
}

static void gl_dev_reset_api_state(gs_device* d)
{
	gs_device_gl* dev = GL_DEV(d);
	if (g_gl_state.point_size && !dev->is_gles)
		glDisable(GL_PROGRAM_POINT_SIZE);
	if (g_gl_state.line_width != 1.0f)
		glLineWidth(1.0f);

	/* clear out DSB */
	glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
	glDisable(GL_BLEND);
	glActiveTexture(GL_TEXTURE0);
}

static void gl_dev_restore_api_state(gs_device* d)
{
	gs_device_gl* dev = GL_DEV(d);
	const float bf    = (float)g_gl_state.bf / 128.0f;
	GLuint i;

	glBindVertexArray(dev->vi.vao);

	if (g_gl_state.fbo)
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gl_state.fbo);
	else
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GL_DEFAULT_FRAMEBUFFER);

	glViewportIndexedf(0, 0, 0, (float)g_gl_state.viewport[0], (float)g_gl_state.viewport[1]);
	glScissorIndexed(0, g_gl_state.scissor[0], g_gl_state.scissor[1], g_gl_state.scissor[2] - g_gl_state.scissor[0],
		g_gl_state.scissor[3] - g_gl_state.scissor[1]);

	glBlendEquationSeparate(g_gl_state.eq_RGB, GL_FUNC_ADD);
	glBlendFuncSeparate(g_gl_state.f_sRGB, g_gl_state.f_dRGB, GL_ONE, GL_ZERO);

	glBlendColor(bf, bf, bf, bf);

	if (g_gl_state.blend)
		glEnable(GL_BLEND);
	else
		glDisable(GL_BLEND);

	glColorMask(g_gl_state.wrgba & 1, (g_gl_state.wrgba >> 1) & 1, (g_gl_state.wrgba >> 2) & 1,
		(g_gl_state.wrgba >> 3) & 1);

	if (g_gl_state.depth)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);
	glDepthFunc(g_gl_state.depth_func);
	glDepthMask(g_gl_state.depth_mask);

	if (g_gl_state.stencil)
		glEnable(GL_STENCIL_TEST);
	else
		glDisable(GL_STENCIL_TEST);

	glStencilFunc(g_gl_state.stencil_func, 1, 1);
	glStencilOp(GL_KEEP, GL_KEEP, g_gl_state.stencil_pass);

	glBindSampler(0, g_gl_state.ps_ss);

	for (i = 0; i < GL_ARRAY_COUNT(g_gl_state.tex_unit); i++)
		glBindTextureUnit(i, g_gl_state.tex_unit[i]);

	if (g_gl_state.point_size && !dev->is_gles)
		glEnable(GL_PROGRAM_POINT_SIZE);
	if (g_gl_state.line_width != 1.0f)
		glLineWidth(g_gl_state.line_width);

	/* Force UBOs to be reuploaded, we don't know what else was bound there. */
	memset(&dev->vs_cb_cache, 0xFF, sizeof(dev->vs_cb_cache));
	memset(&dev->ps_cb_cache, 0xFF, sizeof(dev->ps_cb_cache));
	dev->dynamic_key_valid = false;
}

static gs_texture* gl_dev_create_surface(gs_device* d, unsigned type, int width, int height, int levels,
	unsigned format)
{
	(void)d;
	return gs_texture_gl_create(type, width, height, levels, format);
}

static void* gl_dev_create_download_texture(gs_device* d, u32 width, u32 height, unsigned format)
{
	(void)d;
	return gs_download_texture_gl_create(width, height, format);
}

/* ====================================================================
 * The utility passes
 * ==================================================================== */

static void gl_set_quad(gs_gl_vertex_pt1* v, float x, float y, float s, float t)
{
	memset(v, 0, sizeof(*v));
	v->p[0] = x;
	v->p[1] = y;
	v->t[0] = s;
	v->t[1] = t;
}

/* The quad of dRect in a target of ds_w by ds_h, sampling sRect */
static void gl_quad(gs_gl_vertex_pt1 v[4], const gs_rectf* sRect, const gs_rectf* dRect, float ds_w, float ds_h)
{
	const float left   = dRect->x * 2 / ds_w - 1.0f;
	const float right  = dRect->z * 2 / ds_w - 1.0f;
	/* (top and bottom flipped, which the internal resolution's scaling
	 * needs) */
	const float top    = -1.0f + dRect->y * 2 / ds_h;
	const float bottom = -1.0f + dRect->w * 2 / ds_h;
	gl_set_quad(&v[0], left, top, sRect->x, sRect->y);
	gl_set_quad(&v[1], right, top, sRect->z, sRect->y);
	gl_set_quad(&v[2], left, bottom, sRect->x, sRect->w);
	gl_set_quad(&v[3], right, bottom, sRect->z, sRect->w);
}

static void gl_draw_stretch_rect(gs_device_gl* dev, const gs_rectf* sRect, const gs_rectf* dRect, int ds_x, int ds_y)
{
	gs_gl_vertex_pt1 vertices[4];
	gl_quad(vertices, sRect, dRect, (float)ds_x, (float)ds_y);

	gl_set_vao(dev->vi.vao);
	gl_set_vertex_buffer(dev, vertices, 4);
	dev->draw_topology = GL_TRIANGLE_STRIP;
	gl_draw_primitive(dev);
}

static void gl_stretch_rect_with(gs_device_gl* dev, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, const gs_gl_program* ps, bool alpha_blend, u8 cms, bool linear)
{
	const bool draw_in_depth = dTex->m_type == GS_TEXTYPE_DEPTH_STENCIL;
	gl_commit_clear(dev, sTex, true);

	if (draw_in_depth)
		gl_set_render_targets(dev, NULL, dTex, NULL);
	else
		gl_set_render_targets(dev, dTex, NULL, NULL);

	gs_gl_program_bind(ps);

	gs_gl_depth_stencil_apply(draw_in_depth ? &dev->convert.dss_write : &dev->convert.dss);
	gs_gl_set_blend(alpha_blend, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD, GL_ONE, GL_ZERO, false, 0);
	{
		gs_hw_color_mask m;
		m.key = cms;
		gs_gl_set_color_mask(m);
	}

	gl_set_shader_resource(0, sTex);
	gl_set_sampler(linear ? dev->convert.ln : dev->convert.pt);

	gl_draw_stretch_rect(dev, sRect, dRect, dTex->m_size.x, dTex->m_size.y);
}

static void gl_dev_stretch_rect(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, int shader, bool linear)
{
	gs_device_gl* dev = GL_DEV(d);
	gl_stretch_rect_with(dev, sTex, sRect, dTex, dRect, &dev->convert.ps[shader], false,
		(u8)(gs_shader_convert_write_mask(shader) & 0xF), linear);
}

static void gl_dev_stretch_rect_mask(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, bool red, bool green, bool blue, bool alpha, int shader)
{
	gs_device_gl* dev = GL_DEV(d);
	gs_hw_color_mask cms;
	cms.key = 0xF;
	cms.wr  = red;
	cms.wg  = green;
	cms.wb  = blue;
	cms.wa  = alpha;
	gl_stretch_rect_with(dev, sTex, sRect, dTex, dRect, &dev->convert.ps[shader], false, cms.key, false);
}

static void gl_dev_present_rect(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect)
{
	gs_device_gl* dev       = GL_DEV(d);
	const gs_gl_program* p  = &dev->present;
	const int ds_x          = dTex ? dTex->m_size.x : (int)dev->base.m_window_info.surface_width;
	const int ds_y          = dTex ? dTex->m_size.y : (int)dev->base.m_window_info.surface_height;
	/* (DisplayConstantBuffer's SetSource and SetTarget) */
	float source_rect[4], target_rect[4];
	float source_size[2], target_size[2];
	float target_res[2], rcp_target_res[2], source_res[2], rcp_source_res[2];
	gs_rectf flip_sr;

	gl_commit_clear(dev, sTex, true);

	source_rect[0]    = sRect->x;
	source_rect[1]    = sRect->y;
	source_rect[2]    = sRect->z;
	source_rect[3]    = sRect->w;
	source_res[0]     = (float)sTex->m_size.x;
	source_res[1]     = (float)sTex->m_size.y;
	rcp_source_res[0] = 1.0f / source_res[0];
	rcp_source_res[1] = 1.0f / source_res[1];
	source_size[0]    = (sRect->z - sRect->x) * source_res[0];
	source_size[1]    = (sRect->w - sRect->y) * source_res[1];
	target_rect[0]    = dRect->x;
	target_rect[1]    = dRect->y;
	target_rect[2]    = dRect->z;
	target_rect[3]    = dRect->w;
	target_res[0]     = (float)ds_x;
	target_res[1]     = (float)ds_y;
	rcp_target_res[0] = 1.0f / target_res[0];
	rcp_target_res[1] = 1.0f / target_res[1];
	target_size[0]    = dRect->z - dRect->x;
	target_size[1]    = dRect->w - dRect->y;

	gs_gl_program_bind(p);
	gs_gl_program_uniform4fv(p, 0, source_rect);
	gs_gl_program_uniform4fv(p, 1, target_rect);
	gs_gl_program_uniform2fv(p, 2, source_size);
	gs_gl_program_uniform2fv(p, 3, target_size);
	gs_gl_program_uniform2fv(p, 4, target_res);
	gs_gl_program_uniform2fv(p, 5, rcp_target_res);
	gs_gl_program_uniform2fv(p, 6, source_res);
	gs_gl_program_uniform2fv(p, 7, rcp_source_res);

	gs_gl_depth_stencil_apply(&dev->convert.dss);
	gl_set_blend_off();
	gl_set_color_mask_key(0xF);

	gl_set_shader_resource(0, sTex);
	gl_set_sampler(dev->convert.pt);

	/* Flip y axis only when we render in the backbuffer: everything is
	 * rendered in the DX order otherwise, consistently between passes. */
	flip_sr.x = sRect->x;
	flip_sr.y = sRect->w;
	flip_sr.z = sRect->z;
	flip_sr.w = sRect->y;
	gl_draw_stretch_rect(dev, &flip_sr, dRect, ds_x, ds_y);

	if (!dTex)
	{
		dev->present_size.x = (int)(dRect->z - dRect->x);
		dev->present_size.y = (int)(dRect->w - dRect->y);
	}
}

/* A convert pass, its program bound and given its constants already, over
 * dRect of dTex from sTex */
static void gl_convert_pass(gs_device_gl* dev, gs_texture* sTex, gs_texture* dTex, const gs_rectf* sRect,
	const gs_rectf* dRect, GLuint sampler)
{
	gs_gl_depth_stencil_apply(&dev->convert.dss);
	gl_set_blend_off();
	gl_set_color_mask_key(0xF);
	gl_set_render_targets(dev, dTex, NULL, NULL);

	gl_set_shader_resource(0, sTex);
	gl_set_sampler(sampler);

	gl_draw_stretch_rect(dev, sRect, dRect, dTex->m_size.x, dTex->m_size.y);
}

static const gs_rectf s_gl_zero_rect = {0.0f, 0.0f, 0.0f, 0.0f};

static void gl_dev_update_clut_texture(gs_device* d, gs_texture* sTex, float sScale, float sSample, u32 offsetX,
	u32 offsetY, gs_texture* dTex, u32 dOffset, u32 dSize)
{
	gs_device_gl* dev      = GL_DEV(d);
	const int shader       = (dSize == 16) ? GS_SHADER_CONVERT_CLUT_4 : GS_SHADER_CONVERT_CLUT_8;
	const gs_gl_program* p = &dev->convert.ps[shader];
	gs_rectf dRect;
	gl_commit_clear(dev, sTex, false);

	gs_gl_program_bind(p);
	gs_gl_program_uniform3ui(p, 0, offsetX, offsetY, dOffset);
	gs_gl_program_uniform1f(p, 1, sScale);
	gs_gl_program_uniform1f(p, 2, sSample);

	dRect.x = 0.0f;
	dRect.y = 0.0f;
	dRect.z = (float)dSize;
	dRect.w = 1.0f;
	gl_convert_pass(dev, sTex, dTex, &s_gl_zero_rect, &dRect, dev->convert.ln);
}

static void gl_dev_convert_to_indexed_texture(gs_device* d, gs_texture* sTex, float sScale, u32 offsetX,
	u32 offsetY, u32 SBW, u32 SPSM, gs_texture* dTex, u32 DBW, u32 DPSM)
{
	gs_device_gl* dev      = GL_DEV(d);
	const gs_gl_program* p = &dev->convert.ps[GS_SHADER_CONVERT_RGBA_TO_8I];
	gs_rectf dRect;
	(void)SPSM;
	(void)DPSM;
	gl_commit_clear(dev, sTex, false);

	gs_gl_program_bind(p);
	gs_gl_program_uniform1ui(p, 0, SBW);
	gs_gl_program_uniform1ui(p, 1, DBW);
	gs_gl_program_uniform1f(p, 2, sScale);
	gs_gl_program_uniform1ui(p, 3, gs_indexed_conversion_page_offset(offsetX, offsetY, SBW));

	dRect.x = 0.0f;
	dRect.y = 0.0f;
	dRect.z = (float)dTex->m_size.x;
	dRect.w = (float)dTex->m_size.y;
	gl_convert_pass(dev, sTex, dTex, &s_gl_zero_rect, &dRect, dev->convert.pt);
}

static void gl_dev_filtered_downsample_texture(gs_device* d, gs_texture* sTex, gs_texture* dTex, u32 factor,
	const gs_point* clamp_min, const gs_rectf* dRect)
{
	gs_device_gl* dev      = GL_DEV(d);
	const gs_gl_program* p = &dev->convert.ps[GS_SHADER_CONVERT_DOWNSAMPLE_COPY];
	s32 clamp[2];
	gl_commit_clear(dev, sTex, false);

	clamp[0] = clamp_min->x;
	clamp[1] = clamp_min->y;
	gs_gl_program_bind(p);
	gs_gl_program_uniform2iv(p, 0, clamp);
	gs_gl_program_uniform1i(p, 1, (s32)factor);
	gs_gl_program_uniform1f(p, 2, (float)(factor * factor));

	gl_convert_pass(dev, sTex, dTex, &s_gl_zero_rect, dRect, dev->convert.pt);
}

static void gl_dev_move_texels(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_rectf* dRect,
	const MoveTexelsConstants* cb)
{
	gs_device_gl* dev      = GL_DEV(d);
	const gs_gl_program* p = &dev->convert.ps[GS_SHADER_CONVERT_MOVE_TEXELS];
	gl_commit_clear(dev, sTex, false);

	gs_gl_program_bind(p);
	gs_gl_program_uniform4ui(p, 0, cb->dst_origin, cb->old_base, cb->src_base, cb->remap_base);
	gs_gl_program_uniform4ui(p, 1, cb->flags, 0, 0, 0);

	gl_convert_pass(dev, sTex, dTex, &s_gl_zero_rect, dRect, dev->convert.pt);
}

static void gl_vr_range(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_rectf* dRect,
	const gs_vr_range_constants* cb)
{
	gs_device_gl* dev      = GL_DEV(d);
	const gs_gl_program* p = &dev->vr[0];
	gl_commit_clear(dev, sTex, false);

	gs_gl_program_bind(p);
	gs_gl_program_uniform4iv(p, 0, &cb->src.x);
	gs_gl_program_uniform4iv(p, 1, &cb->params.x);
	gs_gl_program_uniform4fv(p, 2, &cb->fparams.x);

	gl_convert_pass(dev, sTex, dTex, &s_gl_zero_rect, dRect, dev->convert.pt);
}

static void gl_vr_eye(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_rectf* dRect,
	const gs_vr_eye_constants* cb)
{
	static const gs_rectf unit = {0.0f, 0.0f, 1.0f, 1.0f};
	gs_device_gl* dev          = GL_DEV(d);
	const gs_gl_program* p     = &dev->vr[1];
	gl_commit_clear(dev, sTex, false);

	gs_gl_program_bind(p);
	gs_gl_program_uniform4fv(p, 0, &cb->eye_pos.x);
	gs_gl_program_uniform4fv(p, 1, &cb->eye_quat.x);
	gs_gl_program_uniform4fv(p, 2, &cb->fov_tan.x);
	gs_gl_program_uniform4fv(p, 3, &cb->screen.x);
	gs_gl_program_uniform4fv(p, 4, &cb->depth.x);

	gl_convert_pass(dev, sTex, dTex, &unit, dRect, dev->convert.ln);
}

/* A run of rects of one source, sampler and mask: one indexed draw */
static void gl_do_multi_stretch_rects(gs_device_gl* dev, const gs_multi_stretch_rect* rects, u32 num_rects,
	float ds_x, float ds_y)
{
	const u32 vertex_reserve_size = num_rects * 4 * GS_GL_VERTEX_PT1_SIZE;
	const u32 index_reserve_size  = num_rects * 6 * (u32)sizeof(u16);
	const gs_gl_stream_map vertex_map =
		gs_gl_stream_buffer_map(&dev->vertex_stream_buffer, GS_GL_VERTEX_PT1_SIZE, vertex_reserve_size);
	const gs_gl_stream_map index_map =
		gs_gl_stream_buffer_map(&dev->index_stream_buffer, sizeof(u16), index_reserve_size);
	/* Don't use primitive restart here, it ends up slower on some drivers. */
	gs_gl_vertex_pt1* verts = (gs_gl_vertex_pt1*)vertex_map.pointer;
	u16* idx                = (u16*)index_map.pointer;
	u32 icount              = 0;
	u32 vcount              = 0;
	u32 i;
	dev->base.m_vertex.start = vertex_map.index_aligned;
	dev->base.m_index.start  = index_map.index_aligned;

	for (i = 0; i < num_rects; i++)
	{
		const u32 vstart = vcount;
		gl_quad(verts + vcount, &rects[i].src_rect, &rects[i].dst_rect, ds_x, ds_y);
		vcount += 4;

		if (i > 0)
			idx[icount++] = (u16)vstart;

		idx[icount++] = (u16)vstart;
		idx[icount++] = (u16)(vstart + 1);
		idx[icount++] = (u16)(vstart + 2);
		idx[icount++] = (u16)(vstart + 3);
		idx[icount++] = (u16)(vstart + 3);
	}

	dev->base.m_vertex.count = vcount;
	dev->base.m_index.count  = icount;
	gs_gl_stream_buffer_unmap(&dev->vertex_stream_buffer, vcount * GS_GL_VERTEX_PT1_SIZE);
	gs_gl_stream_buffer_unmap(&dev->index_stream_buffer, icount * (u32)sizeof(u16));

	gl_set_shader_resource(0, rects[0].src);
	gl_set_sampler(rects[0].linear ? dev->convert.ln : dev->convert.pt);
	gs_gl_set_color_mask(rects[0].wmask);
	gl_draw_indexed(dev, 0, dev->base.m_index.count);
}

static void gl_dev_draw_multi_stretch_rects(gs_device* d, const gs_multi_stretch_rect* rects, u32 num_rects,
	gs_texture* dTex, int shader)
{
	gs_device_gl* dev = GL_DEV(d);
	const float ds_x  = (float)dTex->m_size.x;
	const float ds_y  = (float)dTex->m_size.y;
	gs_texture* last_tex;
	bool last_linear;
	u8 last_wmask;
	u32 first = 0;
	u32 count = 1;
	u32 i;

	gl_set_vao(dev->vi.vao);
	dev->draw_topology = GL_TRIANGLE_STRIP;
	gs_gl_depth_stencil_apply(gs_shader_convert_has_depth_output(shader) ? &dev->convert.dss_write : &dev->convert.dss);
	gl_set_blend_off();
	gl_set_color_mask_key(0xF);
	if (dTex->m_type != GS_TEXTYPE_DEPTH_STENCIL)
		gl_set_render_targets(dev, dTex, NULL, NULL);
	else
		gl_set_render_targets(dev, NULL, dTex, NULL);
	gs_gl_program_bind(&dev->convert.ps[shader]);

	last_tex    = rects[0].src;
	last_linear = rects[0].linear;
	last_wmask  = rects[0].wmask.wrgba;

	for (i = 1; i < num_rects; i++)
	{
		if (rects[i].src == last_tex && rects[i].linear == last_linear && rects[i].wmask.wrgba == last_wmask)
		{
			count++;
			continue;
		}

		gl_do_multi_stretch_rects(dev, rects + first, count, ds_x, ds_y);
		last_tex    = rects[i].src;
		last_linear = rects[i].linear;
		last_wmask  = rects[i].wmask.wrgba;
		first += count;
		count = 1;
	}

	gl_do_multi_stretch_rects(dev, rects + first, count, ds_x, ds_y);
}

static void gl_dev_copy_rect(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_recti* r, u32 destX, u32 destY)
{
	gs_device_gl* dev = GL_DEV(d);
	const GLuint sid  = GL_TEX(sTex)->id;
	const GLuint did  = GL_TEX(dTex)->id;
	const int w       = r->z - r->x;
	const int h       = r->w - r->y;
	gl_commit_clear(dev, sTex, false);
	gl_commit_clear(dev, dTex, false);
	gl_finish_before_cpu_read(dev);

	if (GLAD_GL_VERSION_4_3 || GLAD_GL_ARB_copy_image)
		glCopyImageSubData(sid, GL_TEXTURE_2D, 0, r->x, r->y, 0, did, GL_TEXTURE_2D, 0, (GLint)destX, (GLint)destY,
			0, w, h, 1);
	else if (GLAD_GL_EXT_copy_image)
		glCopyImageSubDataEXT(sid, GL_TEXTURE_2D, 0, r->x, r->y, 0, did, GL_TEXTURE_2D, 0, (GLint)destX,
			(GLint)destY, 0, w, h, 1);
	else if (GLAD_GL_OES_copy_image)
		glCopyImageSubDataOES(sid, GL_TEXTURE_2D, 0, r->x, r->y, 0, did, GL_TEXTURE_2D, 0, (GLint)destX,
			(GLint)destY, 0, w, h, 1);
	else
	{
		if (dev->fbo_read)
			glBindFramebuffer(GL_READ_FRAMEBUFFER, dev->fbo_read);
		else
			glBindFramebuffer(GL_READ_FRAMEBUFFER, GL_DEFAULT_FRAMEBUFFER);
		if (dev->fbo_write)
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dev->fbo_write);
		else
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GL_DEFAULT_FRAMEBUFFER);

		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sid, 0);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, did, 0);

		glDisable(GL_SCISSOR_TEST);
		glBlitFramebuffer(r->x, r->y, r->x + w, r->y + h, (GLint)destX, (GLint)destY, (GLint)destX + w,
			(GLint)destY + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glEnable(GL_SCISSOR_TEST);

		if (g_gl_state.fbo)
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gl_state.fbo);
		else
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GL_DEFAULT_FRAMEBUFFER);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, GL_DEFAULT_FRAMEBUFFER);
	}
}

static void gl_set_clear_color(gs_texture* t, u32 c)
{
	t->m_state             = GS_TEXSTATE_CLEARED;
	t->m_clear_value.color = c;
}

static void gl_dev_do_merge(gs_device* d, gs_texture* sTex[3], gs_rectf* sRect, gs_texture* dTex, gs_rectf* dRect,
	const GSRegPMODE* PMODE, const GSRegEXTBUF* EXTBUF, u32 c, bool linear)
{
	static const gs_rectf full_r = {0.0f, 0.0f, 1.0f, 1.0f};
	gs_device_gl* dev            = GL_DEV(d);
	const bool feedback_write_2  = PMODE->EN2 && sTex[2] != NULL && EXTBUF->FBIN == 1;
	const bool feedback_write_1  = PMODE->EN1 && sTex[2] != NULL && EXTBUF->FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE->SLBG == 1;

	/* Merge the 2 source textures (sTex[0],sTex[1]). Final results go to
	 * dTex. Feedback write will go to sTex[2]. If either 2nd output is
	 * disabled or SLBG is 1, a background color will be used. Note:
	 * background color is also used when outside of the unit rectangle
	 * area */
	gl_set_color_mask_key(0xF);
	gl_set_clear_color(dTex, c);

	if (sTex[1] && (PMODE->SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		/* 2nd output is enabled and selected. Copy it to destination so we
		 * can blend it with 1st output. Note: value outside of dRect must
		 * contains the background color (c) */
		gl_dev_stretch_rect(d, sTex[1], &sRect[1], dTex, PMODE->SLBG ? &dRect[2] : &dRect[1],
			GS_SHADER_CONVERT_COPY, linear);
	}

	/* Upload constant to select YUV algo */
	if (feedback_write_2 || feedback_write_1)
	{
		/* Write result to feedback loop */
		gs_gl_program_bind(&dev->convert.ps[GS_SHADER_CONVERT_YUV]);
		gs_gl_program_uniform2i(&dev->convert.ps[GS_SHADER_CONVERT_YUV], 0, (s32)EXTBUF->EMODA, (s32)EXTBUF->EMODC);
	}

	/* Save 2nd output */
	if (feedback_write_2)
		gl_dev_stretch_rect(d, dTex, &full_r, sTex[2], &dRect[2], GS_SHADER_CONVERT_YUV, linear);

	/* Restore background color to process the normal merge */
	if (feedback_write_2_but_blend_bg)
		gl_set_clear_color(dTex, c);

	if (sTex[0])
	{
		if (PMODE->AMOD == 1) /* Keep the alpha from the 2nd output */
			gl_set_color_mask_key(0x7);

		/* 1st output is enabled. It must be blended */
		if (PMODE->MMOD == 1)
		{
			/* Blend with a constant alpha */
			float bg[4];
			int i;
			for (i = 0; i < 4; i++)
				bg[i] = (float)((c >> (i * 8)) & 0xFFu) * (1.0f / 255.0f);
			gs_gl_program_bind(&dev->merge_ps[1]);
			gs_gl_program_uniform4fv(&dev->merge_ps[1], 0, bg);
			gl_stretch_rect_with(dev, sTex[0], &sRect[0], dTex, &dRect[0], &dev->merge_ps[1], true, 0xF, linear);
		}
		else
		{
			/* Blend with 2 * input alpha */
			gl_stretch_rect_with(dev, sTex[0], &sRect[0], dTex, &dRect[0], &dev->merge_ps[0], true, 0xF, linear);
		}
	}

	if (feedback_write_1)
		gl_dev_stretch_rect(d, dTex, &full_r, sTex[2], &dRect[2], GS_SHADER_CONVERT_YUV, linear);
}

static void gl_dev_do_interlace(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, int shader, bool linear, const gs_rectf* zrh)
{
	gs_device_gl* dev      = GL_DEV(d);
	const gs_gl_program* p = &dev->interlace_ps[shader];
	gl_set_color_mask_key(0xF);

	gs_gl_program_bind(p);
	gs_gl_program_uniform4fv(p, 0, &zrh->x);

	gl_stretch_rect_with(dev, sTex, sRect, dTex, dRect, p, false, 0xF, linear);
}

static void gl_dev_clear_sampler_cache(gs_device* d)
{
	gs_device_gl* dev = GL_DEV(d);
	u32 key;
	glDeleteSamplers(GL_ARRAY_COUNT(dev->ps_ss), dev->ps_ss);

	for (key = 0; key < GL_ARRAY_COUNT(dev->ps_ss); key++)
		dev->ps_ss[key] = gl_create_sampler((u8)key);
}

/* ====================================================================
 * The HW draw
 * ==================================================================== */

/* (gs_hw_blend_state::IsEffective, GSDevice.h: CONST_ONE 14, CONST_ZERO 15) */
static bool gl_blend_is_effective(const gs_hw_blend_state* b, gs_hw_color_mask colormask)
{
	return b->enable
		&& (((colormask.key & 7u) && (b->src_factor != 14 || b->dst_factor != 15))
			|| ((colormask.key & 8u) && (b->src_factor_alpha != 14 || b->dst_factor_alpha != 15)));
}

/* (gs_hw_ps_selector::IsFeedbackLoop) */
static bool gl_ps_is_feedback_loop(const gs_hw_ps_selector* ps)
{
	const u32 sw_blend_bits      = ps->blend_a | ps->blend_b | ps->blend_d;
	const bool sw_blend_needs_rt = sw_blend_bits != 0 && ((sw_blend_bits | ps->blend_c) & 1u);
	return ps->tex_is_fb || ps->fbmask || (ps->date > 0 && ps->date != 3) || sw_blend_needs_rt;
}

static void gl_set_hw_blend_or_off(const gs_device_gl* dev, const gs_hw_blend_state* b, gs_hw_color_mask colormask,
	const gs_hw_blend_state* applied)
{
	if (gl_blend_is_effective(b, colormask))
		gs_gl_set_hw_blend(applied, dev->base.m_features.dual_source_blend);
	else
		gl_set_blend_off();
}

/* The rect of r over a target of w by h, as floats: [0, 1] */
static void gl_norm_rect(gs_rectf* out, const gs_recti* r, int w, int h)
{
	const float fw = (float)w;
	const float fh = (float)h;
	out->x         = (float)r->x / fw;
	out->y         = (float)r->y / fh;
	out->z         = (float)r->z / fw;
	out->w         = (float)r->w / fh;
}

static void gl_float_rect(gs_rectf* out, const gs_recti* r)
{
	out->x = (float)r->x;
	out->y = (float)r->y;
	out->z = (float)r->z;
	out->w = (float)r->w;
}

/* The colour clip target resolved over area into rt, and done with */
static void gl_colclip_resolve(gs_device_gl* dev, gs_texture* hdr_rt, gs_texture* rt, const gs_recti* area)
{
	gs_rectf dRect, sRect;
	gl_float_rect(&dRect, area);
	gl_norm_rect(&sRect, area, rt->m_size.x, rt->m_size.y);
	gl_dev_stretch_rect(&dev->base, hdr_rt, &sRect, rt, &dRect, GS_SHADER_CONVERT_COLCLIP_RESOLVE, false);
	gs_device_recycle(&dev->base, hdr_rt);
	dev->base.m_colclip_rt = NULL;
}

static void gl_setup_date(gs_device_gl* dev, gs_texture* rt, gs_texture* ds, const gs_gl_vertex_pt1* vertices, int datm)
{
	gs_recti scissor;
	const GLint clear_color = 0;
	/* sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii
	 * shadows, rumble roses shadows, persona4 shadows */
	scissor.x = g_gl_state.scissor[0];
	scissor.y = g_gl_state.scissor[1];
	scissor.z = g_gl_state.scissor[2];
	scissor.w = g_gl_state.scissor[3];
	gl_set_render_targets(dev, NULL, ds, &scissor);

	glClearBufferiv(GL_STENCIL, 0, &clear_color);
	gs_gl_program_bind(&dev->convert.ps[gs_set_datm_shader(datm)]);

	gs_gl_depth_stencil_apply(&dev->date.dss);
	gl_set_blend_off();
	gl_set_color_mask_key(0xF);

	gl_set_vao(dev->vi.vao);
	gl_set_vertex_buffer(dev, vertices, 4);
	dev->draw_topology = GL_TRIANGLE_STRIP;

	gl_set_shader_resource(0, rt);
	gl_set_sampler(dev->convert.pt);

	gl_draw_primitive(dev);

	if (g_gl_state.blend)
		glEnable(GL_BLEND);
}

static gs_texture* gl_init_prim_date_texture(gs_device_gl* dev, gs_texture* rt, const gs_recti* area, int datm)
{
	gs_texture* tex = gs_device_create_render_target(&dev->base, rt->m_size.x, rt->m_size.y, GS_TEX_PRIMID, false);
	gs_rectf sRect, dRect;
	if (!tex)
		return NULL;

	gl_norm_rect(&sRect, area, rt->m_size.x, rt->m_size.y);
	gl_float_rect(&dRect, area);
	gl_stretch_rect_with(dev, rt, &sRect, tex, &dRect, &dev->date.primid_ps[datm], false, 0xF, false);
	return tex;
}

static void gl_send_hw_draw(gs_device_gl* dev, const gs_hw_draw_config* config, bool needs_barrier)
{
	if (config->drawlist)
	{
		const u32 indices_per_prim = config->indices_per_prim;
		const u32 draw_list_size   = config->drawlist_count;
		u32 n, p;

		for (n = 0, p = 0; n < draw_list_size; n++)
		{
			const u32 count = config->drawlist[n] * indices_per_prim;
			glTextureBarrier();
			gl_draw_indexed(dev, p, count);
			p += count;
		}

		return;
	}

	if (needs_barrier && dev->base.m_features.texture_barrier)
	{
		if (config->require_full_barrier)
		{
			const u32 indices_per_prim = config->indices_per_prim;
			u32 p;

			for (p = 0; p < config->nindices; p += indices_per_prim)
			{
				glTextureBarrier();
				gl_draw_indexed(dev, p, indices_per_prim);
			}

			return;
		}

		if (config->require_one_barrier)
			glTextureBarrier();
	}

	gl_draw_indexed(dev, 0, dev->base.m_index.count);
}

static void gl_dev_render_hw(gs_device* d, struct GSHWDrawConfig* cfg)
{
	gs_device_gl* dev          = GL_DEV(d);
	gs_hw_draw_config* config  = (gs_hw_draw_config*)cfg;
	gs_texture* primid_texture = NULL;
	gs_texture* hdr_rt         = dev->base.m_colclip_rt;
	gs_texture* draw_rt_clone  = NULL;
	gs_texture* draw_rt;
	gs_texture* draw_ds;
	gs_gl_program_selector psel;
	GLenum topology = 0;
	int rtsize_x, rtsize_y;

	if (g_gl_state.scissor[0] != config->scissor.x || g_gl_state.scissor[1] != config->scissor.y
		|| g_gl_state.scissor[2] != config->scissor.z || g_gl_state.scissor[3] != config->scissor.w)
	{
		glScissor(config->scissor.x, config->scissor.y, config->scissor.z - config->scissor.x,
			config->scissor.w - config->scissor.y);
		g_gl_state.scissor[0] = config->scissor.x;
		g_gl_state.scissor[1] = config->scissor.y;
		g_gl_state.scissor[2] = config->scissor.z;
		g_gl_state.scissor[3] = config->scissor.w;
	}

	if (config->tex)
		gl_commit_clear(dev, config->tex, true);
	if (config->pal)
		gl_commit_clear(dev, config->pal, true);

	rtsize_x = (config->rt ? config->rt : config->ds)->m_size.x;
	rtsize_y = (config->rt ? config->rt : config->ds)->m_size.y;

	if (hdr_rt)
	{
		if (config->colclip_mode == GS_HW_COLCLIP_EARLY_RESOLVE)
		{
			gl_colclip_resolve(dev, hdr_rt, config->rt, &config->colclip_update_area);
			hdr_rt = NULL;
		}
		else
			config->ps.colclip_hw = 1;
	}

	if (config->ps.colclip_hw)
	{
		if (!hdr_rt)
		{
			gs_recti area;
			gs_rectf dRect, sRect;
			config->colclip_update_area = config->drawarea;

			hdr_rt = gs_device_create_render_target(&dev->base, rtsize_x, rtsize_y, GS_TEX_COLOR_CLIP, false);
			if (!hdr_rt)
				return;
			gl_set_render_targets(dev, hdr_rt, config->ds, NULL);

			dev->base.m_colclip_rt = hdr_rt;

			if (config->colclip_mode == GS_HW_COLCLIP_CONVERT_ONLY)
			{
				area.x = 0;
				area.y = 0;
				area.z = rtsize_x;
				area.w = rtsize_y;
			}
			else
				area = config->drawarea;
			gl_float_rect(&dRect, &area);
			gl_norm_rect(&sRect, &area, rtsize_x, rtsize_y);
			gl_dev_stretch_rect(d, config->rt, &sRect, hdr_rt, &dRect, GS_SHADER_CONVERT_COLCLIP_INIT, false);
		}
	}

	/* Destination Alpha Setup */
	switch (config->destination_alpha)
	{
		case GS_HW_DATE_OFF:
		case GS_HW_DATE_FULL:
			break; /* No setup */
		case GS_HW_DATE_PRIMID:
			primid_texture = gl_init_prim_date_texture(dev, hdr_rt ? hdr_rt : config->rt, &config->drawarea,
				(int)config->datm);
			break;
		case GS_HW_DATE_STENCIL_ONE:
			if (dev->base.m_features.texture_barrier)
				break; /* Cleared after RT bind. */
			/* fallthrough */
		case GS_HW_DATE_STENCIL:
		{
			gs_rectf src, dst;
			gs_gl_vertex_pt1 vertices[4];
			gl_norm_rect(&src, &config->drawarea, config->ds->m_size.x, config->ds->m_size.y);
			dst.x = src.x * 2.f - 1.f;
			dst.y = src.y * 2.f - 1.f;
			dst.z = src.z * 2.f - 1.f;
			dst.w = src.w * 2.f - 1.f;
			gl_set_quad(&vertices[0], dst.x, dst.y, src.x, src.y);
			gl_set_quad(&vertices[1], dst.z, dst.y, src.z, src.y);
			gl_set_quad(&vertices[2], dst.x, dst.w, src.x, src.w);
			gl_set_quad(&vertices[3], dst.z, dst.w, src.z, src.w);
			gl_setup_date(dev, hdr_rt ? hdr_rt : config->rt, config->ds, vertices, (int)config->datm);
		}
	}

	if (config->require_one_barrier && !dev->base.m_features.texture_barrier)
	{
		/* Requires a copy of the RT. */
		draw_rt_clone = gs_device_create_texture(&dev->base, rtsize_x, rtsize_y, 1,
			hdr_rt ? GS_TEX_COLOR_CLIP : GS_TEX_COLOR, true);
		if (draw_rt_clone)
			gl_dev_copy_rect(d, hdr_rt ? hdr_rt : config->rt, draw_rt_clone, &config->drawarea,
				(u32)config->drawarea.x, (u32)config->drawarea.y);
	}
	else if (config->tex && config->tex == config->ds)
	{
		/* Ensure all depth writes are finished before sampling */
		glTextureBarrier();
	}

	gl_set_vertex_buffer(dev, config->verts, config->nverts);
	if (config->vs.expand != GS_HW_VS_EXPAND_NONE && !GLAD_GL_ARB_shader_draw_parameters)
	{
		/* Need to offset the buffer. */
		glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, dev->vertex_stream_buffer.buffer_id,
			(GLintptr)dev->base.m_vertex.start * GS_GL_VERTEX_SIZE, (GLsizeiptr)config->nverts * GS_GL_VERTEX_SIZE);
		dev->base.m_vertex.start = 0;
	}

	if (config->vs.expand == GS_HW_VS_EXPAND_POINT || config->vs.expand == GS_HW_VS_EXPAND_SPRITE)
	{
		gl_set_vao(dev->vi.expand_vao);
		dev->base.m_index.start = 0;
		dev->base.m_index.count = config->nindices;
	}
	else
	{
		gl_set_vao(dev->vi.vao);
		gl_set_index_buffer(dev, config->indices, config->nindices);
	}

	switch (config->topology)
	{
		case GS_HW_TOPOLOGY_POINT:    topology = GL_POINTS;    break;
		case GS_HW_TOPOLOGY_LINE:     topology = GL_LINES;     break;
		case GS_HW_TOPOLOGY_TRIANGLE: topology = GL_TRIANGLES; break;
	}
	dev->draw_topology = topology;

	if (config->tex)
		gl_set_shader_resource(0, config->tex);
	if (config->pal)
		gl_set_shader_resource(1, config->pal);
	if (draw_rt_clone)
		gl_set_shader_resource(2, draw_rt_clone);
	else if (config->require_one_barrier || config->require_full_barrier)
		gl_set_shader_resource(2, hdr_rt ? hdr_rt : config->rt);

	gl_set_sampler(dev->ps_ss[config->sampler.key]);

	if (memcmp(&dev->vs_cb_cache, &config->cb_vs, sizeof(config->cb_vs)))
	{
		memcpy(&dev->vs_cb_cache, &config->cb_vs, sizeof(config->cb_vs));
		gl_write_to_stream_buffer(&dev->vertex_uniform_stream_buffer, GL_VS_CB_INDEX,
			(u32)dev->uniform_buffer_alignment, &config->cb_vs, sizeof(config->cb_vs));
	}
	if (memcmp(&dev->ps_cb_cache, &config->cb_ps, sizeof(config->cb_ps)))
	{
		memcpy(&dev->ps_cb_cache, &config->cb_ps, sizeof(config->cb_ps));
		gl_write_to_stream_buffer(&dev->fragment_uniform_stream_buffer, GL_PS_CB_INDEX,
			(u32)dev->uniform_buffer_alignment, &config->cb_ps, sizeof(config->cb_ps));
	}

	psel.vs        = config->vs;
	psel.ps.key_hi = config->ps.key_hi;
	psel.ps.key_lo = config->ps.key_lo;
	memset(psel.pad, 0, sizeof(psel.pad));

	gl_setup_pipeline(dev, &psel);

	/* additional non-pipeline config stuff */
	{
		const u8 point_size_enabled = (u8)config->vs.point_size;
		if (g_gl_state.point_size != point_size_enabled)
		{
			/* ES always takes the size from the vertex shader. */
			if (!dev->is_gles)
			{
				if (point_size_enabled)
					glEnable(GL_PROGRAM_POINT_SIZE);
				else
					glDisable(GL_PROGRAM_POINT_SIZE);
			}
			g_gl_state.point_size = point_size_enabled;
		}
	}
	if (config->topology == GS_HW_TOPOLOGY_LINE)
	{
		const float line_width = config->line_expand ? config->cb_ps.ScaleFactor.z : 1.0f;
		if (g_gl_state.line_width != line_width)
		{
			g_gl_state.line_width = line_width;
			glLineWidth(line_width);
		}
	}

	if (config->destination_alpha == GS_HW_DATE_PRIMID)
	{
		gs_hw_color_mask mask;
		gs_hw_depth_selector dss = config->depth;
		gl_set_render_targets(dev, primid_texture, config->ds, &config->scissor);
		mask.key = 0;
		mask.wr  = 1;
		gs_gl_set_color_mask(mask);
		gs_gl_set_blend(true, GL_ONE, GL_ONE, GL_MIN, GL_ONE, GL_ZERO, false, 0);
		dss.zwe = 0; /* Don't write depth */
		gl_setup_om(dev, dss);

		/* Compute primitiveID max that pass the date test (Draw without barrier) */
		gl_draw_indexed(dev, 0, dev->base.m_index.count);

		psel.ps.date                     = 3;
		config->alpha_second_pass.ps.date = 3;
		gl_setup_pipeline(dev, &psel);
		gl_set_shader_resource(3, primid_texture);
	}

	gl_set_hw_blend_or_off(dev, &config->blend, config->colormask, &config->blend);

	/* avoid changing framebuffer just to switch from rt+depth to rt and vice versa */
	draw_rt = hdr_rt ? hdr_rt : config->rt;
	draw_ds = config->ds;
	if (!draw_rt && g_gl_state.rt && g_gl_state.ds == (void*)draw_ds && config->tex != (gs_texture*)g_gl_state.rt
		&& gl_same_size((const gs_texture*)g_gl_state.rt, draw_ds))
		draw_rt = (gs_texture*)g_gl_state.rt;
	else if (!draw_ds && g_gl_state.ds && g_gl_state.rt == (void*)draw_rt && config->tex != (gs_texture*)g_gl_state.ds
		&& gl_same_size((const gs_texture*)g_gl_state.ds, draw_rt))
		draw_ds = (gs_texture*)g_gl_state.ds;

	gl_set_render_targets(dev, draw_rt, draw_ds, &config->scissor);
	gs_gl_set_color_mask(config->colormask);
	gl_setup_om(dev, config->depth);

	/* Clear stencil as close as possible to the RT bind, to avoid framebuffer swaps. */
	if (config->destination_alpha == GS_HW_DATE_STENCIL_ONE && dev->base.m_features.texture_barrier)
	{
		const GLint clear_color = 1;
		glClearBufferiv(GL_STENCIL, 0, &clear_color);
	}

	gl_send_hw_draw(dev, config, gl_ps_is_feedback_loop(&psel.ps));

	if (config->blend_multi_pass.enable)
	{
		gl_set_hw_blend_or_off(dev, &config->blend, config->colormask, &config->blend_multi_pass.blend);
		psel.ps.blend_hw = config->blend_multi_pass.blend_hw;
		psel.ps.dither   = config->blend_multi_pass.dither;
		gl_setup_pipeline(dev, &psel);
		gl_send_hw_draw(dev, config, gl_ps_is_feedback_loop(&psel.ps));
	}

	if (config->alpha_second_pass.enable)
	{
		/* cbuffer will definitely be dirty if aref changes, no need to check it */
		if (config->cb_ps.FogColor_AREF.w != config->alpha_second_pass.ps_aref)
		{
			config->cb_ps.FogColor_AREF.w = config->alpha_second_pass.ps_aref;
			gl_write_to_stream_buffer(&dev->fragment_uniform_stream_buffer, GL_PS_CB_INDEX,
				(u32)dev->uniform_buffer_alignment, &config->cb_ps, sizeof(config->cb_ps));
		}

		psel.ps = config->alpha_second_pass.ps;
		gl_setup_pipeline(dev, &psel);
		gs_gl_set_color_mask(config->alpha_second_pass.colormask);
		gl_set_hw_blend_or_off(dev, &config->blend, config->alpha_second_pass.colormask, &config->blend);
		gl_setup_om(dev, config->alpha_second_pass.depth);
		gl_send_hw_draw(dev, config, gl_ps_is_feedback_loop(&psel.ps));
	}

	if (primid_texture)
		gs_device_recycle(&dev->base, primid_texture);
	if (draw_rt_clone)
		gs_device_recycle(&dev->base, draw_rt_clone);

	if (hdr_rt)
	{
		/* (GSVector4i::runion) */
		gs_recti* u = &config->colclip_update_area;
		u->x = u->x < config->drawarea.x ? u->x : config->drawarea.x;
		u->y = u->y < config->drawarea.y ? u->y : config->drawarea.y;
		u->z = u->z > config->drawarea.z ? u->z : config->drawarea.z;
		u->w = u->w > config->drawarea.w ? u->w : config->drawarea.w;

		if (config->colclip_mode == GS_HW_COLCLIP_RESOLVE_ONLY || config->colclip_mode == GS_HW_COLCLIP_CONVERT_AND_RESOLVE)
			gl_colclip_resolve(dev, hdr_rt, config->rt, &config->colclip_update_area);
	}

	config->colclip_mode = GS_HW_COLCLIP_NO_MODIFY;
}

/* ====================================================================
 * The device
 * ==================================================================== */

static const struct gs_device_ops s_opengl_device_ops = {
	gl_dev_free,
	gl_dev_create,
	gl_dev_destroy,
	gl_dev_reset_api_state,
	gl_dev_restore_api_state,
	gl_dev_get_render_api,
	gl_dev_begin_present,
	gl_dev_end_present,
	gl_dev_create_surface,
	gl_dev_create_download_texture,
	gl_dev_do_merge,
	gl_dev_do_interlace,
	gl_dev_copy_rect,
	gl_dev_stretch_rect,
	gl_dev_stretch_rect_mask,
	gl_dev_present_rect,
	gl_dev_draw_multi_stretch_rects,
	gl_dev_update_clut_texture,
	gl_dev_convert_to_indexed_texture,
	gl_dev_filtered_downsample_texture,
	gl_dev_move_texels,
	gl_dev_render_hw,
	gl_dev_clear_sampler_cache
};

/* (gs_device_open) */
gs_device* gs_device_new_opengl(void)
{
	gs_device_gl* dev = (gs_device_gl*)memalign_alloc(32, sizeof(gs_device_gl));
	if (!dev)
		return NULL;
	memset(dev, 0, sizeof(*dev));
	gs_device_init(&dev->base);
	dev->base.m_ops = &s_opengl_device_ops;
	gs_gl_program_cache_init(&dev->programs, sizeof(gs_gl_program_selector), 0);
	gs_gl_shader_cache_init(&dev->shader_cache);
	gs_str_init(&dev->src_vs);
	gs_str_init(&dev->src_ps);
	gs_str_init(&dev->convert.vs);
	gl_for_each_program(dev, gs_gl_program_init);
	return &dev->base;
}
