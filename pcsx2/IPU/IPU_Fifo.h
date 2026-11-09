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

#include "../../common/Pcsx2Defs.h"

/* The IPU's two FIFOs, 8 quadwords each, as rings of 32 words. They are
 * saved into the savestate as they are, so the layout is fixed: these are
 * plain structs. IPU_Fifo.c owns them. */
typedef struct IPU_Fifo_Input
{
	PCSX2_ALIGN(16) u32 data[32];
	int readpos, writepos;
} IPU_Fifo_Input;

typedef struct IPU_Fifo_Output
{
	PCSX2_ALIGN(16) u32 data[32];
	int readpos, writepos;
} IPU_Fifo_Output;

typedef struct IPU_Fifo
{
	PCSX2_ALIGN(16) IPU_Fifo_Input in;
	PCSX2_ALIGN(16) IPU_Fifo_Output out;
} IPU_Fifo;

#ifdef __cplusplus
extern "C" {
#endif

PCSX2_ALIGN(16) extern IPU_Fifo ipu_fifo;

/* Both empty, contents zeroed. */
extern void ipu_fifo_init(void);
/* Both emptied, with their counters in IPU_CTRL and what waits on them. */
extern void ipu_fifo_clear(void);
extern void ipu_fifo_in_clear(void);
extern void ipu_fifo_out_clear(void);

/* Up to size quadwords in from IPU1 (or the FIFO register); returns how
 * many fitted. */
extern int  ipu_fifo_in_write(const u32* pMem, int size);
/* One quadword out to the bit pointer; 0 when there was none. */
extern int  ipu_fifo_in_read(void* value);
/* Up to size quadwords in from the decoder; returns how many fitted. */
extern int  ipu_fifo_out_write(const u32* value, uint size);
/* size quadwords out to IPU0 (or the FIFO register); the caller knows
 * they are there. */
extern void ipu_fifo_out_read(void* value, uint size);

#ifdef __cplusplus
}
#endif
