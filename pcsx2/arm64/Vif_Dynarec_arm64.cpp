// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
// SPDX-License-Identifier: GPL-3.0+
//
// arm64 VIF unpack dynarec (C.19) -- block compiler + dispatch, transplanted
// from isztldav/pcsx2 @ c89cb8a (pcsx2/arm64/Vif_Dynarec.cpp) into lrps2.
// The block-cache logic (hash keys, dVifUnpack flow) is line-for-line the same
// as lrps2's x86 newVif_Dynarec.cpp -- the two trees never diverged here --
// only the emitter is NEON and the code cache is a plain RWX mmap per VIF
// index (lrps2's code-map reserves stay x86-only).

#include "arm64/Vif_UnpackNEON.h"
#include "common/Pcsx2Defs.h"
#include <algorithm>
#include "arm64/AsmHelpers.h"
#include "MTVU.h"


#include <cstdlib>
#include <sys/mman.h>
#include <memmap.h>
#include "../Config.h"

namespace a64 = vixl::aarch64;

static void mVUmergeRegs(const vixl::aarch64::VRegister& dest, const vixl::aarch64::VRegister& src, int xyzw, bool modXYZW = false, bool canModifySrc = false)
{
	xyzw &= 0xf;
	if ((dest.GetCode() != src.GetCode()) && (xyzw != 0))
	{
		if (xyzw == 0x8)
			armAsm->Mov(dest.V4S(), 0, src.V4S(), 0);
		else if (xyzw == 0xf)
			armAsm->Mov(dest.Q(), src.Q());
		else
		{
			if (modXYZW)
			{
				if (xyzw == 1)
				{
					armAsm->Ins(dest.V4S(), 3, src.V4S(), 0);
					return;
				}
				else if (xyzw == 2)
				{
					armAsm->Ins(dest.V4S(), 2, src.V4S(), 0);
					return;
				}
				else if (xyzw == 4)
				{
					armAsm->Ins(dest.V4S(), 1, src.V4S(), 0);
					return;
				}
			}

			if (xyzw == 0)
				return;
			if (xyzw == 15)
			{
				armAsm->Mov(dest, src);
				return;
			}
			if (xyzw == 14 && canModifySrc)
			{
				// xyz - we can get rid of the mov if we swap the RA around
				armAsm->Mov(src.V4S(), 3, dest.V4S(), 3);
				armAsm->Mov(dest.V16B(), src.V16B());
				return;
			}

			// reverse
			xyzw = ((xyzw & 1) << 3) | ((xyzw & 2) << 1) | ((xyzw & 4) >> 1) | ((xyzw & 8) >> 3);

			if ((xyzw & 3) == 3)
			{
				// xy
				armAsm->Mov(dest.V2D(), 0, src.V2D(), 0);
				xyzw &= ~3;
			}
			else if ((xyzw & 12) == 12)
			{
				// zw
				armAsm->Mov(dest.V2D(), 1, src.V2D(), 1);
				xyzw &= ~12;
			}

			// xyzw
			for (u32 i = 0; i < 4; i++)
			{
				if (xyzw & (1u << i))
					armAsm->Mov(dest.V4S(), i, src.V4S(), i);
			}
		}
	}
}

