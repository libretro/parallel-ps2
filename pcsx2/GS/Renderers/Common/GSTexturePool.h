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

/* A device's textures and targets kept for reuse, rather than made and
 * freed around each draw. A PS2 game's targets are a handful of sizes,
 * all inside four megabytes of GS memory, and it asks for the same ones
 * every frame: so the pool is filled once (prewarm), handed out from
 * (fetch) and given back to (put), and only trimmed once a frame when
 * over its ceiling (age). In steady state nothing is made or freed.
 *
 * The lists are reserved at init for more than their ceilings, so a
 * texture given back during play allocates nothing. */
#ifndef GS_TEXTURE_POOL_H
#define GS_TEXTURE_POOL_H

#include "common/Pcsx2Defs.h"
#include "GSTextureBase.h"
#include "GSFastList.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How many each list keeps past a frame, and how many whole GS memories'
 * worth of bytes at the pool's scale */
#define GS_TEX_POOL_MAX_TEXTURES 300
#define GS_TEX_POOL_MAX_TARGETS 300
#define GS_TEX_POOL_LIVE_SETS_TEXTURES 2
#define GS_TEX_POOL_LIVE_SETS_TARGETS 4
/* What each list is reserved for: a frame may give back more than the
 * ceiling before age trims it */
#define GS_TEX_POOL_RESERVE 1024
/* The GS's memory, every target and texture a game has at once */
#define GS_TEX_POOL_VM_SIZE 4194304u

/* The device's own surface maker (gs_texture_type, gs_texture_format):
 * NULL when it could not make one */
typedef gs_texture* (*gs_tex_pool_create_t)(void* device, unsigned type, int width, int height, int levels,
	unsigned format);

typedef struct gs_tex_pool
{
	gs_fast_list list[2]; /* gs_texture*, [textures, targets], latest given back first */
	u64 mem_usage[2];
	u32 frame;            /* stamped on what is given back; prefer_new skips this frame's */
	int software;         /* the software device: its textures are at most 2x */
	int prefer_new;       /* the device wants a texture not used this frame, if it can */
	float upscale;        /* the upscale option */
	gs_tex_pool_create_t create;
	void* device;
} gs_tex_pool;

/* Empty, with both lists reserved; false if that could not be had */
bool gs_tex_pool_init(gs_tex_pool* p, gs_tex_pool_create_t create, void* device);
/* The lists freed; what they hold is not (purge first, on the device
 * that made it) */
void gs_tex_pool_deinit(gs_tex_pool* p);

/* The scale the pool's textures are at (the upscale option; the software
 * device's are at most 2x), and the bytes a list may hold at it */
float gs_tex_pool_scale(const gs_tex_pool* p);
u64 gs_tex_pool_budget(const gs_tex_pool* p, u32 list);

/* A texture of exactly this shape, out of the pool or made: cleared
 * (targets to 0, depth to 0.0) or marked invalidated. prefer_reuse: one
 * used this frame will do. NULL if none could be made, even after
 * freeing the pool. */
gs_texture* gs_tex_pool_fetch(gs_tex_pool* p, unsigned type, int width, int height, int levels, unsigned format,
	bool clear, bool prefer_reuse);
/* Back into the pool; nothing is freed */
void gs_tex_pool_put(gs_tex_pool* p, gs_texture* t);
/* Once a frame: the next frame, and what is over a ceiling freed, the
 * longest unused first */
void gs_tex_pool_age(gs_tex_pool* p);
/* Everything freed */
void gs_tex_pool_purge(gs_tex_pool* p);
/* The common target and depth sizes made up front, within three quarters
 * of the target budget; how many */
u32 gs_tex_pool_prewarm(gs_tex_pool* p);

/* GSTextureCache.cpp: what the texture cache holds, for the message when
 * a texture cannot be made */
const char* GSTextureCacheMemoryUsage(void);

#ifdef __cplusplus
}
#endif

#endif
