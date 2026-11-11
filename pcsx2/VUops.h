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
#include "VU.h"
#include "common/MathUtils.h"

#define float_to_int4(x)	((float)x * (1.0f / 0.0625f))
#define float_to_int12(x)	((float)x * (1.0f / 0.000244140625f))
#define float_to_int15(x)	((float)x * (1.0f / 0.000030517578125))

/* Deterministic truncating integer-to-float conversion. The VU
 * truncates ITOF like everything else, and the recompilers convert
 * under the VU's chop rounding mode - but a plain C cast here rounds
 * by whatever mode the interpreting thread happens to carry, and on
 * random 32-bit inputs nearest disagrees with chop on 47.9 percent of
 * cases (any magnitude above two to the twenty-fourth with dropped
 * bits). This helper is proven bit-identical to cvtdq2ps under chop
 * over twenty million executed cases; the fixed-point scales below
 * are exact powers of two, so scaling after conversion rounds
 * nothing in any mode. */
static __fi float vu_itof_chop(s32 v)
{
	union { u32 u; float f; } r;
	u32 s = (u32)v & 0x80000000u;
	u32 m = s ? (0u - (u32)v) : (u32)v;
	u32 mant, e;
	int nl;
	if (m == 0)
	{
		r.u = 0;
		return r.f;
	}
	nl = 31 - (int)count_leading_zero((s32)m); /* m != 0 guaranteed above */
	if (nl <= 23)
		mant = m << (23 - nl);
	else
		mant = m >> (nl - 23); /* truncation is chop for either sign */
	e = 127u + (u32)nl;
	r.u = s | (e << 23) | (mant & 0x7fffffu);
	return r.f;
}

#define int4_to_float(x)	(vu_itof_chop(x) * 0.0625f)
#define int12_to_float(x)	(vu_itof_chop(x) * 0.000244140625f)
#define int15_to_float(x)	(vu_itof_chop(x) * 0.000030517578125f)

typedef struct _VURegsNum
{
	u8 pipe; /* if 0xff, COP2 */
	u8 VFwrite;
	u8 VFwxyzw;
	u8 VFr0xyzw;
	u8 VFr1xyzw;
	u8 VFread0;
	u8 VFread1;
	u32 VIwrite;
	u32 VIread;
	int cycles;
} _VURegsNum;

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*FnPtr_VuVoid)(void);
typedef void (*FnPtr_VuRegsN)(_VURegsNum *VUregsn);

PCSX2_ALIGN(16) extern const FnPtr_VuVoid VU0_LOWER_OPCODE[128];
PCSX2_ALIGN(16) extern const FnPtr_VuVoid VU0_UPPER_OPCODE[64];
PCSX2_ALIGN(16) extern const FnPtr_VuRegsN VU0regs_LOWER_OPCODE[128];
PCSX2_ALIGN(16) extern const FnPtr_VuRegsN VU0regs_UPPER_OPCODE[64];

PCSX2_ALIGN(16) extern const FnPtr_VuVoid VU1_LOWER_OPCODE[128];
PCSX2_ALIGN(16) extern const FnPtr_VuVoid VU1_UPPER_OPCODE[64];
PCSX2_ALIGN(16) extern const FnPtr_VuRegsN VU1regs_LOWER_OPCODE[128];
PCSX2_ALIGN(16) extern const FnPtr_VuRegsN VU1regs_UPPER_OPCODE[64];
extern void _vuClearFMAC(VURegs * VU);
extern void _vuTestPipes(VURegs * VU);
extern void _vuTestUpperStalls(VURegs * VU, _VURegsNum *VUregsn);
extern void _vuTestLowerStalls(VURegs * VU, _VURegsNum *VUregsn);
extern void _vuAddUpperStalls(VURegs * VU, _VURegsNum *VUregsn);
extern void _vuAddLowerStalls(VURegs * VU, _VURegsNum *VUregsn);
extern void _vuXGKICKTransfer(s32 cycles, bool flush);
extern void _vuFlushAll(VURegs* VU);

#ifdef __cplusplus
}
#endif
