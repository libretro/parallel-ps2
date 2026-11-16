/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023 PCSX2 Dev Team
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

#include <stddef.h>

#include <libretro.h>

#include "common/Pcsx2Defs.h"
#include "GSTexturePool.h"

static u32 gs_texture_mem_usage(const gs_texture* t)
{
	return (u32)(t->m_size.x * t->m_size.y * (t->m_format == GS_TEX_UNORM8 ? 1 : 4));
}

static void tex_pool_free(gs_texture* t)
{
	if (t)
		t->m_ops->free(t);
}

static gs_texture* tex_pool_at(const gs_fast_list* l, u16 i)
{
	gs_texture* t;
	memcpy(&t, gs_fast_list_data(l, i), sizeof(t));
	return t;
}

static void tex_pool_clear_list(gs_tex_pool* p, u32 idx)
{
	gs_fast_list* l = &p->list[idx];
	u16 i;
	u16 next;

	/* The array stays: emptied, not freed, so what was reserved is
	 * still there for the next texture given back */
	for (i = gs_fast_list_first(l); i != 0; i = next)
	{
		next = gs_fast_list_next(l, i);
		tex_pool_free(tex_pool_at(l, i));
		gs_fast_list_erase(l, i);
	}
	p->mem_usage[idx] = 0;
}

bool gs_tex_pool_init(gs_tex_pool* p, gs_tex_pool_create_t create, void* device)
{
	bool ok = true;
	u32 i;

	memset(p, 0, sizeof(*p));
	p->create = create;
	p->device = device;
	for (i = 0; i < 2; i++)
	{
		gs_fast_list_init(&p->list[i], sizeof(gs_texture*));
		while (p->list[i].capacity < GS_TEX_POOL_RESERVE)
		{
			if (!gs_fast_list_grow(&p->list[i]))
			{
				ok = false;
				break;
			}
		}
	}
	return ok;
}

void gs_tex_pool_deinit(gs_tex_pool* p)
{
	gs_fast_list_clear(&p->list[0]);
	gs_fast_list_clear(&p->list[1]);
}

float gs_tex_pool_scale(const gs_tex_pool* p)
{
	/* The software renderer draws at native or 2x whatever the option
	 * says, in system memory: the option at face value would reserve
	 * hundreds of megabytes it never uses */
	if (p->software)
		return p->upscale >= 2.0f ? 2.0f : 1.0f;
	return p->upscale > 0.0f ? p->upscale : 1.0f;
}

u64 gs_tex_pool_budget(const gs_tex_pool* p, u32 list)
{
	/* All of GS memory at the pool's scale, times the sets kept */
	const float scale = gs_tex_pool_scale(p);
	const u64 live_set = (u64)((float)GS_TEX_POOL_VM_SIZE * scale * scale);
	return live_set * (list == 0 ? GS_TEX_POOL_LIVE_SETS_TEXTURES : GS_TEX_POOL_LIVE_SETS_TARGETS);
}

gs_texture* gs_tex_pool_fetch(gs_tex_pool* p, unsigned type, int width, int height, int levels, unsigned format,
	bool clear, bool prefer_reuse)
{
	const u32 idx = (type != GS_TEXTYPE_TEXTURE);
	const bool prefer_new = p->prefer_new && type == GS_TEXTYPE_TEXTURE && !prefer_reuse;
	gs_fast_list* l = &p->list[idx];
	gs_texture* t = NULL;
	u16 fallback = 0;
	u16 i;

	for (i = gs_fast_list_first(l); i != 0; i = gs_fast_list_next(l, i))
	{
		gs_texture* c = tex_pool_at(l, i);
		if (c->m_type != type || c->m_format != format || c->m_size.x != width || c->m_size.y != height
			|| c->m_mipmap_levels != levels)
			continue;
		if (!prefer_new || c->m_last_frame_used != p->frame)
		{
			t = c;
			break;
		}
		if (!fallback)
			fallback = i;
	}

	/* Only a full list hands out one used this frame */
	if (!t && fallback && l->count >= (idx == 0 ? GS_TEX_POOL_MAX_TEXTURES : GS_TEX_POOL_MAX_TARGETS))
	{
		i = fallback;
		t = tex_pool_at(l, i);
	}

	if (t)
	{
		p->mem_usage[idx] -= gs_texture_mem_usage(t);
		gs_fast_list_erase(l, i);
	}
	else
	{
		t = p->create(p->device, type, width, height, levels, format);
		if (!t)
		{
			log_cb(RETRO_LOG_ERROR,
				"GS: Memory allocation failure for %dx%d texture (%u MB). Purging pool and retrying.\n"
				"GS:   held: pool %llu MB textures + %llu MB targets (budgets %llu / %llu), cache %s\n",
				width, height, (unsigned)((u64)width * (u64)height * 4u >> 20),
				(unsigned long long)(p->mem_usage[0] >> 20), (unsigned long long)(p->mem_usage[1] >> 20),
				(unsigned long long)(gs_tex_pool_budget(p, 0) >> 20),
				(unsigned long long)(gs_tex_pool_budget(p, 1) >> 20), GSTextureCacheMemoryUsage());
			gs_tex_pool_purge(p);
			t = p->create(p->device, type, width, height, levels, format);
			if (!t)
			{
				log_cb(RETRO_LOG_ERROR, "GS: Memory allocation failure for %dx%d texture after purging pool.\n",
					width, height);
				return NULL;
			}
		}
	}

	if (type == GS_TEXTYPE_RENDER_TARGET || type == GS_TEXTYPE_DEPTH_STENCIL)
	{
		if (!clear)
			t->m_state = GS_TEXSTATE_INVALIDATED;
		else
		{
			t->m_state = GS_TEXSTATE_CLEARED;
			if (type == GS_TEXTYPE_RENDER_TARGET)
				t->m_clear_value.color = 0;
			else
				t->m_clear_value.depth = 0.0f;
		}
	}
	return t;
}

