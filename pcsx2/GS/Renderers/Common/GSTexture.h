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

#pragma once

/* Copy count rows of copy_size bytes between buffers of differing row
 * pitch -- a texture upload where the staging pitch is the API's, not
 * the image's. One memcpy when the three agree. */
#include <string.h>
#include "common/Pcsx2Defs.h"
#include "GSTextureFormat.h"

static inline void GSStrideMemCpy(void* dst, size_t dst_stride, const void* src, size_t src_stride,
	size_t copy_size, size_t count)
{
	gs_stride_memcpy(dst, dst_stride, src, src_stride, copy_size, count);
}


#include "../../GSVector.h"

class GSTexture
{
public:
	struct GSMap
	{
		u8* bits;
		int pitch;
	};

	enum class Type : u8
	{
		Invalid = 0,
		RenderTarget = 1,
		DepthStencil,
		Texture,
		RWTexture
	};

	enum class Format : u8
	{
		Invalid = 0,  ///< Used for initialization
		Color,        ///< Standard (RGBA8) color texture
		ColorClip,     ///< Color texture with more bits for colclip emulation (RGBA16Unorm)
		DepthStencil, ///< Depth stencil texture
		UNorm8,       ///< A8UNorm texture for paletted textures and the OSD font
		UInt16,       ///< UInt16 texture for reading back 16-bit depth
		UInt32,       ///< UInt32 texture for reading back 24 and 32-bit depth
		PrimID,       ///< Prim ID tracking texture for date emulation
		BC1,          ///< BC1, aka DXT1 compressed texture for replacements
		BC2,          ///< BC2, aka DXT2/3 compressed texture for replacements
		BC3,          ///< BC3, aka DXT4/5 compressed texture for replacements
		BC7           ///< BC7, aka BPTC compressed texture for replacements
	};

	enum class State : u8
	{
		Dirty,
		Cleared,
		Invalidated
	};

	union ClearValue
	{
		u32 color;
		float depth;
	};

protected:
	GSVector2i m_size{};
	int m_mipmap_levels = 0;
	Type m_type = Type::Invalid;
	Format m_format = Format::Invalid;
	State m_state = State::Dirty;

	// frame number (arbitrary base) the texture was recycled on
	// different purpose than texture cache ages, do not attempt to merge
	u32 m_last_frame_used = 0;

	bool m_needs_mipmaps_generated = true;
	ClearValue m_clear_value = {};

public:
	GSTexture() = default;

	/* Not public, and not virtual either. The backends' virtual
	 * functions became the m_ops table in the C conversion and the
	 * destructor went with them - but a destructor that is neither
	 * virtual nor reachable through the table is a destructor that
	 * never runs: "delete t" on a GSTexture* ran this empty one and
	 * left the backend's image and its memory behind. Every texture
	 * the device ever deleted, on every backend, for the life of the
	 * run. m_ops->free is the one that deletes the real type, so
	 * gs_texture_free below is how a texture is destroyed, and this
	 * being protected is what makes the compiler say so. */
protected:
	~GSTexture() {}
public:

	/* What each backend's texture does differently, as a table of plain
	 * functions with the texture as the first argument; this is what the
	 * virtual functions were. Each backend fills one in its own .cpp and
	 * sets m_ops in its constructor, as its device does (gs_device_ops).
	 * A NULL generate_mipmap means the backend has none, as before. */
	const struct gs_texture_ops* m_ops = nullptr;

