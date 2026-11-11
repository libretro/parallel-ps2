/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
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

/* The GS options' enumerations, as C. In C++ each is also a type name; in
 * C, typedefs below make them one. */

#ifndef PCSX2_GSOPTIONENUMS_H
#define PCSX2_GSOPTIONENUMS_H

/* The renderer: AUTO picks one for the frontend's video API. */
enum GSRendererType
{
	GS_RENDERER_AUTO = -1,
	GS_RENDERER_DX11 = 3,
	GS_RENDERER_OGL = 12,
	GS_RENDERER_SW = 13,
	GS_RENDERER_VK = 14,
	GS_RENDERER_DX12 = 15,
	GS_RENDERER_PARALLEL_GS = 18
};

/* Deinterlacing; AUTOMATIC follows the game database. */
enum GSInterlaceMode
{
	GS_INTERLACE_AUTOMATIC,
	GS_INTERLACE_OFF,
	GS_INTERLACE_WEAVE_TFF,
	GS_INTERLACE_WEAVE_BFF,
	GS_INTERLACE_BOB_TFF,
	GS_INTERLACE_BOB_BFF,
	GS_INTERLACE_BLEND_TFF,
	GS_INTERLACE_BLEND_BFF,
	GS_INTERLACE_ADAPTIVE_TFF,
	GS_INTERLACE_ADAPTIVE_BFF,
	GS_INTERLACE_COUNT
};

/* Bilinear filtering. The order keeps older configs' values. */
enum BiFiltering
{
	GS_BIFILTER_NEAREST,
	GS_BIFILTER_FORCED,
	GS_BIFILTER_PS2,
	GS_BIFILTER_FORCED_BUT_SPRITE
};

/* How 32-bit depth is narrowed to 24. */
enum GSLimit24BitDepth
{
	GS_LIMIT24_DISABLED,
	GS_LIMIT24_PRIORITIZE_UPPER,
	GS_LIMIT24_PRIORITIZE_LOWER
};

/* Hardware mipmapping. */
enum GSHWMipmapMode
{
	GS_MIPMAP_DISABLED,
	GS_MIPMAP_ENABLED,
	GS_MIPMAP_ALL_LEVELS,
	GS_MIPMAP_UNCLAMPED,
	GS_MIPMAP_COUNT
};

/* Trilinear filtering; AUTOMATIC follows the game database. */
enum TriFiltering
{
	GS_TRIFILTER_AUTOMATIC = -1,
	GS_TRIFILTER_OFF,
	GS_TRIFILTER_PS2,
	GS_TRIFILTER_FORCED
};

/* How much blending the hardware renderers do exactly. */
enum AccBlendLevel
{
	GS_BLEND_MINIMUM,
	GS_BLEND_BASIC,
	GS_BLEND_MEDIUM,
	GS_BLEND_HIGH,
	GS_BLEND_FULL,
	GS_BLEND_MAXIMUM
};

/* How much of a texture is uploaded ahead. */
enum TexturePreloadingLevel
{
	GS_PRELOAD_OFF,
	GS_PRELOAD_PARTIAL,
	GS_PRELOAD_FULL
};

/* Whether and how GPU results are read back. */
enum GSHardwareDownloadMode
{
	GS_DOWNLOAD_ENABLED,
	GS_DOWNLOAD_NO_READBACKS,
	GS_DOWNLOAD_UNSYNCHRONIZED,
	GS_DOWNLOAD_DISABLED
};

/* Textures found inside render targets. */
enum GSTextureInRtMode
{
	GS_TEXINRT_DISABLED,
	GS_TEXINRT_INSIDE_TARGETS,
	GS_TEXINRT_MERGE_TARGETS
};

/* The half-pixel offset hack when upscaling. */
enum GSHalfPixelOffset
{
	GS_HPO_OFF = 0,
	GS_HPO_NORMAL,
	GS_HPO_SPECIAL,
	GS_HPO_SPECIAL_AGGRESSIVE,
	GS_HPO_NATIVE,
	GS_HPO_COUNT
};

/* Native-resolution scaling of post effects. */
enum GSNativeScaling
{
	GS_NATIVE_SCALING_NORMAL = 0,
	GS_NATIVE_SCALING_AGGRESSIVE,
	GS_NATIVE_SCALING_OFF,
	GS_NATIVE_SCALING_COUNT
};

#ifndef __cplusplus
typedef enum GSRendererType GSRendererType;
typedef enum GSInterlaceMode GSInterlaceMode;
typedef enum BiFiltering BiFiltering;
typedef enum GSLimit24BitDepth GSLimit24BitDepth;
typedef enum GSHWMipmapMode GSHWMipmapMode;
typedef enum TriFiltering TriFiltering;
typedef enum AccBlendLevel AccBlendLevel;
typedef enum TexturePreloadingLevel TexturePreloadingLevel;
typedef enum GSHardwareDownloadMode GSHardwareDownloadMode;
typedef enum GSTextureInRtMode GSTextureInRtMode;
typedef enum GSHalfPixelOffset GSHalfPixelOffset;
typedef enum GSNativeScaling GSNativeScaling;
#endif

#endif
