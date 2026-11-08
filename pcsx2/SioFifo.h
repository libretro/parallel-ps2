/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

/* The byte FIFOs SIO0 and SIO2 exchange commands through: a flat buffer
 * that grows, with a head cursor. A transfer fills one and drains it, so
 * the live bytes stay at the front and compaction is all but never done.
 * C89. */
#pragma once

#include <stddef.h>

#include "../common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SioFifo
{
	u8* data;
	size_t head;  /* the front */
	size_t count; /* one past the back */
	size_t capacity;
} SioFifo;

#define sio_fifo_size(q)  ((q)->count - (q)->head)
#define sio_fifo_empty(q) ((q)->count == (q)->head)
#define sio_fifo_front(q) ((q)->data[(q)->head])

void sio_fifo_clear(SioFifo* q);
void sio_fifo_pop(SioFifo* q);
void sio_fifo_push(SioFifo* q, u8 v);
void sio_fifo_free(SioFifo* q);

struct SaveStateBase;
/* A u32 count, then the bytes front to back: the shape a state has
 * always carried. */
void sio_fifo_freeze(struct SaveStateBase* s, SioFifo* q);

#ifdef __cplusplus
}
#endif
