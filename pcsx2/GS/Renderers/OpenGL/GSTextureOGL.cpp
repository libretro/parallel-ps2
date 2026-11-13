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

#include <limits.h>
#include <memalign.h>
#include "GSDeviceOGL.h"
#include "GSTextureOGL.h"
#include "GLState.h"
#include "GS/GSExtra.h"
#include "common/Align.h"

#include <libretro.h>

#define GL_DEFAULT_FRAMEBUFFER hw_render.get_current_framebuffer()

// Looking across a range of GPUs, the optimal copy alignment for Vulkan drivers seems
// to be between 1 (AMD/NV) and 64 (Intel). So, we'll go with 64 here.
static constexpr u32 TEXTURE_UPLOAD_ALIGNMENT = 64;

// The pitch alignment must be less or equal to the upload alignment.
// We need 32 here for AVX2, so 64 is also fine.
static constexpr u32 TEXTURE_UPLOAD_PITCH_ALIGNMENT = 64;

/* The table GSTexture calls this one through (gs_texture_ops):
 * every entry is this class's function of that name. */
GS_TEXTURE_OPS_DEFINE_MIPMAP(GSTextureOGL, opengl);

GSTextureOGL::GSTextureOGL(Type type, int width, int height, int levels, Format format)
{
	m_ops = &s_opengl_texture_ops;
	// OpenGL didn't like dimensions of size 0
	m_size.x = pcsx2_max_i(1, width);
	m_size.y = pcsx2_max_i(1, height);
	m_format = format;
	m_type = type;
	m_texture_id = 0;
	m_mipmap_levels = 1;
	int gl_fmt = 0;

	// Bunch of constant parameter
	switch (m_format)
	{
		// 1 Channel integer
		case Format::PrimID:
			gl_fmt = GL_R32F;
			m_int_format = GL_RED;
			m_int_type = GL_INT;
			m_int_shift = 2;
			break;
		case Format::UInt32:
			gl_fmt = GL_R32UI;
			m_int_format = GL_RED_INTEGER;
			m_int_type = GL_UNSIGNED_INT;
			m_int_shift = 2;
			break;
		case Format::UInt16:
			gl_fmt = GL_R16UI;
			m_int_format = GL_RED_INTEGER;
			m_int_type = GL_UNSIGNED_SHORT;
			m_int_shift = 1;
			break;

		// 1 Channel normalized
		case Format::UNorm8:
			gl_fmt = GL_R8;
			m_int_format = GL_RED;
			m_int_type = GL_UNSIGNED_BYTE;
			m_int_shift = 0;
			break;

		// 4 channel normalized
		case Format::Color:
			gl_fmt = GL_RGBA8;
			m_int_format = GL_RGBA;
			m_int_type = GL_UNSIGNED_BYTE;
			m_int_shift = 2;
			break;

		// 4 channel float
		case Format::ColorClip:
			if (GSDeviceOGL::GetInstance()->IsColClipFloat())
			{
				// 24 bits of mantissa hold every 16-bit integer, and its sums, exactly.
				gl_fmt = GL_RGBA32F;
				m_int_format = GL_RGBA;
				m_int_type = GL_FLOAT;
				m_int_shift = 4;
			}
			else
			{
				gl_fmt = GL_RGBA16;
				m_int_format = GL_RGBA;
				m_int_type = GL_UNSIGNED_SHORT;
				m_int_shift = 3;
			}
			break;

		// Depth buffer
		case Format::DepthStencil:
		{
			if (!g_gs_device->Features().framebuffer_fetch)
			{
				gl_fmt = GL_DEPTH32F_STENCIL8;
				m_int_format = GL_DEPTH_STENCIL;
				m_int_type = GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
				m_int_shift = 3; // 4 bytes for depth + 4 bytes for stencil by texels
			}
			else
			{
				gl_fmt = GL_DEPTH_COMPONENT32F;
				m_int_format = GL_DEPTH_COMPONENT;
				m_int_type = GL_FLOAT;
				m_int_shift = 2;
			}
		}
		break;

		case Format::BC1:
			gl_fmt = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
			m_int_format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
			m_int_type = GL_UNSIGNED_BYTE;
			m_int_shift = 1;
			break;

		case Format::BC2:
			gl_fmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
			m_int_format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
			m_int_type = GL_UNSIGNED_BYTE;
			m_int_shift = 1;
			break;

		case Format::BC3:
			gl_fmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
			m_int_format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
			m_int_type = GL_UNSIGNED_BYTE;
			m_int_shift = 1;
			break;

		case Format::BC7:
			gl_fmt = GL_COMPRESSED_RGBA_BPTC_UNORM_ARB;
			m_int_format = GL_COMPRESSED_RGBA_BPTC_UNORM_ARB;
			m_int_type = GL_UNSIGNED_BYTE;
			m_int_shift = 1;
			break;

		case Format::Invalid:
			m_int_format = 0;
			m_int_type = 0;
			m_int_shift = 0;
			break;
	}

	// Only 32 bits input texture will be supported for mipmap
	if (m_type == Type::Texture)
		m_mipmap_levels = levels;

	// Create a gl object (texture isn't allocated here)
	glCreateTextures(GL_TEXTURE_2D, 1, &m_texture_id);
	if (m_format == Format::UNorm8)
	{
		// Emulate DX behavior, beside it avoid special code in shader to differentiate
		// palette texture from a GL_RGBA target or a GL_R texture.
		glTextureParameteri(m_texture_id, GL_TEXTURE_SWIZZLE_A, GL_RED);
	}

	glTextureStorage2D(m_texture_id, m_mipmap_levels, gl_fmt, m_size.x, m_size.y);
}

