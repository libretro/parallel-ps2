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

#include <string.h>

#include "Common.h"
#include "Profiler.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "Vif_Unpack.h"
#include "VUmicro.h"

enum UnpackOffset
{
	OFFSET_X = 0,
	OFFSET_Y = 1,
	OFFSET_Z = 2,
	OFFSET_W = 3
};

static __fi u32 setVifRow(vifStruct* vif, u32 reg, u32 data)
{
	vif->MaskRow._u32[reg] = data;
	return data;
}

/* The VIF the unpack writes for: VIF1's copy on the VU1 thread when that
 * runs it. Resolved once per unpack and handed down, not per element. */
static __fi vifStruct* unpack_vif(int idx)
{
	return (idx && EMU_THREAD_VU1) ? mtvu_vif() : VIF_X(idx);
}

static __fi VIFregisters* unpack_vif_regs(int idx)
{
	return (idx && EMU_THREAD_VU1) ? mtvu_vif_regs() : VIF_XREGS(idx);
}

/* The unpack workers, one concrete set per (mode, doMask) pair and source
 * type.
 *
 * mode selects the MaskRow/MaskCol arithmetic and doMask whether the mask
 * register is consulted at all. Both are compile-time constants in each of
 * the VIFfuncTable slots, so the switch on mode and the doMask test fold
 * away per function. The unit does not appear: the VIF state is an
 * argument, so VIF0 and VIF1 share every worker.
 *
 * The masked and unmasked writes are separate because doMask decides
 * whether vif->cl is read at all -- keeping it a runtime argument would
 * put a load and a branch into the innermost write of the geometry path.
 */
#define VIF_WRITE_XYZW_BODY(mode_arith)                                        \
	switch (n)                                                                 \
	{                                                                          \
		case 0:                                                                \
			mode_arith                                                         \
			break;                                                             \
		case 1: *dest = vif->MaskRow._u32[offnum]; break;                      \
		case 2: *dest = vif->MaskCol._u32[pcsx2_min_i(vif->cl, 3)]; break;     \
		case 3: break;                                                         \
	}

#define VIF_MODE_ARITH_0 *dest = data;
#define VIF_MODE_ARITH_1 *dest = data + vif->MaskRow._u32[offnum];
#define VIF_MODE_ARITH_2 *dest = setVifRow(vif, offnum, vif->MaskRow._u32[offnum] + data);
#define VIF_MODE_ARITH_3 *dest = setVifRow(vif, offnum, data);

/* doMask == 0: n is always zero, so only the mode arithmetic remains. */
#define VIF_DEFINE_WRITE_NOMASK(mode)                                          \
static __ri void writeXYZW_##mode##_0(vifStruct* vif, const VIFregisters* regs, \
	u32 offnum, u32* dest, u32 data)                                           \
{                                                                              \
	(void)vif;                                                                 \
	(void)regs;                                                                \
	(void)offnum;                                                              \
	VIF_MODE_ARITH_##mode                                                      \
}

