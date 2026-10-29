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

#include <cstring>

#include "Common.h"
#include "Profiler.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "Vif_Dynarec.h"
#include "MTVU.h"
#include "Config.h"

enum UnpackOffset {
	OFFSET_X = 0,
	OFFSET_Y = 1,
	OFFSET_Z = 2,
	OFFSET_W = 3
};

static __fi u32 setVifRow(vifStruct& vif, u32 reg, u32 data) {
	vif.MaskRow._u32[reg] = data;
	return data;
}

// cycle derives from vif.cl
// mode derives from vifRegs.mode
/* MTVU_VifX / MTVU_VifXRegs read a variable named `idx` from the enclosing
 * scope; these take the unit as a macro argument instead, so the generated
 * per-unit functions can name it directly. */
#define MTVU_VIFX_N(n)     ((n) ? ((THREAD_VU1) ? vu1Thread.vif     : vif1)     : (vif0))
#define MTVU_VIFXREGS_N(n) ((n) ? ((THREAD_VU1) ? vu1Thread.vifRegs : vif1Regs) : (vif0Regs))

/* The unpack workers, one concrete set per (idx, mode, doMask) triple.
 *
 * These were templates on <idx, mode, doMask, T>: idx picks the VIF unit,
 * mode selects the MaskRow/MaskCol arithmetic, doMask decides whether the
 * mask register is consulted at all, and T is the source element type. Every
 * one of those is a compile-time constant in each of the 512 VIFfuncTable
 * slots, so the switches on `mode` and the `if (doMask)` folded away per
 * instantiation. The macros below emit exactly those folded functions.
 *
 * VIF_WRITE_XYZW carries the masked and unmasked forms because the doMask
 * branch is the one that decides whether vif.cl is read at all -- keeping it
 * as a runtime argument would put a load and a branch into the innermost
 * write of the geometry path.
 */
#define VIF_WRITE_XYZW_BODY(mode_arith)                                        \
	switch (n)                                                                 \
	{                                                                          \
		case 0:                                                                \
			mode_arith                                                         \
			break;                                                             \
		case 1: dest = vif.MaskRow._u32[offnum]; break;                        \
		case 2: dest = vif.MaskCol._u32[pcsx2_min_i(vif.cl, 3)]; break;            \
		case 3: break;                                                         \
	}

#define VIF_MODE_ARITH_0 dest = data;
#define VIF_MODE_ARITH_1 dest = data + vif.MaskRow._u32[offnum];
#define VIF_MODE_ARITH_2 dest = setVifRow(vif, offnum, vif.MaskRow._u32[offnum] + data);
#define VIF_MODE_ARITH_3 dest = setVifRow(vif, offnum, data);

/* doMask == 0: n is always zero, so only the mode arithmetic remains. */
#define VIF_DEFINE_WRITE_NOMASK(idx, mode)                                     \
static __ri void writeXYZW_##idx##_##mode##_0(u32 offnum, u32& dest, u32 data) \
{                                                                              \
	vifStruct& vif = MTVU_VIFX_N(idx);                                       \
	(void)offnum;                                                              \
	(void)vif; /* unused in mode 0, where the arithmetic is dest = data */     \
	VIF_MODE_ARITH_##mode                                                      \
}

