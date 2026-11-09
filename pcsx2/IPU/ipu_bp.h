/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
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

#pragma once

/* The IPU's bit pointer: a two-quadword window over the input FIFO and a
 * position within it.
 *
 * The window is a hand-rolled ring of two entries. BP runs 0..127 over the
 * first quadword; once it passes 128 the second quadword moves to the front
 * and another is pulled from the FIFO. FP says how many of the two are
 * filled, so a read of more bits than FP covers has to refill first.
 *
 * This is written whole into savestates, so its layout is fixed. */

#include <string.h>

#include "../../common/Pcsx2Defs.h"
#include "../../common/Pcsx2Types.h"
#include "ipu_status.h"

typedef struct tIPU_BP {
	PCSX2_ALIGN(16) u128 internal_qwc[2];

	u32 BP;		/* Bit stream point (0 to 128*2) */
	u32 IFC;	/* Input FIFO counter (8QWC) (0 to 8) */
	u32 FP;		/* internal FIFO (2QWC) fill status (0 to 2) */
} tIPU_BP;

#ifdef __cplusplus
extern "C" {
#endif

/* One quadword out of the input FIFO (IPU_Fifo.c). */
int ipu_fifo_in_read(void *value);

#ifdef __cplusplus
}
#endif

/* These stay in the header so they keep inlining into the decoder, which
 * steps the position once per VLC symbol. */

/* Pull quadwords from the FIFO until the window covers bits past BP.
 * Returns 0 when the FIFO ran dry before that, having marked the core as
 * waiting on IPU1. */
static PCSX2_INLINE int ipu_bp_fill_buffer(tIPU_BP *bp, u32 bits)
{
	while ((bp->FP * 128) < (bp->BP + bits))
	{
		if (ipu_fifo_in_read(&bp->internal_qwc[bp->FP]) == 0)
		{
			/* The whole window could not be filled. The request is
			 * short either way, so say so and let the caller park
			 * until IPU1 brings more. */
			IPUCoreStatus.WaitingOnIPUTo = 1;
			return 0;
		}

		++bp->FP;
	}

	return 1;
}

/* Step the position on by bits, rotating the window when it crosses a
 * quadword. Refills first, so a caller that does not care whether the data
 * is there can just advance. */
static PCSX2_INLINE void ipu_bp_advance(tIPU_BP *bp, u32 bits)
{
	ipu_bp_fill_buffer(bp, bits);

	bp->BP += bits;

	if (bp->BP >= 128)
	{
		bp->BP -= 128;

		if (bp->FP == 2)
		{
			/* Reading has moved into the second quadword, so it
			 * becomes the first and the second is refilled. */
			memcpy(&bp->internal_qwc[0], &bp->internal_qwc[1], 16);
			bp->FP = 1;
		}
		else
		{
			/* FP == 1: the window is now empty. FP == 0: an empty
			 * window is being advanced, which drops a quadword from
			 * the FIFO. */
			bp->FP = ipu_fifo_in_read(&bp->internal_qwc[0]) ? 1 : 0;
		}
	}
}

/* Round the position up to the next byte. */
static PCSX2_INLINE void ipu_bp_align(tIPU_BP *bp)
{
	bp->BP = (bp->BP + 7) & ~7u;
	ipu_bp_advance(bp, 0);
}
