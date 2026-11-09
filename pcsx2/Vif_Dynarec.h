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

// SPDX-FileCopyrightText: 2002-2023 PCSX2 Dev Team
// SPDX-License-Identifier: LGPL-3.0+
//

#pragma once

#include "Vif.h"
#include "Vif_Unpack.h"
#include "Vif_HashBucket.h"
#include "VU.h"

typedef void (*nVifrecCall)(uptr dest, uptr src);

extern void  dVifReserve (int idx);
extern void  dVifRelease (int idx);
extern void  VifUnpackSSE_Init();
extern void  VifUnpackSSE_Destroy();

_vifT extern void dVifUnpack(const u8* data, int isFill);

/* The recompiler's state per unit. The partial-transfer buffer the
 * interpreter needs is Vif_Unpack.c's vifUnpackBuffer. */
struct nVifStruct
{
	// VIF0 or VIF1 - provided for debugging helpfulness only, and is generally unused.
	// (templates are used for most or all VIF indexing)
	u32                     idx;

	struct CodeReserve      recReserve;  // x86 dynarec; unused on arm64
	u8*                     recWritePtr; // current write pos into the reserve
	u8*                     recEndPtr;   // arm64 dynarec: end of the code region (overflow check)

	vif_hash_bucket_t       vifBlocks;   // Vif Blocks
};

alignas(16) extern nVifStruct nVif[2];
