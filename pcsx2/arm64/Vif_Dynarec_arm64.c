/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
 * SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
 * SPDX-License-Identifier: GPL-3.0+ */

/* arm64 VIF unpack dynarec: block compiler and dispatch. The block-cache
 * logic (hash keys, dVifUnpack flow) is the x86 newVif_Dynarec.c's; only
 * the emitter is NEON and the code cache is an RWX region per VIF index. */

#include <memmap.h>

#include "arm64/Vif_UnpackNEON.h"
#include "arm64/AsmHelpers.h"
#include "common/Pcsx2Defs.h"
#include "MTVU.h"
#include "../Config.h"

static void vif_merge_regs(a64r dest, a64r src, int xyzw)
{
	u32 i;
	xyzw &= 0xf;
	if ((A64_CODE(dest) != A64_CODE(src)) && (xyzw != 0))
	{
		if (xyzw == 0x8)
			a64_ins_elem(armAsm, a64_as(dest, A64K_4S), 0, a64_as(src, A64K_4S), 0);
		else if (xyzw == 0xf)
			a64_mov(armAsm, a64_as(dest, A64K_Q), a64_as(src, A64K_Q));
		else
		{
			if (xyzw == 0)
				return;
			if (xyzw == 15)
			{
				a64_mov(armAsm, dest, src);
				return;
			}
			/* reverse */
			xyzw = ((xyzw & 1) << 3) | ((xyzw & 2) << 1) | ((xyzw & 4) >> 1) | ((xyzw & 8) >> 3);

			if ((xyzw & 3) == 3)
			{
				/* xy */
				a64_ins_elem(armAsm, a64_as(dest, A64K_2D), 0, a64_as(src, A64K_2D), 0);
				xyzw &= ~3;
			}
			else if ((xyzw & 12) == 12)
			{
				/* zw */
				a64_ins_elem(armAsm, a64_as(dest, A64K_2D), 1, a64_as(src, A64K_2D), 1);
				xyzw &= ~12;
			}

			/* xyzw */
			for (i = 0; i < 4; i++)
			{
				if (xyzw & (1u << i))
					a64_ins_elem(armAsm, a64_as(dest, A64K_4S), i, a64_as(src, A64K_4S), i);
			}
		}
	}
}

static void maskedVecWrite(a64r reg, a64_mem addr, int xyzw)
{
	switch (xyzw)
	{
		case 5: /* YW */
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 4);
			a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 1, a64_mem_off(RSCRATCHADDR, 0)); /* Y */
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 12);
			a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 3, a64_mem_off(RSCRATCHADDR, 0)); /* W */
			break;

		case 9: /* XW */
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 12);
			a64_str(armAsm, a64_as(reg, A64K_S), addr); /* X */
			a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 3, a64_mem_off(RSCRATCHADDR, 0)); /* W */
			break;

		case 10: /* XZ */
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 8);
			a64_str(armAsm, a64_as(reg, A64K_S), addr); /* X */
			a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 2, a64_mem_off(RSCRATCHADDR, 0)); /* Z */
			break;

		case 3: /* ZW */
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 8);
			a64_st1_lane(armAsm, a64_as(reg, A64K_2D), 1, a64_mem_off(RSCRATCHADDR, 0));
			break;

		case 11: /* XZW */
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 8);
			a64_str(armAsm, a64_as(reg, A64K_S), addr); /* X */
			a64_st1_lane(armAsm, a64_as(reg, A64K_2D), 1, a64_mem_off(RSCRATCHADDR, 0)); /* ZW */
			break;

		case 13: /* XYW */
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 12);
			a64_str(armAsm, a64_as(reg, A64K_D), addr);
			a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 3, a64_mem_off(RSCRATCHADDR, 0));
			break;

		case 6: /* YZ */
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 4);
			a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 1, a64_mem_post(RSCRATCHADDR, 4));
			a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 2, a64_mem_off(RSCRATCHADDR, 0));
			break;

		case 7: /* YZW */
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 4);
			a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 1, a64_mem_post(RSCRATCHADDR, 4));
			a64_st1_lane(armAsm, a64_as(reg, A64K_2D), 1, a64_mem_off(RSCRATCHADDR, 0));
			break;

		case 12: /* XY */
			a64_str(armAsm, a64_as(reg, A64K_D), addr);
			break;

		case 14: /* XYZ */
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 8);
			a64_str(armAsm, a64_as(reg, A64K_D), addr);
			a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 2, a64_mem_off(RSCRATCHADDR, 0)); /* Z */
			break;

		case 4:
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 4);
			a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 1, a64_mem_off(RSCRATCHADDR, 0));
			break; /* Y */
		case 2:
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 8);
			a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 2, a64_mem_off(RSCRATCHADDR, 0));
			break; /* Z */
		case 1:
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 12);
			a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 3, a64_mem_off(RSCRATCHADDR, 0));
			break; /* W */
		case 8:
			a64_str(armAsm, a64_as(reg, A64K_S), addr);
			break; /* X */

		case 0:
			log_cb(RETRO_LOG_ERROR, "maskedVecWrite case 0!\n");
			break;

		default:
			a64_str(armAsm, a64_as(reg, A64K_Q), addr);
			break; /* XYZW */
	}
}

