// SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "common/Pcsx2Defs.h"

// ARM64 microVU recompiler — host register allocator (Phase 7, task 7.2b).
//
// This is the ARM64 counterpart to the `microRegAlloc` class in
// pcsx2/x86/microVU_IR.h (lines 226-1139). Per the Phase 7 strategy
// (PROGRESS.md / [[arm64-microvu-architecture]]) microVU is *parallel-cloned*
// into pcsx2/arm64/ rather than #included from x86/.
//
// What changed vs the x86 original:
//   * VF registers live in NEON v-regs (q view, 128-bit) instead of x86 xmm.
//   * VI registers live in ARM64 w-regs instead of x86 GPRs.
//   * The x86 emitter (absolute-address ptr[...] loads/stores) becomes
//     base+offset addressing against a designated VU-state pointer register
//     (RVUSTATE = x19 = &vuRegs[index]).
//   * The COP2 / macro-mode path (`regAllocCOP2`, `_allocVFtoXMMreg`,
//     `_allocX86reg`, the pxmmregs/x86regs interaction, updateCOP2AllocState,
//     flushPartialForCOP2, clearRegCOP2/clearGPRCOP2) is DROPPED. That path is
//     the EE-side VU0-macro allocator which is Phase 7.9; on ARM64 the macro
//     ops currently run through the Phase 5.3 inline-interp fallback. Removing
//     it makes the bookkeeping a faithful but much smaller mirror of the x86
//     allocator's "normal" (microVU-thread) path.
//
// The emission helpers (mVUloadReg/mVUsaveReg/mVUmergeRegs/loadIreg) and the
// register-map constants below are PROVISIONAL: they are exercised for real
// only once the dispatcher (7.2d) and opcode emission (7.5) land and can pin
// the final ABI. They are written to mirror the x86 lane semantics exactly.

#include "aVU.h"

#include "arm64/AsmHelpers.h"

#include <array>


//------------------------------------------------------------------
// ARM64 microVU host register map (provisional — see header note)
//------------------------------------------------------------------
// NEON (VF):
//   v0 .. v(mVU_VF_TOTAL-1)  — the VF allocation pool (slot i == host v_i)
//   v24                      — PQ latency reg (Q/P instances), reserved
//   v29/v30/v31              — shared emitter scratch (AsmHelpers RQSCRATCH*), reserved
//
// GPR (VI + scratch):
//   x19  RVUSTATE  — &vuRegs[index]; base for all VF/VI/ACC/I memory access
//   w9   gprT1     — emit scratch temp
//   w10  gprT2     — emit scratch temp
//   x17  RVUADDR   — transient address-calc scratch (for NEON lane stores)
//   w23..w26 gprF0..gprF3 — the 4 Status-flag instances kept live across a block
//   the remaining usable w-regs form the VI allocation pool.
static constexpr int mVU_VF_TOTAL = 24; // v0..v23 allocatable for VF
static constexpr int mVU_GPR_TOTAL = 27; // w0..w26 tracked by the allocator

static constexpr int mVU_T1 = 9;
static constexpr int mVU_T2 = 10;
static constexpr int mVU_F0 = 23, mVU_F1 = 24, mVU_F2 = 25, mVU_F3 = 26;

#define RVUSTATE A64_X(19) // base ptr = &vuRegs[index]
#define RVUADDR A64_X(17)  // transient address scratch
// C.74: &mVU, pinned by the micro-mode dispatchers (AB and CD prologues) for the
// life of the JIT session. C.78: valid in macro (COP2) mode too -- x27 is an EE
// GPR cache slot there, but the cache is retired+reset before every macro op,
// so setupMacroOp materializes &microVU0 into it for the duration of the
// setup..end bracket (hoisted to once per macro run) and the EE cache simply
// refills the slot when it next allocates it. Outside the VI allocator's
// tracked pool (w0..w26), callee-saved, so helper C calls preserve it.
#define RVUMVU A64_X(27)   // base ptr = &mVU

// C.74: pick the cheapest addressing for a VU-state global. Priority:
//   1. &mVU.* field within the scaled-immediate window of the pinned x27
//      (micro mode always; macro mode since C.78 -- the hot scalars were
//      moved ahead of `prog` in the struct so they qualify);
//   2. VURegs field of the CURRENT VU addressed off RVUSTATE (x19), which is
//      &mVU.regs() during BOTH micro blocks and a macro run;
//   3. adrp-folded absolute access (C.72) for everything else (other-VU regs,
//      heap pointers).
// The emit-time gate reads compile-time state (mVU.cop2), matching what the
// registers will hold when the emitted code runs.
static inline a64_mem mvuAbsMem(microVU& mVU, const void* addr, unsigned size)
{
	const uintptr_t moff = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(&mVU);
	if (moff < sizeof(microVU) && (moff % size) == 0 && (moff / size) <= 0xFFFu)
		return a64_mem_off(RVUMVU, static_cast<int64_t>(moff));
	const uintptr_t roff = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(&mVU.regs());
	if (roff < sizeof(VURegs) && (roff % size) == 0 && (roff / size) <= 0xFFFu)
		return a64_mem_off(RVUSTATE, static_cast<int64_t>(roff));
	return armAbsMemOperand(RSCRATCHADDR, addr, size);
}


// Named host registers for the emit layer (x86: microVU_Misc.h #defines gprT1..
// /gprF0..). W registers matching the index constants above; getFlagReg() (aVU_Alloc.inl) indexes gprF0..gprF3.
#define gprT1 A64_W(9)  // emit scratch temp
#define gprT2 A64_W(10) // emit scratch temp
#define gprF0 A64_W(23) // Status flag instance 0
#define gprF1 A64_W(24) // Status flag instance 1
#define gprF2 A64_W(25) // Status flag instance 2
#define gprF3 A64_W(26) // Status flag instance 3

