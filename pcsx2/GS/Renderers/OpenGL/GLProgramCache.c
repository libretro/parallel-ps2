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

#include <stdlib.h>

#include "GLProgramCache.h"

int gs_gl_program_cache_init(gs_gl_program_cache* c, size_t key_size, u32 capacity)
{
	u32 i;
	c->pool     = NULL;
	c->capacity = 0;
	c->used     = 0;
	c->restarts = 0;
	gs_flat_map_init(&c->map, key_size, sizeof(gs_gl_program*));
	if (!capacity)
		return 0;
	c->pool = (gs_gl_program*)malloc((size_t)capacity * sizeof(*c->pool));
	if (!c->pool || !gs_flat_map_reserve(&c->map, capacity))
	{
		gs_gl_program_cache_free(c);
		return 0;
	}
	for (i = 0; i < capacity; i++)
		gs_gl_program_init(&c->pool[i]);
	c->capacity = capacity;
	return 1;
}

void gs_gl_program_cache_clear(gs_gl_program_cache* c)
{
	u32 i;
	for (i = 0; i < c->used; i++)
	{
		gs_gl_program_destroy(&c->pool[i]);
		gs_gl_program_init(&c->pool[i]);
	}
	c->used = 0;
	gs_flat_map_clear(&c->map);
}

void gs_gl_program_cache_free(gs_gl_program_cache* c)
{
	gs_gl_program_cache_clear(c);
	free(c->pool);
	c->pool     = NULL;
	c->capacity = 0;
	gs_flat_map_free(&c->map);
}

gs_gl_program* gs_gl_program_cache_add(gs_gl_program_cache* c, const void* key, size_t hash)
{
	gs_gl_program** slot;
	gs_gl_program* p;
	int inserted;

	if (!c->capacity)
		return NULL;
	if (c->used == c->capacity)
	{
		log_cb(RETRO_LOG_WARN, "OpenGL: all %u draw programs in use, starting the cache over\n", (unsigned)c->capacity);
		gs_gl_program_cache_clear(c);
		c->restarts++;
	}
	p = &c->pool[c->used++];
	/* (never grows: the slots were made for the whole pool) */
	slot  = (gs_gl_program**)gs_flat_map_insert(&c->map, key, hash, &inserted);
	*slot = p;
	return p;
}
