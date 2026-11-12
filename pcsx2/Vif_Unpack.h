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

/* The VIF unpackers (Vif_Unpack.c): UNPACK's data into VU memory, the
 * interpreted per-vector workers, and the hand-off to the recompiled
 * ones (x86/newVif_Dynarec.c, arm64/Vif_Dynarec_arm64.cpp). */

struct vifStruct;
struct VIFregisters;

#ifdef __cplusplus
extern "C" {
#endif

/* One vector from src into the quadword at dest, for the VIF state given:
 * vif's MaskRow and MaskCol, and in the masked forms regs' MASK. */
typedef void (*UNPACKFUNCTYPE)(void* dest, const void* src,
	struct vifStruct* vif, const struct VIFregisters* regs);

/* A recompiled per-cycle kernel: one vector from src into dest. */
typedef u32 (*nVifCall)(void*, const void*);

PCSX2_ALIGN(16) extern const u8 nVifT[16];

/* [mode] [doMask, usn, VN, VL] -- the masked half after the unmasked one,
 * the unsigned quarter of each after its signed one. The same workers
 * serve both units. */
PCSX2_ALIGN(16) extern const UNPACKFUNCTYPE VIFfuncTable[4][(4 * 4 * 2 * 2)];

/* The recompilers fill these: ([USN][Masking][Unpack Type]) [curCycle],
 * and the masks the interpreted SSE kernels read. */
PCSX2_ALIGN(16) extern nVifCall nVifUpk[(2 * 2 * 16) * 4];
PCSX2_ALIGN(16) extern u32      nVifMask[3][4][4];

/* An UNPACK that arrives in parts is gathered here until it is whole. */
typedef struct VifUnpackBuffer
{
	/* NUM is at most 256 vectors of a quadword each */
	PCSX2_ALIGN(16) u8 buffer[256 * 16];
	u32 bSize; /* bytes held */
} VifUnpackBuffer;

PCSX2_ALIGN(16) extern VifUnpackBuffer vifUnpackBuffer[2];

extern void resetNewVif(int idx);

/* UNPACK's code sets the transfer up, and its data is unpacked into VU
 * memory, the words taken returned (Vif_Codes.c). */
extern void vif_unpack_setup(int idx, const u32* data);
extern int  vif_unpack(int idx, const u8* data);

/* The interpreted unpack loop, for the recompilers' fallback. */
extern void _nVifUnpack(int idx, const u8* data, uint mode, bool isFill);

/* The recompilers (x86/newVif_Dynarec.c, arm64/Vif_Dynarec_arm64.cpp). */
extern void dVifReset(int idx);
extern void dVifReserve(int idx);
extern void dVifRelease(int idx);
/* The unpack kernels, made and freed with the VM's CPU providers. */
extern void VifUnpackSSE_Init(void);
extern void VifUnpackSSE_Destroy(void);
extern void dvif_unpack(int idx, const u8* data, int isFill);

#ifdef __cplusplus
}
#endif