// PQ latency NEON register.
static const a64r mVU_xmmPQ = a64_vreg(24, 128, 1);

// Sentinel "no register" for optional NEON params (x86: xEmptyReg): the
// zero a64r, which A64_ISREG rejects. Its code reads as 0.
static const a64r xEmptyReg = {0};

// Emit-layer NEON temp registers (x86: microVU_Misc.h xmmT1=xmm0/xmmT2=xmm1, the
// regAlloc temps). On ARM64 the VF allocation pool is v0-v23, so these instead map
// to the shared NEON scratch (q30/q31). Every flag/branch/endProgram path that uses
// them does so *after* a regAlloc flushAll(), so they never overlap a live VF reg.
#define xmmT1 RQSCRATCH  // q30
#define xmmT2 RQSCRATCH2 // q31

// x86 xSHUF.PS(dst, src, imm) — arbitrary 4-lane (32-bit) permute: out lane i takes
// src lane ((imm >> (2*i)) & 3). NEON has no immediate 4-lane shuffle, so copy src
// into a scratch (q29) and Ins each selected lane. Safe when dst == src.
static inline void mVUshufflePS(const a64r& dst, const a64r& src, u8 imm)
{
	// C.73: the naive form is copy + 4 Ins (5 instructions) for EVERY permute.
	// Most microVU shuffles are broadcasts, 64-bit-half moves, rotations or
	// 2-lane swaps -- pick the cheap NEON form when one exists, and in the
	// general case insert only the lanes that actually move (the PQ shuffles
	// 0xe1/0xb4/0xC6/0x27 are 2-lane swaps: 5 -> 3 instructions).
	const int l[4] = {imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3};
	if (l[0] == l[1] && l[1] == l[2] && l[2] == l[3]) // broadcast
	{
		a64_dup_elem(armAsm, a64_as(dst, A64K_4S), a64_as(src, A64K_4S), l[0]);
		return;
	}
	switch (imm)
	{
		case 0xE4: // identity
			if (A64_CODE(dst) != A64_CODE(src))
				a64_mov(armAsm, a64_as(dst, A64K_16B), a64_as(src, A64K_16B));
			return;
		case 0x44: a64_dup_elem(armAsm, a64_as(dst, A64K_2D), a64_as(src, A64K_2D), 0); return;   // [0,1,0,1]
		case 0xEE: a64_dup_elem(armAsm, a64_as(dst, A64K_2D), a64_as(src, A64K_2D), 1); return;   // [2,3,2,3]
		case 0xB1: a64_rev64(armAsm, a64_as(dst, A64K_4S), a64_as(src, A64K_4S)); return;    // [1,0,3,2]
		case 0x4E: a64_ext(armAsm, a64_as(dst, A64K_16B), a64_as(src, A64K_16B), a64_as(src, A64K_16B), 8); return;  // [2,3,0,1]
		case 0x39: a64_ext(armAsm, a64_as(dst, A64K_16B), a64_as(src, A64K_16B), a64_as(src, A64K_16B), 4); return;  // [1,2,3,0]
		case 0x93: a64_ext(armAsm, a64_as(dst, A64K_16B), a64_as(src, A64K_16B), a64_as(src, A64K_16B), 12); return; // [3,0,1,2]
		case 0x1B: // full reverse [3,2,1,0]
			a64_rev64(armAsm, a64_as(dst, A64K_4S), a64_as(src, A64K_4S));
			a64_ext(armAsm, a64_as(dst, A64K_16B), a64_as(dst, A64K_16B), a64_as(dst, A64K_16B), 8);
			return;
		default:
			break;
	}
	if (A64_CODE(dst) != A64_CODE(src))
	{
		// src is a distinct register and stays intact: copy, then patch the
		// moved lanes straight from src -- no scratch needed.
		a64_mov(armAsm, a64_as(dst, A64K_16B), a64_as(src, A64K_16B));
		for (int i = 0; i < 4; i++)
			if (l[i] != i)
				a64_ins_elem(armAsm, a64_as(dst, A64K_4S), i, a64_as(src, A64K_4S), l[i]);
		return;
	}
	a64_mov(armAsm, a64_as(RQSCRATCH3, A64K_16B), a64_as(src, A64K_16B));
	for (int i = 0; i < 4; i++)
		if (l[i] != i)
			a64_ins_elem(armAsm, a64_as(dst, A64K_4S), i, a64_as(RQSCRATCH3, A64K_4S), l[i]);
}

// Lane0-preserving scalar FP ops on the PQ latency reg (mVU_xmmPQ). AArch64 scalar
// FP (Fsqrt/Fadd/Fmul on .S()) writes Sd and ZEROES Vd[127:32]; x86 SSE scalar ops
// (SQRTSS/ADDSS/MULSS) preserve the upper 3 lanes. The EFU P-pipe ops shuffle the
// target P instance into lane0, compute there, then shuffle back, and DEPEND on the
// other 3 lanes (the two Q instances + the other P instance) surviving — writing
// mVU_xmmPQ.S() in place wipes them, so a later MULq reads Q=0 (→ unprojected
// geometry, e.g. MGS2's broken polygons). Compute into a scratch and merge only
// lane0 back. q31 (RQSCRATCH2) is outside the regalloc pool (v0..v23) and not used
// by the EFU sequences (which use q30 via mVUclampedArith / q29 via mVUshufflePS).
static inline void mVUscalarSqrtKeep(const a64r& dst, const a64r& src)
{
	a64_fsqrt(armAsm, a64_as(RQSCRATCH2, A64K_S), a64_as(src, A64K_S));
	a64_ins_elem(armAsm, a64_as(dst, A64K_4S), 0, a64_as(RQSCRATCH2, A64K_4S), 0);
}
static inline void mVUscalarAddKeep(const a64r& dst, const a64r& a, const a64r& b)
{
	a64_fadd(armAsm, a64_as(RQSCRATCH2, A64K_S), a64_as(a, A64K_S), a64_as(b, A64K_S));
	a64_ins_elem(armAsm, a64_as(dst, A64K_4S), 0, a64_as(RQSCRATCH2, A64K_4S), 0);
}
static inline void mVUscalarMulKeep(const a64r& dst, const a64r& a, const a64r& b)
{
	a64_fmul(armAsm, a64_as(RQSCRATCH2, A64K_S), a64_as(a, A64K_S), a64_as(b, A64K_S));
	a64_ins_elem(armAsm, a64_as(dst, A64K_4S), 0, a64_as(RQSCRATCH2, A64K_4S), 0);
}