	void Free();
	bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0);
	bool Map(GSMap& m, const GSVector4i* r = NULL, int layer = 0);
	void Unmap();
	void GenerateMipmap();

	__fi int GetWidth() const { return m_size.x; }
	__fi int GetHeight() const { return m_size.y; }
	__fi const GSVector2i& GetSize() const { return m_size; }
	__fi GSVector4i GetRect() const { return GSVector4i::loadh(m_size); }

	__fi int GetMipmapLevels() const { return m_mipmap_levels; }
	__fi bool IsMipmap() const { return m_mipmap_levels > 1; }

	__fi Type GetType() const { return m_type; }
	__fi Format GetFormat() const { return m_format; }
	__fi bool IsCompressedFormat() const { return IsCompressedFormat(m_format); }

	/* The format math is C (GSTextureFormat.c) */
	static u32 GetCompressedBytesPerBlock(Format format) { return gs_texture_block_bytes(static_cast<unsigned>(format)); }
	static u32 GetCompressedBlockSize(Format format) { return gs_texture_block_size(static_cast<unsigned>(format)); }
	static u32 CalcUploadPitch(Format format, u32 width) { return gs_texture_upload_pitch(static_cast<unsigned>(format), width); }
	static u32 CalcUploadRowLengthFromPitch(Format format, u32 pitch) { return gs_texture_row_length(static_cast<unsigned>(format), pitch); }
	static u32 CalcUploadSize(Format format, u32 height, u32 pitch) { return gs_texture_upload_size(static_cast<unsigned>(format), height, pitch); }

	u32 GetCompressedBlockSize() const { return GetCompressedBlockSize(m_format); }
	u32 CalcUploadPitch(u32 width) const { return CalcUploadPitch(m_format, width); }
	u32 CalcUploadRowLengthFromPitch(u32 pitch) const { return CalcUploadRowLengthFromPitch(m_format, pitch); }
	u32 CalcUploadSize(u32 height, u32 pitch) const { return CalcUploadSize(m_format, height, pitch); }

	__fi bool IsRenderTargetOrDepthStencil() const
	{
		return (m_type >= Type::RenderTarget && m_type <= Type::DepthStencil);
	}
	__fi bool IsRenderTarget() const
	{
		return (m_type == Type::RenderTarget);
	}
	__fi bool IsDepthStencil() const
	{
		return (m_type == Type::DepthStencil);
	}
	__fi bool IsTexture() const
	{
		return (m_type == Type::Texture);
	}

	__fi State GetState() const { return m_state; }
	__fi void SetState(State state) { m_state = state; }

	__fi u32 GetLastFrameUsed() const { return m_last_frame_used; }
	void SetLastFrameUsed(u32 frame) { m_last_frame_used = frame; }

	__fi u32 GetClearColor() const { return m_clear_value.color; }
	__fi float GetClearDepth() const { return m_clear_value.depth; }
	__fi GSVector4 GetUNormClearColor() const { return GSVector4::unorm8(m_clear_value.color); }

	__fi void SetClearColor(u32 color)
	{
		m_state             = State::Cleared;
		m_clear_value.color = color;
	}
	__fi void SetClearDepth(float depth)
	{
		m_state = State::Cleared;
		m_clear_value.depth = depth;
	}

	void GenerateMipmapsIfNeeded();
	void ClearMipmapGenerationFlag() { m_needs_mipmaps_generated = false; }

	// Typical size of a RGBA texture
	u32 GetMemUsage() const { return m_size.x * m_size.y * (m_format == Format::UNorm8 ? 1 : 4); }

	// Helper routines for formats/types
	static bool IsCompressedFormat(Format format) { return gs_texture_is_compressed(static_cast<unsigned>(format)) != 0; }
};

static_assert(static_cast<unsigned>(GSTexture::Format::BC7) == GS_TEX_BC7 && static_cast<unsigned>(GSTexture::Format::PrimID) == GS_TEX_PRIMID
	&& static_cast<unsigned>(GSTexture::Format::ColorClip) == GS_TEX_COLOR_CLIP, "GSTexture::Format is gs_texture_format");

class GSDownloadTexture
{
public:
	GSDownloadTexture(u32 width, u32 height, GSTexture::Format format)
		: m_width(width), m_height(height), m_format(format) {}
	~GSDownloadTexture() = default;

	/* As GSTexture above: a table, not a vtable (gs_download_texture_ops). */
	const struct gs_download_texture_ops* m_ops = nullptr;

	void Free();

	/// Basically, this has dimensions only because of DX11.
	__fi u32 GetWidth() const { return m_width; }
	__fi u32 GetHeight() const { return m_height; }
	__fi GSTexture::Format GetFormat() const { return m_format; }
	__fi bool NeedsFlush() const { return m_needs_flush; }
	__fi bool IsMapped() const { return (m_map_pointer != nullptr); }
	__fi const u8* GetMapPointer() const { return m_map_pointer; }
	__fi u32 GetMapPitch() const { return m_current_pitch; }

	/// Calculates the pitch of a transfer.
	u32 GetTransferPitch(u32 width, u32 pitch_align) const
	{ return gs_texture_transfer_pitch(static_cast<unsigned>(m_format), width, pitch_align); }

	/// Calculates the size of the data you should transfer.
	void GetTransferSize(const GSVector4i& rc, u32* copy_offset, u32* copy_size, u32* copy_rows) const
	{
		const gs_texture_transfer t = gs_texture_transfer_of(static_cast<unsigned>(m_format), m_current_pitch,
			static_cast<u32>(rc.x), static_cast<u32>(rc.y), static_cast<u32>(rc.width()), static_cast<u32>(rc.height()));
		*copy_offset = t.offset;
		*copy_size   = t.size;
		*copy_rows   = t.rows;
	}