#define VIF_DEFINE_WRITE_MASK(idx, mode)                                       \
static __ri void writeXYZW_##idx##_##mode##_1(u32 offnum, u32& dest, u32 data) \
{                                                                              \
	vifStruct& vif = MTVU_VIFX_N(idx);                                       \
	const VIFregisters& regs = MTVU_VIFXREGS_N(idx);                         \
	int n;                                                                     \
	switch (vif.cl)                                                            \
	{                                                                          \
		case 0:  n = (regs.mask >> (offnum * 2)) & 0x3;        break;          \
		case 1:  n = (regs.mask >> ( 8 + (offnum * 2))) & 0x3; break;          \
		case 2:  n = (regs.mask >> (16 + (offnum * 2))) & 0x3; break;          \
		default: n = (regs.mask >> (24 + (offnum * 2))) & 0x3; break;          \
	}                                                                          \
	VIF_WRITE_XYZW_BODY(VIF_MODE_ARITH_##mode)                                 \
}

// Placeholder for invalid VN/VL unpack combinations. These slots in
// VIFfuncTable were previously NULL; if a game (or a malformed transfer) ever
// selected one, the unpack dispatch would call through a NULL pointer and
// crash. Warn once and do nothing instead, so an invalid unpack is survivable.
static void UNPACK_Invalid(u32* dest, const void* src)
{
	static bool warned = false;
	if (!warned)
		warned = true;
}

/* The four unpack shapes, per (idx, mode, doMask) and per source type.
 * V3 and V4 both use the V4 logic -- confirmed hardware behaviour that Ape
 * Escape 3 depends on -- and V2 writes v1v0v1v0, the second pair officially
 * indeterminate but relied on by games. V4_5 ignores MODE and acts as
 * mode 0 always, so it is generated per (idx, doMask) only. */
#define VIF_DEFINE_UNPACKS(idx, mode, dm, T, tag)                              \
static void UNPACK_S_##idx##_##mode##_##dm##_##tag(u32* dest, const T* src)    \
{                                                                              \
	u32 data = *src;                                                           \
	/* S-# is always a complete packet, so the offset bits can be skipped. */  \
	writeXYZW_##idx##_##mode##_##dm(OFFSET_X, *(dest+0), data);                \
	writeXYZW_##idx##_##mode##_##dm(OFFSET_Y, *(dest+1), data);                \
	writeXYZW_##idx##_##mode##_##dm(OFFSET_Z, *(dest+2), data);                \
	writeXYZW_##idx##_##mode##_##dm(OFFSET_W, *(dest+3), data);                \
}                                                                              \
static void UNPACK_V2_##idx##_##mode##_##dm##_##tag(u32* dest, const T* src)   \
{                                                                              \
	writeXYZW_##idx##_##mode##_##dm(OFFSET_X, *(dest+0), *(src+0));            \
	writeXYZW_##idx##_##mode##_##dm(OFFSET_Y, *(dest+1), *(src+1));            \
	writeXYZW_##idx##_##mode##_##dm(OFFSET_Z, *(dest+2), *(src+0));            \
	writeXYZW_##idx##_##mode##_##dm(OFFSET_W, *(dest+3), *(src+1));            \
}                                                                              \
static void UNPACK_V4_##idx##_##mode##_##dm##_##tag(u32* dest, const T* src)   \
{                                                                              \
	writeXYZW_##idx##_##mode##_##dm(OFFSET_X, *(dest+0), *(src+0));            \
	writeXYZW_##idx##_##mode##_##dm(OFFSET_Y, *(dest+1), *(src+1));            \
	writeXYZW_##idx##_##mode##_##dm(OFFSET_Z, *(dest+2), *(src+2));            \
	writeXYZW_##idx##_##mode##_##dm(OFFSET_W, *(dest+3), *(src+3));            \
}

#define VIF_DEFINE_V4_5(idx, dm)                                               \
static void UNPACK_V4_5_##idx##_##dm(u32* dest, const u32* src)                \
{                                                                              \
	u32 data = *src;                                                           \
	writeXYZW_##idx##_0_##dm(OFFSET_X, *(dest+0), ((data & 0x001f) << 3));     \
	writeXYZW_##idx##_0_##dm(OFFSET_Y, *(dest+1), ((data & 0x03e0) >> 2));     \
	writeXYZW_##idx##_0_##dm(OFFSET_Z, *(dest+2), ((data & 0x7c00) >> 7));     \
	writeXYZW_##idx##_0_##dm(OFFSET_W, *(dest+3), ((data & 0x8000) >> 8));     \
}

/* One (idx, mode, doMask) group: the five source types the table names. */
#define VIF_DEFINE_GROUP(idx, mode, dm)                                        \
	VIF_DEFINE_UNPACKS(idx, mode, dm, u32, u32)                                \
	VIF_DEFINE_UNPACKS(idx, mode, dm, s16, s16)                                \
	VIF_DEFINE_UNPACKS(idx, mode, dm, s8,  s8)                                 \
	VIF_DEFINE_UNPACKS(idx, mode, dm, u16, u16)                                \
	VIF_DEFINE_UNPACKS(idx, mode, dm, u8,  u8)

#define VIF_DEFINE_IDX(idx)                                                    \
	VIF_DEFINE_WRITE_NOMASK(idx, 0) VIF_DEFINE_WRITE_MASK(idx, 0)              \
	VIF_DEFINE_WRITE_NOMASK(idx, 1) VIF_DEFINE_WRITE_MASK(idx, 1)              \
	VIF_DEFINE_WRITE_NOMASK(idx, 2) VIF_DEFINE_WRITE_MASK(idx, 2)              \
	VIF_DEFINE_WRITE_NOMASK(idx, 3) VIF_DEFINE_WRITE_MASK(idx, 3)              \
	VIF_DEFINE_GROUP(idx, 0, 0) VIF_DEFINE_GROUP(idx, 0, 1)                    \
	VIF_DEFINE_GROUP(idx, 1, 0) VIF_DEFINE_GROUP(idx, 1, 1)                    \
	VIF_DEFINE_GROUP(idx, 2, 0) VIF_DEFINE_GROUP(idx, 2, 1)                    \
	VIF_DEFINE_GROUP(idx, 3, 0) VIF_DEFINE_GROUP(idx, 3, 1)                    \
	VIF_DEFINE_V4_5(idx, 0) VIF_DEFINE_V4_5(idx, 1)

VIF_DEFINE_IDX(0)
VIF_DEFINE_IDX(1)

/* Table rows. `usn` selects the signed or unsigned 16/8-bit source type,
 * matching the old UnpackFuncSet(vt, idx, mode, usn, doMask). */
#define UnpackFuncSet(vt, idx, mode, usn, dm) \
	(UNPACKFUNCTYPE)UNPACK_##vt##_##idx##_##mode##_##dm##_u32, \
	(UNPACKFUNCTYPE)UNPACK_##vt##_##idx##_##mode##_##dm##_##usn##16, \
	(UNPACKFUNCTYPE)UNPACK_##vt##_##idx##_##mode##_##dm##_##usn##8

#define UnpackV4_5set(idx, dm) (UNPACKFUNCTYPE)UNPACK_V4_5_##idx##_##dm

#define UnpackModeSet(idx, mode) \
	UnpackFuncSet( S,  idx, mode, s, 0 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V2, idx, mode, s, 0 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, idx, mode, s, 0 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, idx, mode, s, 0 ), UnpackV4_5set(idx, 0), \
 \
	UnpackFuncSet( S,  idx, mode, u, 0 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V2, idx, mode, u, 0 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, idx, mode, u, 0 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, idx, mode, u, 0 ), UnpackV4_5set(idx, 0), \
 \
	UnpackFuncSet( S,  idx, mode, s, 1 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V2, idx, mode, s, 1 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, idx, mode, s, 1 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, idx, mode, s, 1 ), UnpackV4_5set(idx, 1), \
 \
	UnpackFuncSet( S,  idx, mode, u, 1 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V2, idx, mode, u, 1 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, idx, mode, u, 1 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, idx, mode, u, 1 ), UnpackV4_5set(idx, 1)

alignas(16) const UNPACKFUNCTYPE VIFfuncTable[2][4][4 * 4 * 2 * 2] =
{
	{
		{ UnpackModeSet(0,0) },
		{ UnpackModeSet(0,1) },
		{ UnpackModeSet(0,2) },
		{ UnpackModeSet(0,3) }
	},

	{
		{ UnpackModeSet(1,0) },
		{ UnpackModeSet(1,1) },
		{ UnpackModeSet(1,2) },
		{ UnpackModeSet(1,3) }
	}
};

//----------------------------------------------------------------------------
// Unpack Setup Code
//----------------------------------------------------------------------------
__fi static int _limit(int a, int max)
{
	return ((a > max) ? max : a);
}

_vifT void vifUnpackSetup(const u32 *data) {

	vifStruct& vifX = GetVifX;

	GetVifX.unpackcalls++;

	if (GetVifX.unpackcalls > 3)
	{
		vifExecQueue(idx);
	}
	//if (!idx) vif0FLUSH(); // Only VU0?

	vifX.usn   = (vifXRegs.code >> 14) & 0x01;
	int vifNum = (vifXRegs.code >> 16) & 0xff;

	if (vifNum == 0) vifNum = 256;
	vifXRegs.num =  vifNum;

	// This is for use when XGKick is synced as VIF can overwrite XG Kick data as it's transferring out
	// Test with Aggressive Inline Skating, or K-1 Premium 2005 Dynamite!
	// VU currently flushes XGKICK on VU1 end so no need for this, yet
	/*if (idx == 1 && VU1.xgkickenable && !(VU0.VI[REG_TPC].UL & 0x100))
	{
		// Catch up first, then the unpack cycles
		_vuXGKICKTransfer(cpuRegs.cycle - VU1.xgkicklastcycle, false);
		_vuXGKICKTransfer(vifNum * 2, false);
	}*/

	// Traditional-style way of calculating the gsize, based on VN/VL parameters.
	// Useful when VN/VL are known template params, but currently they are not so we use
	// the LUT instead (for now).
	//uint vl = vifX.cmd & 0x03;
	//uint vn = (vifX.cmd >> 2) & 0x3;
	//uint gsize = ((32 >> vl) * (vn+1)) / 8;

	const u8& gsize = nVifT[vifX.cmd & 0x0f];

	uint wl = vifXRegs.cycle.wl ? vifXRegs.cycle.wl : 256;

	if (wl <= vifXRegs.cycle.cl) { //Skipping write
		vifX.tag.size = ((vifNum * gsize) + 3) / 4;
	}
	else { //Filling write
		int n = vifXRegs.cycle.cl * (vifNum / wl) +
		        _limit(vifNum % wl, vifXRegs.cycle.cl);

		vifX.tag.size = ((n * gsize) + 3) >> 2;
	}

	u32 addr = vifXRegs.code;
	if (idx && ((addr>>15)&1)) addr += vif1Regs.tops;
	vifX.tag.addr = (addr<<4) & (idx ? 0x3ff0 : 0xff0);

	vifX.cl			 = 0;
	vifX.tag.cmd	 = vifX.cmd;
	GetVifX.pass	 = 1;

	//Ugh things are never easy.
	//Alright, in most cases with V2 and V3 we only need to know if its offset 32bits.
	//However in V3-16 if the data it requires ends on a QW boundary of the source data
	//the W vector becomes 0, so we need to know how far through the current QW the data begins.
	//same happens with V3 8
	vifX.start_aligned = 4-((vifX.vifpacketsize-1) & 0x3);
}

template void vifUnpackSetup<0>(const u32 *data);
template void vifUnpackSetup<1>(const u32 *data);

alignas(16) nVifStruct nVif[2];

// Interpreter-style SSE unpacks.  Array layout matches the interpreter C unpacks.
//  ([USN][Masking][Unpack Type]) [curCycle]
alignas(16) nVifCall nVifUpk[(2 * 2 * 16) * 4];

// This is used by the interpreted SSE unpacks only.  Recompiled SSE unpacks
// and the interpreted C unpacks use the vif.MaskRow/MaskCol members directly.
//  [MaskNumber][CycleNumber][Vector]
alignas(16) u32 nVifMask[3][4][4] = {};

// Number of bytes of data in the source stream needed for each vector.
// [equivalent to ((32 >> VL) * (VN+1)) / 8]
alignas(16) const u8 nVifT[16] = {
	4, // S-32
	2, // S-16
	1, // S-8
	0, // ----
	8, // V2-32
	4, // V2-16
	2, // V2-8
	0, // ----
	12,// V3-32
	6, // V3-16
	3, // V3-8
	0, // ----
	16,// V4-32
	8, // V4-16
	4, // V4-8
	2, // V4-5
};

// ----------------------------------------------------------------------------
template <int idx, bool doMode, bool isFill>
__ri void _nVifUnpackLoop(const u8* data);

typedef void FnType_VifUnpackLoop(const u8* data);
typedef FnType_VifUnpackLoop* Fnptr_VifUnpackLoop;

// Unpacks Until 'Num' is 0
alignas(16) static const Fnptr_VifUnpackLoop UnpackLoopTable[2][2][2] = {
	{
		{_nVifUnpackLoop<0, 0, 0>, _nVifUnpackLoop<0, 0, 1>},
		{_nVifUnpackLoop<0, 1, 0>, _nVifUnpackLoop<0, 1, 1>},
	},
	{
		{_nVifUnpackLoop<1, 0, 0>, _nVifUnpackLoop<1, 0, 1>},
		{_nVifUnpackLoop<1, 1, 0>, _nVifUnpackLoop<1, 1, 1>},
	},
};
// ----------------------------------------------------------------------------

void resetNewVif(int idx)
{
	// Safety Reset : Reassign all VIF structure info, just in case the VU1 pointers have
	// changed for some reason.

	nVif[idx].idx   = idx;
	nVif[idx].bSize = 0;
	memset(nVif[idx].buffer, 0, sizeof(nVif[idx].buffer));

	dVifReset(idx);
}

_vifT int nVifUnpack(const u8* data)
{
	PROFILE_SCOPE(ZONE_VIF_UNPACK);
	nVifStruct&   v       = nVif[idx];
	vifStruct&    vif     = GetVifX;
	VIFregisters& vifRegs = vifXRegs;

	const uint wl     = vifRegs.cycle.wl ? vifRegs.cycle.wl : 256;
	const uint ret    = pcsx2_min_i(vif.vifpacketsize, vif.tag.size);
	const bool isFill = (vifRegs.cycle.cl < wl);
	s32        size   = ret << 2;

	if (ret == vif.tag.size) // Full Transfer
	{
		if (v.bSize) // Last transfer was partial
		{
			memcpy(&v.buffer[v.bSize], data, size);
			v.bSize += size;
			size = v.bSize;
			data = v.buffer;

			vif.cl = 0;
			vifRegs.num = (vifXRegs.code >> 16) & 0xff; // grab NUM form the original VIFcode input.
			if (!vifRegs.num)
				vifRegs.num = 256;
		}

		if (!idx || !THREAD_VU1)
		{
			dVifUnpack<idx>(data, isFill);
		}
		else
			vu1Thread.VifUnpack(vif, vifRegs, (u8*)data, size);

		vif.pass     = 0;
		vif.tag.size = 0;
		vif.cmd      = 0;
		vifRegs.num  = 0;
		v.bSize      = 0;
	}
	else // Partial Transfer
	{
		memcpy(&v.buffer[v.bSize], data, size);
		v.bSize += size;
		vif.tag.size -= ret;

		const u8& vSize = nVifT[vif.cmd & 0x0f];

		// We need to provide accurate accounting of the NUM register, in case games decided
		// to read back from it mid-transfer.  Since so few games actually use partial transfers
		// of VIF unpacks, this code should not be any bottleneck.

		if (!isFill)
		{
			vifRegs.num -= (size / vSize);
		}
		else
		{
			int dataSize = (size / vSize);
			vifRegs.num = vifRegs.num - (((dataSize / vifRegs.cycle.cl) * (vifRegs.cycle.wl - vifRegs.cycle.cl)) + dataSize);
		}
	}

	return ret;
}

template int nVifUnpack<0>(const u8* data);
template int nVifUnpack<1>(const u8* data);

// This is used by the interpreted SSE unpacks only.  Recompiled SSE unpacks
// and the interpreted C unpacks use the vif.MaskRow/MaskCol members directly.
static void setMasks(const vifStruct& vif, const VIFregisters& v)
{
	for (int i = 0; i < 16; i++)
	{
		int m = (v.mask >> (i * 2)) & 3;
		switch (m)
		{
			case 0: // Data
				nVifMask[0][i / 4][i % 4] = 0xffffffff;
				nVifMask[1][i / 4][i % 4] = 0;
				nVifMask[2][i / 4][i % 4] = 0;
				break;
			case 1: // MaskRow
				nVifMask[0][i / 4][i % 4] = 0;
				nVifMask[1][i / 4][i % 4] = 0;
				nVifMask[2][i / 4][i % 4] = vif.MaskRow._u32[i % 4];
				break;
			case 2: // MaskCol
				nVifMask[0][i / 4][i % 4] = 0;
				nVifMask[1][i / 4][i % 4] = 0;
				nVifMask[2][i / 4][i % 4] = vif.MaskCol._u32[i / 4];
				break;
			case 3: // Write Protect
				nVifMask[0][i / 4][i % 4] = 0;
				nVifMask[1][i / 4][i % 4] = 0xffffffff;
				nVifMask[2][i / 4][i % 4] = 0;
				break;
		}
	}
}

// ----------------------------------------------------------------------------
//  Unpacking Optimization notes:
// ----------------------------------------------------------------------------
// Some games send a LOT of single-cycle packets (God of War, SotC, TriAce games, etc),
// so we always need to be weary of keeping loop setup code optimized.  It's not always
// a "win" to move code outside the loop, like normally in most other loop scenarios.
//
// The biggest bottleneck of the current code is the call/ret needed to invoke the SSE
// unpackers.  A better option is to generate the entire vifRegs.num loop code as part
// of the SSE template, and inline the SSE code into the heart of it.  This both avoids
// the call/ret and opens the door for resolving some register dependency chains in the
// current emitted functions.  (this is what zero's SSE does to get it's final bit of
// speed advantage over the new vif). --air
//
// The BEST optimizatin strategy here is to use data available to us from the UNPACK dispatch
// -- namely the unpack type and mask flag -- in combination mode and usn values -- to
// generate ~600 special versions of this function.  But since it's an interpreter, who gives
// a crap?  Really? :p
//

// size - size of the packet fragment incoming from DMAC.
template <int idx, bool doMode, bool isFill>
__ri void _nVifUnpackLoop(const u8* data)
{

	vifStruct& vif = MTVU_VifX;
	VIFregisters& vifRegs = MTVU_VifXRegs;

	// skipSize used for skipping writes only
	const int skipSize = (vifRegs.cycle.cl - vifRegs.cycle.wl) * 16;

	if (!doMode && (vif.cmd & 0x10))
		setMasks(vif, vifRegs);

	const int usn    = !!vif.usn;
	const int upkNum = vif.cmd & 0x1f;
	const u8& vSize  = nVifT[upkNum & 0x0f];
	//uint vl = vif.cmd & 0x03;
	//uint vn = (vif.cmd >> 2) & 0x3;
	//uint vSize = ((32 >> vl) * (vn+1)) / 8;		// size of data (in bytes) used for each write cycle

	const nVifCall* fnbase = &nVifUpk[((usn * 2 * 16) + upkNum) * (4 * 1)];
	/* Two tables, two layouts. nVifUpk is filled by the recompilers with
	 * usn at +32 and the mask at +16 -- see usnpart and maskpart in
	 * x86/newVif_UnpackSSE.cpp and arm64/Vif_UnpackNEON.cpp -- and
	 * upkNum already carries the mask bit at 0x10, so (usn * 2 * 16) +
	 * upkNum is right for that one. VIFfuncTable is laid out the other way
	 * round: UnpackModeSet emits signed-unmasked, unsigned-unmasked,
	 * signed-masked, unsigned-masked, so the mask is at +32 and usn at
	 * +16. Indexing it with the nVifUpk formula picks the unsigned
	 * unmasked entry for a masked signed unpack, which drops the mask
	 * entirely, and the signed masked entry for an unmasked unsigned one,
	 * which applies a mask that was never asked for.
	 *
	 * Scored against the masked scenarios in ps2autotests
	 * tests/dma/vif/stmod.expected that is 8 of 16; with the mask and usn
	 * the right way round it is 16. */
	const int ftIdx = ((upkNum & 0x10) ? 32 : 0) + (usn ? 16 : 0) + (upkNum & 0x0f);
	const UNPACKFUNCTYPE ft = VIFfuncTable[idx][doMode ? vifRegs.mode : 0][ftIdx];

	do
	{
		u8* dest = (u8*)(vuRegs[idx].Mem + (vif.tag.addr & (idx ? 0x3ff0 : 0xff0)));

		if (doMode)
		{
			ft(dest, data);
		}
		else
		{
#ifdef ARCH_ARM64
			// nVifUpk holds the NEON-generated kernels (C.19); it is all-NULL
			// when the dynarec code region failed to map -- then the portable C
			// table handles the
			// same no-mode case (upkNum bit 4 selects the doMask variant and
			// writeXYZW applies the per-cycle mask via vif.cl internally).
			uint cl3 = pcsx2_min_i(vif.cl, 3);
			nVifCall f = fnbase[cl3];
			if (f)
				f(dest, data);
			else
				ft(dest, data);
#else
			uint cl3 = pcsx2_min_i(vif.cl, 3);
			fnbase[cl3](dest, data);
#endif
		}

		vif.tag.addr += 16;
		--vifRegs.num;
		++vif.cl;

		if (isFill)
		{
			if (vif.cl <= vifRegs.cycle.cl)
				data += vSize;
			else if (vif.cl == vifRegs.cycle.wl)
				vif.cl = 0;
		}
		else
		{
			data += vSize;

			if (vif.cl >= vifRegs.cycle.wl)
			{
				vif.tag.addr += skipSize;
				vif.cl = 0;
			}
		}
	} while (vifRegs.num);
}

void _nVifUnpack(int idx, const u8* data, uint mode, bool isFill)
{
	UnpackLoopTable[idx][!!mode][isFill](data);
}
