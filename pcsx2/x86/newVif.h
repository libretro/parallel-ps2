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

#include "../Vif_Dynarec.h"
#include "../VirtualMemory.h"

#include "../../common/emitter/x86emitter.h"
#ifdef __cplusplus
extern "C" {
#endif
extern void  mVUmergeRegs(int dest, int src,  int xyzw, int modXYZW);
extern void  mVUsaveReg(int reg, struct e_mem ptr, int xyzw, int modXYZW);
#ifdef __cplusplus
}
#endif

#define VUFT VIFUnpackFuncTable
#define _v0 0
#define _v1 0x55
#define _v2 0xaa
#define _v3 0xff
#define xmmCol0 2
#define xmmCol1 3
#define xmmCol2 4
#define xmmCol3 5
#define xmmRow 6
#define xmmTemp 7
