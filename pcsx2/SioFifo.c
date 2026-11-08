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

/* SIO byte FIFOs. See SioFifo.h. */
#include "SioFifo.h"

#include <stdlib.h>
#include <string.h>

#include "SaveState.h"

void sio_fifo_clear(SioFifo* q)
{
	q->head = 0;
	q->count = 0;
}

void sio_fifo_pop(SioFifo* q)
{
	/* Drained, it starts over at the front: no move, no growth. */
	if (++q->head == q->count)
		q->head = q->count = 0;
}

void sio_fifo_push(SioFifo* q, u8 v)
{
	if (q->count == q->capacity)
	{
		if (q->head > 0)
		{
			memmove(q->data, q->data + q->head, q->count - q->head);
			q->count -= q->head;
			q->head = 0;
		}
		if (q->count == q->capacity)
		{
			const size_t cap = q->capacity ? q->capacity * 2 : 64;
			u8* grown = (u8*)realloc(q->data, cap);
			if (!grown)
				return; /* the byte is lost, not written past the end */
			q->data = grown;
			q->capacity = cap;
		}
	}
	q->data[q->count++] = v;
}

void sio_fifo_free(SioFifo* q)
{
	free(q->data);
	q->data = NULL;
	q->head = q->count = q->capacity = 0;
}

void sio_fifo_freeze(SaveStateBase* s, SioFifo* q)
{
	u32 count = (u32)sio_fifo_size(q);
	u32 i;

	SaveState_Freeze(s, count);
	if (count == 0)
	{
		if (SaveState_IsLoading(s))
			sio_fifo_clear(q);
		return;
	}
	if (SaveState_IsSaving(s))
	{
		SaveState_FreezeMem(s, q->data + q->head, (int)count);
		return;
	}
	/* A queue cannot have held more bytes than the block has left: a
	 * count from nowhere is refused before anything grows for it. */
	if ((size_t)count > SaveState_BytesLeft(s))
	{
		s->error = true;
		sio_fifo_clear(q);
		return;
	}
	/* Sized first, then read in place: a cleared fifo pushed count times
	 * holds them from data[0]. */
	sio_fifo_clear(q);
	for (i = 0; i < count; i++)
		sio_fifo_push(q, 0);
	if (sio_fifo_size(q) != count)
	{
		s->error = true;
		sio_fifo_clear(q);
		return;
	}
	SaveState_FreezeMem(s, q->data, (int)count);
}