/* Plain RWX mmap per VIF index (8MB, same budget the x86 side gives its
 * code reserve). Bases live here; nVifStruct keeps the write/end
 * cursors so dVifCompile's overflow check mirrors the ARMSX2 original. */
static u8* s_vifCode[2] = {NULL, NULL};
#define kVifCodeSize (8 * 1024 * 1024)

void dVifReserve(int idx)
{
	if (s_vifCode[idx])
		return;
	s_vifCode[idx] = (u8*)memjit_alloc(kVifCodeSize);
	if (!s_vifCode[idx])
	{
		s_vifCode[idx] = NULL;
		log_cb(RETRO_LOG_ERROR, "arm64 VIF%d dynarec: mmap failed; unpacks fall back to the C reference path.\n", idx);
		return;
	}
	nVif[idx].recWritePtr = s_vifCode[idx];
	nVif[idx].recEndPtr = s_vifCode[idx] + (kVifCodeSize - 256 * 1024);
}

PCSX2_ALIGN(16) nVifStruct nVif[2];

void dVifReset(int idx)
{
	nVif[idx].idx = idx;
	vif_hash_reset(&nVif[idx].vifBlocks);
	if (s_vifCode[idx])
	{
		nVif[idx].recWritePtr = s_vifCode[idx];
		nVif[idx].recEndPtr = s_vifCode[idx] + (kVifCodeSize - 256 * 1024);
	}
}

void dVifRelease(int idx)
{
	vif_hash_clear(&nVif[idx].vifBlocks);
	if (s_vifCode[idx])
	{
		memjit_free(s_vifCode[idx], kVifCodeSize);
		s_vifCode[idx] = NULL;
		nVif[idx].recWritePtr = NULL;
		nVif[idx].recEndPtr = NULL;
	}
}

static void vneon_init_dynarec(VifUnpackNEON* p, const nVifStruct* vif_, const nVifBlock* vifBlock_)
{
	const int wl = vifBlock_->f.wl ? vifBlock_->f.wl : 256; /* 0 is taken as 256 (KH2) */
	vneon_init(p);
	p->dynarec   = true;
	p->v         = vif_;
	p->vB        = vifBlock_;
	p->isFill    = (vifBlock_->f.cl < wl);
	p->usn       = (vifBlock_->f.upkType >> 5) & 1;
	p->doMask    = (vifBlock_->f.upkType >> 4) & 1;
	p->doMode    = vifBlock_->f.mode & 3;
	p->IsAligned = vifBlock_->f.aligned;
	p->vCL       = 0;
}

static __fi u32 makeMergeMask(u32 x)
{
	return ((x & 0x40) >> 6) | ((x & 0x10) >> 3) | (x & 4) | ((x & 1) << 3);
}

