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

#include "../../common/Pcsx2Types.h"

/* The IPU as the EE hardware sees it: its registers on page 2 (IPU.cpp)
 * and its two DMA channels (IPUdma.cpp), with C linkage for Hw.c and
 * Dmac.c. The read and write return like the other units': the value
 * read, and false when the write was taken care of. */
#ifdef __cplusplus
extern "C" {
#endif

extern void ipuReset(void);

extern u32  ipuRead32(u32 mem);
extern u64  ipuRead64(u32 mem);
extern bool ipuWrite32(u32 mem, u32 value);
extern bool ipuWrite64(u32 mem, u64 value);

extern void dmaIPU0(void);
extern void dmaIPU1(void);

#ifdef __cplusplus
}
#endif
