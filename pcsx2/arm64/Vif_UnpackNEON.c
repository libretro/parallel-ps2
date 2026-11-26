/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
 * SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
 * SPDX-License-Identifier: GPL-3.0+ */

/* NEON VIF unpack kernels: the 256 simple ones the interpreters call, and
 * the per-format bodies the dynarec (Vif_Dynarec_arm64.c) strings together. */

#include <string.h>

#include <memmap.h>

#include "arm64/Vif_UnpackNEON.h"
#include "common/Pcsx2Defs.h"

#define IsWriteProtectedOp(p) ((p)->skipProcessing)
#define IsInputMasked(p) ((p)->inputMasked)
#define IsUnmaskedOp(p) (!(p)->doMode && !(p)->doMask)

void vneon_init(VifUnpackNEON* p)
{
	memset(p, 0, sizeof(*p));
	p->dstIndirect = a64_mem_off(RXARG1, 0);
	p->srcIndirect = a64_mem_off(RXARG2, 0);
	p->workReg     = A64_Q(1);
	p->destReg     = A64_Q(0);
	p->workGprW    = A64_W(4);
}

static void simple_mask_write(const VifUnpackNEON* p, a64r regX);

void vneon_mov_dest(const VifUnpackNEON* p)
{
	if (!IsWriteProtectedOp(p))
	{
		if (IsUnmaskedOp(p))
			a64_str(armAsm, p->destReg, p->dstIndirect);
		else if (p->dynarec)
			vneon_dynarec_mask_write(p, p->destReg);
		else
			simple_mask_write(p, p->destReg);
	}
}

static void xShiftR(const VifUnpackNEON* p, a64r regX, int n)
{
	if (p->usn)
		a64_ushr(armAsm, a64_as(regX, A64K_4S), a64_as(regX, A64K_4S), n);
	else
		a64_sshr(armAsm, a64_as(regX, A64K_4S), a64_as(regX, A64K_4S), n);
}

