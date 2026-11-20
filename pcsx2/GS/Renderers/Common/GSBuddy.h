/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2026 PCSX2 Dev Team
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

#ifndef GS_BUDDY_H
#define GS_BUDDY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One block of memory made up front and handed out in power-of-two
 * pieces (a buddy allocator): a piece freed joins its free twin, so the
 * block does not fragment into pieces too small to use. One thread. */
typedef struct gs_buddy
{
	unsigned char* base;
	unsigned char* state;  /* a byte a smallest piece: 0 inside a piece,
	                        * else its order + 1, | 0x80 while free */
	void* free_head[32];   /* the free pieces of each order */
	size_t size;           /* bytes, 1 << (min_shift + orders - 1) */
	size_t used;           /* bytes handed out */
	unsigned min_shift;    /* log2 of the smallest piece */
	unsigned orders;       /* piece sizes, smallest to the whole */
} gs_buddy;

/* size_shift: log2 of the whole; min_shift: of the smallest piece (at
 * least 6: a piece starts on a cache line). Zero if it could not be made,
 * and then every alloc is NULL. */
int gs_buddy_init(gs_buddy* b, unsigned size_shift, unsigned min_shift);
void gs_buddy_deinit(gs_buddy* b);
/* At least bytes, aligned to the smallest piece; NULL with no free piece
 * that big */
void* gs_buddy_alloc(gs_buddy* b, size_t bytes);
/* Gives back what gs_buddy_alloc gave: one if p was in the block, zero if
 * it is not (and still the caller's) */
int gs_buddy_free(gs_buddy* b, void* p);

#ifdef __cplusplus
}
#endif

#endif