static __fi void SetMasks(const VifUnpackNEON* p, int cS)
{
	u32 m0;
	u32 m3;
	u32 m2;
	const int idx = p->v->idx;
	const vifStruct* vif = MTVU_VIF_X(idx);

	/* This could have ended up copying the row when there was no row to write.1810080 */
	m0 = p->vB->f.mask; /* The actual mask example 0x03020100 */
	m3 = ((m0 & 0xaaaaaaaa) >> 1) & ~m0; /* all the upper bits, so our example 0x01010000 & 0xFCFDFEFF = 0x00010000 just the cols (shifted right for maskmerge) */
	m2 = (m0 & 0x55555555) & (~m0 >> 1); /* 0x1000100 & 0xFE7EFF7F = 0x00000100 Just the row */

	if ((p->doMask && m2) || p->doMode)
	{
		armLoadPtr(xmmRow, &vif->MaskRow);
	}
	if (p->doMask && m3)
	{
		armLoadPtr(xmmCol0, &vif->MaskCol);
		if ((cS >= 2) && (m3 & 0x0000ff00))
			a64_dup_elem(armAsm, a64_as(xmmCol1, A64K_4S), a64_as(xmmCol0, A64K_4S), 1);
		if ((cS >= 3) && (m3 & 0x00ff0000))
			a64_dup_elem(armAsm, a64_as(xmmCol2, A64K_4S), a64_as(xmmCol0, A64K_4S), 2);
		if ((cS >= 4) && (m3 & 0xff000000))
			a64_dup_elem(armAsm, a64_as(xmmCol3, A64K_4S), a64_as(xmmCol0, A64K_4S), 3);
		if ((cS >= 1) && (m3 & 0x000000ff))
			a64_dup_elem(armAsm, a64_as(xmmCol0, A64K_4S), a64_as(xmmCol0, A64K_4S), 0);
	}
}

void vneon_dynarec_mask_write(const VifUnpackNEON* p, a64r regX)
{
	const int cc = p->vCL < 3 ? p->vCL : 3;
	u32 m0 = (p->vB->f.mask >> (cc * 8)) & 0xff; /* The actual mask example 0xE4 (protect, col, row, clear) */
	u32 m3 = ((m0 & 0xaa) >> 1) & ~m0; /* all the upper bits (cols shifted right) cancelling out any write protects 0x10 */
	u32 m2 = (m0 & 0x55) & (~m0 >> 1); /* all the lower bits (rows)cancelling out any write protects 0x04 */
	u32 m4 = (m0 & ~((m3 << 1) | m2)) & 0x55; /* = 0xC0 & 0x55 = 0x40 (for merge mask) */

	m2 = makeMergeMask(m2);
	m3 = makeMergeMask(m3);
	m4 = makeMergeMask(m4);

	if (p->doMask && m2) /* Merge MaskRow */
	{
		vif_merge_regs(regX, xmmRow, m2);
	}

	if (p->doMask && m3) /* Merge MaskCol */
	{
		vif_merge_regs(regX, armQRegister(A64_CODE(xmmCol0) + cc), m3);
	}

	if (p->doMode)
	{
		u32 m5 = ~(m2 | m3 | m4) & 0xf;

		if (!p->doMask)
			m5 = 0xf;

		if (m5 < 0xf)
		{
			a64_movi(armAsm, a64_as(xmmTemp, A64K_4S), 0);
			if (p->doMode == 3)
			{
				vif_merge_regs(xmmRow, regX, m5);
			}
			else
			{
				vif_merge_regs(xmmTemp, xmmRow, m5);
				a64_add(armAsm, a64_as(regX, A64K_4S), a64_as(regX, A64K_4S), a64_as(xmmTemp, A64K_4S));
				if (p->doMode == 2)
					vif_merge_regs(xmmRow, regX, m5);
			}
		}
		else
		{
			if (p->doMode == 3)
			{
				a64_mov(armAsm, xmmRow, regX);
			}
			else
			{
				a64_add(armAsm, a64_as(regX, A64K_4S), a64_as(regX, A64K_4S), a64_as(xmmRow, A64K_4S));
				if (p->doMode == 2)
				{
					a64_mov(armAsm, xmmRow, regX);
				}
			}
		}
	}

	if (p->doMask && m4)
		maskedVecWrite(regX, p->dstIndirect, m4 ^ 0xf);
	else
		a64_str(armAsm, regX, p->dstIndirect);
}

static void writeBackRow(const VifUnpackNEON* p)
{
	const int idx = p->v->idx;
	armStorePtr(xmmRow, &MTVU_VIF_X(idx)->MaskRow);
}

