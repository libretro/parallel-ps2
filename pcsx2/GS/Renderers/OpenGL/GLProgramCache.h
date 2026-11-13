/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#ifndef GS_GL_PROGRAM_CACHE_H
#define GS_GL_PROGRAM_CACHE_H

#include <retro_inline.h>

#include "GLProgram.h"
#include "GS/Renderers/Common/GSFlatMap.h"

/* The draw programs by key, compared bit for bit. Programs live in a pool
 * and the map's slots are made with it, so adding one during play never
 * allocates, and a program stays put while the cache holds it. */
typedef struct gs_gl_program_cache
{
	gs_gl_program* pool;
	u32 capacity;
	u32 used;
	u32 restarts; /* times it filled and started over */
	gs_flat_map_t map; /* key -> gs_gl_program* */
} gs_gl_program_cache;

#ifdef __cplusplus
extern "C" {
#endif

/* Room for capacity programs under keys of key_size bytes. Zero if it
 * could not be made (the cache is then empty and adds nothing). */
int gs_gl_program_cache_init(gs_gl_program_cache* c, size_t key_size, u32 capacity);

/* Destroys the programs and frees the pool and slots */
void gs_gl_program_cache_free(gs_gl_program_cache* c);

/* Destroys the programs; the pool and slots are kept */
void gs_gl_program_cache_clear(gs_gl_program_cache* c);

/* A new, empty program for key, which is not in the cache. When the pool
 * is full every program is destroyed first and the cache starts over (so
 * no program found before stays valid). NULL only without a pool. */
gs_gl_program* gs_gl_program_cache_add(gs_gl_program_cache* c, const void* key, size_t hash);

#ifdef __cplusplus
}
#endif

/* The program under key, or NULL; key_size as given to init, a constant
 * at the caller so the compare inlines */
static INLINE gs_gl_program* gs_gl_program_cache_find(const gs_gl_program_cache* c, const void* key, size_t key_size, size_t hash)
{
	gs_gl_program* const* found = (gs_gl_program* const*)gs_flat_map_find(&c->map, key, key_size, hash);
	return found ? *found : NULL;
}

#endif
