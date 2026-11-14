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

#include <memalign.h>

#include "GLDevice.h"
#include "GLState.h"
#include "GLTextureFormat.h"
#include "GSTextureOGL.h"

/* The optimal copy alignment across drivers is between 1 (AMD/NV) and 64
 * (Intel); a row's pitch is no more than that, and 32 is AVX2's */
#define GS_GL_UPLOAD_ALIGN       64
#define GS_GL_UPLOAD_PITCH_ALIGN 64
#define GS_GL_CPU_BUFFER_ALIGN   32

static u32 gs_gl_pitch_of(u32 width, u32 shift)
{
	return ((width << shift) + (GS_GL_UPLOAD_PITCH_ALIGN - 1)) & ~(u32)(GS_GL_UPLOAD_PITCH_ALIGN - 1);
}

static void gs_texture_gl_free(gs_texture* base)
{
	gs_texture_gl* t = (gs_texture_gl*)base;
	size_t i;

	/* Textures are not detached from framebuffers as they go... */
	gs_gl_device_unbind_texture(base);
	/* ...but they are unbound from the units */
	for (i = 0; i < sizeof(g_gl_state.tex_unit) / sizeof(g_gl_state.tex_unit[0]); i++)
	{
		if (g_gl_state.tex_unit[i] == t->id)
			g_gl_state.tex_unit[i] = 0;
	}

	glDeleteTextures(1, &t->id);
	free(t);
}

static int gs_texture_gl_update(gs_texture* base, const gs_texture_rect* r, const void* data, int pitch, int layer)
{
	gs_texture_gl* t        = (gs_texture_gl*)base;
	const int w             = r->right - r->left;
	const int h             = r->bottom - r->top;
	const u32 upload_pitch  = gs_gl_pitch_of((u32)w, t->int_shift);
	const u32 map_size      = (u32)h * upload_pitch;
	gs_gl_stream_buffer* sb;

	if (layer >= base->m_mipmap_levels)
		return 1;

	/* Mostly palettes, or textures too big for the upload buffer: sent to
	 * the GL as they are, past the buffer when they would not fit (it
	 * would only sync, or worse) */
	gs_gl_device_commit_clear(base, 1);
	sb = gs_gl_device_upload_buffer();

	if (gs_texture_is_compressed(base->m_format))
	{
		glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)gs_texture_row_length(base->m_format, (u32)pitch));
		glCompressedTextureSubImage2D(t->id, layer, r->left, r->top, w, h, t->int_format,
			(GLsizei)gs_texture_upload_size(base->m_format, (u32)h, (u32)pitch), data);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	}
	else if (!sb || map_size > gs_gl_stream_buffer_chunk_size(sb))
	{
		glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch >> t->int_shift);
		glTextureSubImage2D(t->id, layer, r->left, r->top, w, h, t->int_format, t->int_type, data);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	}
	else
	{
		const gs_gl_stream_map map = gs_gl_stream_buffer_map(sb, GS_GL_UPLOAD_ALIGN, map_size);
		gs_stride_memcpy(map.pointer, upload_pitch, data, (size_t)pitch, (size_t)w << t->int_shift, (size_t)h);
		gs_gl_stream_buffer_unmap(sb, map_size);
		gs_gl_stream_buffer_bind(sb);
		/* (in GL's texels: the format's bytes are not always them) */
		glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(upload_pitch >> t->int_shift));
		glTextureSubImage2D(t->id, layer, r->left, r->top, w, h, t->int_format, t->int_type,
			(const void*)(uintptr_t)map.buffer_offset);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		gs_gl_stream_buffer_unbind(sb);
	}

	base->m_needs_mipmaps_generated = 1;
	return 1;
}

static int gs_texture_gl_map(gs_texture* base, gs_texture_map* m, const gs_texture_rect* r, int layer)
{
	gs_texture_gl* t = (gs_texture_gl*)base;
	gs_texture_rect full;
	gs_gl_stream_buffer* sb;
	gs_gl_stream_map map;
	u32 pitch, size;

	if (layer >= base->m_mipmap_levels || gs_texture_is_compressed(base->m_format))
		return 0;

	gs_gl_device_commit_clear(base, 1);
	if (!r)
	{
		full.left   = 0;
		full.top    = 0;
		full.right  = base->m_size.x;
		full.bottom = base->m_size.y;
		r           = &full;
	}

	pitch    = gs_gl_pitch_of((u32)(r->right - r->left), t->int_shift);
	m->pitch = (int)pitch;
	if (base->m_type != GS_TEXTYPE_TEXTURE && base->m_type != GS_TEXTYPE_RENDER_TARGET)
		return 0;

	size = gs_texture_upload_size(base->m_format, (u32)(r->bottom - r->top), pitch);
	sb   = gs_gl_device_upload_buffer();
	if (!sb || size > gs_gl_stream_buffer_chunk_size(sb))
		return 0;

	map     = gs_gl_stream_buffer_map(sb, GS_GL_UPLOAD_ALIGN, size);
	m->bits = (u8*)map.pointer;

	/* the area, for the unmap */
	t->r_x        = r->left;
	t->r_y        = r->top;
	t->r_w        = r->right - r->left;
	t->r_h        = r->bottom - r->top;
	t->layer      = layer;
	t->map_offset = map.buffer_offset;
	return 1;
}

