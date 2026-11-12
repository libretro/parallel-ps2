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
#include "../microVU/microVU_Const.h"

#define xmmT1  0  /* Used for regAlloc */
#define xmmT2  1  /* Used for regAlloc */
#define xmmT3  2  /* Used for regAlloc */
#define xmmT4  3  /* Used for regAlloc */
#define xmmT5  4  /* Used for regAlloc */
#define xmmT6  5  /* Used for regAlloc */
#define xmmT7  6  /* Used for regAlloc */
#define xmmPQ  15 /* Holds the Value and Backup Values of P and Q regs */

#define gprT1  XE_AX /* eax - Temp Reg */
#define gprT2  XE_CX /* ecx - Temp Reg */
#define gprT1q XE_AX /* eax - Temp Reg */
#define gprT2q XE_CX /* ecx - Temp Reg */
#define gprT1b XE_AX /* Low 16-bit of gprT1 (eax) */
#define gprT2b XE_CX /* Low 16-bit of gprT2 (ecx) */

#define gprF0  XE_BX /* Status Flag 0 */
#define gprF1  12 /* Status Flag 1 */
#define gprF2  13 /* Status Flag 2 */
#define gprF3  14 /* Status Flag 3 */

#ifdef __cplusplus
extern "C" {
#endif
extern void mVUmergeRegs(int dest, int src, int xyzw, int modXYZW);
extern void mVUsaveReg(int reg, struct e_mem ptr, int xyzw, int modXYZW);
extern void mVUloadReg(int reg, struct e_mem ptr, int xyzw);
#ifdef __cplusplus
}
#endif
