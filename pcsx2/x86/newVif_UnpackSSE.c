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

#include "newVif_UnpackSSE.h"
#include "common/emitter/c89ops.h"

/* alignas(__pagesize) static u8 nVifUpkExec[__pagesize*4]; */
static struct CodeReserve nVifUpkExec;
static int nVifUpkAssigned = 0;

extern void VifUnpackSSE_doMaskWrite_Dynarec(const struct VifUnpackSSE* p, int regX);

/* ===================================================================================================== */
/*  VifUnpackSSE_Base Section */
/* ===================================================================================================== */
void VifUnpackSSE_Init_State(struct VifUnpackSSE* p, int kind)
{
	memset(p, 0, sizeof(*p));
	p->kind        = kind;
	p->dstIndirect = e_mem_bd(XE_ARG1, 0);
	p->srcIndirect = e_mem_bd(XE_ARG2, 0);
	p->zeroReg     = 15;
	p->workReg     = 1;
	p->destReg     = 0;
}

/* The four behaviours that used to be virtual. */
int VifUnpackSSE_IsWriteProtectedOp(const struct VifUnpackSSE* p)
{
	return (p->kind == VIFUNPACK_DYNAREC) ? p->skipProcessing : 0;
}

int VifUnpackSSE_IsInputMasked(const struct VifUnpackSSE* p)
{
	return (p->kind == VIFUNPACK_DYNAREC) ? p->inputMasked : 0;
}

int VifUnpackSSE_IsUnmaskedOp(const struct VifUnpackSSE* p)
{
	if (p->kind == VIFUNPACK_DYNAREC)
		return !p->doMode && !p->doMask;
	return !p->doMask;
}

void VifUnpackSSE_xMovDest(const struct VifUnpackSSE* p)
{
	if (!VifUnpackSSE_IsWriteProtectedOp(p))
	{
		if (VifUnpackSSE_IsUnmaskedOp(p))
			xe_movaps_memxg(p->dstIndirect, p->destReg);
		else
			VifUnpackSSE_doMaskWrite(p, p->destReg);
	}
}

void VifUnpackSSE_xPMOVXX8(const struct VifUnpackSSE* p, int regX)
{
	if (p->usn) xe_pmovzxbd_xmemg(regX, p->srcIndirect);
	else     xe_pmovsxbd_xmemg(regX, p->srcIndirect);
}

void VifUnpackSSE_xPMOVXX16(const struct VifUnpackSSE* p, int regX)
{
	if (p->usn) xe_pmovzxwd_xmemg(regX, p->srcIndirect);
	else     xe_pmovsxwd_xmemg(regX, p->srcIndirect);
}

void VifUnpackSSE_xUPK_S_32(const struct VifUnpackSSE* p)
{
	switch (p->UnpkLoopIteration)
	{
		case 0:
			xe_movups_xmemg(p->workReg, p->srcIndirect);
			if (!VifUnpackSSE_IsInputMasked(p))
				xe_pshufd_xxi(p->destReg, p->workReg, _v0);
			break;
		case 1:
			if (!VifUnpackSSE_IsInputMasked(p))
				xe_pshufd_xxi(p->destReg, p->workReg, _v1);
			break;
		case 2:
			if (!VifUnpackSSE_IsInputMasked(p))
				xe_pshufd_xxi(p->destReg, p->workReg, _v2);
			break;
		case 3:
			if (!VifUnpackSSE_IsInputMasked(p))
				xe_pshufd_xxi(p->destReg, p->workReg, _v3);
			break;
	}
}

void VifUnpackSSE_xUPK_S_16(const struct VifUnpackSSE* p)
{
	switch (p->UnpkLoopIteration)
	{
		case 0:
			VifUnpackSSE_xPMOVXX16(p, p->workReg);
			if (!VifUnpackSSE_IsInputMasked(p))
				xe_pshufd_xxi(p->destReg, p->workReg, _v0);
			break;
		case 1:
			if (!VifUnpackSSE_IsInputMasked(p))
				xe_pshufd_xxi(p->destReg, p->workReg, _v1);
			break;
		case 2:
			if (!VifUnpackSSE_IsInputMasked(p))
				xe_pshufd_xxi(p->destReg, p->workReg, _v2);
			break;
		case 3:
			if (!VifUnpackSSE_IsInputMasked(p))
				xe_pshufd_xxi(p->destReg, p->workReg, _v3);
			break;
	}
}