static void gs_texture_gl_unmap(gs_texture* base)
{
	gs_texture_gl* t = (gs_texture_gl*)base;
	gs_gl_stream_buffer* sb;
	u32 pitch;

	if (base->m_type != GS_TEXTYPE_TEXTURE && base->m_type != GS_TEXTYPE_RENDER_TARGET)
		return;

	gs_gl_device_commit_clear(base, 1);
	pitch = gs_gl_pitch_of((u32)t->r_w, t->int_shift);
	sb    = gs_gl_device_upload_buffer();
	gs_gl_stream_buffer_unmap(sb, pitch * (u32)t->r_h);
	gs_gl_stream_buffer_bind(sb);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch >> t->int_shift));
	glTextureSubImage2D(t->id, t->layer, t->r_x, t->r_y, t->r_w, t->r_h, t->int_format, t->int_type,
		(const void*)(uintptr_t)t->map_offset);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	gs_gl_stream_buffer_unbind(sb);
	base->m_needs_mipmaps_generated = 1;
}

static void gs_texture_gl_generate_mipmap(gs_texture* base)
{
	gs_gl_device_commit_clear(base, 1);
	glGenerateTextureMipmap(((gs_texture_gl*)base)->id);
}

static const gs_texture_ops s_gl_texture_ops = {gs_texture_gl_free, gs_texture_gl_update, gs_texture_gl_map,
	gs_texture_gl_unmap, gs_texture_gl_generate_mipmap};

gs_texture* gs_texture_gl_create(unsigned type, int width, int height, int levels, unsigned format)
{
	gs_texture_gl* t = (gs_texture_gl*)calloc(1, sizeof(*t));
	gs_gl_texture_format gl;
	if (!t)
		return NULL;

	/* (the GL takes no texture of no size) */
	gs_texture_base_init(&t->base, type, format, width > 1 ? width : 1, height > 1 ? height : 1);
	t->base.m_ops           = &s_gl_texture_ops;
	t->base.m_mipmap_levels = type == GS_TEXTYPE_TEXTURE ? levels : 1;

	gl = gs_gl_texture_format_of(format, gs_gl_device_colclip_float(), gs_gl_device_framebuffer_fetch());
	t->int_format = gl.format;
	t->int_type   = gl.type;
	t->int_shift  = gl.shift;

	glCreateTextures(GL_TEXTURE_2D, 1, &t->id);
	/* a palette reads its red as alpha too, as on DX, so the shader
	 * treats it as it treats an RGBA target */
	if (format == GS_TEX_UNORM8)
		glTextureParameteri(t->id, GL_TEXTURE_SWIZZLE_A, GL_RED);
	glTextureStorage2D(t->id, t->base.m_mipmap_levels, gl.internal_format, t->base.m_size.x, t->base.m_size.y);
	return &t->base;
}

/* --- read back ------------------------------------------------------- */

static void gs_download_texture_gl_free(gs_download_texture* base)
{
	gs_download_texture_gl* t = (gs_download_texture_gl*)base;
	if (t->buffer_id != 0)
	{
		if (t->sync)
			glDeleteSync(t->sync);
		if (base->m_map_pointer)
		{
			glBindBuffer(GL_PIXEL_PACK_BUFFER, t->buffer_id);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		}
		glDeleteBuffers(1, &t->buffer_id);
	}
	else
		memalign_free(t->cpu_buffer);
	free(t);
}