static void maskedVecWrite(const a64::VRegister& reg, const a64::MemOperand& addr, int xyzw)
{
	switch (xyzw)
	{
		case 5: // YW
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 4);
			armAsm->St1(reg.V4S(), 1, a64::MemOperand(RSCRATCHADDR)); // Y
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 12);
			armAsm->St1(reg.V4S(), 3, a64::MemOperand(RSCRATCHADDR)); // W
			break;

		case 9: // XW
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 12);
			armAsm->Str(reg.S(), addr); // X
			armAsm->St1(reg.V4S(), 3, a64::MemOperand(RSCRATCHADDR)); // W
			break;

		case 10: //XZ
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 8);
			armAsm->Str(reg.S(), addr); // X
			armAsm->St1(reg.V4S(), 2, a64::MemOperand(RSCRATCHADDR)); // Z
			break;

		case 3: // ZW
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 8);
			armAsm->St1(reg.V2D(), 1, a64::MemOperand(RSCRATCHADDR));
			break;

		case 11: //XZW
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 8);
			armAsm->Str(reg.S(), addr); // X
			armAsm->St1(reg.V2D(), 1, a64::MemOperand(RSCRATCHADDR)); // ZW
			break;

		case 13: // XYW
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 12);
			armAsm->Str(reg.D(), addr);
			armAsm->St1(reg.V4S(), 3, a64::MemOperand(RSCRATCHADDR));
			break;

		case 6: // YZ
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 4);
			armAsm->St1(reg.V4S(), 1, a64::MemOperand(RSCRATCHADDR, 4, a64::PostIndex));
			armAsm->St1(reg.V4S(), 2, a64::MemOperand(RSCRATCHADDR));
			break;

		case 7: // YZW
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 4);
			armAsm->St1(reg.V4S(), 1, a64::MemOperand(RSCRATCHADDR, 4, a64::PostIndex));
			armAsm->St1(reg.V2D(), 1, a64::MemOperand(RSCRATCHADDR));
			break;

		case 12: // XY
			armAsm->Str(reg.D(), addr);
			break;

		case 14: // XYZ
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 8);
			armAsm->Str(reg.D(), addr);
			armAsm->St1(reg.V4S(), 2, a64::MemOperand(RSCRATCHADDR)); // Z
			break;

		case 4:
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 4);
			armAsm->St1(reg.V4S(), 1, a64::MemOperand(RSCRATCHADDR));
			break; // Y
		case 2:
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 8);
			armAsm->St1(reg.V4S(), 2, a64::MemOperand(RSCRATCHADDR));
			break; // Z
		case 1:
			armGetMemOperandInRegister(RSCRATCHADDR, addr, 12);
			armAsm->St1(reg.V4S(), 3, a64::MemOperand(RSCRATCHADDR));
			break; // W
		case 8:
			armAsm->Str(reg.S(), addr);
			break; // X

		case 0:
			log_cb(RETRO_LOG_ERROR, "maskedVecWrite case 0!\n");
			break;

		default:
			armAsm->Str(reg.Q(), addr);
			break; // XYZW
	}
}

// Plain RWX mmap per VIF index (8MB, same budget the x86 side gives its
// code reserve). Bases live here; nVifStruct keeps the write/end
// cursors so dVifCompile's overflow check mirrors the ARMSX2 original.
static u8* s_vifCode[2] = {nullptr, nullptr};
static constexpr size_t kVifCodeSize = 8 * 1024 * 1024;

void dVifReserve(int idx)
{
	if (s_vifCode[idx])
		return;
	s_vifCode[idx] = (u8*)memjit_alloc(kVifCodeSize);
	if (!s_vifCode[idx])
	{
		s_vifCode[idx] = nullptr;
		log_cb(RETRO_LOG_ERROR, "arm64 VIF%d dynarec: mmap failed; unpacks fall back to the C reference path.\n", idx);
		return;
	}
	nVif[idx].recWritePtr = s_vifCode[idx];
	nVif[idx].recEndPtr = s_vifCode[idx] + (kVifCodeSize - 256 * 1024);
}

alignas(16) nVifStruct nVif[2];

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
		s_vifCode[idx] = nullptr;
		nVif[idx].recWritePtr = nullptr;
		nVif[idx].recEndPtr = nullptr;
	}
}

VifUnpackNEON_Dynarec::VifUnpackNEON_Dynarec(const nVifStruct& vif_, const nVifBlock& vifBlock_)
	: v(vif_)
	, vB(vifBlock_)
{
	const int wl = vB.f.wl ? vB.f.wl : 256; //0 is taken as 256 (KH2)
	isFill = (vB.f.cl < wl);
	usn = (vB.f.upkType >> 5) & 1;
	doMask = (vB.f.upkType >> 4) & 1;
	doMode = vB.f.mode & 3;
	IsAligned = vB.f.aligned;
	vCL = 0;
}

__fi void makeMergeMask(u32& x)
{
	x = ((x & 0x40) >> 6) | ((x & 0x10) >> 3) | (x & 4) | ((x & 1) << 3);
}