#define VIF_DEFINE_WRITE_MASK(mode)                                            \
static __ri void writeXYZW_##mode##_1(vifStruct* vif, const VIFregisters* regs, \
	u32 offnum, u32* dest, u32 data)                                           \
{                                                                              \
	int n;                                                                     \
	switch (vif->cl)                                                           \
	{                                                                          \
		case 0:  n = (regs->mask >> (offnum * 2)) & 0x3;        break;         \
		case 1:  n = (regs->mask >> ( 8 + (offnum * 2))) & 0x3; break;         \
		case 2:  n = (regs->mask >> (16 + (offnum * 2))) & 0x3; break;         \
		default: n = (regs->mask >> (24 + (offnum * 2))) & 0x3; break;         \
	}                                                                          \
	VIF_WRITE_XYZW_BODY(VIF_MODE_ARITH_##mode)                                 \
}

/* The invalid VN/VL combinations: nothing is written. These slots used to
 * be NULL, which a malformed transfer selecting one would have called. */
static void UNPACK_Invalid(u32* dest, const void* src, vifStruct* vif, const VIFregisters* regs)
{
	(void)dest; (void)src; (void)vif; (void)regs;
}

/* The four unpack shapes, per (mode, doMask) and per source type.
 * V3 and V4 both use the V4 logic -- confirmed hardware behaviour that Ape
 * Escape 3 depends on -- and V2 writes v1v0v1v0, the second pair officially
 * indeterminate but relied on by games. V4_5 ignores MODE and acts as
 * mode 0 always, so it is generated per doMask only. */
#define VIF_DEFINE_UNPACKS(mode, dm, T, tag)                                   \
static void UNPACK_S_##mode##_##dm##_##tag(u32* dest, const T* src,            \
	vifStruct* vif, const VIFregisters* regs)                                  \
{                                                                              \
	const u32 data = *src;                                                     \
	/* S-# is always a complete packet, so the offset bits can be skipped. */  \
	writeXYZW_##mode##_##dm(vif, regs, OFFSET_X, dest + 0, data);              \
	writeXYZW_##mode##_##dm(vif, regs, OFFSET_Y, dest + 1, data);              \
	writeXYZW_##mode##_##dm(vif, regs, OFFSET_Z, dest + 2, data);              \
	writeXYZW_##mode##_##dm(vif, regs, OFFSET_W, dest + 3, data);              \
}                                                                              \
static void UNPACK_V2_##mode##_##dm##_##tag(u32* dest, const T* src,           \
	vifStruct* vif, const VIFregisters* regs)                                  \
{                                                                              \
	writeXYZW_##mode##_##dm(vif, regs, OFFSET_X, dest + 0, src[0]);            \
	writeXYZW_##mode##_##dm(vif, regs, OFFSET_Y, dest + 1, src[1]);            \
	writeXYZW_##mode##_##dm(vif, regs, OFFSET_Z, dest + 2, src[0]);            \
	writeXYZW_##mode##_##dm(vif, regs, OFFSET_W, dest + 3, src[1]);            \
}                                                                              \
static void UNPACK_V4_##mode##_##dm##_##tag(u32* dest, const T* src,           \
	vifStruct* vif, const VIFregisters* regs)                                  \
{                                                                              \
	writeXYZW_##mode##_##dm(vif, regs, OFFSET_X, dest + 0, src[0]);            \
	writeXYZW_##mode##_##dm(vif, regs, OFFSET_Y, dest + 1, src[1]);            \
	writeXYZW_##mode##_##dm(vif, regs, OFFSET_Z, dest + 2, src[2]);            \
	writeXYZW_##mode##_##dm(vif, regs, OFFSET_W, dest + 3, src[3]);            \
}

/* Each V4-5 element is a halfword, two bytes after the last: read as one,
 * not as a word that is misaligned every other element and runs two
 * bytes past the last. */
#define VIF_DEFINE_V4_5(dm)                                                    \
static void UNPACK_V4_5_##dm(u32* dest, const u16* src,                        \
	vifStruct* vif, const VIFregisters* regs)                                  \
{                                                                              \
	const u32 data = *src;                                                     \
	writeXYZW_0_##dm(vif, regs, OFFSET_X, dest + 0, ((data & 0x001f) << 3));   \
	writeXYZW_0_##dm(vif, regs, OFFSET_Y, dest + 1, ((data & 0x03e0) >> 2));   \
	writeXYZW_0_##dm(vif, regs, OFFSET_Z, dest + 2, ((data & 0x7c00) >> 7));   \
	writeXYZW_0_##dm(vif, regs, OFFSET_W, dest + 3, ((data & 0x8000) >> 8));   \
}

