// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
// SPDX-License-Identifier: GPL-3.0+
// Transplanted into lrps2 from isztldav/pcsx2 @ c89cb8a (C.19); near-verbatim
// except VifUnpackSSE_Init/Destroy, which use a local mmap'd code region
// instead of SysMemory.

#include "Vif_UnpackNEON.h"
#include "common/Pcsx2Defs.h"
#include <algorithm>


#include <cstdlib>
#include <sys/mman.h>
#include <memmap.h>


// =====================================================================================================
//  VifUnpackSSE_Base Section
// =====================================================================================================
VifUnpackNEON_Base::VifUnpackNEON_Base()
	: usn(false)
	, doMask(false)
	, UnpkLoopIteration(0)
	, UnpkNoOfIterations(0)
	, IsAligned(0)
	, dstIndirect(a64_mem_off(RXARG1, 0))
	, srcIndirect(a64_mem_off(RXARG2, 0))
	, workReg(A64_Q(1))
	, destReg(A64_Q(0))
	, workGprW(A64_W(4))
{
}

void VifUnpackNEON_Base::xMovDest() const
{
	if (!IsWriteProtectedOp())
	{
		if (IsUnmaskedOp())
			a64_str(armAsm, destReg, dstIndirect);
		else
			doMaskWrite(destReg);
	}
}

void VifUnpackNEON_Base::xShiftR(const a64r& regX, int n) const
{
	if (usn)
		a64_ushr(armAsm, a64_as(regX, A64K_4S), a64_as(regX, A64K_4S), n);
	else
		a64_sshr(armAsm, a64_as(regX, A64K_4S), a64_as(regX, A64K_4S), n);
}

void VifUnpackNEON_Base::xPMOVXX8(const a64r& regX) const
{
	// TODO(Stenzek): Check this
	a64_ldr(armAsm, a64_as(regX, A64K_S), srcIndirect);

	if (usn)
	{
		a64_ushll(armAsm, a64_as(regX, A64K_8H), a64_as(regX, A64K_8B), 0);
		a64_ushll(armAsm, a64_as(regX, A64K_4S), a64_as(regX, A64K_4H), 0);
	}
	else
	{
		a64_sshll(armAsm, a64_as(regX, A64K_8H), a64_as(regX, A64K_8B), 0);
		a64_sshll(armAsm, a64_as(regX, A64K_4S), a64_as(regX, A64K_4H), 0);
	}
}

void VifUnpackNEON_Base::xPMOVXX16(const a64r& regX) const
{
	a64_ldr(armAsm, a64_as(regX, A64K_D), srcIndirect);

	if (usn)
		a64_ushll(armAsm, a64_as(regX, A64K_4S), a64_as(regX, A64K_4H), 0);
	else
		a64_sshll(armAsm, a64_as(regX, A64K_4S), a64_as(regX, A64K_4H), 0);
}

void VifUnpackNEON_Base::xUPK_S_32() const
{
	if (UnpkLoopIteration == 0)
		a64_ldr(armAsm, workReg, srcIndirect);

	if (IsInputMasked())
		return;

	switch (UnpkLoopIteration)
	{
		case 0:
			a64_dup_elem(armAsm, a64_as(destReg, A64K_4S), a64_as(workReg, A64K_4S), 0);
			break;
		case 1:
			a64_dup_elem(armAsm, a64_as(destReg, A64K_4S), a64_as(workReg, A64K_4S), 1);
			break;
		case 2:
			a64_dup_elem(armAsm, a64_as(destReg, A64K_4S), a64_as(workReg, A64K_4S), 2);
			break;
		case 3:
			a64_dup_elem(armAsm, a64_as(destReg, A64K_4S), a64_as(workReg, A64K_4S), 3);
			break;
	}
}

