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

#ifndef GS_TEXTURE_BASE_H
#define GS_TEXTURE_BASE_H

#include "common/Pcsx2Defs.h"
#include "GSTextureFormat.h"

#ifdef __cplusplus
#include "GS/GSVector.h"

/* GSTexture's Type, Format and State; in C the fields are their u8 */
enum class GSTextureType : u8
{
	Invalid = 0,
	RenderTarget = 1,
	DepthStencil,
	Texture,
	RWTexture
};

enum class GSTextureFormatKind : u8
{
	Invalid = 0, /* used for initialization */
	Color,       /* RGBA8 */
	ColorClip,   /* more bits for colclip emulation (RGBA16Unorm) */
	DepthStencil,
	UNorm8,      /* A8UNorm: palettes and the OSD font */
	UInt16,      /* 16-bit depth read back */
	UInt32,      /* 24 and 32-bit depth read back */
	PrimID,      /* DATE's primitive IDs */
	BC1,         /* DXT1, for replacements */
	BC2,         /* DXT2/3 */
	BC3,         /* DXT4/5 */
	BC7          /* BPTC */
};

enum class GSTextureState : u8
{
	Dirty,
	Cleared,
	Invalidated
};
#else
#ifndef GS_INT2_DEFINED
#define GS_INT2_DEFINED
typedef struct gs_int2
{
	int x, y;
} gs_int2;
#endif
#endif

/* gs_texture.m_type's values */
enum gs_texture_type
{
	GS_TEXTYPE_INVALID = 0,
	GS_TEXTYPE_RENDER_TARGET,
	GS_TEXTYPE_DEPTH_STENCIL,
	GS_TEXTYPE_TEXTURE,
	GS_TEXTYPE_RW_TEXTURE
};

/* gs_texture.m_state's values */
enum gs_texture_state
{
	GS_TEXSTATE_DIRTY = 0,
	GS_TEXSTATE_CLEARED,
	GS_TEXSTATE_INVALIDATED
};

/* Where a mapped texture's texels are */
typedef struct gs_texture_map
{
	u8* bits;
	int pitch;
} gs_texture_map;

/* A rectangle of texels: left and top in, right and bottom out (the
 * layout of GSVector4i) */
typedef struct gs_texture_rect
{
	int left, top, right, bottom;
} gs_texture_rect;

struct gs_texture;

/* What each backend's texture does differently, as a table of plain
 * functions with the texture as the first argument (what the virtual
 * functions were). A NULL generate_mipmap: the backend has none. */
typedef struct gs_texture_ops
{
	void (*free)(struct gs_texture* t);
	int (*update)(struct gs_texture* t, const gs_texture_rect* r, const void* data, int pitch, int layer);
	int (*map)(struct gs_texture* t, gs_texture_map* m, const gs_texture_rect* r, int layer);
	void (*unmap)(struct gs_texture* t);
	void (*generate_mipmap)(struct gs_texture* t);
} gs_texture_ops;

/* A texture's common part, first in every backend's texture (C++:
 * GSTexture derives from it). Free it with m_ops->free. */
typedef struct gs_texture
{
	const gs_texture_ops* m_ops;
#ifdef __cplusplus
	GSVector2i m_size;
	int m_mipmap_levels;
	GSTextureType m_type;
	GSTextureFormatKind m_format;
	GSTextureState m_state;
	bool m_needs_mipmaps_generated;
#else
	gs_int2 m_size;
	int m_mipmap_levels;
	u8 m_type;   /* gs_texture_type */
	u8 m_format; /* gs_texture_format */
	u8 m_state;  /* gs_texture_state */
	u8 m_needs_mipmaps_generated;
#endif
	/* frame number (arbitrary base) the texture was recycled on; not the
	 * texture cache's ages */
	u32 m_last_frame_used;
	union
	{
		u32 color;
		float depth;
	} m_clear_value;
} gs_texture;

/* A texture's copy in CPU-readable memory, read back into: the common
 * part, first in every backend's (C++: GSDownloadTexture derives from it) */
struct gs_download_texture;

typedef struct gs_download_texture_ops
{
	void (*free)(struct gs_download_texture* t);
	/* Queues a copy of src (level src_level) of stex into drc; flush
	 * before reading. use_transfer_pitch packs the rows to the copy's
	 * width (one copy before the read). */
	void (*copy_from_texture)(struct gs_download_texture* t, const gs_texture_rect* drc, gs_texture* stex,
		const gs_texture_rect* src, u32 src_level, int use_transfer_pitch);
	int (*map)(struct gs_download_texture* t, const gs_texture_rect* read_rc);
	void (*unmap)(struct gs_download_texture* t);
	void (*flush)(struct gs_download_texture* t);
} gs_download_texture_ops;

typedef struct gs_download_texture
{
	const gs_download_texture_ops* m_ops;
	u32 m_width;
	u32 m_height;
	const u8* m_map_pointer;
	u32 m_current_pitch;
#ifdef __cplusplus
	GSTextureFormatKind m_format;
	bool m_needs_flush;
#else
	u8 m_format; /* gs_texture_format */
	u8 m_needs_flush;
#endif
} gs_download_texture;

#ifdef __cplusplus
extern "C" {
#endif

/* The common part as a new texture of a backend starts: no ops yet,
 * Dirty, mipmaps to generate */
void gs_texture_base_init(gs_texture* t, unsigned type, unsigned format, int width, int height);
/* The common part as a new download texture starts: no ops yet, unmapped */
void gs_download_texture_base_init(gs_download_texture* t, u32 width, u32 height, unsigned format);

#ifdef __cplusplus
}
#endif

#endif