/* One (mode, doMask) group: the five source types the table names. */
#define VIF_DEFINE_GROUP(mode, dm)                                             \
	VIF_DEFINE_UNPACKS(mode, dm, u32, u32)                                     \
	VIF_DEFINE_UNPACKS(mode, dm, s16, s16)                                     \
	VIF_DEFINE_UNPACKS(mode, dm, s8,  s8)                                      \
	VIF_DEFINE_UNPACKS(mode, dm, u16, u16)                                     \
	VIF_DEFINE_UNPACKS(mode, dm, u8,  u8)

VIF_DEFINE_WRITE_NOMASK(0) VIF_DEFINE_WRITE_MASK(0)
VIF_DEFINE_WRITE_NOMASK(1) VIF_DEFINE_WRITE_MASK(1)
VIF_DEFINE_WRITE_NOMASK(2) VIF_DEFINE_WRITE_MASK(2)
VIF_DEFINE_WRITE_NOMASK(3) VIF_DEFINE_WRITE_MASK(3)
VIF_DEFINE_GROUP(0, 0) VIF_DEFINE_GROUP(0, 1)
VIF_DEFINE_GROUP(1, 0) VIF_DEFINE_GROUP(1, 1)
VIF_DEFINE_GROUP(2, 0) VIF_DEFINE_GROUP(2, 1)
VIF_DEFINE_GROUP(3, 0) VIF_DEFINE_GROUP(3, 1)
VIF_DEFINE_V4_5(0) VIF_DEFINE_V4_5(1)

/* Table rows. `usn` selects the signed or unsigned 16/8-bit source type. */
#define UnpackFuncSet(vt, mode, usn, dm) \
	(UNPACKFUNCTYPE)UNPACK_##vt##_##mode##_##dm##_u32, \
	(UNPACKFUNCTYPE)UNPACK_##vt##_##mode##_##dm##_##usn##16, \
	(UNPACKFUNCTYPE)UNPACK_##vt##_##mode##_##dm##_##usn##8

#define UnpackV4_5set(dm) (UNPACKFUNCTYPE)UNPACK_V4_5_##dm

#define UnpackModeSet(mode) \
	UnpackFuncSet( S,  mode, s, 0 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V2, mode, s, 0 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, mode, s, 0 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, mode, s, 0 ), UnpackV4_5set(0), \
 \
	UnpackFuncSet( S,  mode, u, 0 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V2, mode, u, 0 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, mode, u, 0 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, mode, u, 0 ), UnpackV4_5set(0), \
 \
	UnpackFuncSet( S,  mode, s, 1 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V2, mode, s, 1 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, mode, s, 1 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, mode, s, 1 ), UnpackV4_5set(1), \
 \
	UnpackFuncSet( S,  mode, u, 1 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V2, mode, u, 1 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, mode, u, 1 ), (UNPACKFUNCTYPE)UNPACK_Invalid,  \
	UnpackFuncSet( V4, mode, u, 1 ), UnpackV4_5set(1)

PCSX2_ALIGN(16) const UNPACKFUNCTYPE VIFfuncTable[4][4 * 4 * 2 * 2] =
{
	{ UnpackModeSet(0) },
	{ UnpackModeSet(1) },
	{ UnpackModeSet(2) },
	{ UnpackModeSet(3) }
};

/* ---- Unpack setup -------------------------------------------------- */

static __fi int _limit(int a, int max)
{
	return ((a > max) ? max : a);
}