void gs_tex_pool_put(gs_tex_pool* p, gs_texture* t)
{
	u32 idx;

	if (!t)
		return;
	idx = (t->m_type != GS_TEXTYPE_TEXTURE);
	t->m_last_frame_used = p->frame;
	/* Reserved for more than age lets it keep: this only grows past a
	 * reservation a frame gave back over a thousand textures into, and
	 * if even that cannot be had the texture goes rather than leaks */
	if (!gs_fast_list_insert_front(&p->list[idx], &t))
	{
		tex_pool_free(t);
		return;
	}
	p->mem_usage[idx] += gs_texture_mem_usage(t);
}

void gs_tex_pool_age(gs_tex_pool* p)
{
	u32 idx;

	/* Age alone frees nothing: only being over a ceiling does, which in
	 * steady state the pool never is. (The frame moves on first, so all
	 * the pool holds is from frames past: none is spared for being
	 * recent, only for being nearer the front.) */
	p->frame++;
	for (idx = 0; idx < 2; idx++)
	{
		const u32 max_count = (idx == 0) ? GS_TEX_POOL_MAX_TEXTURES : GS_TEX_POOL_MAX_TARGETS;
		const u64 max_bytes = gs_tex_pool_budget(p, idx);
		gs_fast_list* l = &p->list[idx];
		u16 back;

		while ((back = gs_fast_list_last(l)) != 0)
		{
			gs_texture* t = tex_pool_at(l, back);
			if (l->count <= max_count && p->mem_usage[idx] <= max_bytes)
				break;
			p->mem_usage[idx] -= gs_texture_mem_usage(t);
			tex_pool_free(t);
			gs_fast_list_erase(l, back);
		}
	}
}

void gs_tex_pool_purge(gs_tex_pool* p)
{
	tex_pool_clear_list(p, 0);
	tex_pool_clear_list(p, 1);
}

u32 gs_tex_pool_prewarm(gs_tex_pool* p)
{
	/* Native sizes, most used first, since the budget runs out before the
	 * list does: NTSC, the 512s and half heights games pick to save GS
	 * memory, PAL, and 704, the widest FBW in common use */
	static const struct
	{
		short w, h;
	} sizes[] = {
		{ 640, 448 }, { 512, 448 }, { 640, 224 }, { 512, 224 },
		{ 640, 512 }, { 512, 256 }, { 640, 256 }, { 512, 512 },
		{ 704, 448 }, { 704, 512 }, { 704, 224 }, { 704, 256 },
	};
	const float scale = gs_tex_pool_scale(p);
	/* Three quarters: the rest is for sizes not on the list, made once on
	 * first sight and kept */
	const u64 limit = (gs_tex_pool_budget(p, 1) / 4u) * 3u;
	u64 spent = 0;
	u32 made = 0;
	u32 pass;
	u32 i;

	/* Colour, then depth: if the budget stretches to one of a pair the
	 * colour target is the one that matters */
	for (pass = 0; pass < 2; pass++)
	{
		const unsigned type = pass == 0 ? GS_TEXTYPE_RENDER_TARGET : GS_TEXTYPE_DEPTH_STENCIL;
		const unsigned format = pass == 0 ? GS_TEX_COLOR : GS_TEX_DEPTH_STENCIL;

		for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
		{
			const int w = (int)((float)sizes[i].w * scale);
			const int h = (int)((float)sizes[i].h * scale);
			const u64 cost = (u64)w * (u64)h * 4u;
			gs_texture* t;

			if (spent + cost > limit)
				break;
			t = p->create(p->device, type, w, h, 1, format);
			if (!t)
				break;
			spent += cost;
			made++;
			gs_tex_pool_put(p, t);
		}
	}

	log_cb(RETRO_LOG_INFO, "GS: pool prewarmed, %u target sizes, %llu MB.\n", made,
		(unsigned long long)(spent >> 20));
	return made;
}