void VifUnpackNEON_Base::xUPK_S_16() const
{
	if (UnpkLoopIteration == 0)
		xPMOVXX16(workReg);

	if (IsInputMasked())
		return;

	switch (UnpkLoopIteration)
	{
		case 0:
			a64_dup_elem(armAsm, a64_as(destReg, A64K_4S), a64_as(workReg, A64K_4S), 0);
			break;
		case 1:
			a64_dup_elem(armAsm, a64_as(destReg, A64K_4S), a64_as(workReg, A64K_4S), 1);
			break;
		case 2:
			a64_dup_elem(armAsm, a64_as(destReg, A64K_4S), a64_as(workReg, A64K_4S), 2);
			break;
		case 3:
			a64_dup_elem(armAsm, a64_as(destReg, A64K_4S), a64_as(workReg, A64K_4S), 3);
			break;
	}
}

void VifUnpackNEON_Base::xUPK_S_8() const
{
	if (UnpkLoopIteration == 0)
		xPMOVXX8(workReg);

	if (IsInputMasked())
		return;

	switch (UnpkLoopIteration)
	{
		case 0:
			a64_dup_elem(armAsm, a64_as(destReg, A64K_4S), a64_as(workReg, A64K_4S), 0);
			break;
		case 1:
			a64_dup_elem(armAsm, a64_as(destReg, A64K_4S), a64_as(workReg, A64K_4S), 1);
			break;
		case 2:
			a64_dup_elem(armAsm, a64_as(destReg, A64K_4S), a64_as(workReg, A64K_4S), 2);
			break;
		case 3:
			a64_dup_elem(armAsm, a64_as(destReg, A64K_4S), a64_as(workReg, A64K_4S), 3);
			break;
	}
}

// The V2 + V3 unpacks have freaky behaviour, the manual claims "indeterminate".
// After testing on the PS2, it's very much determinate in 99% of cases
// and games like Lemmings, And1 Streetball rely on this data to be like this!
// I have commented after each shuffle to show what data is going where - Ref

void VifUnpackNEON_Base::xUPK_V2_32() const
{
	if (UnpkLoopIteration == 0)
	{
		a64_ldr(armAsm, workReg, srcIndirect);

		if (IsInputMasked())
			return;

		a64_dup_elem(armAsm, a64_as(destReg, A64K_2D), a64_as(workReg, A64K_2D), 0); //v1v0v1v0
		if (IsAligned)
			a64_ins(armAsm, a64_as(destReg, A64K_4S), 3, A64_WZR); //zero last word - tested on ps2
	}
	else
	{
		if (IsInputMasked())
			return;

		a64_dup_elem(armAsm, a64_as(destReg, A64K_2D), a64_as(workReg, A64K_2D), 1); //v3v2v3v2
		if (IsAligned)
			a64_ins(armAsm, a64_as(destReg, A64K_4S), 3, A64_WZR); //zero last word - tested on ps2
	}
}

void VifUnpackNEON_Base::xUPK_V2_16() const
{
	if (UnpkLoopIteration == 0)
	{
		xPMOVXX16(workReg);

		if (IsInputMasked())
			return;

		a64_dup_elem(armAsm, a64_as(destReg, A64K_2D), a64_as(workReg, A64K_2D), 0); //v1v0v1v0
	}
	else
	{
		if (IsInputMasked())
			return;

		a64_dup_elem(armAsm, a64_as(destReg, A64K_2D), a64_as(workReg, A64K_2D), 1); //v3v2v3v2
	}
}

void VifUnpackNEON_Base::xUPK_V2_8() const
{
	if (UnpkLoopIteration == 0)
	{
		xPMOVXX8(workReg);

		if (IsInputMasked())
			return;

		a64_dup_elem(armAsm, a64_as(destReg, A64K_2D), a64_as(workReg, A64K_2D), 0); //v1v0v1v0
	}
	else
	{
		if (IsInputMasked())
			return;

		a64_dup_elem(armAsm, a64_as(destReg, A64K_2D), a64_as(workReg, A64K_2D), 1); //v3v2v3v2
	}
}

void VifUnpackNEON_Base::xUPK_V3_32() const
{
	if (IsInputMasked())
		return;

	a64_ldr(armAsm, destReg, srcIndirect);
	if (UnpkLoopIteration != IsAligned)
		a64_ins(armAsm, a64_as(destReg, A64K_4S), 3, A64_WZR);
}