//------------------------------------------------------------------
// Byte offsets into VURegs (addressed from RVUSTATE = &vuRegs[index])
//------------------------------------------------------------------
static constexpr u32 mVUoffVF(int n) { return static_cast<u32>(offsetof(VURegs, VF) + n * sizeof(VECTOR)); }
static constexpr u32 mVUoffVI(int n) { return static_cast<u32>(offsetof(VURegs, VI) + n * sizeof(REG_VI)); }
static constexpr u32 mVUoffACC = static_cast<u32>(offsetof(VURegs, ACC));
static constexpr u32 mVUoffI = static_cast<u32>(offsetof(VURegs, VI) + REG_I * sizeof(REG_VI));

//------------------------------------------------------------------
// NEON reg load/save/merge helpers (mirror microVU_Misc.inl lane semantics)
//------------------------------------------------------------------
// xyzw is the microVU 4-bit mask: bit3(0x8)=X=lane0, bit2(0x4)=Y=lane1,
// bit1(0x2)=Z=lane2, bit0(0x1)=W=lane3. NEON V4S lane order matches xmm
// (lane0 = lowest 32 bits = X), so the mapping is one-to-one.

static inline bool mVU_isSS(int xyzw) // exactly one subvector selected
{
	return (xyzw == 8) || (xyzw == 4) || (xyzw == 2) || (xyzw == 1);
}

// Broadcast a single lane of srcreg across all 4 lanes of dstreg (x86:
// xPSHUF.D with a constant splat pattern — 0x00=XXXX, 0x55=YYYY, 0xaa=ZZZZ,
// 0xff=WWWW). The unpack index 0..3 selects lane X/Y/Z/W directly, so NEON Dup
// from that lane is a one-to-one translation.
static inline void mVUunpack_xyzw(const a64r& dstreg, const a64r& srcreg, int xyzw)
{
	a64_dup_elem(armAsm, a64_as(dstreg, A64K_4S), a64_as(srcreg, A64K_4S), xyzw & 3);
}

// Load VF/ACC reg bytes at `off` (from RVUSTATE) into `reg`. Single-subvector
// masks load that one 32-bit lane into lane0 (zero-extending the rest, like
// xMOVSSZX); anything else loads the full 128 bits.
static inline void mVUloadReg(const a64r& reg, u32 off, int xyzw)
{
	switch (xyzw)
	{
		case 8:  a64_ldr(armAsm, a64_as(reg, A64K_S), a64_mem_off(RVUSTATE, off + 0));  break; // X
		case 4:  a64_ldr(armAsm, a64_as(reg, A64K_S), a64_mem_off(RVUSTATE, off + 4));  break; // Y
		case 2:  a64_ldr(armAsm, a64_as(reg, A64K_S), a64_mem_off(RVUSTATE, off + 8));  break; // Z
		case 1:  a64_ldr(armAsm, a64_as(reg, A64K_S), a64_mem_off(RVUSTATE, off + 12)); break; // W
		default: a64_ldr(armAsm, a64_as(reg, A64K_Q), a64_mem_off(RVUSTATE, off));      break;
	}
}

// Write back the selected lanes of `reg` to VF/ACC bytes at `off`.
// modXYZW: when true the value of a single-subvector (Y/Z/W) result lives in
// lane0 (microVU produces SS results pre-shuffled into lane0); when false the
// lanes are in their natural positions. Multi-lane masks are always natural.
static inline void mVUsaveReg(const a64r& reg, u32 off, int xyzw, bool modXYZW)
{
	if (xyzw == 0xf)
	{
		a64_str(armAsm, a64_as(reg, A64K_Q), a64_mem_off(RVUSTATE, off));
		return;
	}

	// Single subvector with the value already shuffled into lane0.
	if (modXYZW && (xyzw == 4 || xyzw == 2 || xyzw == 1))
	{
		const u32 coff = (xyzw == 4) ? 4u : (xyzw == 2) ? 8u : 12u;
		a64_str(armAsm, a64_as(reg, A64K_S), a64_mem_off(RVUSTATE, off + coff));
		return;
	}

	// Natural-position store of each selected lane. X (lane0) uses a scalar
	// store with an immediate offset; Y/Z/W need an explicit address because
	// NEON single-lane stores can't carry an immediate offset.
	if (xyzw & 8)
		a64_str(armAsm, a64_as(reg, A64K_S), a64_mem_off(RVUSTATE, off + 0));
	if (xyzw & 4)
	{
		a64_add_imm(armAsm, RVUADDR, RVUSTATE, off + 4);
		a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 1, a64_mem_off(RVUADDR, 0));
	}
	if (xyzw & 2)
	{
		a64_add_imm(armAsm, RVUADDR, RVUSTATE, off + 8);
		a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 2, a64_mem_off(RVUADDR, 0));
	}
	if (xyzw & 1)
	{
		a64_add_imm(armAsm, RVUADDR, RVUSTATE, off + 12);
		a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 3, a64_mem_off(RVUADDR, 0));
	}
}

