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

#include "../Common.h"
#include "../Vif_Dma.h"
#include "newVif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------------------- */
/*  VifUnpackSSE */
/* -------------------------------------------------------------------------------------- */
/* One state struct for both unpack generators; `kind` selects the four */
/* behaviours that used to be virtual overrides (write-protect, input-mask, */
/* unmasked-op, and the masked write itself). Everything else was virtual by */
/* declaration but never overridden, so those are plain functions now. */

enum VifUnpackKind
{
	VIFUNPACK_SIMPLE = 0,
	VIFUNPACK_DYNAREC = 1
};

struct VifUnpackSSE
{
	int kind;

	int usn;    /* unsigned flag */
	int doMask; /* masking write enable flag */
	int UnpkLoopIteration;
	int IsAligned;

	struct e_mem dstIndirect;
	struct e_mem srcIndirect;
	int zeroReg;
	int workReg;
	int destReg;

	/* SIMPLE */
	int curCycle;

	/* DYNAREC */
	int isFill;
	int doMode; /* two bit value representing difference mode */
	int skipProcessing;
	int inputMasked;
	const nVifStruct* v;  /* vif0 or vif1 */
	const nVifBlock*  vB; /* some pre-collected data from VifStruct */
	int vCL;              /* internal copy of vif->cl */
};

void VifUnpackSSE_Init_State(struct VifUnpackSSE* p, int kind);
void VifUnpackSSE_InitDynarec(struct VifUnpackSSE* p, const nVifStruct* vif_, const nVifBlock* vifBlock_);

int  VifUnpackSSE_IsWriteProtectedOp(const struct VifUnpackSSE* p);
int  VifUnpackSSE_IsInputMasked(const struct VifUnpackSSE* p);
int  VifUnpackSSE_IsUnmaskedOp(const struct VifUnpackSSE* p);

void VifUnpackSSE_xUnpack(const struct VifUnpackSSE* p, int upktype);
void VifUnpackSSE_xMovDest(const struct VifUnpackSSE* p);
void VifUnpackSSE_doMaskWrite(const struct VifUnpackSSE* p, int regX);

void VifUnpackSSE_ModUnpack(struct VifUnpackSSE* p, int upknum, int PostOp);
void VifUnpackSSE_ProcessMasks(struct VifUnpackSSE* p);
void VifUnpackSSE_CompileRoutine(struct VifUnpackSSE* p);

#ifdef __cplusplus
}
#endif