void vif_unpack_setup(int idx, const u32* data)
{
	vifStruct* vifX    = VIF_X(idx);
	VIFregisters* regs = VIF_XREGS(idx);
	int vifNum;
	u32 gsize, wl, addr;

	(void)data;
	vifX->unpackcalls++;

	if (vifX->unpackcalls > 3)
		vifExecQueue(idx);

	vifX->usn = (regs->code >> 14) & 0x01;
	vifNum    = (regs->code >> 16) & 0xff;

	if (vifNum == 0)
		vifNum = 256;
	regs->num = vifNum;

	/* Bytes of source data per vector, from the VN/VL bits */
	gsize = nVifT[vifX->cmd & 0x0f];
	wl    = regs->cycle.wl ? regs->cycle.wl : 256;

	if (wl <= regs->cycle.cl)
	{ /* Skipping write */
		vifX->tag.size = ((vifNum * gsize) + 3) / 4;
	}
	else
	{ /* Filling write */
		const int n = regs->cycle.cl * (vifNum / wl) +
		              _limit(vifNum % wl, regs->cycle.cl);

		vifX->tag.size = ((n * gsize) + 3) >> 2;
	}

	addr = regs->code;
	if (idx && ((addr >> 15) & 1))
		addr += vif1Regs.tops;
	vifX->tag.addr = (addr << 4) & (idx ? 0x3ff0 : 0xff0);

	vifX->cl      = 0;
	vifX->tag.cmd = vifX->cmd;
	vifX->pass    = 1;

	/* In most V2 and V3 cases only a 32-bit offset matters, but V3-16 and
	 * V3-8 data that ends on a quadword boundary of the source zeroes W,
	 * so how far into its quadword the data starts is kept. */
	vifX->start_aligned = 4 - ((vifX->vifpacketsize - 1) & 0x3);
}

PCSX2_ALIGN(16) VifUnpackBuffer vifUnpackBuffer[2];

/* Interpreter-style SSE unpacks. Array layout matches the interpreter C
 * unpacks: ([USN][Masking][Unpack Type]) [curCycle] */
PCSX2_ALIGN(16) nVifCall nVifUpk[(2 * 2 * 16) * 4];

/* For the interpreted SSE unpacks only; the recompiled ones and the C
 * unpacks use vif->MaskRow/MaskCol directly. [MaskNumber][CycleNumber][Vector] */
PCSX2_ALIGN(16) u32 nVifMask[3][4][4];

/* Bytes of source data needed for each vector,
 * ((32 >> VL) * (VN+1)) / 8. */
PCSX2_ALIGN(16) const u8 nVifT[16] = {
	4,  /* S-32 */
	2,  /* S-16 */
	1,  /* S-8 */
	0,  /* ---- */
	8,  /* V2-32 */
	4,  /* V2-16 */
	2,  /* V2-8 */
	0,  /* ---- */
	12, /* V3-32 */
	6,  /* V3-16 */
	3,  /* V3-8 */
	0,  /* ---- */
	16, /* V4-32 */
	8,  /* V4-16 */
	4,  /* V4-8 */
	2,  /* V4-5 */
};

void resetNewVif(int idx)
{
	vifUnpackBuffer[idx].bSize = 0;
	memset(vifUnpackBuffer[idx].buffer, 0, sizeof(vifUnpackBuffer[idx].buffer));

	dVifReset(idx);
}

/* The rest of an UNPACK whose state does not add up, which only a
 * savestate can produce: dropped rather than written past the buffer or
 * divided by zero. */
static void vif_unpack_abandon(vifStruct* vif, VIFregisters* vifRegs, VifUnpackBuffer* v)
{
	vif->pass     = 0;
	vif->tag.size = 0;
	vif->cmd      = 0;
	vifRegs->num  = 0;
	v->bSize      = 0;
}