static void gs_download_texture_gl_copy(gs_download_texture* base, const gs_texture_rect* drc, gs_texture* stex,
	const gs_texture_rect* src, u32 src_level, int use_transfer_pitch)
{
	gs_download_texture_gl* t = (gs_download_texture_gl*)base;
	gs_texture_gl* tex        = (gs_texture_gl*)stex;
	gs_texture_transfer xfer;
	(void)src_level;

	gs_gl_device_commit_clear(stex, 1);
	gs_gl_device_finish_before_cpu_read();

	base->m_current_pitch = gs_texture_transfer_pitch(base->m_format,
		use_transfer_pitch ? (u32)(drc->right - drc->left) : base->m_width, GS_GL_UPLOAD_PITCH_ALIGN);
	xfer = gs_texture_transfer_of(base->m_format, base->m_current_pitch, (u32)drc->left, (u32)drc->top,
		(u32)(drc->right - drc->left), (u32)(drc->bottom - drc->top));

	glBindFramebuffer(GL_READ_FRAMEBUFFER, gs_gl_device_fbo_read());
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex->id, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1 << tex->int_shift);
	glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)gs_texture_row_length(base->m_format, base->m_current_pitch));

	if (!t->cpu_buffer)
		glBindBuffer(GL_PIXEL_PACK_BUFFER, t->buffer_id);
	/* (into the buffer bound, at the offset: the pointer is the offset) */
	glReadPixels(src->left, src->top, src->right - src->left, src->bottom - src->top, tex->int_format, tex->int_type,
		(void*)((uintptr_t)t->cpu_buffer + xfer.offset));
	glBindFramebuffer(GL_READ_FRAMEBUFFER, gs_gl_device_default_framebuffer());

	if (t->cpu_buffer)
		base->m_needs_flush = 0; /* read synchronously */
	else
	{
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		/* a fence to know when the GPU has copied */
		if (t->sync)
			glDeleteSync(t->sync);
		t->sync             = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		base->m_needs_flush = 1;
	}
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
}

static int gs_download_texture_gl_map(gs_download_texture* base, const gs_texture_rect* read_rc)
{
	(void)base;
	(void)read_rc;
	return 1; /* always mapped, or CPU memory */
}

static void gs_download_texture_gl_unmap(gs_download_texture* base)
{
	(void)base;
}

static void gs_download_texture_gl_flush(gs_download_texture* base)
{
	gs_download_texture_gl* t = (gs_download_texture_gl*)base;
	if (!base->m_needs_flush || !t->sync)
		return;
	base->m_needs_flush = 0;
	glClientWaitSync(t->sync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
	glDeleteSync(t->sync);
	t->sync = NULL;
}

static const gs_download_texture_ops s_gl_download_ops = {gs_download_texture_gl_free, gs_download_texture_gl_copy,
	gs_download_texture_gl_map, gs_download_texture_gl_unmap, gs_download_texture_gl_flush};

gs_download_texture* gs_download_texture_gl_create(u32 width, u32 height, unsigned format)
{
	const u32 size = gs_texture_buffer_size(format, width, height, GS_GL_UPLOAD_PITCH_ALIGN);
	gs_download_texture_gl* t;

	if ((GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage || GLAD_GL_EXT_buffer_storage)
		&& !gs_gl_device_download_pbo_disabled())
	{
		const GLbitfield flags     = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
		const GLbitfield map_flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT;
		GLuint buffer_id           = 0;
		u8* mapped;

		glGenBuffers(1, &buffer_id);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer_id);
		if (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage)
			glBufferStorage(GL_PIXEL_PACK_BUFFER, size, NULL, flags);
		else
			glBufferStorageEXT(GL_PIXEL_PACK_BUFFER, size, NULL, flags);
		mapped = (u8*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, size, map_flags);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		if (!mapped)
		{
			glDeleteBuffers(1, &buffer_id);
			return NULL;
		}

		t = (gs_download_texture_gl*)calloc(1, sizeof(*t));
		if (!t)
		{
			glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer_id);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
			glDeleteBuffers(1, &buffer_id);
			return NULL;
		}
		gs_download_texture_base_init(&t->base, width, height, format);
		t->base.m_ops         = &s_gl_download_ops;
		t->buffer_id          = buffer_id;
		t->buffer_size        = size;
		t->base.m_map_pointer = mapped;
		return &t->base;
	}

	/* glReadPixels into CPU memory */
	t = (gs_download_texture_gl*)calloc(1, sizeof(*t));
	if (!t)
		return NULL;
	t->cpu_buffer = (u8*)memalign_alloc(GS_GL_CPU_BUFFER_ALIGN, size);
	if (!t->cpu_buffer)
	{
		free(t);
		return NULL;
	}
	gs_download_texture_base_init(&t->base, width, height, format);
	t->base.m_ops         = &s_gl_download_ops;
	t->base.m_map_pointer = t->cpu_buffer;
	return &t->base;
}