void VifUnpackNEON_Base::xUPK_V3_16() const
{
	if (IsInputMasked())
		return;

	xPMOVXX16(destReg);

	//With V3-16, it takes the first vector from the next position as the W vector
	//However - IF the end of this iteration of the unpack falls on a quadword boundary, W becomes 0
	//IsAligned is the position through the current QW in the vif packet
	//Iteration counts where we are in the packet.
	int result = (((UnpkLoopIteration / 4) + 1 + (4 - IsAligned)) & 0x3);

	if ((UnpkLoopIteration & 0x1) == 0 && result == 0)
		a64_ins(armAsm, a64_as(destReg, A64K_4S), 3, A64_WZR); //zero last word on QW boundary if whole 32bit word is used - tested on ps2
}

void VifUnpackNEON_Base::xUPK_V3_8() const
{
	if (IsInputMasked())
		return;

	xPMOVXX8(destReg);
	if (UnpkLoopIteration != IsAligned)
		a64_ins(armAsm, a64_as(destReg, A64K_4S), 3, A64_WZR);
}

void VifUnpackNEON_Base::xUPK_V4_32() const
{
	if (IsInputMasked())
		return;

	a64_ldr(armAsm, a64_as(destReg, A64K_Q), srcIndirect);
}

void VifUnpackNEON_Base::xUPK_V4_16() const
{
	if (IsInputMasked())
		return;

	xPMOVXX16(destReg);
}

void VifUnpackNEON_Base::xUPK_V4_8() const
{
	if (IsInputMasked())
		return;

	xPMOVXX8(destReg);
}

void VifUnpackNEON_Base::xUPK_V4_5() const
{
	if (IsInputMasked())
		return;

	a64_ldrh(armAsm, workGprW, srcIndirect);
	a64_lsl_imm(armAsm, workGprW, workGprW, 3); // ABG|R5.000
	a64_dup(armAsm, a64_as(destReg, A64K_4S), workGprW); // x|x|x|R
	a64_lsr_imm(armAsm, workGprW, workGprW, 8); // ABG
	a64_lsl_imm(armAsm, workGprW, workGprW, 3); // AB|G5.000
	a64_ins(armAsm, a64_as(destReg, A64K_4S), 1, workGprW); // x|x|G|R
	a64_lsr_imm(armAsm, workGprW, workGprW, 8); // AB
	a64_lsl_imm(armAsm, workGprW, workGprW, 3); // A|B5.000
	a64_ins(armAsm, a64_as(destReg, A64K_4S), 2, workGprW); // x|B|G|R
	a64_lsr_imm(armAsm, workGprW, workGprW, 8); // A
	a64_lsl_imm(armAsm, workGprW, workGprW, 7); // A.0000000
	a64_ins(armAsm, a64_as(destReg, A64K_4S), 3, workGprW); // A|B|G|R
	a64_shl(armAsm, a64_as(destReg, A64K_4S), a64_as(destReg, A64K_4S), 24); // can optimize to
	a64_ushr(armAsm, a64_as(destReg, A64K_4S), a64_as(destReg, A64K_4S), 24); // single AND...
}

void VifUnpackNEON_Base::xUnpack(int upknum) const
{
	switch (upknum)
	{
		case 0:
			xUPK_S_32();
			break;
		case 1:
			xUPK_S_16();
			break;
		case 2:
			xUPK_S_8();
			break;

		case 4:
			xUPK_V2_32();
			break;
		case 5:
			xUPK_V2_16();
			break;
		case 6:
			xUPK_V2_8();
			break;

		case 8:
			xUPK_V3_32();
			break;
		case 9:
			xUPK_V3_16();
			break;
		case 10:
			xUPK_V3_8();
			break;

		case 12:
			xUPK_V4_32();
			break;
		case 13:
			xUPK_V4_16();
			break;
		case 14:
			xUPK_V4_8();
			break;
		case 15:
			xUPK_V4_5();
			break;

		case 3:
		case 7:
		case 11:
			// TODO: Needs hardware testing.
			// Dynasty Warriors 5: Empire  - Player 2 chose a character menu.
			log_cb(RETRO_LOG_WARN, "Vpu/Vif: Invalid Unpack %d\n", upknum);
			break;
	}
}