	/// Queues a copy from the specified texture to this buffer.
	/// Does not complete immediately, you should flush before accessing the buffer.
	/// use_transfer_pitch should be true if there's only a single texture being copied to this buffer before
	/// it will be used. This allows the image to be packed tighter together, and buffer reuse.
	void CopyFromTexture(
		const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch = true);

	/// Maps the texture into the CPU address space, enabling it to read the contents.
	/// The Map call may not perform synchronization. If the contents of the staging texture
	/// has been updated by a CopyFromTexture() call, you must call Flush() first.
	/// If persistent mapping is supported in the backend, this may be a no-op.
	bool Map(const GSVector4i& read_rc);

	/// Unmaps the CPU-readable copy of the texture. May be a no-op on backends which
	/// support persistent-mapped buffers.
	void Unmap();

	/// Flushes pending writes from the CPU to the GPU, and reads from the GPU to the CPU.
	/// This may cause a command buffer submit depending on if one has occurred between the last
	/// call to CopyFromTexture() and the Flush() call.
	void Flush();

	/// Reads the specified rectangle from the staging texture to out_ptr, with the specified stride
	/// (length in bytes of each row). CopyFromTexture() must be called first. The contents of any
	/// texels outside of the rectangle used for CopyFromTexture is undefined.
	bool ReadTexels(const GSVector4i& rc, void* out_ptr, u32 out_stride);

	/// Returns what the size of the specified texture would be, in bytes.
	static u32 GetBufferSize(u32 width, u32 height, GSTexture::Format format, u32 pitch_align = 1)
	{ return gs_texture_buffer_size(static_cast<unsigned>(format), width, height, pitch_align); }

protected:
	u32 m_width;
	u32 m_height;
	GSTexture::Format m_format;

	const u8* m_map_pointer = nullptr;
	u32 m_current_pitch = 0;

	bool m_needs_flush = false;
};

/* --- the tables the two above answer through ----------------------------
 * GS_TEXTURE_OPS_DEFINE(GSTexture11, d3d11) writes s_d3d11_texture_ops
 * out of GSTexture11's member functions, and the download one likewise;
 * the thunks are members of a struct the class befriends, so a function
 * that was protected stays protected. */
struct gs_texture_ops
{
	void (*free)(GSTexture* t);
	bool (*update)(GSTexture* t, const GSVector4i* r, const void* data, int pitch, int layer);
	bool (*map)(GSTexture* t, GSTexture::GSMap* m, const GSVector4i* r, int layer);
	void (*unmap)(GSTexture* t);
	void (*generate_mipmap)(GSTexture* t);
};

struct gs_download_texture_ops
{
	void (*free)(GSDownloadTexture* t);
	void (*copy_from_texture)(GSDownloadTexture* t, const GSVector4i* drc, GSTexture* stex, const GSVector4i* src, u32 src_level, bool use_transfer_pitch);
	bool (*map)(GSDownloadTexture* t, const GSVector4i* read_rc);
	void (*unmap)(GSDownloadTexture* t);
	void (*flush)(GSDownloadTexture* t);
};

__forceinline_odr void GSTexture::Free() { m_ops->free(this); }
/* What "delete t" was meant to be: dispatches to the backend's free,
 * which deletes the real type and so runs the real destructor. */
__forceinline_odr void gs_texture_free(GSTexture* t) { if (t) t->Free(); }
__forceinline_odr bool GSTexture::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{ return m_ops->update(this, &r, data, pitch, layer); }
__forceinline_odr bool GSTexture::Map(GSMap& m, const GSVector4i* r, int layer)
{ return m_ops->map(this, &m, r, layer); }
__forceinline_odr void GSTexture::Unmap() { m_ops->unmap(this); }
__forceinline_odr void GSTexture::GenerateMipmap() { if (m_ops->generate_mipmap) m_ops->generate_mipmap(this); }
__forceinline_odr void GSTexture::GenerateMipmapsIfNeeded()
{
	if (!m_needs_mipmaps_generated || m_mipmap_levels <= 1 || IsCompressedFormat())
		return;
	m_needs_mipmaps_generated = false;
	GenerateMipmap();
}

/* A download texture, like a texture, is freed by its backend: the base
 * has no virtual destructor, so a plain delete would run the wrong one. */
__forceinline_odr void gs_download_texture_free(GSDownloadTexture* t);