void VifUnpackSSE_xUPK_S_8(const struct VifUnpackSSE* p)
{
	switch (p->UnpkLoopIteration)
	{
		case 0:
			VifUnpackSSE_xPMOVXX8(p, p->workReg);
			if (!VifUnpackSSE_IsInputMasked(p))
				xe_pshufd_xxi(p->destReg, p->workReg, _v0);
			break;
		case 1:
			if (!VifUnpackSSE_IsInputMasked(p))
				xe_pshufd_xxi(p->destReg, p->workReg, _v1);
			break;
		case 2:
			if (!VifUnpackSSE_IsInputMasked(p))
				xe_pshufd_xxi(p->destReg, p->workReg, _v2);
			break;
		case 3:
			if (!VifUnpackSSE_IsInputMasked(p))
				xe_pshufd_xxi(p->destReg, p->workReg, _v3);
			break;
	}
}

/* The V2 + V3 unpacks have freaky behaviour, the manual claims "indeterminate". */
/* After testing on the PS2, it's very much determinate in 99% of cases */
/* and games like Lemmings, And1 Streetball rely on this data to be like this! */
/* I have commented after each shuffle to show what data is going where - Ref */

void VifUnpackSSE_xUPK_V2_32(const struct VifUnpackSSE* p)
{
	if (p->UnpkLoopIteration == 0)
	{
		xe_movups_xmemg(p->workReg, p->srcIndirect);
		if (VifUnpackSSE_IsInputMasked(p))
			return;
		xe_pshufd_xxi(p->destReg, p->workReg, 0x44); /* v1v0v1v0 */
	}
	else
	{
		if (VifUnpackSSE_IsInputMasked(p))
			return;
		xe_pshufd_xxi(p->destReg, p->workReg, 0xEE); /* v3v2v3v2 */
	}
	if (p->IsAligned)
		xe_blendps_xxi(p->destReg, p->zeroReg, 0x8); /* zero last word - tested on ps2 */
}

void VifUnpackSSE_xUPK_V2_16(const struct VifUnpackSSE* p)
{
	if (p->UnpkLoopIteration == 0)
	{
		VifUnpackSSE_xPMOVXX16(p, p->workReg);

		if (VifUnpackSSE_IsInputMasked(p))
			return;

		xe_pshufd_xxi(p->destReg, p->workReg, 0x44); /* v1v0v1v0 */
	}
	else
	{
		if (VifUnpackSSE_IsInputMasked(p))
			return;
		
		xe_pshufd_xxi(p->destReg, p->workReg, 0xEE); /* v3v2v3v2 */
	}
}

void VifUnpackSSE_xUPK_V2_8(const struct VifUnpackSSE* p)
{
	if (p->UnpkLoopIteration == 0)
	{
		VifUnpackSSE_xPMOVXX8(p, p->workReg);

		if (!VifUnpackSSE_IsInputMasked(p))
			xe_pshufd_xxi(p->destReg, p->workReg, 0x44); /* v1v0v1v0 */
	}
	else
	{
		if (!VifUnpackSSE_IsInputMasked(p))
			xe_pshufd_xxi(p->destReg, p->workReg, 0xEE); /* v3v2v3v2 */
	}
}

void VifUnpackSSE_xUPK_V3_32(const struct VifUnpackSSE* p)
{
	if (VifUnpackSSE_IsInputMasked(p))
		return;

	xe_movups_xmemg(p->destReg, p->srcIndirect);
	if (p->UnpkLoopIteration != p->IsAligned)
		xe_blendps_xxi(p->destReg, p->zeroReg, 0x8); /* zero last word - tested on ps2 */
}

