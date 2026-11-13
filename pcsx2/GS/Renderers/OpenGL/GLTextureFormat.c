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

#include "GLTextureFormat.h"

/* By gs_texture_format; the colclip and depth entries are the defaults
 * the flags below swap */
static const gs_gl_texture_format s_formats[GS_TEX_FORMATS] = {
	{0, 0, 0, 0},                                                                     /* Invalid */
	{GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 2},                                          /* Color */
	{GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, 3},                                        /* ColorClip */
	{GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 3},     /* DepthStencil: 4 bytes depth, 4 stencil */
	{GL_R8, GL_RED, GL_UNSIGNED_BYTE, 0},                                              /* UNorm8 */
	{GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, 1},                                  /* UInt16 */
	{GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 2},                                    /* UInt32 */
	{GL_R32F, GL_RED, GL_INT, 2},                                                      /* PrimID */
	{GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_UNSIGNED_BYTE, 1}, /* BC1 */
	{GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_UNSIGNED_BYTE, 1}, /* BC2 */
	{GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_UNSIGNED_BYTE, 1}, /* BC3 */
	{GL_COMPRESSED_RGBA_BPTC_UNORM_ARB, GL_COMPRESSED_RGBA_BPTC_UNORM_ARB, GL_UNSIGNED_BYTE, 1} /* BC7 */
};

gs_gl_texture_format gs_gl_texture_format_of(unsigned format, int colclip_float, int depth_only)
{
	gs_gl_texture_format f = s_formats[format < GS_TEX_FORMATS ? format : GS_TEX_INVALID];
	if (format == GS_TEX_COLOR_CLIP && colclip_float)
	{
		/* 24 bits of mantissa hold every 16-bit integer, and its sums, exactly */
		f.internal_format = GL_RGBA32F;
		f.type            = GL_FLOAT;
		f.shift           = 4;
	}
	else if (format == GS_TEX_DEPTH_STENCIL && depth_only)
	{
		f.internal_format = GL_DEPTH_COMPONENT32F;
		f.format          = GL_DEPTH_COMPONENT;
		f.type            = GL_FLOAT;
		f.shift           = 2;
	}
	return f;
}
