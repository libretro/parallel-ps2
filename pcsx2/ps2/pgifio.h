/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2016-2021  PCSX2 Dev Team
 *  Copyright (C) 2016 Wisi
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

/* The PGIF (ps2/pgif.c): the PS1 GPU registers and GPU DMA window as the
 * IOP's hardware register handlers reach them, and the EE side's PGIF
 * registers as Hw.cpp reaches them. */

#ifndef PGIFIO_H
#define PGIFIO_H

#include "common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void psxGPUw(int addr, u32 data);
extern u32 psxGPUr(int addr);

extern u32 psxDma2GpuR(u32 addr);
extern void psxDma2GpuW(u32 addr, u32 data);

extern void pgifInit(void);

extern void PGIFw(int addr, u32 data);
extern u32 PGIFr(int addr);

extern void PGIFwQword(u32 addr, void* data);
extern void PGIFrQword(u32 addr, void* data);

#ifdef __cplusplus
}
#endif

#endif
