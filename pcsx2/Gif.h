/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
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

#include "Gif_hw.h"
#include "ps2/HwInternal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gifStruct
{
	int  gifstate;
	bool gspath3done;

	u32 gscycles;
	u32 prevcycles;
	u32 mfifocycles;
	u32 gifqwc;
	bool gifmfifoirq;
} gifStruct;

/* The GIF's 16-quadword DMA FIFO. */
typedef struct GIF_Fifo
{
	unsigned int data[64];
	unsigned int fifoSize;
} GIF_Fifo;

PCSX2_ALIGN(16) extern gifStruct gif;
PCSX2_ALIGN(16) extern GIF_Fifo gif_fifo;

extern void gif_fifo_init(void);
/* Takes up to size quadwords from pMem; returns how many it took. */
extern int  gif_fifo_write(u32* pMem, int size);
/* Hands what it holds to PATH3; returns the quadwords taken. */
extern int  gif_fifo_read(void);

extern void GIFdma(void);
extern void mfifoGIFtransfer(void);
extern void gifMFIFOInterrupt(void);
extern void gifCheckPathStatus(bool calledFromGIF);

#ifdef __cplusplus
}
#endif
