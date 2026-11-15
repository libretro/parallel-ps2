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
#include <string.h>

#include <memalign.h>

/* The header's inline operations are static, and this file calls few. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "GSFastList.h"

/* Elements in a list's first array: no growth on the first insertions */
#define GS_FAST_LIST_FIRST 4

void gs_fast_list_init(gs_fast_list* l, size_t elem_size)
{
	/* an element and its two indexes, at the element's alignment */
	const size_t align = elem_size >= 8 ? 8 : 4;
	l->buf       = NULL;
	l->capacity  = 0;
	l->count     = 0;
	l->elem_size = (u16)elem_size;
	l->stride    = (u16)((elem_size + 2 * sizeof(u16) + align - 1) & ~(align - 1));
	l->link      = (u16)(l->stride - 2 * sizeof(u16));
}

void gs_fast_list_clear(gs_fast_list* l)
{
	memalign_free(l->buf);
	l->buf      = NULL;
	l->capacity = 0;
	l->count    = 0;
}

int gs_fast_list_grow(gs_fast_list* l)
{
	const u16 capacity = !l->buf ? GS_FAST_LIST_FIRST
		: l->capacity <= USHRT_MAX / 2 ? (u16)(l->capacity * 2) : (u16)USHRT_MAX;
	unsigned char* buf;
	u16* stack;
	u16 i;

	/* (index 0 is the head: a full array of 65535 holds 65534) */
	if (capacity == l->capacity)
		return 0;
	buf = (unsigned char*)memalign_alloc(64, (size_t)capacity * l->stride + (size_t)(capacity - 1) * sizeof(u16));
	if (!buf)
		return 0;
	stack = (u16*)(buf + (size_t)capacity * l->stride);

	if (l->buf)
	{
		/* in order: a kept index still names its element */
		memcpy(buf, l->buf, (size_t)l->capacity * l->stride);
		memcpy(stack, GS_FAST_LIST_FREE(l), (size_t)(l->capacity - 1) * sizeof(u16));
		memalign_free(l->buf);
		i = (u16)(l->capacity - 1);
	}
	else
	{
		/* the head, chained to itself */
		memset(buf, 0, l->stride);
		i = 0;
	}
	for (; i < capacity - 1; i++)
		stack[i] = (u16)(i + 1);

	l->buf      = buf;
	l->capacity = capacity;
	return 1;
}