// Merge the selected lanes of `src` into `dest`. Same modXYZW convention.
static inline void mVUmergeRegs(const a64r& dest, const a64r& src, int xyzw, bool modXYZW)
{
	xyzw &= 0xf;
	if ((A64_CODE(dest) == A64_CODE(src)) || (xyzw == 0))
		return;

	if (xyzw == 0xf)
	{
		a64_mov(armAsm, a64_as(dest, A64K_16B), a64_as(src, A64K_16B));
		return;
	}

	if (modXYZW && (xyzw == 4 || xyzw == 2 || xyzw == 1))
	{
		const int lane = (xyzw == 4) ? 1 : (xyzw == 2) ? 2 : 3;
		a64_ins_elem(armAsm, a64_as(dest, A64K_4S), lane, a64_as(src, A64K_4S), 0);
		return;
	}

	if (xyzw & 8) a64_ins_elem(armAsm, a64_as(dest, A64K_4S), 0, a64_as(src, A64K_4S), 0);
	if (xyzw & 4) a64_ins_elem(armAsm, a64_as(dest, A64K_4S), 1, a64_as(src, A64K_4S), 1);
	if (xyzw & 2) a64_ins_elem(armAsm, a64_as(dest, A64K_4S), 2, a64_as(src, A64K_4S), 2);
	if (xyzw & 1) a64_ins_elem(armAsm, a64_as(dest, A64K_4S), 3, a64_as(src, A64K_4S), 3);
}

//------------------------------------------------------------------
// Reg Alloc maps (mirror microVU_IR.h microMapXMM/microMapGPR)
//------------------------------------------------------------------

struct microMapXMM
{
	int  VFreg;    // VF Reg Number Stored (-1 = Temp; 0 = vf0 and will not be written back; 32 = ACC; 33 = I reg)
	int  xyzw;     // xyzw to write back (0 = Don't write back anything AND cached vfReg has all vectors valid)
	int  count;    // Count of when last used
	bool isNeeded; // Is needed for current instruction
	bool isZero;   // Register was loaded from VF00 and doesn't need clamping
};

struct microMapGPR
{
	int VIreg;
	int count;
	bool isNeeded;
	bool dirty;
	bool isZeroExtended;
	bool usable;
};

class microRegAlloc
{
protected:
	static const int xmmTotal = mVU_VF_TOTAL;  // PQ register is reserved (lives at v24)
	static const int gprTotal = mVU_GPR_TOTAL;

	std::array<microMapXMM, xmmTotal> xmmMap;
	std::array<microMapGPR, gprTotal> gprMap;

	int counter; // Current allocation count
	int index;   // VU0 or VU1

	// Helper functions to get VU regs
	VURegs& regs() const { return ::vuRegs[index]; }
	__fi REG_VI& getVI(uint reg) const { return regs().VI[reg]; }
	__fi VECTOR& getVF(uint reg) const { return regs().VF[reg]; }

	// Host register objects by allocator index.
	static a64r vfReg(int i) { return a64_vreg(i, 128, 1); }
	static a64r gprW(int i) { return armWRegister(i); }

	__ri void loadIreg(const a64r& reg, int xyzw)
	{
		for (int i = 0; i < gprTotal; i++)
		{
			if (gprMap[i].VIreg == REG_I)
			{
				a64_fmov(armAsm, a64_as(reg, A64K_S), gprW(i)); // GPR32 -> lane0, zero rest (xMOVDZX)
				if (!mVU_isSS(xyzw))
					a64_dup_elem(armAsm, a64_as(reg, A64K_4S), a64_as(reg, A64K_4S), 0);
				return;
			}
		}

		a64_ldr(armAsm, a64_as(reg, A64K_S), a64_mem_off(RVUSTATE, mVUoffI));
		if (!mVU_isSS(xyzw))
			a64_dup_elem(armAsm, a64_as(reg, A64K_4S), a64_as(reg, A64K_4S), 0);
	}

	int findFreeRegRec(int startIdx)
	{
		for (int i = startIdx; i < xmmTotal; i++)
		{
			if (!xmmMap[i].isNeeded)
			{
				int x = findFreeRegRec(i + 1);
				if (x == -1)
					return i;
				return ((xmmMap[i].count < xmmMap[x].count) ? i : x);
			}
		}
		return -1;
	}

	int findFreeReg(int vfreg)
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			if (!xmmMap[i].isNeeded && (xmmMap[i].VFreg < 0))
			{
				return i; // Reg is not needed and was a temp reg
			}
		}
		int x = findFreeRegRec(0);
		pxAssertMsg(x >= 0, "microVU register allocation failure!");
		return x;
	}

	int findFreeGPRRec(int startIdx)
	{
		for (int i = startIdx; i < gprTotal; i++)
		{
			if (gprMap[i].usable && !gprMap[i].isNeeded)
			{
				int x = findFreeGPRRec(i + 1);
				if (x == -1)
					return i;
				return ((gprMap[i].count < gprMap[x].count) ? i : x);
			}
		}
		return -1;
	}

	int findFreeGPR(int vireg)
	{
		for (int i = 0; i < gprTotal; i++)
		{
			if (gprMap[i].usable && !gprMap[i].isNeeded && (gprMap[i].VIreg < 0))
			{
				return i; // Reg is not needed and was a temp reg
			}
		}
		int x = findFreeGPRRec(0);
		pxAssertMsg(x >= 0, "microVU register allocation failure!");
		return x;
	}

	void writeVIBackup(const a64r& reg)
	{
		microVU& mVU = index ? microVU1 : microVU0;
		armMoveAddressToReg(RVUADDR, &mVU.VIbackup);
		a64_str(armAsm, a64_asw(reg), a64_mem_off(RVUADDR, 0));
	}