__forceinline_odr void GSDownloadTexture::Free() { m_ops->free(this); }
__forceinline_odr void gs_download_texture_free(GSDownloadTexture* t) { if (t) t->Free(); }
__forceinline_odr void GSDownloadTexture::CopyFromTexture(const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
{ m_ops->copy_from_texture(this, &drc, stex, &src, src_level, use_transfer_pitch); }
__forceinline_odr bool GSDownloadTexture::Map(const GSVector4i& read_rc) { return m_ops->map(this, &read_rc); }
__forceinline_odr void GSDownloadTexture::Unmap() { m_ops->unmap(this); }
__forceinline_odr void GSDownloadTexture::Flush() { m_ops->flush(this); }
__forceinline_odr bool GSDownloadTexture::ReadTexels(const GSVector4i& rc, void* out_ptr, u32 out_stride)
{
	u32 offset, size, rows;
	if (m_needs_flush)
		Flush();
	if (!Map(rc))
		return false;
	GetTransferSize(rc, &offset, &size, &rows);
	gs_stride_memcpy(out_ptr, out_stride, m_map_pointer + offset, m_current_pitch, size, rows);
	return true;
}

#define GS_TEXTURE_OPS_DEFINE(klass, tag) \
struct klass##_ops_access { \
	static void tag##_tex_free(GSTexture* t) { delete static_cast<klass*>(t); } \
	static bool tag##_tex_update(GSTexture* t, const GSVector4i* r, const void* data, int pitch, int layer) { return static_cast<klass*>(t)->Update(*r, data, pitch, layer); } \
	static bool tag##_tex_map(GSTexture* t, GSTexture::GSMap* m, const GSVector4i* r, int layer) { return static_cast<klass*>(t)->Map(*m, r, layer); } \
	static void tag##_tex_unmap(GSTexture* t) { static_cast<klass*>(t)->Unmap(); } \
}; \
static const struct gs_texture_ops s_##tag##_texture_ops = { \
	klass##_ops_access::tag##_tex_free, klass##_ops_access::tag##_tex_update, \
	klass##_ops_access::tag##_tex_map, klass##_ops_access::tag##_tex_unmap, NULL }

#define GS_TEXTURE_OPS_DEFINE_MIPMAP(klass, tag) \
struct klass##_ops_access { \
	static void tag##_tex_free(GSTexture* t) { delete static_cast<klass*>(t); } \
	static bool tag##_tex_update(GSTexture* t, const GSVector4i* r, const void* data, int pitch, int layer) { return static_cast<klass*>(t)->Update(*r, data, pitch, layer); } \
	static bool tag##_tex_map(GSTexture* t, GSTexture::GSMap* m, const GSVector4i* r, int layer) { return static_cast<klass*>(t)->Map(*m, r, layer); } \
	static void tag##_tex_unmap(GSTexture* t) { static_cast<klass*>(t)->Unmap(); } \
	static void tag##_tex_generate_mipmap(GSTexture* t) { static_cast<klass*>(t)->GenerateMipmap(); } \
}; \
static const struct gs_texture_ops s_##tag##_texture_ops = { \
	klass##_ops_access::tag##_tex_free, klass##_ops_access::tag##_tex_update, \
	klass##_ops_access::tag##_tex_map, klass##_ops_access::tag##_tex_unmap, \
	klass##_ops_access::tag##_tex_generate_mipmap }

#define GS_DOWNLOAD_TEXTURE_OPS_DEFINE(klass, tag) \
struct klass##_ops_access { \
	static void tag##_dl_free(GSDownloadTexture* t) { delete static_cast<klass*>(t); } \
	static void tag##_dl_copy_from_texture(GSDownloadTexture* t, const GSVector4i* drc, GSTexture* stex, const GSVector4i* src, u32 src_level, bool use_transfer_pitch) { static_cast<klass*>(t)->CopyFromTexture(*drc, stex, *src, src_level, use_transfer_pitch); } \
	static bool tag##_dl_map(GSDownloadTexture* t, const GSVector4i* read_rc) { return static_cast<klass*>(t)->Map(*read_rc); } \
	static void tag##_dl_unmap(GSDownloadTexture* t) { static_cast<klass*>(t)->Unmap(); } \
	static void tag##_dl_flush(GSDownloadTexture* t) { static_cast<klass*>(t)->Flush(); } \
}; \
static const struct gs_download_texture_ops s_##tag##_download_texture_ops = { \
	klass##_ops_access::tag##_dl_free, klass##_ops_access::tag##_dl_copy_from_texture, \
	klass##_ops_access::tag##_dl_map, klass##_ops_access::tag##_dl_unmap, \
	klass##_ops_access::tag##_dl_flush }
