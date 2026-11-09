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

#include "../common/Pcsx2Defs.h"

struct vifStruct;

typedef void (*UNPACKFUNCTYPE)(void* dest, const void* src);

#define create_unpack_u_type(bits)		typedef void (*UNPACKFUNCTYPE_u##bits)(u32* dest, const u##bits* src)
#define create_unpack_s_type(bits)		typedef void (*UNPACKFUNCTYPE_s##bits)(u32* dest, const s##bits* src)

#define create_some_unpacks(bits)		\
		create_unpack_u_type(bits);		\
		create_unpack_s_type(bits)

create_some_unpacks(32);
create_some_unpacks(16);
create_some_unpacks(8);

PCSX2_ALIGN(16) extern const u8 nVifT[16];

/* Array sub-dimension order: [vifidx] [mode] (VN * VL * USN * doMask) */
PCSX2_ALIGN(16) extern const UNPACKFUNCTYPE VIFfuncTable[2][4][(4 * 4 * 2 * 2)];

extern void resetNewVif(int idx);

/* Both are templates on the VIF index. */
#ifdef __cplusplus
template< int idx >
extern int nVifUnpack(const u8* data);

template< int idx >
extern void vifUnpackSetup(const u32* data);
#endif

/* The same for C (Vif_Codes.c), the unit as an argument: UNPACK's code
 * sets the transfer up, and its data is unpacked into VU memory, the
 * words taken returned. */
#ifdef __cplusplus
extern "C" {
#endif
extern void vif_unpack_setup(int idx, const u32* data);
extern int  vif_unpack(int idx, const u8* data);
#ifdef __cplusplus
}
#endif