__fi void VifUnpackNEON_Dynarec::SetMasks(int cS) const
{
	const int idx = v.idx;
	const vifStruct& vif = MTVU_VifX;

	//This could have ended up copying the row when there was no row to write.1810080
	u32 m0 = vB.f.mask; //The actual mask example 0x03020100
	u32 m3 = ((m0 & 0xaaaaaaaa) >> 1) & ~m0; //all the upper bits, so our example 0x01010000 & 0xFCFDFEFF = 0x00010000 just the cols (shifted right for maskmerge)
	u32 m2 = (m0 & 0x55555555) & (~m0 >> 1); // 0x1000100 & 0xFE7EFF7F = 0x00000100 Just the row

	if ((doMask && m2) || doMode)
	{
		armLoadPtr(xmmRow, &vif.MaskRow);
	}
	if (doMask && m3)
	{
		armLoadPtr(xmmCol0, &vif.MaskCol);
		if ((cS >= 2) && (m3 & 0x0000ff00))
			armAsm->Dup(xmmCol1.V4S(), xmmCol0.V4S(), 1);
		if ((cS >= 3) && (m3 & 0x00ff0000))
			armAsm->Dup(xmmCol2.V4S(), xmmCol0.V4S(), 2);
		if ((cS >= 4) && (m3 & 0xff000000))
			armAsm->Dup(xmmCol3.V4S(), xmmCol0.V4S(), 3);
		if ((cS >= 1) && (m3 & 0x000000ff))
			armAsm->Dup(xmmCol0.V4S(), xmmCol0.V4S(), 0);
	}
}

void VifUnpackNEON_Dynarec::doMaskWrite(const vixl::aarch64::VRegister& regX) const
{
	const int cc = std::min(vCL, 3);
	u32 m0 = (vB.f.mask >> (cc * 8)) & 0xff; //The actual mask example 0xE4 (protect, col, row, clear)
	u32 m3 = ((m0 & 0xaa) >> 1) & ~m0; //all the upper bits (cols shifted right) cancelling out any write protects 0x10
	u32 m2 = (m0 & 0x55) & (~m0 >> 1); // all the lower bits (rows)cancelling out any write protects 0x04
	u32 m4 = (m0 & ~((m3 << 1) | m2)) & 0x55; //  = 0xC0 & 0x55 = 0x40 (for merge mask)

	makeMergeMask(m2);
	makeMergeMask(m3);
	makeMergeMask(m4);

	if (doMask && m2) // Merge MaskRow
	{
		mVUmergeRegs(regX, xmmRow, m2);
	}

	if (doMask && m3) // Merge MaskCol
	{
		mVUmergeRegs(regX, armQRegister(xmmCol0.GetCode() + cc), m3);
	}

	if (doMode)
	{
		u32 m5 = ~(m2 | m3 | m4) & 0xf;

		if (!doMask)
			m5 = 0xf;

		if (m5 < 0xf)
		{
			armAsm->Movi(xmmTemp.V4S(), 0);
			if (doMode == 3)
			{
				mVUmergeRegs(xmmRow, regX, m5, false, false);
			}
			else
			{
				mVUmergeRegs(xmmTemp, xmmRow, m5, false, false);
				armAsm->Add(regX.V4S(), regX.V4S(), xmmTemp.V4S());
				if (doMode == 2)
					mVUmergeRegs(xmmRow, regX, m5, false, false);
			}
		}
		else
		{
			if (doMode == 3)
			{
				armAsm->Mov(xmmRow, regX);
			}
			else
			{
				armAsm->Add(regX.V4S(), regX.V4S(), xmmRow.V4S());
				if (doMode == 2)
				{
					armAsm->Mov(xmmRow, regX);
				}
			}
		}
	}

	if (doMask && m4)
		maskedVecWrite(regX, dstIndirect, m4 ^ 0xf);
	else
		armAsm->Str(regX, dstIndirect);
}

void VifUnpackNEON_Dynarec::writeBackRow() const
{
	const int idx = v.idx;
	armStorePtr(xmmRow, &(MTVU_VifX.MaskRow));
}

