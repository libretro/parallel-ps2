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

#include "GSTextureSW.h"

static int gs_texture_sw_bpp(const gs_texture_sw* t)
{
	return t->base.m_format == GS_TEX_UNORM8 ? 1 : 4;
}

static void gs_texture_sw_free(gs_texture* base)
{
	gs_texture_sw* t = (gs_texture_sw*)base;
	memalign_free(t->data);
	free(t);
}

static int gs_texture_sw_update(gs_texture* base, const gs_texture_rect* r, const void* data, int pitch, int layer)
{
	gs_texture_sw* t       = (gs_texture_sw*)base;
	const int bpp          = gs_texture_sw_bpp(t);
	const int rw           = r->right - r->left;
	const int rh           = r->bottom - r->top;
	const unsigned char* s = (const unsigned char*)data;
	unsigned char* d;
	int y;
	(void)layer;

	if (!t->data || !data)
		return 0;
	if (r->left < 0 || r->top < 0 || rw <= 0 || rh <= 0)
		return 0;
	if (r->left + rw > base->m_size.x || r->top + rh > base->m_size.y)
		return 0;

	d = t->data + (size_t)r->top * t->pitch + (size_t)r->left * bpp;
	for (y = 0; y < rh; y++)
	{
		memcpy(d, s, (size_t)rw * bpp);
		d += t->pitch;
		s += pitch;
	}
	return 1;
}

static int gs_texture_sw_map(gs_texture* base, gs_texture_map* m, const gs_texture_rect* r, int layer)
{
	gs_texture_sw* t = (gs_texture_sw*)base;
	const int x      = r ? r->left : 0;
	const int y      = r ? r->top : 0;
	(void)layer;

	if (!t->data || t->mapped)
		return 0;
	m->bits   = t->data + (size_t)y * t->pitch + (size_t)x * gs_texture_sw_bpp(t);
	m->pitch  = t->pitch;
	t->mapped = 1;
	return 1;
}

static void gs_texture_sw_unmap(gs_texture* base)
{
	((gs_texture_sw*)base)->mapped = 0;
}

static const gs_texture_ops s_sw_ops = {
	gs_texture_sw_free, gs_texture_sw_update, gs_texture_sw_map, gs_texture_sw_unmap, NULL};

gs_texture* gs_texture_sw_create(unsigned type, int width, int height, unsigned format)
{
	gs_texture_sw* t = (gs_texture_sw*)malloc(sizeof(*t));
	size_t bytes;
	if (!t)
		return NULL;
	gs_texture_base_init(&t->base, type, format, width, height);
	t->base.m_ops           = &s_sw_ops;
	t->base.m_mipmap_levels = 1;
	t->mapped               = 0;
	/* rows rounded up to the alignment, so SIMD reads stay in a row */
	t->pitch = (int)(((u32)width * (u32)gs_texture_sw_bpp(t) + (GS_TEXTURE_SW_ALIGN - 1)) & ~(u32)(GS_TEXTURE_SW_ALIGN - 1));
	bytes    = (size_t)t->pitch * (size_t)height;
	t->data  = (u8*)memalign_alloc(GS_TEXTURE_SW_ALIGN, bytes ? bytes : GS_TEXTURE_SW_ALIGN);
	if (t->data && bytes)
		memset(t->data, 0, bytes);
	return &t->base;
}