public:
	microRegAlloc(int _index)
	{
		index = _index;

		// mark gpr registers as usable
		gprMap.fill({0, 0, false, false, false, false});
		for (int i = 0; i < gprTotal; i++)
		{
			if (i == mVU_T1 || i == mVU_T2 ||
				i == mVU_F0 || i == mVU_F1 || i == mVU_F2 || i == mVU_F3 ||
				i == 16 || i == 17 || i == 18 || i == 19)
			{
				continue; // scratch / reserved / RVUSTATE
			}

			gprMap[i].usable = true;
		}

		reset();
	}

	// Fully resets the regalloc by clearing all cached data
	void reset()
	{
		for (int i = 0; i < xmmTotal; i++)
			clearReg(i);
		for (int i = 0; i < gprTotal; i++)
			clearGPR(i);

		counter = 0;

		// Macro (COP2) mode VI GPR pool gate (M5.4). When microVU0 emits a macro op
		// memory-backed from inside the EE recompiler (mVU.cop2 != 0), allocGPR must
		// not hand out the EE rec's own pinned host registers or it clobbers live EE
		// state for the rest of the block. The EE rec pins x19 (RESTATEPTR — already
		// excluded as index 19 in the ctor), x21 (REVTLBPTR = vtlb vmap base, loaded
		// once at block entry and NEVER reloaded), and x20/x22/x23-x28 (the guest-GPR
		// cache, REC_GPR_CACHE_REGS in aR5900.cpp). Of the cache regs, x23-x26 already
		// coincide with mVU_F0-F3 (excluded in the ctor) and x27/x28 are outside the
		// w0-w26 tracked range, so only x20/x21/x22 (indices 20/21/22) need a runtime
		// gate. In the standalone VU rec (cop2 == 0) all three stay usable — this is a
		// true no-op there. Only VU0 ever runs macro mode (microVU1.cop2 is always 0).
		// setupMacroOp sets cop2 = 1 BEFORE calling reset(); endMacroOp sets it back to
		// 0 before its reset(), so the pool is reconfigured correctly at each bracket.
		microVU& mVU = index ? microVU1 : microVU0;
		const bool macroMode = (mVU.cop2 != 0);
		gprMap[20].usable = !macroMode;
		gprMap[21].usable = !macroMode;
		gprMap[22].usable = !macroMode;
	}

	int getXmmCount()
	{
		return xmmTotal + 1;
	}

	int getFreeXmmCount()
	{
		int count = 0;

		for (int i = 0; i < xmmTotal; i++)
		{
			if (!xmmMap[i].isNeeded && (xmmMap[i].VFreg < 0))
				count++;
		}

		return count;
	}

	bool hasRegVF(int vfreg)
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			if (xmmMap[i].VFreg == vfreg)
				return true;
		}

		return false;
	}

	int getRegVF(int i)
	{
		return (i < xmmTotal) ? xmmMap[i].VFreg : -1;
	}

	int getGPRCount()
	{
		return gprTotal;
	}

	int getFreeGPRCount()
	{
		int count = 0;

		for (int i = 0; i < gprTotal; i++)
		{
			if (!gprMap[i].usable && (gprMap[i].VIreg < 0))
				count++;
		}

		return count;
	}

	bool hasRegVI(int vireg)
	{
		for (int i = 0; i < gprTotal; i++)
		{
			if (gprMap[i].VIreg == vireg)
				return true;
		}

		return false;
	}

	int getRegVI(int i)
	{
		return (i < gprTotal) ? gprMap[i].VIreg : -1;
	}

	// Flushes all allocated registers (i.e. writes-back to memory all modified registers).
	// If clearState is 0, then it keeps cached reg data valid
	// If clearState is 1, then it invalidates all cached reg data after write-back
	void flushAll(bool clearState = true)
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			writeBackReg(vfReg(i));
			if (clearState)
				clearReg(i);
		}

		for (int i = 0; i < gprTotal; i++)
		{
			writeBackReg(gprW(i), true);
			if (clearState)
				clearGPR(i);
		}
	}

	void flushCallerSavedRegisters(bool clearNeeded = false)
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			if (!vfIsCallerSaved(i))
				continue;

			writeBackReg(vfReg(i));
			if (clearNeeded || !xmmMap[i].isNeeded)
				clearReg(i);
		}

		for (int i = 0; i < gprTotal; i++)
		{
			if (armIsCalleeSavedRegister(i))
				continue;

			writeBackReg(gprW(i), true);
			if (clearNeeded || !gprMap[i].isNeeded)
				clearGPR(i);
		}
	}

	void TDwritebackAll()
	{
		// NOTE: We don't clear state here, this happens in an optional branch

		for (int i = 0; i < xmmTotal; i++)
		{
			microMapXMM& mapX = xmmMap[i];

			if ((mapX.VFreg > 0) && mapX.xyzw) // Reg was modified and not Temp or vf0
			{
				if (mapX.VFreg == 33)
					a64_str(armAsm, a64_as(vfReg(i), A64K_S), a64_mem_off(RVUSTATE, mVUoffI));
				else if (mapX.VFreg == 32)
					mVUsaveReg(vfReg(i), mVUoffACC, mapX.xyzw, true);
				else
					mVUsaveReg(vfReg(i), mVUoffVF(mapX.VFreg), mapX.xyzw, true);
			}
		}

		for (int i = 0; i < gprTotal; i++)
			writeBackReg(gprW(i), false);
	}

	bool checkVFClamp(int regId)
	{
		if (regId != static_cast<int>(A64_CODE(mVU_xmmPQ)) && ((xmmMap[regId].VFreg == 33 && !EmuConfig.Gamefixes.IbitHack) || xmmMap[regId].isZero))
			return false;
		else
			return true;
	}

	bool checkCachedReg(int regId)
	{
		if (regId < xmmTotal)
			return xmmMap[regId].VFreg >= 0;
		else
			return false;
	}

	bool checkCachedGPR(int regId)
	{
		if (regId < gprTotal)
			return gprMap[regId].VIreg >= 0 || gprMap[regId].isNeeded;
		else
			return false;
	}

	void clearReg(const a64r& reg) { clearReg(A64_CODE(reg)); }
	void clearReg(int regId)
	{
		microMapXMM& clear = xmmMap[regId];
		clear = {-1, 0, 0, false, false};
	}

	void clearRegVF(int VFreg)
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			if (xmmMap[i].VFreg == VFreg)
				clearReg(i);
		}
	}

	// Writes back modified reg to memory.
	// If all vectors modified, then keeps the VF reg cached in the xmm register.
	// If reg was not modified, then keeps the VF reg cached in the xmm register.
	void writeBackRegV(const a64r& reg, bool invalidateRegs)
	{
		microMapXMM& mapX = xmmMap[A64_CODE(reg)];

		if ((mapX.VFreg > 0) && mapX.xyzw) // Reg was modified and not Temp or vf0
		{
			if (mapX.VFreg == 33)
				a64_str(armAsm, a64_as(reg, A64K_S), a64_mem_off(RVUSTATE, mVUoffI));
			else if (mapX.VFreg == 32)
				mVUsaveReg(reg, mVUoffACC, mapX.xyzw, true);
			else
				mVUsaveReg(reg, mVUoffVF(mapX.VFreg), mapX.xyzw, true);

			if (invalidateRegs)
			{
				for (int i = 0; i < xmmTotal; i++)
				{
					microMapXMM& mapI = xmmMap[i];

					if ((i == static_cast<int>(A64_CODE(reg))) || mapI.isNeeded)
						continue;

					if (mapI.VFreg == mapX.VFreg)
					{
						if (mapI.xyzw && mapI.xyzw < 0xf)
							log_cb(RETRO_LOG_ERROR, "microVU Error: writeBackReg() [%d]\n", mapI.VFreg);
						clearReg(i); // Invalidate any Cached Regs of same vf Reg
					}
				}
			}
			if (mapX.xyzw == 0xf) // Make Cached Reg if All Vectors were Modified
			{
				mapX.count    = counter;
				mapX.xyzw     = 0;
				mapX.isNeeded = false;
				return;
			}
			clearReg(reg);
		}
		else if (mapX.xyzw) // Clear reg if modified and is VF0 or temp reg...
		{
			clearReg(reg);
		}
	}

	// Use this when done using the allocated register, it clears its "Needed" status.
	// The register that was written to, should be cleared before other registers are cleared.
	// This is to guarantee proper merging between registers... When a written-to reg is cleared,
	// it invalidates other cached registers of the same VF reg, and merges partial-vector
	// writes into them.
	void clearNeededV(const a64r& reg)
	{
		const int rid = A64_CODE(reg);
		if ((rid < 0) || (rid >= xmmTotal)) // Sometimes mVU_xmmPQ hits this
			return;

		microMapXMM& clear = xmmMap[rid];
		clear.isNeeded = false;
		if (clear.xyzw) // Reg was modified
		{
			if (clear.VFreg > 0)
			{
				int mergeRegs = 0;
				if (clear.xyzw < 0xf) // Try to merge partial writes
					mergeRegs = 1;
				for (int i = 0; i < xmmTotal; i++) // Invalidate any other read-only regs of same vfReg
				{
					if (i == rid)
						continue;
					microMapXMM& mapI = xmmMap[i];
					if (mapI.VFreg == clear.VFreg)
					{
						if (mapI.xyzw && mapI.xyzw < 0xf)
						{
							log_cb(RETRO_LOG_ERROR, "microVU Error: clearNeeded() [%d]\n", mapI.VFreg);
						}
						if (mergeRegs == 1)
						{
							mVUmergeRegs(vfReg(i), reg, clear.xyzw, true);
							mapI.xyzw  = 0xf;
							mapI.count = counter;
							mergeRegs  = 2;
						}
						else
							clearReg(i); // Clears when mergeRegs is 0 or 2
					}
				}
				if (mergeRegs == 2) // Clear Current Reg if Merged
					clearReg(reg);
				else if (mergeRegs == 1) // Write Back Partial Writes if couldn't merge
					writeBackReg(reg);
			}
			else
				clearReg(reg); // If Reg was temp or vf0, then invalidate itself
		}
	}

	// vfLoadReg  = VF reg to be loaded to the NEON register
	// vfWriteReg = VF reg that the returned NEON register will be considered as
	// xyzw       = XYZW vectors that will be modified (and loaded)
	// cloneWrite = When loading a reg that will be written to, it copies it to its own NEON reg instead of overwriting the cached one...
	// Notes:
	// To load a temp reg use the default param values, vfLoadReg = -1 and vfWriteReg = -1.
	// To load a full reg which won't be modified and you want cached, specify vfLoadReg >= 0 and vfWriteReg = -1
	// To load a reg which you don't want written back or cached, specify vfLoadReg >= 0 and vfWriteReg = 0
	const a64r allocReg(int vfLoadReg = -1, int vfWriteReg = -1, int xyzw = 0, bool cloneWrite = true)
	{
		counter++;
		if (vfLoadReg >= 0) // Search For Cached Regs
		{
			for (int i = 0; i < xmmTotal; i++)
			{
				microMapXMM& mapI = xmmMap[i];
				if ((mapI.VFreg == vfLoadReg)
				 && (!mapI.xyzw                           // Reg Was Not Modified
				  || (mapI.VFreg && (mapI.xyzw == 0xf)))) // Reg Had All Vectors Modified and != VF0
				{
					int z = i;
					if (vfWriteReg >= 0) // Reg will be modified
					{
						if (cloneWrite) // Clone Reg so as not to use the same Cached Reg
						{
							z = findFreeReg(vfWriteReg);
							const a64r xmmZ = vfReg(z);
							writeBackReg(xmmZ);

							if (xyzw == 4)
								a64_dup_elem(armAsm, a64_as(xmmZ, A64K_4S), a64_as(vfReg(i), A64K_4S), 1); // Y -> lane0
							else if (xyzw == 2)
								a64_dup_elem(armAsm, a64_as(xmmZ, A64K_4S), a64_as(vfReg(i), A64K_4S), 2); // Z -> lane0
							else if (xyzw == 1)
								a64_dup_elem(armAsm, a64_as(xmmZ, A64K_4S), a64_as(vfReg(i), A64K_4S), 3); // W -> lane0
							else if (z != i)
								a64_mov(armAsm, a64_as(xmmZ, A64K_16B), a64_as(vfReg(i), A64K_16B));

							mapI.count = counter; // Reg i was used, so update counter
						}
						else // Don't clone reg, but shuffle to adjust for SS ops
						{
							if ((vfLoadReg != vfWriteReg) || (xyzw != 0xf))
								writeBackReg(vfReg(i));

							if (xyzw == 4)
								a64_dup_elem(armAsm, a64_as(vfReg(i), A64K_4S), a64_as(vfReg(i), A64K_4S), 1);
							else if (xyzw == 2)
								a64_dup_elem(armAsm, a64_as(vfReg(i), A64K_4S), a64_as(vfReg(i), A64K_4S), 2);
							else if (xyzw == 1)
								a64_dup_elem(armAsm, a64_as(vfReg(i), A64K_4S), a64_as(vfReg(i), A64K_4S), 3);
						}
						xmmMap[z].VFreg = vfWriteReg;
						xmmMap[z].xyzw = xyzw;
						xmmMap[z].isZero = (vfLoadReg == 0);
					}
					xmmMap[z].count = counter;
					xmmMap[z].isNeeded = true;
					return vfReg(z);
				}
			}
		}
		int x = findFreeReg((vfWriteReg >= 0) ? vfWriteReg : vfLoadReg);
		const a64r xmmX = vfReg(x);
		writeBackReg(xmmX);

		if (vfWriteReg >= 0) // Reg Will Be Modified (allow partial reg loading)
		{
			if ((vfLoadReg == 0) && !(xyzw & 1))
				a64_eor(armAsm, a64_as(xmmX, A64K_16B), a64_as(xmmX, A64K_16B), a64_as(xmmX, A64K_16B));
			else if (vfLoadReg == 33)
				loadIreg(xmmX, xyzw);
			else if (vfLoadReg == 32)
				mVUloadReg(xmmX, mVUoffACC, xyzw);
			else if (vfLoadReg >= 0)
				mVUloadReg(xmmX, mVUoffVF(vfLoadReg), xyzw);

			xmmMap[x].VFreg = vfWriteReg;
			xmmMap[x].xyzw  = xyzw;
		}
		else // Reg Will Not Be Modified (always load full reg for caching)
		{
			if (vfLoadReg == 33)
				loadIreg(xmmX, 0xf);
			else if (vfLoadReg == 32)
				a64_ldr(armAsm, a64_as(xmmX, A64K_Q), a64_mem_off(RVUSTATE, mVUoffACC));
			else if (vfLoadReg >= 0)
				a64_ldr(armAsm, a64_as(xmmX, A64K_Q), a64_mem_off(RVUSTATE, mVUoffVF(vfLoadReg)));

			xmmMap[x].VFreg = vfLoadReg;
			xmmMap[x].xyzw  = 0;
		}
		xmmMap[x].isZero = (vfLoadReg == 0);
		xmmMap[x].count    = counter;
		xmmMap[x].isNeeded = true;
		return xmmX;
	}

	void clearGPR(const a64r& reg) { clearGPR(A64_CODE(reg)); }

	void clearGPR(int regId)
	{
		microMapGPR& clear = gprMap[regId];
		clear.VIreg = -1;
		clear.count = 0;
		clear.isNeeded = 0;
		clear.dirty = false;
		clear.isZeroExtended = false;
	}

	void writeBackRegG(const a64r& reg, bool clearDirty)
	{
		microMapGPR& mapX = gprMap[A64_CODE(reg)];
		pxAssert(mapX.usable || !mapX.dirty);
		if (mapX.dirty)
		{
			pxAssert(mapX.VIreg > 0);
			if (mapX.VIreg < 16)
				a64_strh(armAsm, a64_asw(reg), a64_mem_off(RVUSTATE, mVUoffVI(mapX.VIreg)));
			if (clearDirty)
				mapX.dirty = false;
		}
	}

	void clearNeededG(const a64r& reg)
	{
		pxAssert(A64_CODE(reg) < gprTotal);
		microMapGPR& clear = gprMap[A64_CODE(reg)];
		clear.isNeeded = false;
	}

	// A VI register (W/X) or a VF one (anything else, xEmptyReg included)
	static bool isGPR(const a64r& reg) { return A64_KIND(reg) >= A64K_W && A64_KIND(reg) <= A64K_SP; }
	void writeBackReg(const a64r& reg, bool flag = true)
	{
		if (isGPR(reg))
			writeBackRegG(reg, flag);
		else
			writeBackRegV(reg, flag);
	}
	void clearNeeded(const a64r& reg)
	{
		if (isGPR(reg))
			clearNeededG(reg);
		else
			clearNeededV(reg);
	}

	void unbindAnyVIAllocations(int reg, bool& backup)
	{
		for (int i = 0; i < gprTotal; i++)
		{
			microMapGPR& mapI = gprMap[i];
			if (mapI.VIreg == reg)
			{
				if (backup)
				{
					writeVIBackup(gprW(i));
					backup = false;
				}

				// if it's needed, we just unbind the allocation and preserve it, otherwise clear
				if (mapI.isNeeded)
				{
					mapI.VIreg = -1;
					mapI.dirty = false;
					mapI.isZeroExtended = false;
				}
				else
				{
					clearGPR(i);
				}

				// shouldn't be any others...
				for (int j = i + 1; j < gprTotal; j++)
				{
					pxAssert(gprMap[j].VIreg != reg);
				}

				break;
			}
		}
	}

	const a64r allocGPR(int viLoadReg = -1, int viWriteReg = -1, bool backup = false, bool zext_if_dirty = false)
	{
		// TODO: When load != write, we should check whether load is used later, and if so, copy it.
		const int this_counter = counter++;
		if (viLoadReg == 0 || viWriteReg == 0)
		{
			// write zero register as temp and discard later
			if (viWriteReg == 0)
			{
				int x = findFreeGPR(-1);
				const a64r gprX = gprW(x);
				writeBackReg(gprX, true);
				a64_mov(armAsm, a64_asw(gprX), A64_WZR);
				gprMap[x].VIreg = -1;
				gprMap[x].dirty = false;
				gprMap[x].count = this_counter;
				gprMap[x].isNeeded = true;
				gprMap[x].isZeroExtended = true;
				return gprX;
			}
		}

		if (viLoadReg >= 0) // Search For Cached Regs
		{
			for (int i = 0; i < gprTotal; i++)
			{
				microMapGPR& mapI = gprMap[i];
				if (mapI.VIreg == viLoadReg)
				{
					// Do this first, there is a case where when loadReg != writeReg, the findFreeGPR can steal the loadReg
					gprMap[i].count = this_counter;

					if (viWriteReg >= 0) // Reg will be modified
					{
						if (viLoadReg != viWriteReg)
						{
							// kill any allocations of viWriteReg
							unbindAnyVIAllocations(viWriteReg, backup);

							// allocate a new register for writing to
							int x = findFreeGPR(viWriteReg);
							const a64r gprX = gprW(x);

							writeBackReg(gprX, true);

							// writeReg not cached, needs backing up
							if (backup && gprMap[x].VIreg != viWriteReg)
							{
								a64_ldrh(armAsm, a64_asw(gprX), a64_mem_off(RVUSTATE, mVUoffVI(viWriteReg)));
								writeVIBackup(gprX);
								backup = false;
							}

							if (zext_if_dirty)
								a64_uxth(armAsm, a64_asw(gprX), a64_asw(gprW(i)));
							else
								a64_mov(armAsm, a64_asw(gprX), a64_asw(gprW(i)));
							gprMap[x].isZeroExtended = zext_if_dirty;
							std::swap(x, i);
						}
						else
						{
							// writing to it, no longer zero extended
							gprMap[i].isZeroExtended = false;
						}

						gprMap[i].VIreg = viWriteReg;
						gprMap[i].dirty = true;
					}
					else if (zext_if_dirty && !gprMap[i].isZeroExtended)
					{
						a64_uxth(armAsm, a64_asw(gprW(i)), a64_asw(gprW(i)));
						gprMap[i].isZeroExtended = true;
					}

					gprMap[i].isNeeded = true;

					if (backup)
						writeVIBackup(gprW(i));

					return gprW(i);
				}
			}
		}

		if (viWriteReg >= 0) // Writing a new value, make sure this register isn't cached already
			unbindAnyVIAllocations(viWriteReg, backup);

		int x = findFreeGPR(viLoadReg);
		const a64r gprX = gprW(x);
		writeBackReg(gprX, true);

		// Special case: we need to back up the destination register, but it might not have already
		// been cached. If so, we need to load the old value from state and back it up. Otherwise,
		// it's going to get lost when we eventually write this register back.
		if (backup && viLoadReg >= 0 && viWriteReg > 0 && viLoadReg != viWriteReg)
		{
			a64_ldrh(armAsm, a64_asw(gprX), a64_mem_off(RVUSTATE, mVUoffVI(viWriteReg)));
			writeVIBackup(gprX);
			backup = false;
		}

		if (viLoadReg > 0)
			a64_ldrh(armAsm, a64_asw(gprX), a64_mem_off(RVUSTATE, mVUoffVI(viLoadReg)));
		else if (viLoadReg == 0)
			a64_mov(armAsm, a64_asw(gprX), A64_WZR);

		gprMap[x].VIreg = viLoadReg;
		gprMap[x].isZeroExtended = true;
		if (viWriteReg >= 0)
		{
			gprMap[x].VIreg = viWriteReg;
			gprMap[x].dirty = true;
			gprMap[x].isZeroExtended = false;

			if (backup)
			{
				if (viLoadReg < 0 && viWriteReg > 0)
					a64_ldrh(armAsm, a64_asw(gprX), a64_mem_off(RVUSTATE, mVUoffVI(viWriteReg)));

				writeVIBackup(gprX);
			}
		}

		gprMap[x].count = this_counter;
		gprMap[x].isNeeded = true;

		return gprX;
	}

	void moveVIToGPR(const a64r& reg, int vi, bool signext = false)
	{
		pxAssert(vi >= 0);
		if (vi == 0)
		{
			a64_mov(armAsm, a64_asw(reg), A64_WZR);
			return;
		}

		// TODO: Check liveness/usedness before allocating.
		const a64r srcreg = allocGPR(vi);
		if (signext)
			a64_sxth(armAsm, a64_asw(reg), a64_asw(srcreg));
		else
			a64_uxth(armAsm, a64_asw(reg), a64_asw(srcreg));
		clearNeeded(srcreg);
	}

private:
	// Every VF pool register must be treated as caller-saved: AAPCS64 only
	// preserves the *low 64 bits* of v8-v15 across a C call, but VF regs are
	// 128-bit, so even those cannot survive a call intact. This matches the x86
	// SysV behaviour (no xmm is callee-saved there either) — flushCallerSaved
	// writes back every cached VF reg before a call.
	static bool vfIsCallerSaved([[maybe_unused]] int i) { return true; }
};