void VifUnpackNEON_Dynarec::ModUnpack(int upknum, bool PostOp)
{
	switch (upknum)
	{
		case 0:
		case 1:
		case 2:
			if (PostOp)
			{
				UnpkLoopIteration++;
				UnpkLoopIteration = UnpkLoopIteration & 0x3;
			}
			break;

		case 4:
		case 5:
		case 6:
			if (PostOp)
			{
				UnpkLoopIteration++;
				UnpkLoopIteration = UnpkLoopIteration & 0x1;
			}
			break;

		case 8:
			if (PostOp)
			{
				UnpkLoopIteration++;
				UnpkLoopIteration = UnpkLoopIteration & 0x1;
			}
			break;
		case 9:
			if (!PostOp)
			{
				UnpkLoopIteration++;
			}
			break;
		case 10:
			if (!PostOp)
			{
				UnpkLoopIteration++;
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
			// TODO: Needs hardware testing.
			// Dynasty Warriors 5: Empire  - Player 2 chose a character menu.
			log_cb(RETRO_LOG_WARN, "Vpu/Vif: Invalid Unpack %d\n", upknum);
			break;
	}
}

void VifUnpackNEON_Dynarec::ProcessMasks()
{
	skipProcessing = false;
	inputMasked = false;

	if (!doMask)
		return;

	const int cc = std::min(vCL, 3);
	const u32 full_mask = (vB.f.mask >> (cc * 8)) & 0xff;
	const u32 rowcol_mask = ((full_mask >> 1) | full_mask) & 0x55; // Rows or Cols being written instead of data, or protected.

	// Every channel is write protected for this cycle, no need to process anything.
	skipProcessing = full_mask == 0xff;

	// All channels are masked, no reason to process anything here.
	inputMasked = rowcol_mask == 0x55;
}

void VifUnpackNEON_Dynarec::CompileRoutine()
{
	const int wl = vB.f.wl ? vB.f.wl : 256; //0 is taken as 256 (KH2)
	const int upkNum = vB.f.upkType & 0xf;
	const u8& vift = nVifT[upkNum];
	const int cycleSize = isFill ? vB.f.cl : wl;
	const int blockSize = isFill ? wl : vB.f.cl;
	const int skipSize = blockSize - cycleSize;

	uint vNum = vB.f.num ? vB.f.num : 256;
	doMode = (upkNum == 0xf) ? 0 : doMode; // V4_5 has no mode feature.
	UnpkNoOfIterations = 0;

	// Value passed determines # of col regs we need to load
	SetMasks(isFill ? blockSize : cycleSize);

	while (vNum)
	{
		// Determine if reads/processing can be skipped.
		ProcessMasks();

		if (vCL < cycleSize)
		{
			ModUnpack(upkNum, false);
			xUnpack(upkNum);
			xMovDest();
			ModUnpack(upkNum, true);

			dstIndirect = armOffsetMemOperand(dstIndirect, 16);
			srcIndirect = armOffsetMemOperand(srcIndirect, vift);

			vNum--;
			if (++vCL == blockSize)
				vCL = 0;
		}
		else if (isFill)
		{
			xUnpack(upkNum);
			xMovDest();

			// dstIndirect += 16;
			dstIndirect = armOffsetMemOperand(dstIndirect, 16);

			vNum--;
			if (++vCL == blockSize)
				vCL = 0;
		}
		else
		{
			// dstIndirect += (16 * skipSize);
			dstIndirect = armOffsetMemOperand(dstIndirect, 16 * skipSize);
			vCL = 0;
		}
	}

	if (doMode >= 2)
		writeBackRow();

	armAsm->Ret();
}

static u16 dVifComputeLength(uint cl, uint wl, u8 num, bool isFill)
{
	uint length = (num > 0) ? (num * 16) : 4096; // 0 = 256

	if (!isFill)
	{
		uint skipSize = (cl - wl) * 16;
		uint blocks = (num + (wl - 1)) / wl; //Need to round up num's to calculate skip size correctly.
		length += (blocks - 1) * skipSize;
	}

	return std::min(length, 0xFFFFu);
}

_vifT __fi nVifBlock* dVifCompile(nVifBlock& block, bool isFill)
{
	nVifStruct& v = nVif[idx];

	// Check size before the compilation
	if (v.recWritePtr >= v.recEndPtr)
	{
		log_cb(RETRO_LOG_INFO, "nVif Recompiler Cache Reset! [0x%016\n" PRIXPTR " > 0x%016" PRIXPTR "]",
			(uptr)v.recWritePtr, (uptr)v.recEndPtr);
		dVifReset(idx);
	}

	// Compile the block now
	armSetAsmPtr(v.recWritePtr, v.recEndPtr - v.recWritePtr, nullptr);

	// +1 bias keeps 0 as the empty-cell sentinel; reserve is 8MB so u32 always fits
	/* Offsets are relative to this port's own VIF code region: arm64 mmaps
	 * s_vifCode[idx] in dVifReserve and never populates nVifStruct::
	 * recReserve, the x86 code reserve, which stays empty here. */
	block.f.startOffset = (u32)((u8*)armStartBlock() - s_vifCode[idx]) + 1;
	block.f.length = dVifComputeLength(block.f.cl, block.f.wl, block.f.num, isFill);
	vif_hash_add(&v.vifBlocks, &block);

	VifUnpackNEON_Dynarec(v, block).CompileRoutine();

	v.recWritePtr = armEndBlock();

	return &block;
}

_vifT __fi void dVifUnpack(const u8* data, int isFill)
{
	nVifStruct& v = nVif[idx];
	vifStruct& vif = MTVU_VifX;
	VIFregisters& vifRegs = MTVU_VifXRegs;

	// The C reference path runs when the code region failed to map
	// (pre-C.19 arm64 behaviour).
	if (!s_vifCode[idx])
	{
		_nVifUnpack(idx, data, vifRegs.mode, isFill);
		return;
	}

	const u8 upkType = (vif.cmd & 0x1f) | (vif.usn << 5);
	const int doMask = isFill ? 1 : (vif.cmd & 0x10);

	nVifBlock block;

	// Performance note: initial code was using u8/u16 field of the struct
	// directly. However reading back the data (as u32) in HashBucket.find
	// leads to various memory stalls. So it is way faster to manually build the data
	// in u32 (aka x86 register).
	//
	// Warning the order of data in hash_key/key0/key1 depends on the nVifBlock struct
	u32 hash_key = (u32)(upkType & 0xFF) << 8 | (vifRegs.num & 0xFF);

	u32 key1 = ((u32)vifRegs.cycle.wl << 24) | ((u32)vifRegs.cycle.cl << 16) | ((u32)(vif.start_aligned & 0xFF) << 8) | ((u32)vifRegs.mode & 0xFF);
	if ((upkType & 0xf) != 9)
		key1 &= 0xFFFF01FF;

	// Zero out the mask parameter if it's unused -- games leave random junk
	// values here which cause false recblock cache misses.
	u32 key0 = doMask ? vifRegs.mask : 0;

	block.k.hash_key = hash_key;
	block.k.key0 = key0;
	block.k.key1 = key1;

	// Seach in cache before trying to compile the block
	nVifBlock* b = vif_hash_find(&v.vifBlocks, hash_key, (u64)key0 | ((u64)key1 << 32));
	if (unlikely(b == nullptr))
		b = dVifCompile<idx>(block, isFill);

	{ // Execute the block
		const VURegs& VU = vuRegs[idx];
		const uint vuMemLimit = idx ? 0x4000 : 0x1000;

		u8* startmem = VU.Mem + (vif.tag.addr & (vuMemLimit - 0x10));
		u8* endmem = VU.Mem + vuMemLimit;

		if (likely((startmem + b->f.length) <= endmem))
		{
			// No wrapping, you can run the fast dynarec
			((nVifrecCall)(s_vifCode[idx] + (b->f.startOffset - 1)))((uptr)startmem, (uptr)data);
		}
		else
		{
			_nVifUnpack(idx, data, vifRegs.mode, isFill);
		}
	}
}

template void dVifUnpack<0>(const u8* data, int isFill);
template void dVifUnpack<1>(const u8* data, int isFill);

/* For Vif_Unpack.c. */
void dvif_unpack(int idx, const u8* data, int isFill)
{
	if (idx)
		dVifUnpack<1>(data, isFill);
	else
		dVifUnpack<0>(data, isFill);
}