int vif_unpack(int idx, const u8* data)
{
	VifUnpackBuffer* v     = &vifUnpackBuffer[idx];
	vifStruct* vif         = VIF_X(idx);
	VIFregisters* vifRegs  = VIF_XREGS(idx);
	const int prof         = PROFILE_ZONE_BEGIN(ZONE_VIF_UNPACK);
	const u32 wl           = vifRegs->cycle.wl ? vifRegs->cycle.wl : 256;
	const u32 ret          = pcsx2_min_i(vif->vifpacketsize, vif->tag.size);
	const bool isFill      = (vifRegs->cycle.cl < wl);
	s32 size               = ret << 2;

	if (v->bSize + (u32)size > sizeof(v->buffer))
	{
		vif_unpack_abandon(vif, vifRegs, v);
		PROFILE_ZONE_END(ZONE_VIF_UNPACK, prof);
		return ret;
	}

	if (ret == vif->tag.size)
	{ /* Full transfer */
		if (v->bSize)
		{ /* The last transfer was partial: the rest joins it */
			memcpy(&v->buffer[v->bSize], data, size);
			v->bSize += size;
			size = v->bSize;
			data = v->buffer;

			vif->cl = 0;
			/* NUM from the original VIFcode */
			vifRegs->num = (vifRegs->code >> 16) & 0xff;
			if (!vifRegs->num)
				vifRegs->num = 256;
		}

		if (!idx || !EMU_THREAD_VU1)
			dvif_unpack(idx, data, isFill);
		else
			mtvu_vif_unpack(data, size);

		vif->pass     = 0;
		vif->tag.size = 0;
		vif->cmd      = 0;
		vifRegs->num  = 0;
		v->bSize      = 0;
	}
	else
	{ /* Partial transfer: held until the rest arrives */
		const u32 vSize = nVifT[vif->cmd & 0x0f];

		/* A reserved format, or filling with CL 0, sizes an UNPACK to no
		 * data at all, so it never arrives in parts; the counts below
		 * would divide by zero for it. */
		if (vSize == 0 || (isFill && vifRegs->cycle.cl == 0))
		{
			vif_unpack_abandon(vif, vifRegs, v);
			PROFILE_ZONE_END(ZONE_VIF_UNPACK, prof);
			return ret;
		}

		memcpy(&v->buffer[v->bSize], data, size);
		v->bSize += size;
		vif->tag.size -= ret;

		/* NUM counts down as the data arrives, for games that read it back
		 * mid-transfer; few use partial unpacks, so this is not hot. */
		if (!isFill)
			vifRegs->num -= (size / vSize);
		else
		{
			const int dataSize = (size / vSize);
			vifRegs->num = vifRegs->num - (((dataSize / vifRegs->cycle.cl) * (vifRegs->cycle.wl - vifRegs->cycle.cl)) + dataSize);
		}
	}

	PROFILE_ZONE_END(ZONE_VIF_UNPACK, prof);
	return ret;
}

/* For the interpreted SSE unpacks only. */
static void setMasks(const vifStruct* vif, const VIFregisters* v)
{
	int i;
	for (i = 0; i < 16; i++)
	{
		const int m = (v->mask >> (i * 2)) & 3;
		switch (m)
		{
			case 0: /* Data */
				nVifMask[0][i / 4][i % 4] = 0xffffffff;
				nVifMask[1][i / 4][i % 4] = 0;
				nVifMask[2][i / 4][i % 4] = 0;
				break;
			case 1: /* MaskRow */
				nVifMask[0][i / 4][i % 4] = 0;
				nVifMask[1][i / 4][i % 4] = 0;
				nVifMask[2][i / 4][i % 4] = vif->MaskRow._u32[i % 4];
				break;
			case 2: /* MaskCol */
				nVifMask[0][i / 4][i % 4] = 0;
				nVifMask[1][i / 4][i % 4] = 0;
				nVifMask[2][i / 4][i % 4] = vif->MaskCol._u32[i / 4];
				break;
			case 3: /* Write protect */
				nVifMask[0][i / 4][i % 4] = 0;
				nVifMask[1][i / 4][i % 4] = 0xffffffff;
				nVifMask[2][i / 4][i % 4] = 0;
				break;
		}
	}
}

/* Unpacks until NUM is 0. Games that send many single-cycle packets (God
 * of War, SotC, the tri-Ace games) keep this loop's setup on the hot path,
 * so it stays small; idx, doMode and isFill are constants in each of the
 * eight copies UnpackLoopTable holds. */