static void ModUnpack(VifUnpackNEON* p, int upknum, bool PostOp)
{
	switch (upknum)
	{
		case 0:
		case 1:
		case 2:
			if (PostOp)
			{
				p->UnpkLoopIteration++;
				p->UnpkLoopIteration = p->UnpkLoopIteration & 0x3;
			}
			break;

		case 4:
		case 5:
		case 6:
			if (PostOp)
			{
				p->UnpkLoopIteration++;
				p->UnpkLoopIteration = p->UnpkLoopIteration & 0x1;
			}
			break;

		case 8:
			if (PostOp)
			{
				p->UnpkLoopIteration++;
				p->UnpkLoopIteration = p->UnpkLoopIteration & 0x1;
			}
			break;
		case 9:
			if (!PostOp)
			{
				p->UnpkLoopIteration++;
			}
			break;
		case 10:
			if (!PostOp)
			{
				p->UnpkLoopIteration++;
			}
			break;

		case 12:
			break;
		case 13:
			break;
		case 14:
			break;
		case 15:
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

static void ProcessMasks(VifUnpackNEON* p)
{
	int cc;
	u32 full_mask;
	u32 rowcol_mask;
	p->skipProcessing = false;
	p->inputMasked = false;

	if (!p->doMask)
		return;

	cc = p->vCL < 3 ? p->vCL : 3;
	full_mask = (p->vB->f.mask >> (cc * 8)) & 0xff;
	rowcol_mask = ((full_mask >> 1) | full_mask) & 0x55; /* Rows or Cols being written instead of data, or protected. */

	/* Every channel is write protected for this cycle, no need to process anything. */
	p->skipProcessing = full_mask == 0xff;

	/* All channels are masked, no reason to process anything here. */
	p->inputMasked = rowcol_mask == 0x55;
}

static void CompileRoutine(VifUnpackNEON* p)
{
	const int wl = p->vB->f.wl ? p->vB->f.wl : 256; /* 0 is taken as 256 (KH2) */
	const int upkNum = p->vB->f.upkType & 0xf;
	const u8 vift = nVifT[upkNum];
	const int cycleSize = p->isFill ? p->vB->f.cl : wl;
	const int blockSize = p->isFill ? wl : p->vB->f.cl;
	const int skipSize = blockSize - cycleSize;

	uint vNum = p->vB->f.num ? p->vB->f.num : 256;
	p->doMode = (upkNum == 0xf) ? 0 : p->doMode; /* V4_5 has no mode feature. */
	p->UnpkNoOfIterations = 0;

	/* Value passed determines # of col regs we need to load */
	SetMasks(p, p->isFill ? blockSize : cycleSize);

	while (vNum)
	{
		/* Determine if reads/processing can be skipped. */
		ProcessMasks(p);

		if (p->vCL < cycleSize)
		{
			ModUnpack(p, upkNum, false);
			vneon_unpack(p, upkNum);
			vneon_mov_dest(p);
			ModUnpack(p, upkNum, true);

			p->dstIndirect = armOffsetMemOperand(p->dstIndirect, 16);
			p->srcIndirect = armOffsetMemOperand(p->srcIndirect, vift);

			vNum--;
			if (++p->vCL == blockSize)
				p->vCL = 0;
		}
		else if (p->isFill)
		{
			vneon_unpack(p, upkNum);
			vneon_mov_dest(p);

			/* p->dstIndirect += 16; */
			p->dstIndirect = armOffsetMemOperand(p->dstIndirect, 16);

			vNum--;
			if (++p->vCL == blockSize)
				p->vCL = 0;
		}
		else
		{
			/* p->dstIndirect += (16 * skipSize); */
			p->dstIndirect = armOffsetMemOperand(p->dstIndirect, 16 * skipSize);
			p->vCL = 0;
		}
	}

	if (p->doMode >= 2)
		writeBackRow(p);

	a64_ret(armAsm);
}

static u16 dVifComputeLength(uint cl, uint wl, u8 num, bool isFill)
{
	uint length = (num > 0) ? (num * 16) : 4096; /* 0 = 256 */

	if (!isFill)
	{
		uint skipSize = (cl - wl) * 16;
		uint blocks = (num + (wl - 1)) / wl; /* Need to round up num's to calculate skip size correctly. */
		length += (blocks - 1) * skipSize;
	}

	return (u16)(length < 0xFFFFu ? length : 0xFFFFu);
}

static __fi nVifBlock* dVifCompile(int idx, nVifBlock* block, bool isFill)
{
	VifUnpackNEON vpu;
	nVifStruct* v = &nVif[idx];

	/* Check size before the compilation */
	if (v->recWritePtr >= v->recEndPtr)
	{
		log_cb(RETRO_LOG_INFO, "nVif Recompiler Cache Reset! [0x%016" PRIXPTR " > 0x%016" PRIXPTR "]\n",
			(uptr)v->recWritePtr, (uptr)v->recEndPtr);
		dVifReset(idx);
	}

	/* Compile the block now */
	armSetAsmPtr(v->recWritePtr, v->recEndPtr - v->recWritePtr);

	/* +1 bias keeps 0 as the empty-cell sentinel; reserve is 8MB so u32 always fits.
	 * Offsets are relative to s_vifCode[idx]; recReserve, the x86 code
	 * reserve, stays empty here. */
	block->f.startOffset = (u32)((u8*)armStartBlock() - s_vifCode[idx]) + 1;
	block->f.length = dVifComputeLength(block->f.cl, block->f.wl, block->f.num, isFill);
	vif_hash_add(&v->vifBlocks, block);

	vneon_init_dynarec(&vpu, v, block);
	CompileRoutine(&vpu);

	v->recWritePtr = armEndBlock();

	return block;
}

/* Inlined into dvif_unpack once per unit, so idx is a constant in each
 * copy just as the template parameter was. */
static __fi void dVifUnpackT(int idx, const u8* data, int isFill)
{
	nVifStruct* v = &nVif[idx];
	vifStruct* vif = MTVU_VIF_X(idx);
	VIFregisters* vifRegs = MTVU_VIF_XREGS(idx);
	u8 upkType;
	int doMask;
	nVifBlock block;
	nVifBlock* b;
	u32 hash_key, key0, key1;

	/* The C reference path runs when the code region failed to map. */
	if (!s_vifCode[idx])
	{
		_nVifUnpack(idx, data, vifRegs->mode, isFill);
		return;
	}

	upkType = (vif->cmd & 0x1f) | (vif->usn << 5);
	doMask = isFill ? 1 : (vif->cmd & 0x10);

	/* Performance note: initial code was using u8/u16 field of the struct
	 * directly. However reading back the data (as u32) in HashBucket.find
	 * leads to various memory stalls. So it is way faster to manually build the data
	 * in u32 (aka x86 register).
	 *
	 * Warning the order of data in hash_key/key0/key1 depends on the nVifBlock struct */
	hash_key = (u32)(upkType & 0xFF) << 8 | (vifRegs->num & 0xFF);

	key1 = ((u32)vifRegs->cycle.wl << 24) | ((u32)vifRegs->cycle.cl << 16) | ((u32)(vif->start_aligned & 0xFF) << 8) | ((u32)vifRegs->mode & 0xFF);
	if ((upkType & 0xf) != 9)
		key1 &= 0xFFFF01FF;

	/* Zero out the mask parameter if it's unused -- games leave random junk
	 * values here which cause false recblock cache misses. */
	key0 = doMask ? vifRegs->mask : 0;

	block.k.hash_key = hash_key;
	block.k.key0 = key0;
	block.k.key1 = key1;

	/* Search the cache before trying to compile the block */
	b = vif_hash_find(&v->vifBlocks, hash_key, (u64)key0 | ((u64)key1 << 32));
	if (unlikely(b == NULL))
		b = dVifCompile(idx, &block, isFill);

	/* Execute the block */
	{
		u8* startmem;
		u8* endmem;
		const VURegs* VU = &vuRegs[idx];
		const uint vuMemLimit = idx ? 0x4000 : 0x1000;

		startmem = VU->Mem + (vif->tag.addr & (vuMemLimit - 0x10));
		endmem = VU->Mem + vuMemLimit;

		/* No wrapping, you can run the fast dynarec */
		if (likely((startmem + b->f.length) <= endmem))
			((nVifrecCall)(s_vifCode[idx] + (b->f.startOffset - 1)))((uptr)startmem, (uptr)data);
		else
			_nVifUnpack(idx, data, vifRegs->mode, isFill);
	}
}

/* For Vif_Unpack.c. */
void dvif_unpack(int idx, const u8* data, int isFill)
{
	if (idx)
		dVifUnpackT(1, data, isFill);
	else
		dVifUnpackT(0, data, isFill);
}