GSTextureOGL::~GSTextureOGL()
{
	// Textures aren't cleared from attachments on deletion.
	GSDeviceOGL::GetInstance()->OMUnbindTexture(this);

	// But they are unbound.
	for (GLuint& tex : g_gl_state.tex_unit)
	{
		if (m_texture_id == tex)
			tex = 0;
	}

	glDeleteTextures(1, &m_texture_id);
}

bool GSTextureOGL::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
	if (layer >= m_mipmap_levels)
		return true;

	// Default upload path for the texture is the Map/Unmap
	// This path is mostly used for palette. But also for texture that could
	// overflow the pbo buffer
	// Data upload is rather small typically 64B or 1024B. So don't bother with PBO
	// and directly send the data to the GL synchronously
	GSDeviceOGL::GetInstance()->CommitClear(this, true);

	const u32 preferred_pitch = pcsx2_align_up_pow2_u32(r.width() << m_int_shift, TEXTURE_UPLOAD_PITCH_ALIGNMENT);
	const u32 map_size = r.height() * preferred_pitch;

	// Don't use PBOs for huge texture uploads, let the driver sort it out.
	// Otherwise we'll just be syncing, or worse, crashing because the PBO routine above isn't great.
	gs_gl_stream_buffer* const sb = GSDeviceOGL::GetInstance()->GetTextureUploadBuffer();
	if (IsCompressedFormat())
	{
		const u32 row_length = CalcUploadRowLengthFromPitch(pitch);
		const u32 upload_size = CalcUploadSize(r.height(), pitch);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
		glCompressedTextureSubImage2D(m_texture_id, layer, r.x, r.y, r.width(), r.height(), m_int_format, upload_size, data);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	}
	else if (!sb || map_size > gs_gl_stream_buffer_chunk_size(sb))
	{
		glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch >> m_int_shift);
		glTextureSubImage2D(m_texture_id, layer, r.x, r.y, r.width(), r.height(), m_int_format, m_int_type, data);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); // Restore default behavior
	}
	else
	{
		const gs_gl_stream_map map = gs_gl_stream_buffer_map(sb, TEXTURE_UPLOAD_ALIGNMENT, map_size);
		GSStrideMemCpy(map.pointer, preferred_pitch, data, pitch, r.width() << m_int_shift, r.height());
		gs_gl_stream_buffer_unmap(sb, map_size);
		gs_gl_stream_buffer_bind(sb);

		const u32 row_length = CalcUploadRowLengthFromPitch(preferred_pitch);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);

		glTextureSubImage2D(m_texture_id, layer, r.x, r.y, r.width(), r.height(), m_int_format, m_int_type,
			reinterpret_cast<void*>(static_cast<uintptr_t>(map.buffer_offset)));

		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

		gs_gl_stream_buffer_unbind(sb);
	}

	m_needs_mipmaps_generated = true;

	return true;
}

