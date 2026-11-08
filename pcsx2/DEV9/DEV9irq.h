/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
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

/* What the IOP's interrupt, DMA and counter units reach of DEV9, in a
 * header C reads. DEV9.h includes it, so DEV9.cpp defines these with the
 * same linkage. */

#ifndef DEV9IRQ_H
#define DEV9IRQ_H

#include "common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IopIrq.c: raises the DEV9 event, cycles from now. */
extern void dev9Irq(int cycles);

int DEV9irqHandler(void);
void DEV9async(u32 cycles);
void DEV9writeDMA8Mem(u32* pMem, int size);
void DEV9readDMA8Mem(u32* pMem, int size);

#ifdef __cplusplus
}
#endif

#endif
