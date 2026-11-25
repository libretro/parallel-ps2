// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
// SPDX-License-Identifier: GPL-3.0+
// Transplanted into lrps2 from isztldav/pcsx2 @ c89cb8a (C.19); near-verbatim.

#pragma once

#include "Common.h"
#include "Vif_Dma.h"
#include "Vif_Dynarec.h"
#include "Vif_Unpack.h" // nVifT
#include "arm64/AsmHelpers.h" // lrps2: minimal ARMSX2-compatible subset

#define xmmCol0 A64_Q(2)
#define xmmCol1 A64_Q(3)
#define xmmCol2 A64_Q(4)
#define xmmCol3 A64_Q(5)
#define xmmRow A64_Q(6)
#define xmmTemp A64_Q(7)

// --------------------------------------------------------------------------------------
//  VifUnpackSSE_Base
// --------------------------------------------------------------------------------------
class VifUnpackNEON_Base
{
public:
	bool usn; // unsigned flag
	bool doMask; // masking write enable flag
	int UnpkLoopIteration;
	int UnpkNoOfIterations;
	int IsAligned;


protected:
	a64_mem dstIndirect;
	a64_mem srcIndirect;
	a64r workReg;
	a64r destReg;
	a64r workGprW;

public:
	VifUnpackNEON_Base();
	virtual ~VifUnpackNEON_Base() = default;

	virtual void xUnpack(int upktype) const;
	virtual bool IsWriteProtectedOp() const = 0;
	virtual bool IsInputMasked() const = 0;
	virtual bool IsUnmaskedOp() const = 0;
	virtual void xMovDest() const;

protected:
	virtual void doMaskWrite(const a64r& regX) const = 0;

	virtual void xShiftR(const a64r& regX, int n) const;
	virtual void xPMOVXX8(const a64r& regX) const;
	virtual void xPMOVXX16(const a64r& regX) const;

	virtual void xUPK_S_32() const;
	virtual void xUPK_S_16() const;
	virtual void xUPK_S_8() const;

	virtual void xUPK_V2_32() const;
	virtual void xUPK_V2_16() const;
	virtual void xUPK_V2_8() const;

	virtual void xUPK_V3_32() const;
	virtual void xUPK_V3_16() const;
	virtual void xUPK_V3_8() const;

	virtual void xUPK_V4_32() const;
	virtual void xUPK_V4_16() const;
	virtual void xUPK_V4_8() const;
	virtual void xUPK_V4_5() const;
};

// --------------------------------------------------------------------------------------
//  VifUnpackSSE_Simple
// --------------------------------------------------------------------------------------
class VifUnpackNEON_Simple : public VifUnpackNEON_Base
{
	typedef VifUnpackNEON_Base _parent;

public:
	int curCycle;

public:
	VifUnpackNEON_Simple(bool usn_, bool domask_, int curCycle_);
	virtual ~VifUnpackNEON_Simple() = default;

	virtual bool IsWriteProtectedOp() const { return false; }
	virtual bool IsInputMasked() const { return false; }
	virtual bool IsUnmaskedOp() const { return !doMask; }

protected:
	virtual void doMaskWrite(const a64r& regX) const;
};

// --------------------------------------------------------------------------------------
//  VifUnpackSSE_Dynarec
// --------------------------------------------------------------------------------------
class VifUnpackNEON_Dynarec : public VifUnpackNEON_Base
{
	typedef VifUnpackNEON_Base _parent;

public:
	bool isFill;
	int doMode; // two bit value representing difference mode
	bool skipProcessing;
	bool inputMasked;

protected:
	const nVifStruct& v; // vif0 or vif1
	const nVifBlock& vB; // some pre-collected data from VifStruct
	int vCL; // internal copy of vif->cl

public:
	VifUnpackNEON_Dynarec(const nVifStruct& vif_, const nVifBlock& vifBlock_);
	VifUnpackNEON_Dynarec(const VifUnpackNEON_Dynarec& src) // copy constructor
		: _parent(src)
		, v(src.v)
		, vB(src.vB)
	{
		isFill = src.isFill;
		vCL = src.vCL;
	}

	virtual ~VifUnpackNEON_Dynarec() = default;

	virtual bool IsWriteProtectedOp() const { return skipProcessing; }
	virtual bool IsInputMasked() const { return inputMasked; }
	virtual bool IsUnmaskedOp() const { return !doMode && !doMask; }

	void ModUnpack(int upknum, bool PostOp);
	void ProcessMasks();
	void CompileRoutine();

protected:
	virtual void doMaskWrite(const a64r& regX) const;
	void SetMasks(int cS) const;
	void writeBackRow() const;

	static VifUnpackNEON_Dynarec FillingWrite(const VifUnpackNEON_Dynarec& src)
	{
		VifUnpackNEON_Dynarec fillingWrite(src);
		fillingWrite.doMask = true;
		fillingWrite.doMode = 0;
		return fillingWrite;
	}
};
