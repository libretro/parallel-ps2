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

#include "Vif.h"
#include "Vif_Unpack.h"
#include "Vif_HashBucket.h"
#include "VU.h"
#include "VirtualMemory.h"

typedef void (*nVifrecCall)(uptr dest, uptr src);

#ifdef __cplusplus
/* The arm64 dynarec keeps its per-unit template. */
_vifT extern void dVifUnpack(const u8* data, int isFill);
extern "C" {
#endif

/* The recompiler's state per unit. The partial-transfer buffer the
 * interpreter needs is Vif_Unpack.c's vifUnpackBuffer. */
typedef struct nVifStruct
{
	u32                     idx;         /* VIF0 or VIF1 */
	struct CodeReserve      recReserve;  /* x86 dynarec; unused on arm64 */
	u8*                     recWritePtr; /* current write pos into the reserve */
	u8*                     recEndPtr;   /* arm64 dynarec: end of the code region (overflow check) */
	vif_hash_bucket_t       vifBlocks;   /* Vif Blocks */
} nVifStruct;

PCSX2_ALIGN(16) extern nVifStruct nVif[2];


#ifdef __cplusplus
}
#endif
