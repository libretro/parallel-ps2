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

struct gifStruct {
	int  gifstate;
	bool gspath3done;

	u32 gscycles;
	u32 prevcycles;
	u32 mfifocycles;
	u32 gifqwc;
	bool gifmfifoirq;
};

alignas(16) extern gifStruct gif;

struct GIF_Fifo
{
	unsigned int data[64]; //16 QW FIFO
	unsigned int fifoSize;

	int write_fifo(u32* pMem, int size);
	int read_fifo();

	void init();
};

extern GIF_Fifo gif_fifo;


#include "ps2/HwInternal.h"

extern void GIFdma(void);
extern void mfifoGIFtransfer(void);
extern void gifMFIFOInterrupt(void);
extern void gifCheckPathStatus(bool calledFromGIF);