// =====================================================================================================
//  VifUnpackSSE_Simple
// =====================================================================================================

VifUnpackNEON_Simple::VifUnpackNEON_Simple(bool usn_, bool domask_, int curCycle_)
{
	curCycle = curCycle_;
	usn = usn_;
	doMask = domask_;
	IsAligned = true;
}

void VifUnpackNEON_Simple::doMaskWrite(const a64r& regX) const
{
	a64_ldr(armAsm, A64_Q(7), dstIndirect);

	int offX = std::min(curCycle, 3);
	armMoveAddressToReg(RXSCRATCH, nVifMask);
	a64_ldr(armAsm, A64_Q(29), a64_mem_off(RXSCRATCH, reinterpret_cast<const u8*>(nVifMask[0][offX]) - reinterpret_cast<const u8*>(nVifMask)));
	a64_ldr(armAsm, A64_Q(30), a64_mem_off(RXSCRATCH, reinterpret_cast<const u8*>(nVifMask[1][offX]) - reinterpret_cast<const u8*>(nVifMask)));
	a64_ldr(armAsm, A64_Q(31), a64_mem_off(RXSCRATCH, reinterpret_cast<const u8*>(nVifMask[2][offX]) - reinterpret_cast<const u8*>(nVifMask)));
	a64_and(armAsm, a64_as(regX, A64K_16B), a64_as(regX, A64K_16B), a64_as(A64_Q(29), A64K_16B));
	a64_and(armAsm, a64_as(A64_Q(7), A64K_16B), a64_as(A64_Q(7), A64K_16B), a64_as(A64_Q(30), A64K_16B));
	a64_orr(armAsm, a64_as(regX, A64K_16B), a64_as(regX, A64K_16B), a64_as(A64_Q(31), A64K_16B));
	a64_orr(armAsm, a64_as(regX, A64K_16B), a64_as(regX, A64K_16B), a64_as(A64_Q(7), A64K_16B));
	a64_str(armAsm, regX, dstIndirect);
}

// ecx = dest, edx = src
static void nVifGen(int usn, int mask, int curCycle)
{

	int usnpart = usn * 2 * 16;
	int maskpart = mask * 16;

	VifUnpackNEON_Simple vpugen(!!usn, !!mask, curCycle);

	for (int i = 0; i < 16; ++i)
	{
		nVifCall& ucall(nVifUpk[((usnpart + maskpart + i) * 4) + curCycle]);
		ucall = NULL;
		if (nVifT[i] == 0)
			continue;

		ucall = (nVifCall)armStartBlock();
		vpugen.xUnpack(i);
		vpugen.xMovDest();
		a64_ret(armAsm);
		armEndBlock();
	}
}

// Code region for the 256 simple unpack kernels (measured ~30KB; 128KB gives
// slack). RWX like the other lrps2 arm64 code caches.
static u8* s_upkCode = nullptr;
static constexpr size_t kUpkCodeSize = 128 * 1024;

void VifUnpackSSE_Init()
{
	if (!s_upkCode)
	{
		s_upkCode = (u8*)memjit_alloc(kUpkCodeSize);
		if (!s_upkCode)
		{
			s_upkCode = nullptr;
			log_cb(RETRO_LOG_ERROR, "arm64 VIF unpack dynarec: mmap failed; using the C reference path.\n");
			return;
		}
	}

	log_cb(RETRO_LOG_INFO, "Generating NEON-optimized unpacking functions for VIF interpreters...\n");

	armSetAsmPtr(s_upkCode, kUpkCodeSize, nullptr);

	for (int a = 0; a < 2; a++)
		for (int b = 0; b < 2; b++)
			for (int c = 0; c < 4; c++)
				nVifGen(a, b, c);
}

void VifUnpackSSE_Destroy()
{
	if (s_upkCode)
	{
		memjit_free(s_upkCode, kUpkCodeSize);
		s_upkCode = nullptr;
	}
	memset(nVifUpk, 0, sizeof(nVifUpk));
}