static __fi void nvif_unpack_loop(int idx, bool doMode, bool isFill, const u8* data)
{
	vifStruct* vif        = unpack_vif(idx);
	VIFregisters* vifRegs = unpack_vif_regs(idx);

	/* skipSize used for skipping writes only */
	const int skipSize = (vifRegs->cycle.cl - vifRegs->cycle.wl) * 16;
	const int usn      = !!vif->usn;
	const int upkNum   = vif->cmd & 0x1f;
	const u32 vSize    = nVifT[upkNum & 0x0f];
	const nVifCall* fnbase = &nVifUpk[((usn * 2 * 16) + upkNum) * (4 * 1)];
	/* Two tables, two layouts. nVifUpk is filled by the recompilers with
	 * usn at +32 and the mask at +16 -- see usnpart and maskpart in
	 * x86/newVif_UnpackSSE.c and arm64/Vif_UnpackNEON.cpp -- and upkNum
	 * already carries the mask bit at 0x10, so (usn * 2 * 16) + upkNum is
	 * right for that one. VIFfuncTable is laid out the other way round:
	 * UnpackModeSet emits signed-unmasked, unsigned-unmasked,
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
	const UNPACKFUNCTYPE ft = VIFfuncTable[doMode ? vifRegs->mode : 0][ftIdx];
	u8* const vuMem   = vuRegs[idx].Mem;
	const u32 addrMask = idx ? 0x3ff0 : 0xff0;

	if (!doMode && (vif->cmd & 0x10))
		setMasks(vif, vifRegs);

	do
	{
		u8* dest = vuMem + (vif->tag.addr & addrMask);

		if (doMode)
			ft(dest, data, vif, vifRegs);
		else
		{
			const u32 cl3 = pcsx2_min_i(vif->cl, 3);
#ifdef ARCH_ARM64
			/* nVifUpk holds the NEON kernels; it is all NULL when the
			 * dynarec code region failed to map, and the C table handles
			 * the same no-mode case then. */
			const nVifCall f = fnbase[cl3];
			if (f)
				f(dest, data);
			else
				ft(dest, data, vif, vifRegs);
#else
			fnbase[cl3](dest, data);
#endif
		}

		vif->tag.addr += 16;
		--vifRegs->num;
		++vif->cl;

		if (isFill)
		{
			if (vif->cl <= vifRegs->cycle.cl)
				data += vSize;
			else if (vif->cl == vifRegs->cycle.wl)
				vif->cl = 0;
		}
		else
		{
			data += vSize;

			if (vif->cl >= vifRegs->cycle.wl)
			{
				vif->tag.addr += skipSize;
				vif->cl = 0;
			}
		}
	} while (vifRegs->num);
}

#define VIF_UNPACK_LOOP(i, m, f) \
	static void nvif_unpack_loop_##i##m##f(const u8* data) { nvif_unpack_loop(i, m, f, data); }
VIF_UNPACK_LOOP(0, 0, 0) VIF_UNPACK_LOOP(0, 0, 1) VIF_UNPACK_LOOP(0, 1, 0) VIF_UNPACK_LOOP(0, 1, 1)
VIF_UNPACK_LOOP(1, 0, 0) VIF_UNPACK_LOOP(1, 0, 1) VIF_UNPACK_LOOP(1, 1, 0) VIF_UNPACK_LOOP(1, 1, 1)

typedef void (*Fnptr_VifUnpackLoop)(const u8* data);

PCSX2_ALIGN(16) static const Fnptr_VifUnpackLoop UnpackLoopTable[2][2][2] = {
	{
		{ nvif_unpack_loop_000, nvif_unpack_loop_001 },
		{ nvif_unpack_loop_010, nvif_unpack_loop_011 },
	},
	{
		{ nvif_unpack_loop_100, nvif_unpack_loop_101 },
		{ nvif_unpack_loop_110, nvif_unpack_loop_111 },
	},
};

void _nVifUnpack(int idx, const u8* data, uint mode, bool isFill)
{
	UnpackLoopTable[idx][!!mode][isFill](data);
}