bool GSTextureOGL::Map(GSMap& m, const GSVector4i* _r, int layer)
{
	if (layer >= m_mipmap_levels || IsCompressedFormat())
		return false;

	GSDeviceOGL::GetInstance()->CommitClear(this, true);

	GSVector4i r = _r ? *_r : GSVector4i(0, 0, m_size.x, m_size.y);

	const u32 pitch = pcsx2_align_up_pow2_u32(r.width() << m_int_shift, TEXTURE_UPLOAD_PITCH_ALIGNMENT);
	m.pitch = pitch;

	if (m_type == Type::Texture || m_type == Type::RenderTarget)
	{
		const u32 upload_size = CalcUploadSize(r.height(), pitch);
		gs_gl_stream_buffer* sb = GSDeviceOGL::GetInstance()->GetTextureUploadBuffer();
		if (!sb || upload_size > gs_gl_stream_buffer_chunk_size(sb))
			return false;

		const gs_gl_stream_map map = gs_gl_stream_buffer_map(sb, TEXTURE_UPLOAD_ALIGNMENT, upload_size);
		m.bits = static_cast<u8*>(map.pointer);

		// Save the area for the unmap
		m_r_x = r.x;
		m_r_y = r.y;
		m_r_w = r.width();
		m_r_h = r.height();
		m_layer = layer;
		m_map_offset = map.buffer_offset;

		return true;
	}

	return false;
}

void GSTextureOGL::Unmap()
{
	if (m_type == Type::Texture || m_type == Type::RenderTarget)
	{
		GSDeviceOGL::GetInstance()->CommitClear(this, true);

		const u32 pitch = pcsx2_align_up_pow2_u32(m_r_w << m_int_shift, TEXTURE_UPLOAD_PITCH_ALIGNMENT);
		const u32 upload_size = pitch * m_r_h;
		gs_gl_stream_buffer* sb = GSDeviceOGL::GetInstance()->GetTextureUploadBuffer();
		gs_gl_stream_buffer_unmap(sb, upload_size);
		gs_gl_stream_buffer_bind(sb);

		const u32 row_length = CalcUploadRowLengthFromPitch(pitch);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);

		glTextureSubImage2D(m_texture_id, m_layer, m_r_x, m_r_y, m_r_w, m_r_h, m_int_format, m_int_type,
			reinterpret_cast<void*>(static_cast<uintptr_t>(m_map_offset)));

		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

		gs_gl_stream_buffer_unbind(sb);

		m_needs_mipmaps_generated = true;
	}
}

void GSTextureOGL::GenerateMipmap()
{
	GSDeviceOGL::GetInstance()->CommitClear(this, true);
	glGenerateTextureMipmap(m_texture_id);
}

/* The table GSDownloadTexture calls this one through. */
GS_DOWNLOAD_TEXTURE_OPS_DEFINE(GSDownloadTextureOGL, opengl_dl);

GSDownloadTextureOGL::GSDownloadTextureOGL(u32 width, u32 height, GSTexture::Format format)
	: GSDownloadTexture(width, height, format)
{
	m_ops = &s_opengl_dl_download_texture_ops;
}

GSDownloadTextureOGL::~GSDownloadTextureOGL()
{
	if (m_buffer_id != 0)
	{
		if (m_sync)
			glDeleteSync(m_sync);

		if (m_map_pointer)
		{
			glBindBuffer(GL_PIXEL_PACK_BUFFER, m_buffer_id);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		}

		glDeleteBuffers(1, &m_buffer_id);
	}
	else if (m_cpu_buffer)
	{
		memalign_free(m_cpu_buffer);
	}
}