void VifUnpackSSE_xUPK_V3_16(const struct VifUnpackSSE* p)
{
	int result;
	if (VifUnpackSSE_IsInputMasked(p))
		return;

	VifUnpackSSE_xPMOVXX16(p, p->destReg);

	/* With V3-16, it takes the first vector from the next position as the W vector */
	/* However - IF the end of this iteration of the unpack falls on a quadword boundary, W becomes 0 */
	/* p->IsAligned is the position through the current QW in the vif packet */
	/* Iteration counts where we are in the packet. */
	result = (((p->UnpkLoopIteration / 4) + 1 + (4 - p->IsAligned)) & 0x3);

	if ((p->UnpkLoopIteration & 0x1) == 0 && result == 0)
		xe_blendps_xxi(p->destReg, p->zeroReg, 0x8); /* zero last word - tested on ps2 */
}

void VifUnpackSSE_xUPK_V3_8(const struct VifUnpackSSE* p)
{
	if (VifUnpackSSE_IsInputMasked(p))
		return;

	VifUnpackSSE_xPMOVXX8(p, p->destReg);
	if (p->UnpkLoopIteration != p->IsAligned)
		xe_blendps_xxi(p->destReg, p->zeroReg, 0x8); /* zero last word - tested on ps2 */
}

void VifUnpackSSE_xUPK_V4_32(const struct VifUnpackSSE* p)
{
	if (!VifUnpackSSE_IsInputMasked(p))
		xe_movups_xmemg(p->destReg, p->srcIndirect);
}

void VifUnpackSSE_xUPK_V4_16(const struct VifUnpackSSE* p)
{
	if (!VifUnpackSSE_IsInputMasked(p))
		VifUnpackSSE_xPMOVXX16(p, p->destReg);
}

void VifUnpackSSE_xUPK_V4_8(const struct VifUnpackSSE* p)
{
	if (!VifUnpackSSE_IsInputMasked(p))
		VifUnpackSSE_xPMOVXX8(p, p->destReg);
}

void VifUnpackSSE_xUPK_V4_5(const struct VifUnpackSSE* p)
{
	if (VifUnpackSSE_IsInputMasked(p))
		return;

	xe_movss_xmemg(p->workReg, p->srcIndirect);
	xe_pshufd_xxi(p->workReg, p->workReg, _v0);
	xe_pslld_xi(p->workReg, 3);           /* ABG|R5.000 */
	xe_movaps_xx(p->destReg, p->workReg);     /* x|x|x|R */
	xe_psrld_xi(p->workReg, 8);           /* ABG */
	xe_pslld_xi(p->workReg, 3);           /* AB|G5.000 */
	mVUmergeRegs(p->destReg, p->workReg, 0x4, 0);/* x|x|G|R */
	xe_psrld_xi(p->workReg, 8);           /* AB */
	xe_pslld_xi(p->workReg, 3);           /* A|B5.000 */
	mVUmergeRegs(p->destReg, p->workReg, 0x2, 0);/* x|B|G|R */
	xe_psrld_xi(p->workReg, 8);           /* A */
	xe_pslld_xi(p->workReg, 7);           /* A.0000000 */
	mVUmergeRegs(p->destReg, p->workReg, 0x1, 0);/* A|B|G|R */
	xe_pslld_xi(p->destReg, 24); /* can optimize to */
	xe_psrld_xi(p->destReg, 24); /* single AND... */
}

void VifUnpackSSE_xUnpack(const struct VifUnpackSSE* p, int upknum)
{
	switch (upknum)
	{
		case 0:  VifUnpackSSE_xUPK_S_32(p);  break;
		case 1:  VifUnpackSSE_xUPK_S_16(p);  break;
		case 2:  VifUnpackSSE_xUPK_S_8(p);   break;

		case 4:  VifUnpackSSE_xUPK_V2_32(p); break;
		case 5:  VifUnpackSSE_xUPK_V2_16(p); break;
		case 6:  VifUnpackSSE_xUPK_V2_8(p);  break;

		case 8:  VifUnpackSSE_xUPK_V3_32(p); break;
		case 9:  VifUnpackSSE_xUPK_V3_16(p); break;
		case 10: VifUnpackSSE_xUPK_V3_8(p);  break;

		case 12: VifUnpackSSE_xUPK_V4_32(p); break;
		case 13: VifUnpackSSE_xUPK_V4_16(p); break;
		case 14: VifUnpackSSE_xUPK_V4_8(p);  break;
		case 15: VifUnpackSSE_xUPK_V4_5(p);  break;

		case 3:
		case 7:
		case 11:
			break;
	}
}