static void xPMOVXX8(const VifUnpackNEON* p, a64r regX)
{
	/* TODO(Stenzek): Check this */
	a64_ldr(armAsm, a64_as(regX, A64K_S), p->srcIndirect);

	if (p->usn)
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

static void xPMOVXX16(const VifUnpackNEON* p, a64r regX)
{
	a64_ldr(armAsm, a64_as(regX, A64K_D), p->srcIndirect);

	if (p->usn)
		a64_ushll(armAsm, a64_as(regX, A64K_4S), a64_as(regX, A64K_4H), 0);
	else
		a64_sshll(armAsm, a64_as(regX, A64K_4S), a64_as(regX, A64K_4H), 0);
}

static void xUPK_S_32(const VifUnpackNEON* p)
{
	if (p->UnpkLoopIteration == 0)
		a64_ldr(armAsm, p->workReg, p->srcIndirect);

	if (IsInputMasked(p))
		return;

	switch (p->UnpkLoopIteration)
	{
		case 0:
			a64_dup_elem(armAsm, a64_as(p->destReg, A64K_4S), a64_as(p->workReg, A64K_4S), 0);
			break;
		case 1:
			a64_dup_elem(armAsm, a64_as(p->destReg, A64K_4S), a64_as(p->workReg, A64K_4S), 1);
			break;
		case 2:
			a64_dup_elem(armAsm, a64_as(p->destReg, A64K_4S), a64_as(p->workReg, A64K_4S), 2);
			break;
		case 3:
			a64_dup_elem(armAsm, a64_as(p->destReg, A64K_4S), a64_as(p->workReg, A64K_4S), 3);
			break;
	}
}

static void xUPK_S_16(const VifUnpackNEON* p)
{
	if (p->UnpkLoopIteration == 0)
		xPMOVXX16(p, p->workReg);

	if (IsInputMasked(p))
		return;

	switch (p->UnpkLoopIteration)
	{
		case 0:
			a64_dup_elem(armAsm, a64_as(p->destReg, A64K_4S), a64_as(p->workReg, A64K_4S), 0);
			break;
		case 1:
			a64_dup_elem(armAsm, a64_as(p->destReg, A64K_4S), a64_as(p->workReg, A64K_4S), 1);
			break;
		case 2:
			a64_dup_elem(armAsm, a64_as(p->destReg, A64K_4S), a64_as(p->workReg, A64K_4S), 2);
			break;
		case 3:
			a64_dup_elem(armAsm, a64_as(p->destReg, A64K_4S), a64_as(p->workReg, A64K_4S), 3);
			break;
	}
}

static void xUPK_S_8(const VifUnpackNEON* p)
{
	if (p->UnpkLoopIteration == 0)
		xPMOVXX8(p, p->workReg);

	if (IsInputMasked(p))
		return;

	switch (p->UnpkLoopIteration)
	{
		case 0:
			a64_dup_elem(armAsm, a64_as(p->destReg, A64K_4S), a64_as(p->workReg, A64K_4S), 0);
			break;
		case 1:
			a64_dup_elem(armAsm, a64_as(p->destReg, A64K_4S), a64_as(p->workReg, A64K_4S), 1);
			break;
		case 2:
			a64_dup_elem(armAsm, a64_as(p->destReg, A64K_4S), a64_as(p->workReg, A64K_4S), 2);
			break;
		case 3:
			a64_dup_elem(armAsm, a64_as(p->destReg, A64K_4S), a64_as(p->workReg, A64K_4S), 3);
			break;
	}
}

/* The V2 + V3 unpacks have freaky behaviour, the manual claims "indeterminate".
 * After testing on the PS2, it's very much determinate in 99% of cases
 * and games like Lemmings, And1 Streetball rely on this data to be like this!
 * I have commented after each shuffle to show what data is going where - Ref */

static void xUPK_V2_32(const VifUnpackNEON* p)
{
	if (p->UnpkLoopIteration == 0)
	{
		a64_ldr(armAsm, p->workReg, p->srcIndirect);

		if (IsInputMasked(p))
			return;

		a64_dup_elem(armAsm, a64_as(p->destReg, A64K_2D), a64_as(p->workReg, A64K_2D), 0); /* v1v0v1v0 */
		if (p->IsAligned)
			a64_ins(armAsm, a64_as(p->destReg, A64K_4S), 3, A64_WZR); /* zero last word - tested on ps2 */
	}
	else
	{
		if (IsInputMasked(p))
			return;

		a64_dup_elem(armAsm, a64_as(p->destReg, A64K_2D), a64_as(p->workReg, A64K_2D), 1); /* v3v2v3v2 */
		if (p->IsAligned)
			a64_ins(armAsm, a64_as(p->destReg, A64K_4S), 3, A64_WZR); /* zero last word - tested on ps2 */
	}
}

static void xUPK_V2_16(const VifUnpackNEON* p)
{
	if (p->UnpkLoopIteration == 0)
	{
		xPMOVXX16(p, p->workReg);

		if (IsInputMasked(p))
			return;

		a64_dup_elem(armAsm, a64_as(p->destReg, A64K_2D), a64_as(p->workReg, A64K_2D), 0); /* v1v0v1v0 */
	}
	else
	{
		if (IsInputMasked(p))
			return;

		a64_dup_elem(armAsm, a64_as(p->destReg, A64K_2D), a64_as(p->workReg, A64K_2D), 1); /* v3v2v3v2 */
	}
}

static void xUPK_V2_8(const VifUnpackNEON* p)
{
	if (p->UnpkLoopIteration == 0)
	{
		xPMOVXX8(p, p->workReg);

		if (IsInputMasked(p))
			return;

		a64_dup_elem(armAsm, a64_as(p->destReg, A64K_2D), a64_as(p->workReg, A64K_2D), 0); /* v1v0v1v0 */
	}
	else
	{
		if (IsInputMasked(p))
			return;

		a64_dup_elem(armAsm, a64_as(p->destReg, A64K_2D), a64_as(p->workReg, A64K_2D), 1); /* v3v2v3v2 */
	}
}

static void xUPK_V3_32(const VifUnpackNEON* p)
{
	if (IsInputMasked(p))
		return;

	a64_ldr(armAsm, p->destReg, p->srcIndirect);
	if (p->UnpkLoopIteration != p->IsAligned)
		a64_ins(armAsm, a64_as(p->destReg, A64K_4S), 3, A64_WZR);
}

static void xUPK_V3_16(const VifUnpackNEON* p)
{
	int result;
	if (IsInputMasked(p))
		return;

	xPMOVXX16(p, p->destReg);

	/* With V3-16, it takes the first vector from the next position as the W vector
	 * However - IF the end of this iteration of the unpack falls on a quadword boundary, W becomes 0
	 * IsAligned is the position through the current QW in the vif packet
	 * Iteration counts where we are in the packet. */
	result = (((p->UnpkLoopIteration / 4) + 1 + (4 - p->IsAligned)) & 0x3);

	if ((p->UnpkLoopIteration & 0x1) == 0 && result == 0)
		a64_ins(armAsm, a64_as(p->destReg, A64K_4S), 3, A64_WZR); /* zero last word on QW boundary if whole 32bit word is used - tested on ps2 */
}

static void xUPK_V3_8(const VifUnpackNEON* p)
{
	if (IsInputMasked(p))
		return;

	xPMOVXX8(p, p->destReg);
	if (p->UnpkLoopIteration != p->IsAligned)
		a64_ins(armAsm, a64_as(p->destReg, A64K_4S), 3, A64_WZR);
}

static void xUPK_V4_32(const VifUnpackNEON* p)
{
	if (IsInputMasked(p))
		return;

	a64_ldr(armAsm, a64_as(p->destReg, A64K_Q), p->srcIndirect);
}

static void xUPK_V4_16(const VifUnpackNEON* p)
{
	if (IsInputMasked(p))
		return;

	xPMOVXX16(p, p->destReg);
}

static void xUPK_V4_8(const VifUnpackNEON* p)
{
	if (IsInputMasked(p))
		return;

	xPMOVXX8(p, p->destReg);
}

static void xUPK_V4_5(const VifUnpackNEON* p)
{
	if (IsInputMasked(p))
		return;

	a64_ldrh(armAsm, p->workGprW, p->srcIndirect);
	a64_lsl_imm(armAsm, p->workGprW, p->workGprW, 3); /* ABG|R5.000 */
	a64_dup(armAsm, a64_as(p->destReg, A64K_4S), p->workGprW); /* x|x|x|R */
	a64_lsr_imm(armAsm, p->workGprW, p->workGprW, 8); /* ABG */
	a64_lsl_imm(armAsm, p->workGprW, p->workGprW, 3); /* AB|G5.000 */
	a64_ins(armAsm, a64_as(p->destReg, A64K_4S), 1, p->workGprW); /* x|x|G|R */
	a64_lsr_imm(armAsm, p->workGprW, p->workGprW, 8); /* AB */
	a64_lsl_imm(armAsm, p->workGprW, p->workGprW, 3); /* A|B5.000 */
	a64_ins(armAsm, a64_as(p->destReg, A64K_4S), 2, p->workGprW); /* x|B|G|R */
	a64_lsr_imm(armAsm, p->workGprW, p->workGprW, 8); /* A */
	a64_lsl_imm(armAsm, p->workGprW, p->workGprW, 7); /* A.0000000 */
	a64_ins(armAsm, a64_as(p->destReg, A64K_4S), 3, p->workGprW); /* A|B|G|R */
	a64_shl(armAsm, a64_as(p->destReg, A64K_4S), a64_as(p->destReg, A64K_4S), 24); /* can optimize to */
	a64_ushr(armAsm, a64_as(p->destReg, A64K_4S), a64_as(p->destReg, A64K_4S), 24); /* single AND... */
}

void vneon_unpack(const VifUnpackNEON* p, int upknum)
{
	switch (upknum)
	{
		case 0:
			xUPK_S_32(p);
			break;
		case 1:
			xUPK_S_16(p);
			break;
		case 2:
			xUPK_S_8(p);
			break;

		case 4:
			xUPK_V2_32(p);
			break;
		case 5:
			xUPK_V2_16(p);
			break;
		case 6:
			xUPK_V2_8(p);
			break;

		case 8:
			xUPK_V3_32(p);
			break;
		case 9:
			xUPK_V3_16(p);
			break;
		case 10:
			xUPK_V3_8(p);
			break;

		case 12:
			xUPK_V4_32(p);
			break;
		case 13:
			xUPK_V4_16(p);
			break;
		case 14:
			xUPK_V4_8(p);
			break;
		case 15:
			xUPK_V4_5(p);
			break;

		case 3:
		case 7:
		case 11:
			/* TODO: Needs hardware testing.
			 * Dynasty Warriors 5: Empire  - Player 2 chose a character menu. */
			log_cb(RETRO_LOG_WARN, "Vpu/Vif: Invalid Unpack %d\n", upknum);
			break;
	}
}

static void simple_mask_write(const VifUnpackNEON* p, a64r regX)
{
	const int offX = p->curCycle < 3 ? p->curCycle : 3;
	a64_ldr(armAsm, A64_Q(7), p->dstIndirect);

	armMoveAddressToReg(RXSCRATCH, nVifMask);
	a64_ldr(armAsm, A64_Q(29), a64_mem_off(RXSCRATCH, (const u8*)nVifMask[0][offX] - (const u8*)nVifMask));
	a64_ldr(armAsm, A64_Q(30), a64_mem_off(RXSCRATCH, (const u8*)nVifMask[1][offX] - (const u8*)nVifMask));
	a64_ldr(armAsm, A64_Q(31), a64_mem_off(RXSCRATCH, (const u8*)nVifMask[2][offX] - (const u8*)nVifMask));
	a64_and(armAsm, a64_as(regX, A64K_16B), a64_as(regX, A64K_16B), a64_as(A64_Q(29), A64K_16B));
	a64_and(armAsm, a64_as(A64_Q(7), A64K_16B), a64_as(A64_Q(7), A64K_16B), a64_as(A64_Q(30), A64K_16B));
	a64_orr(armAsm, a64_as(regX, A64K_16B), a64_as(regX, A64K_16B), a64_as(A64_Q(31), A64K_16B));
	a64_orr(armAsm, a64_as(regX, A64K_16B), a64_as(regX, A64K_16B), a64_as(A64_Q(7), A64K_16B));
	a64_str(armAsm, regX, p->dstIndirect);
}

/* RXARG1 = dest, RXARG2 = src */
static void nVifGen(int usn, int mask, int curCycle)
{
	int usnpart = usn * 2 * 16;
	int maskpart = mask * 16;
	int i;
	VifUnpackNEON vpugen;

	vneon_init(&vpugen);
	vpugen.curCycle  = curCycle;
	vpugen.usn       = !!usn;
	vpugen.doMask    = !!mask;
	vpugen.IsAligned = true;

	for (i = 0; i < 16; ++i)
	{
		nVifCall* ucall = &nVifUpk[((usnpart + maskpart + i) * 4) + curCycle];
		*ucall = NULL;
		if (nVifT[i] == 0)
			continue;

		*ucall = (nVifCall)armStartBlock();
		vneon_unpack(&vpugen, i);
		vneon_mov_dest(&vpugen);
		a64_ret(armAsm);
		armEndBlock();
	}
}

/* Code region for the 256 simple unpack kernels (measured ~30KB; 128KB gives
 * slack). RWX like the other lrps2 arm64 code caches. */
static u8* s_upkCode = NULL;
#define kUpkCodeSize (128 * 1024)

void VifUnpackSSE_Init(void)
{
	int a, b, c;

	if (!s_upkCode)
	{
		s_upkCode = (u8*)memjit_alloc(kUpkCodeSize);
		if (!s_upkCode)
		{
			s_upkCode = NULL;
			log_cb(RETRO_LOG_ERROR, "arm64 VIF unpack dynarec: mmap failed; using the C reference path.\n");
			return;
		}
	}

	log_cb(RETRO_LOG_INFO, "Generating NEON-optimized unpacking functions for VIF interpreters...\n");

	armSetAsmPtr(s_upkCode, kUpkCodeSize);

	for (a = 0; a < 2; a++)
		for (b = 0; b < 2; b++)
			for (c = 0; c < 4; c++)
				nVifGen(a, b, c);
}

void VifUnpackSSE_Destroy(void)
{
	if (s_upkCode)
	{
		memjit_free(s_upkCode, kUpkCodeSize);
		s_upkCode = NULL;
	}
	memset(nVifUpk, 0, sizeof(nVifUpk));
}