GSDownloadTextureOGL* GSDownloadTextureOGL::Create(u32 width, u32 height, GSTexture::Format format)
{
	const u32 buffer_size = GetBufferSize(width, height, format, TEXTURE_UPLOAD_PITCH_ALIGNMENT);

	const bool use_buffer_storage = (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage || GLAD_GL_EXT_buffer_storage) &&
									!GSDeviceOGL::GetInstance()->IsDownloadPBODisabled();
	if (use_buffer_storage)
	{
		GLuint buffer_id;
		glGenBuffers(1, &buffer_id);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer_id);

		const u32 flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
		const u32 map_flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT;

		if (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage)
			glBufferStorage(GL_PIXEL_PACK_BUFFER, buffer_size, nullptr, flags);
		else if (GLAD_GL_EXT_buffer_storage)
			glBufferStorageEXT(GL_PIXEL_PACK_BUFFER, buffer_size, nullptr, flags);

		u8* buffer_map = static_cast<u8*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, buffer_size, map_flags));

		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

		if (!buffer_map)
		{
			glDeleteBuffers(1, &buffer_id);
			return nullptr;
		}

		GSDownloadTextureOGL* ret = new GSDownloadTextureOGL(width, height, format);
		ret->m_buffer_id = buffer_id;
		ret->m_buffer_size = buffer_size;
		ret->m_map_pointer = buffer_map;
		return ret;
	}

	// Fallback to glReadPixels() + CPU buffer.
	u8* cpu_buffer = static_cast<u8*>(memalign_alloc(VECTOR_ALIGNMENT, buffer_size));
	if (!cpu_buffer)
		return nullptr;

	GSDownloadTextureOGL* ret = new GSDownloadTextureOGL(width, height, format);
	ret->m_cpu_buffer = cpu_buffer;
	ret->m_map_pointer = cpu_buffer;
	return ret;
}

void GSDownloadTextureOGL::CopyFromTexture(
	const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
{
	GSTextureOGL* const glTex = static_cast<GSTextureOGL*>(stex);
	GSDeviceOGL::GetInstance()->CommitClear(glTex, true);
	GSDeviceOGL::GetInstance()->FinishBeforeCpuRead();

	u32 copy_offset, copy_size, copy_rows;
	m_current_pitch = GetTransferPitch(use_transfer_pitch ? static_cast<u32>(drc.width()) : m_width, TEXTURE_UPLOAD_PITCH_ALIGNMENT);
	GetTransferSize(drc, &copy_offset, &copy_size, &copy_rows);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, GSDeviceOGL::GetInstance()->GetFBORead());
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTex->GetID(), 0);

	glPixelStorei(GL_PACK_ALIGNMENT, 1u << glTex->GetIntShift());
	glPixelStorei(GL_PACK_ROW_LENGTH, GSTexture::CalcUploadRowLengthFromPitch(m_format, m_current_pitch));

	// Read to PBO.
	if (!m_cpu_buffer)
		glBindBuffer(GL_PIXEL_PACK_BUFFER, m_buffer_id);

	glReadPixels(src.left, src.top, src.width(), src.height(), glTex->GetIntFormat(), glTex->GetIntType(), m_cpu_buffer + copy_offset);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, GL_DEFAULT_FRAMEBUFFER);

	if (m_cpu_buffer)
	{
		// If using CPU buffers, we never need to flush.
		m_needs_flush = false;
	}
	else
	{
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

		// Create a sync object so we know when the GPU is done copying.
		if (m_sync)
			glDeleteSync(m_sync);

		m_sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		m_needs_flush = true;
	}

	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
}

bool GSDownloadTextureOGL::Map(const GSVector4i& read_rc)
{
	// Either always mapped, or CPU buffer.
	return true;
}

void GSDownloadTextureOGL::Unmap()
{
	// Either always mapped, or CPU buffer.
}

void GSDownloadTextureOGL::Flush()
{
	// If we're using CPU buffers, we did the readback synchronously...
	if (!m_needs_flush || !m_sync)
		return;

	m_needs_flush = false;

	glClientWaitSync(m_sync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
	glDeleteSync(m_sync);
	m_sync = {};
}