/* ===================================================================================================== */
/*  VifUnpackSSE_Simple */
/* ===================================================================================================== */

static void VifUnpackSSE_doMaskWrite_Simple(const struct VifUnpackSSE* p, int regX)
{
	/* The mask rows are module statics, and this routine is emitted into
	 * the code arena, which the OS may map beyond rip and abs32 range of
	 * the image - the layout Windows produces. Mirror the reference
	 * doMaskWrite: materialize the row address far-safely once (rax is
	 * the reference's scratch here too), then walk the planes by their
	 * 64-byte stride through the same operand. */
	struct e_mem xm_;
	int offX = C89_MIN(p->curCycle, 3);

	xe_movaps_xmemg(7, p->dstIndirect);
	xe_complexaddr(xm_, 0 /* rax */, &nVifMask[0][offX], E_NOREG);
	xe_pand_xmem(regX, xm_);
	xm_.disp += sizeof(nVifMask[0]) / 4; /* -> nVifMask[1][offX] */
	xe_pand_xmem(7, xm_);
	xm_.disp += sizeof(nVifMask[0]) / 4; /* -> nVifMask[2][offX] */
	xe_por_xmem(regX, xm_);
	xe_por_xx(regX, 7);
	xe_movaps_memxg(p->dstIndirect, regX);
}

void VifUnpackSSE_doMaskWrite(const struct VifUnpackSSE* p, int regX)
{
	if (p->kind == VIFUNPACK_DYNAREC)
		VifUnpackSSE_doMaskWrite_Dynarec(p, regX);
	else
		VifUnpackSSE_doMaskWrite_Simple(p, regX);
}

/* ecx = dest, edx = src */
static void nVifGen(int usn, int mask, int curCycle)
{
	struct VifUnpackSSE vpugen;
	int usnpart      = usn * 2 * 16;
	int maskpart     = mask * 16;
	int i;

	VifUnpackSSE_Init_State(&vpugen, VIFUNPACK_SIMPLE);
	vpugen.usn       = !!usn;
	vpugen.doMask    = !!mask;
	vpugen.curCycle  = curCycle;
	vpugen.IsAligned = 1;

	for (i = 0; i < 16; ++i)
	{
		nVifCall* ucall = &nVifUpk[((usnpart + maskpart + i) * 4) + curCycle];
		*ucall = NULL;
		if (nVifT[i] == 0)
			continue;

		*ucall = (nVifCall)x86Ptr;
		VifUnpackSSE_xUnpack(&vpugen, i);
		VifUnpackSSE_xMovDest(&vpugen);
		xe_ret();
	}
}

void VifUnpackSSE_Init(void)
{
	int a, b, c;
	if (nVifUpkAssigned)
		return;

	code_reserve_init(&nVifUpkExec);
	nVifUpkAssigned = 1;
	code_reserve_assign(&nVifUpkExec, &GetVmMemory()->code, HOSTMEM_VIFUNPACKREC_OFFSET, _1mb);
	x86Ptr = (u8*)(nVifUpkExec.baseptr);

	for (a = 0; a < 2; a++)
		for (b = 0; b < 2; b++)
			for (c = 0; c < 4; c++)
				nVifGen(a, b, c);

	code_reserve_forbid_modification(&nVifUpkExec);
}

void VifUnpackSSE_Destroy(void)
{
	code_reserve_release(&nVifUpkExec);
	nVifUpkAssigned = 0;
}
