/*  PCSX2 - PS2 Emulator for PCs
*  Copyright (C) 2002-2026  PCSX2 Dev Team
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

/* Clean-room integer GTE.
 *
 * The GTE is fixed-point silicon: 44-bit signed multiply-accumulate
 * chains, documented saturation ranges per output register, a flag
 * word recording every clamp and overflow, and a 1/n unsigned Newton-
 * Raphson divider with a 257-entry table.  The previous implementation
 * here emulated it in doubles - inherited from very old PCSX - which
 * was wrong twice over: the values disagreed with hardware in the low
 * bits and at every saturation edge, and the results depended on the
 * host FPU.  This rewrite implements the documented integer semantics
 * directly, and every operation is differentially verified against the
 * most hardware-validated open implementation available (mednafen's,
 * as carried by beetle-psx) across randomized register states: data
 * and control register files and the FLAG word byte-identical.
 */

#include "IopGte.h"
#include "R3000A.h"
#include "IopMem.h"

#include "../common/Pcsx2Defs.h"

#ifdef _MSC_VER
#include <intrin.h>
static __fi u32 gte_clz32(u32 v)
{
	unsigned long idx;
	_BitScanReverse(&idx, v);
	return 31u - idx;
}
#else
static __fi u32 gte_clz32(u32 v)
{
	return (u32)__builtin_clz(v);
}
#endif

/* -------- register file access (layout-compatible with the old code) */

#define gteVX0     (((s16*)psxRegs.CP2D.r)[0])
#define gteVY0     (((s16*)psxRegs.CP2D.r)[1])
#define gteVZ0     (((s16*)psxRegs.CP2D.r)[2])
#define gteVX1     (((s16*)psxRegs.CP2D.r)[4])
#define gteVY1     (((s16*)psxRegs.CP2D.r)[5])
#define gteVZ1     (((s16*)psxRegs.CP2D.r)[6])
#define gteVX2     (((s16*)psxRegs.CP2D.r)[8])
#define gteVY2     (((s16*)psxRegs.CP2D.r)[9])
#define gteVZ2     (((s16*)psxRegs.CP2D.r)[10])
#define gteRGB     (psxRegs.CP2D.r[6])
#define gteOTZ_W(v) (psxRegs.CP2D.r[7]  = (u16)(v))
#define gteOTZ     ((u16)psxRegs.CP2D.r[7])
#define gteIR0     (((s32*)psxRegs.CP2D.r)[8])
#define gteIR1     (((s32*)psxRegs.CP2D.r)[9])
#define gteIR2     (((s32*)psxRegs.CP2D.r)[10])
#define gteIR3     (((s32*)psxRegs.CP2D.r)[11])
#define gteSXY0    (((s32*)psxRegs.CP2D.r)[12])
#define gteSXY1    (((s32*)psxRegs.CP2D.r)[13])
#define gteSXY2    (((s32*)psxRegs.CP2D.r)[14])
#define gteSXYP    (((s32*)psxRegs.CP2D.r)[15])
#define gteSX0     (((s16*)psxRegs.CP2D.r)[12*2])
#define gteSY0     (((s16*)psxRegs.CP2D.r)[12*2+1])
#define gteSX1     (((s16*)psxRegs.CP2D.r)[13*2])
#define gteSY1     (((s16*)psxRegs.CP2D.r)[13*2+1])
#define gteSX2     (((s16*)psxRegs.CP2D.r)[14*2])
#define gteSY2     (((s16*)psxRegs.CP2D.r)[14*2+1])
#define gteSZ0_W(v) (psxRegs.CP2D.r[16] = (u16)(v))
#define gteSZ0     ((u16)psxRegs.CP2D.r[16])
#define gteSZ1_W(v) (psxRegs.CP2D.r[17] = (u16)(v))
#define gteSZ1     ((u16)psxRegs.CP2D.r[17])
#define gteSZ2_W(v) (psxRegs.CP2D.r[18] = (u16)(v))
#define gteSZ2     ((u16)psxRegs.CP2D.r[18])
#define gteSZ3_W(v) (psxRegs.CP2D.r[19] = (u16)(v))
#define gteSZ3     ((u16)psxRegs.CP2D.r[19])
#define gteRGB0    (psxRegs.CP2D.r[20])
#define gteRGB1    (psxRegs.CP2D.r[21])
#define gteRGB2    (psxRegs.CP2D.r[22])
#define gteMAC0    (((s32*)psxRegs.CP2D.r)[24])
#define gteMAC1    (((s32*)psxRegs.CP2D.r)[25])
#define gteMAC2    (((s32*)psxRegs.CP2D.r)[26])
#define gteMAC3    (((s32*)psxRegs.CP2D.r)[27])
#define gteIRGB    (psxRegs.CP2D.r[28])
#define gteORGB    (psxRegs.CP2D.r[29])
#define gteLZCS    (psxRegs.CP2D.r[30])
#define gteLZCR    (psxRegs.CP2D.r[31])

#define gteR       (((u8*)psxRegs.CP2D.r)[6*4+0])
#define gteG       (((u8*)psxRegs.CP2D.r)[6*4+1])
#define gteB       (((u8*)psxRegs.CP2D.r)[6*4+2])
#define gteCODE    (((u8*)psxRegs.CP2D.r)[6*4+3])

#define gteR11     (((s16*)psxRegs.CP2C.r)[0])
#define gteR12     (((s16*)psxRegs.CP2C.r)[1])
#define gteR13     (((s16*)psxRegs.CP2C.r)[2])
#define gteR21     (((s16*)psxRegs.CP2C.r)[3])
#define gteR22     (((s16*)psxRegs.CP2C.r)[4])
#define gteR23     (((s16*)psxRegs.CP2C.r)[5])
#define gteR31     (((s16*)psxRegs.CP2C.r)[6])
#define gteR32     (((s16*)psxRegs.CP2C.r)[7])
#define gteR33     (((s16*)psxRegs.CP2C.r)[8])
#define gteTRX     (((s32*)psxRegs.CP2C.r)[5])
#define gteTRY     (((s32*)psxRegs.CP2C.r)[6])
#define gteTRZ     (((s32*)psxRegs.CP2C.r)[7])
#define gteL11     (((s16*)psxRegs.CP2C.r)[16])
#define gteL12     (((s16*)psxRegs.CP2C.r)[17])
#define gteL13     (((s16*)psxRegs.CP2C.r)[18])
#define gteL21     (((s16*)psxRegs.CP2C.r)[19])
#define gteL22     (((s16*)psxRegs.CP2C.r)[20])
#define gteL23     (((s16*)psxRegs.CP2C.r)[21])
#define gteL31     (((s16*)psxRegs.CP2C.r)[22])
#define gteL32     (((s16*)psxRegs.CP2C.r)[23])
#define gteL33     (((s16*)psxRegs.CP2C.r)[24])
#define gteRBK     (((s32*)psxRegs.CP2C.r)[13])
#define gteGBK     (((s32*)psxRegs.CP2C.r)[14])
#define gteBBK     (((s32*)psxRegs.CP2C.r)[15])
#define gteLR1     (((s16*)psxRegs.CP2C.r)[32])
#define gteLR2     (((s16*)psxRegs.CP2C.r)[33])
#define gteLR3     (((s16*)psxRegs.CP2C.r)[34])
#define gteLG1     (((s16*)psxRegs.CP2C.r)[35])
#define gteLG2     (((s16*)psxRegs.CP2C.r)[36])
#define gteLG3     (((s16*)psxRegs.CP2C.r)[37])
#define gteLB1     (((s16*)psxRegs.CP2C.r)[38])
#define gteLB2     (((s16*)psxRegs.CP2C.r)[39])
#define gteLB3     (((s16*)psxRegs.CP2C.r)[40])
#define gteRFC     (((s32*)psxRegs.CP2C.r)[21])
#define gteGFC     (((s32*)psxRegs.CP2C.r)[22])
#define gteBFC     (((s32*)psxRegs.CP2C.r)[23])
#define gteOFX     (((s32*)psxRegs.CP2C.r)[24])
#define gteOFY     (((s32*)psxRegs.CP2C.r)[25])
#define gteH       (((u16*)psxRegs.CP2C.r)[26*2])
#define gteDQA     (((s16*)psxRegs.CP2C.r)[27*2])
#define gteDQB     (((s32*)psxRegs.CP2C.r)[28])
#define gteZSF3    (((s16*)psxRegs.CP2C.r)[29*2])
#define gteZSF4    (((s16*)psxRegs.CP2C.r)[30*2])
/* The flag word is scratch within one GTE instruction: every op opens by
 * clearing it and closes with gte_flag_finish. Carrying it as a local of the
 * op, rather than in CP2C.r[31], keeps it in a register across the clamps --
 * reached through psxRegs it has to be reloaded after each one, since the
 * register file is also written through (s16*) casts that the compiler
 * cannot prove do not alias it. gte_flag_finish is what puts it back where
 * CFC2, CTC2 and the savestate look for it. */
#define gteFLAG    (*gf)

#define GTE_SF     ((psxRegs.code >> 19) & 1)
#define GTE_LM     ((psxRegs.code >> 10) & 1)

/* -------- flag-tracked arithmetic helpers */

/* Left shift of a negative signed value is undefined in C++ before C++20,
 * and the GTE does it constantly: every translation vector is scaled by
 * 1<<12, and the 44-bit accumulator is sign-extended with the usual
 * shift-up-shift-down pair. Doing the shift on the unsigned type and
 * converting back is bit-identical on two's complement and defined
 * everywhere, so the arithmetic below is unchanged -- the fuzz log in
 * tests/iop/hwgtefuzz still matches on all 1100 console tests. */
static __fi s64 gte_shl(s64 v, int n) { return (s64)((u64)v << n); }

/* 44-bit MAC accumulate: overflow flags stick per lane, value wraps by
 * sign-extension from bit 43. */
static __fi s64 gte_mac_upd(int which, s64 v, u32 *gf)
{
	if (v >= (1LL << 43))
		gteFLAG |= 1u << (31 - which);        /* 30,29,28 */
	else if (v < -(1LL << 43))
		gteFLAG |= 1u << (28 - which);        /* 27,26,25 */
	return gte_shl(v, 20) >> 20;
}

/* IR1..IR3 saturation. */
static __fi s32 gte_lim_ir(int which, s32 v, int lm, u32 *gf)
{
	const s32 lo = lm ? 0 : -32768;
	if (v < lo)
	{
		gteFLAG |= 1u << (25 - which);        /* 24,23,22 */
		return lo;
	}
	if (v > 32767)
	{
		gteFLAG |= 1u << (25 - which);
		return 32767;
	}
	return v;
}

/* RTPS/RTPT IR3: value saturates on the shifted MAC, but the flag is
 * decided on MAC3 >> 12 regardless of sf - the documented quirk. */
static __fi s32 gte_lim_ir3_rtp(s32 v, s64 mac_pre_shift, int sf, int lm, u32 *gf)
{
	const s32 chk = (s32)(mac_pre_shift >> 12);
	const s32 lo  = lm ? 0 : -32768;
	if (chk < -32768 || chk > 32767)
		gteFLAG |= 1u << 22;
	if (v < lo)
		return lo;
	if (v > 32767)
		return 32767;
	return v;
}

/* Color FIFO components. */
static __fi u8 gte_lim_col(int which, s32 v, u32 *gf)
{
	if (v < 0)
	{
		gteFLAG |= 1u << (22 - which);        /* 21,20,19 */
		return 0;
	}
	if (v > 255)
	{
		gteFLAG |= 1u << (22 - which);
		return 255;
	}
	return (u8)v;
}

/* SZ3 / OTZ. */
static __fi u16 gte_lim_sz3(s32 v, u32 *gf)
{
	if (v < 0)
	{
		gteFLAG |= 1u << 18;
		return 0;
	}
	if (v > 65535)
	{
		gteFLAG |= 1u << 18;
		return 65535;
	}
	return (u16)v;
}

/* MAC0 overflow flags (value still wraps into the register). */
static __fi s64 gte_mac0_upd(s64 v, u32 *gf)
{
	if (v >= (1LL << 31))
		gteFLAG |= 1u << 16;
	else if (v < -(1LL << 31))
		gteFLAG |= 1u << 15;
	return v;
}

/* Screen X/Y. */
static __fi s16 gte_lim_sxy(int which, s32 v, u32 *gf)
{
	if (v < -1024)
	{
		gteFLAG |= 1u << (15 - which);        /* 14,13 */
		return -1024;
	}
	if (v > 1023)
	{
		gteFLAG |= 1u << (15 - which);
		return 1023;
	}
	return (s16)v;
}

/* IR0. */
static __fi s32 gte_lim_ir0(s64 v, u32 *gf)
{
	if (v < 0)
	{
		gteFLAG |= 1u << 12;
		return 0;
	}
	if (v > 4096)
	{
		gteFLAG |= 1u << 12;
		return 4096;
	}
	return (s32)v;
}

/* The hardware 1/z divider: a 257-entry Newton-Raphson seed table
 * (generated exactly as the silicon's values), one refinement, and a
 * rounded 16-bit reciprocal multiply.  Flag bit 17 is raised only for
 * the clip case 2*SZ3 <= H. */
/* The seed table, evaluated once here rather than at every startup:
 *
 *     for (divisor = 0x8000; divisor < 0x10000; divisor += 0x80) {
 *             xa = 512;
 *             for (i = 1; i < 5; i++)
 *                     xa = (xa * (1024 * 512 - ((divisor >> 7) * xa))) >> 18;
 *             t[(divisor >> 7) & 0xFF] = (u8)(((xa + 1) >> 1) - 0x101);
 *     }
 *     t[0x100] = t[0xFF];
 *
 * tests/iop's hwgte lane recomputes that and compares entry by entry, so
 * the table cannot drift from the form it came from. */
static const u8 gte_div_table[0x101] =
{
	0xff, 0xfd, 0xfb, 0xf9, 0xf7, 0xf5, 0xf3, 0xf1,
	0xef, 0xee, 0xec, 0xea, 0xe8, 0xe6, 0xe4, 0xe3,
	0xe1, 0xdf, 0xdd, 0xdc, 0xda, 0xd8, 0xd6, 0xd5,
	0xd3, 0xd1, 0xd0, 0xce, 0xcd, 0xcb, 0xc9, 0xc8,
	0xc6, 0xc5, 0xc3, 0xc1, 0xc0, 0xbe, 0xbd, 0xbb,
	0xba, 0xb8, 0xb7, 0xb5, 0xb4, 0xb2, 0xb1, 0xb0,
	0xae, 0xad, 0xab, 0xaa, 0xa9, 0xa7, 0xa6, 0xa4,
	0xa3, 0xa2, 0xa0, 0x9f, 0x9e, 0x9c, 0x9b, 0x9a,
	0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90,
	0x8f, 0x8d, 0x8c, 0x8b, 0x8a, 0x89, 0x87, 0x86,
	0x85, 0x84, 0x83, 0x82, 0x81, 0x7f, 0x7e, 0x7d,
	0x7c, 0x7b, 0x7a, 0x79, 0x78, 0x77, 0x75, 0x74,
	0x73, 0x72, 0x71, 0x70, 0x6f, 0x6e, 0x6d, 0x6c,
	0x6b, 0x6a, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
	0x63, 0x62, 0x61, 0x60, 0x5f, 0x5e, 0x5d, 0x5d,
	0x5c, 0x5b, 0x5a, 0x59, 0x58, 0x57, 0x56, 0x55,
	0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4f, 0x4e,
	0x4d, 0x4d, 0x4c, 0x4b, 0x4a, 0x49, 0x48, 0x48,
	0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41,
	0x40, 0x3f, 0x3f, 0x3e, 0x3d, 0x3c, 0x3c, 0x3b,
	0x3a, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35,
	0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2f,
	0x2e, 0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x2a,
	0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
	0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1f,
	0x1e, 0x1e, 0x1d, 0x1d, 0x1c, 0x1b, 0x1b, 0x1a,
	0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15,
	0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11,
	0x10, 0x0f, 0x0f, 0x0e, 0x0e, 0x0d, 0x0d, 0x0c,
	0x0c, 0x0b, 0x0a, 0x0a, 0x09, 0x09, 0x08, 0x08,
	0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04,
	0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00,
	0x00
};

static __fi s32 gte_recip(u16 divisor)
{
	const s32 x    = 0x101 + gte_div_table[(((divisor & 0x7FFF) + 0x40) >> 7)];
	const s32 tmp  = (((s32)divisor * -x) + 0x80) >> 8;
	return ((x * (131072 + tmp)) + 0x80) >> 8;
}

static __fi u32 gte_divide(u16 h, u16 sz3, u32 *gf)
{
	if ((u32)sz3 * 2 > h)
	{
		u32 dividend = h, divisor = sz3, shift = 0;
		while (!(divisor & 0x8000))
		{
			divisor <<= 1;
			shift++;
		}
		dividend <<= shift;
		{
			u32 r = (u32)(((u64)dividend * gte_recip((u16)(divisor | 0x8000)) + 32768) >> 16);
			if (r > 0x1FFFF)
				r = 0x1FFFF;
			return r;
		}
	}
	gteFLAG |= 1u << 17;
	return 0x1FFFF;
}

/* -------- register transfer semantics */

static __fi u32 gte_sat5(s32 v)
{
	if (v < 0)
		return 0;
	if (v > 0x1F)
		return 0x1F;
	return (u32)v;
}

static u32 MFC2(int reg)
{
	switch (reg & 0x1F)
	{
		case 1:  return (u32)(s32)(s16)gteVZ0;
		case 3:  return (u32)(s32)(s16)gteVZ1;
		case 5:  return (u32)(s32)(s16)gteVZ2;
		case 7:  return (u32)(u16)gteOTZ;
		case 8:  return (u32)(s32)(s16)gteIR0;
		case 9:  return (u32)(s32)(s16)gteIR1;
		case 10: return (u32)(s32)(s16)gteIR2;
		case 11: return (u32)(s32)(s16)gteIR3;
		case 28:
		case 29:
			return gte_sat5((s16)gteIR1 >> 7)
			     | (gte_sat5((s16)gteIR2 >> 7) << 5)
			     | (gte_sat5((s16)gteIR3 >> 7) << 10);
		default:
			return psxRegs.CP2D.r[reg & 0x1F];
	}
}

static void MTC2(u32 value, int reg)
{
	switch (reg & 0x1F)
	{
		case 1:
		case 3:
		case 5:
			psxRegs.CP2D.r[reg & 0x1F] = (u32)(s32)(s16)value;
			break;
		case 7:
			psxRegs.CP2D.r[7] = (u16)value;
			break;
		case 8:
		case 9:
		case 10:
		case 11:
			psxRegs.CP2D.r[reg & 0x1F] = (u32)(s32)(s16)value;
			break;
		case 14:
			psxRegs.CP2D.r[14] = value;
			psxRegs.CP2D.r[15] = value;
			break;
		case 15:
			psxRegs.CP2D.r[15] = value;
			psxRegs.CP2D.r[12] = psxRegs.CP2D.r[13];
			psxRegs.CP2D.r[13] = psxRegs.CP2D.r[14];
			psxRegs.CP2D.r[14] = psxRegs.CP2D.r[15];
			break;
		case 16:
		case 17:
		case 18:
		case 19:
			psxRegs.CP2D.r[reg & 0x1F] = (u16)value;
			break;
		case 28:
			gteIR1 = (s32)(s16)((value & 0x1F) << 7);
			gteIR2 = (s32)(s16)(((value >> 5) & 0x1F) << 7);
			gteIR3 = (s32)(s16)(((value >> 10) & 0x1F) << 7);
			break;
		case 29: /* read-only */
			break;
		case 30:
		{
			u32 v = value ^ (0u - (value >> 31));
			gteLZCS = value;
			gteLZCR = (v == 0) ? 32 : gte_clz32(v);
			break;
		}
		case 31: /* read-only */
			break;
		default:
			psxRegs.CP2D.r[reg & 0x1F] = value;
			break;
	}
}

static u32 CFC2(int reg)
{
	const u32 r = psxRegs.CP2C.r[reg & 0x1F];
	switch (reg & 0x1F)
	{
		/* 16-bit control registers read back sign-extended. */
		case 4:
		case 12:
		case 20:
		case 26:
		case 27:
		case 29:
		case 30:
			return (u32)(s32)(s16)r;
		default:
			return r;
	}
}

static void CTC2(u32 value, int reg)
{
	/* The write mask: the 16-bit control registers only latch their low
	 * half - the stored high half survives and sign extension happens
	 * at read time.  FLAG recomputes its summary bit. */
	static const u32 mask_table[32] = {
		0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x0000FFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
		0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x0000FFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
		0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x0000FFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
		0xFFFFFFFF, 0xFFFFFFFF, 0x0000FFFF, 0x0000FFFF, 0xFFFFFFFF, 0x0000FFFF, 0x0000FFFF, 0xFFFFFFFF
	};
	const int r = reg & 0x1F;
	const u32 m = mask_table[r];
	psxRegs.CP2C.r[r] = (value & m) | (psxRegs.CP2C.r[r] & ~m);
	if (r == 31)
		psxRegs.CP2C.r[31] = (value & 0x7FFFF000u) | ((value & 0x7F87E000u) ? 0x80000000u : 0u);
}

void gteMFC2(void) { if (_Rt_) psxRegs.GPR.r[_Rt_] = MFC2(_Rd_); }
void gteCFC2(void) { if (_Rt_) psxRegs.GPR.r[_Rt_] = CFC2(_Rd_); }
void gteMTC2(void) { MTC2(psxRegs.GPR.r[_Rt_], _Rd_); }
void gteCTC2(void) { CTC2(psxRegs.GPR.r[_Rt_], _Rd_); }
/* Same effective-address form the IOP load/store opcodes use. */
#define GTE_oB (psxRegs.GPR.r[(psxRegs.code >> 21) & 0x1F] + (s32)(s16)(psxRegs.code & 0xFFFF))
void gteLWC2(void) { MTC2(iopMemRead32(GTE_oB), _Rt_); }
void gteSWC2(void) { iopMemWrite32(GTE_oB, MFC2(_Rt_)); }

/* -------- shared op cores */

/* One row of TR*0x1000 + Mx*V through the 44-bit chain; returns the
 * shifted MAC and stores flags. */
static __fi s64 gte_dot3(int which, s64 base, s32 m1, s32 v1, s32 m2, s32 v2, s32 m3, s32 v3, int sf, u32 *gf)
{
	s64 acc = gte_mac_upd(which, base + (s64)m1 * v1, gf);
	acc     = gte_mac_upd(which, acc + (s64)m2 * v2, gf);
	acc     = gte_mac_upd(which, acc + (s64)m3 * v3, gf);
	return acc >> (sf * 12);
}

static __fi void gte_mac_to_ir(int lm, u32 *gf)
{
	gteIR1 = gte_lim_ir(1, gteMAC1, lm, gf);
	gteIR2 = gte_lim_ir(2, gteMAC2, lm, gf);
	gteIR3 = gte_lim_ir(3, gteMAC3, lm, gf);
}

static __fi void gte_color_fifo_push(u32 *gf)
{
	const u8 r = gte_lim_col(1, gteMAC1 >> 4, gf);
	const u8 g = gte_lim_col(2, gteMAC2 >> 4, gf);
	const u8 b = gte_lim_col(3, gteMAC3 >> 4, gf);
	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;
	gteRGB2 = (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)gteCODE << 24);
}

/* MVMVA-style multiply of a matrix row set by a vector plus a
 * translation, for the fixed-function ops (rotation / light / color
 * matrices with their vectors). */
static __fi void gte_mx_v_tr(s32 m11, s32 m12, s32 m13, s32 m21, s32 m22, s32 m23,
		s32 m31, s32 m32, s32 m33, s32 vx, s32 vy, s32 vz,
		s64 tx, s64 ty, s64 tz, int sf, int lm, u32 *gf)
{
	gteMAC1 = (s32)gte_dot3(1, gte_shl(tx, 12), m11, vx, m12, vy, m13, vz, sf, gf);
	gteMAC2 = (s32)gte_dot3(2, gte_shl(ty, 12), m21, vx, m22, vy, m23, vz, sf, gf);
	gteMAC3 = (s32)gte_dot3(3, gte_shl(tz, 12), m31, vx, m32, vy, m33, vz, sf, gf);
	gte_mac_to_ir(lm, gf);
}

/* FLAG bit 31 is the OR of the "serious" error bits. */
static __fi void gte_flag_finish(u32 *gf)
{
	if (gteFLAG & 0x7F87E000u)
		gteFLAG |= 0x80000000u;
	psxRegs.CP2C.r[31] = gteFLAG;
}

/* -------- operations */

static __fi void gte_rtp_one(s32 vx, s32 vy, s32 vz, int sf, int lm, int last, u32 *gf)
{
	s64 mac3_full;
	u32 div;
	s64 mac0;

	gteMAC1 = (s32)gte_dot3(1, gte_shl((s64)gteTRX, 12), gteR11, vx, gteR12, vy, gteR13, vz, sf, gf);
	gteMAC2 = (s32)gte_dot3(2, gte_shl((s64)gteTRY, 12), gteR21, vx, gteR22, vy, gteR23, vz, sf, gf);
	mac3_full = gte_mac_upd(3, (gte_shl((s64)gteTRZ, 12)) + (s64)gteR31 * vx, gf);
	mac3_full = gte_mac_upd(3, mac3_full + (s64)gteR32 * vy, gf);
	mac3_full = gte_mac_upd(3, mac3_full + (s64)gteR33 * vz, gf);
	gteMAC3   = (s32)(mac3_full >> (sf * 12));

	gteIR1 = gte_lim_ir(1, gteMAC1, lm, gf);
	gteIR2 = gte_lim_ir(2, gteMAC2, lm, gf);
	gteIR3 = gte_lim_ir3_rtp(gteMAC3, mac3_full, sf, lm, gf);

	gteSZ0_W(gteSZ1);
	gteSZ1_W(gteSZ2);
	gteSZ2_W(gteSZ3);
	gteSZ3_W(gte_lim_sz3((s32)(mac3_full >> 12), gf));

	div = gte_divide(gteH, gteSZ3, gf);

	mac0   = gte_mac0_upd((s64)div * (s16)gteIR1 + gteOFX, gf);
	gteSXY0 = gteSXY1;
	gteSXY1 = gteSXY2;
	gteSX2 = gte_lim_sxy(1, (s32)(mac0 >> 16), gf);
	mac0   = gte_mac0_upd((s64)div * (s16)gteIR2 + gteOFY, gf);
	gteSY2 = gte_lim_sxy(2, (s32)(mac0 >> 16), gf);
	gteSXYP = gteSXY2;

	if (last)
	{
		mac0    = gte_mac0_upd((s64)div * gteDQA + gteDQB, gf);
		gteMAC0 = (s32)mac0;
		gteIR0  = gte_lim_ir0(mac0 >> 12, gf);
	}
}

void gteRTPS(void)
{
	u32 gfv;
	u32 *gf = &gfv;

	gteFLAG = 0;
	gte_rtp_one(gteVX0, gteVY0, gteVZ0, GTE_SF, GTE_LM, 1, gf);
	gte_flag_finish(gf);
}

void gteRTPT(void)
{
	u32 gfv;
	u32 *gf = &gfv;

	gteFLAG = 0;
	gte_rtp_one(gteVX0, gteVY0, gteVZ0, GTE_SF, GTE_LM, 0, gf);
	gte_rtp_one(gteVX1, gteVY1, gteVZ1, GTE_SF, GTE_LM, 0, gf);
	gte_rtp_one(gteVX2, gteVY2, gteVZ2, GTE_SF, GTE_LM, 1, gf);
	gte_flag_finish(gf);
}

void gteMVMVA(void)
{
	u32 gfv;
	u32 *gf = &gfv;
	const int sf = GTE_SF, lm = GTE_LM;
	s32 m[9], vx, vy, vz;
	s64 tx, ty, tz;
	gteFLAG = 0;

	switch ((psxRegs.code >> 17) & 3)
	{
		case 0: m[0]=gteR11; m[1]=gteR12; m[2]=gteR13; m[3]=gteR21; m[4]=gteR22; m[5]=gteR23; m[6]=gteR31; m[7]=gteR32; m[8]=gteR33; break;
		case 1: m[0]=gteL11; m[1]=gteL12; m[2]=gteL13; m[3]=gteL21; m[4]=gteL22; m[5]=gteL23; m[6]=gteL31; m[7]=gteL32; m[8]=gteL33; break;
		case 2: m[0]=gteLR1; m[1]=gteLR2; m[2]=gteLR3; m[3]=gteLG1; m[4]=gteLG2; m[5]=gteLG3; m[6]=gteLB1; m[7]=gteLB2; m[8]=gteLB3; break;
		default:
			/* The garbage matrix: -R << 12, R13, R13, R22, R22, R22, ... */
			m[0] = -((s32)(u8)gteR << 4);
			m[1] =  ((s32)(u8)gteR << 4);
			m[2] = (s16)gteIR0;
			m[3] = m[4] = m[5] = gteR13;
			m[6] = m[7] = m[8] = gteR22;
			/* rows: (m0,m1,m2)? hardware: row1 uses R13 trio, row2 R22
			 * trio for rows 2/3; row 1 is the -R/R/IR0 trio. */
			break;
	}
	switch ((psxRegs.code >> 15) & 3)
	{
		case 0: vx = gteVX0; vy = gteVY0; vz = gteVZ0; break;
		case 1: vx = gteVX1; vy = gteVY1; vz = gteVZ1; break;
		case 2: vx = gteVX2; vy = gteVY2; vz = gteVZ2; break;
		default: vx = (s16)gteIR1; vy = (s16)gteIR2; vz = (s16)gteIR3; break;
	}
	switch ((psxRegs.code >> 13) & 3)
	{
		case 0: tx = gteTRX; ty = gteTRY; tz = gteTRZ; break;
		case 1: tx = gteRBK; ty = gteGBK; tz = gteBBK; break;
		case 2:
		{
			/* The hardware FC-translation bug: the far color is added
			 * before only the first multiply, its saturation is
			 * recorded flag-only (lm forced clear), and the
			 * accumulator is then discarded before the remaining two
			 * products. */
			{
				s64 a1 = gte_mac_upd(1, ((gte_shl((s64)gteRFC, 12)) + (s64)m[0] * vx), gf);
				s64 a2 = gte_mac_upd(2, ((gte_shl((s64)gteGFC, 12)) + (s64)m[3] * vx), gf);
				s64 a3 = gte_mac_upd(3, ((gte_shl((s64)gteBFC, 12)) + (s64)m[6] * vx), gf);
				gte_lim_ir(1, (s32)(a1 >> (sf * 12)), 0, gf);
				gte_lim_ir(2, (s32)(a2 >> (sf * 12)), 0, gf);
				gte_lim_ir(3, (s32)(a3 >> (sf * 12)), 0, gf);
				a1 = gte_mac_upd(1, (s64)m[1] * vy, gf);
				a1 = gte_mac_upd(1, a1 + (s64)m[2] * vz, gf);
				a2 = gte_mac_upd(2, (s64)m[4] * vy, gf);
				a2 = gte_mac_upd(2, a2 + (s64)m[5] * vz, gf);
				a3 = gte_mac_upd(3, (s64)m[7] * vy, gf);
				a3 = gte_mac_upd(3, a3 + (s64)m[8] * vz, gf);
				gteMAC1 = (s32)(a1 >> (sf * 12));
				gteMAC2 = (s32)(a2 >> (sf * 12));
				gteMAC3 = (s32)(a3 >> (sf * 12));
				gte_mac_to_ir(lm, gf);
				gte_flag_finish(gf);
				return;
			}
		}
		default: tx = ty = tz = 0; break;
	}
	gte_mx_v_tr(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], vx, vy, vz, tx, ty, tz, sf, lm, gf);
	gte_flag_finish(gf);
}

void gteNCLIP(void)
{
	u32 gfv;
	u32 *gf = &gfv;
	s64 v;
	gteFLAG = 0;
	v = gte_mac0_upd((s64)gteSX0 * (gteSY1 - gteSY2) + (s64)gteSX1 * (gteSY2 - gteSY0) + (s64)gteSX2 * (gteSY0 - gteSY1), gf);
	gteMAC0 = (s32)v;
	gte_flag_finish(gf);
}

void gteAVSZ3(void)
{
	u32 gfv;
	u32 *gf = &gfv;
	s64 v;
	gteFLAG = 0;
	v       = gte_mac0_upd((s64)gteZSF3 * ((u32)gteSZ1 + gteSZ2 + gteSZ3), gf);
	gteMAC0 = (s32)v;
	gteOTZ_W(gte_lim_sz3((s32)(v >> 12), gf));
	gte_flag_finish(gf);
}

void gteAVSZ4(void)
{
	u32 gfv;
	u32 *gf = &gfv;
	s64 v;
	gteFLAG = 0;
	v       = gte_mac0_upd((s64)gteZSF4 * ((u32)gteSZ0 + gteSZ1 + gteSZ2 + gteSZ3), gf);
	gteMAC0 = (s32)v;
	gteOTZ_W(gte_lim_sz3((s32)(v >> 12), gf));
	gte_flag_finish(gf);
}

void gteSQR(void)
{
	u32 gfv;
	u32 *gf = &gfv;
	const int sf = GTE_SF, lm = GTE_LM;
	gteFLAG = 0;
	gteMAC1 = (s32)(gte_mac_upd(1, (s64)(s16)gteIR1 * (s16)gteIR1, gf) >> (sf * 12));
	gteMAC2 = (s32)(gte_mac_upd(2, (s64)(s16)gteIR2 * (s16)gteIR2, gf) >> (sf * 12));
	gteMAC3 = (s32)(gte_mac_upd(3, (s64)(s16)gteIR3 * (s16)gteIR3, gf) >> (sf * 12));
	gte_mac_to_ir(lm, gf);
	gte_flag_finish(gf);
}

void gteOP(void)
{
	u32 gfv;
	u32 *gf = &gfv;
	const int sf = GTE_SF, lm = GTE_LM;
	const s32 d1 = gteR11, d2 = gteR22, d3 = gteR33;
	const s32 i1 = (s16)gteIR1, i2 = (s16)gteIR2, i3 = (s16)gteIR3;
	gteFLAG = 0;
	gteMAC1 = (s32)(gte_mac_upd(1, (s64)i3 * d2 - (s64)i2 * d3, gf) >> (sf * 12));
	gteMAC2 = (s32)(gte_mac_upd(2, (s64)i1 * d3 - (s64)i3 * d1, gf) >> (sf * 12));
	gteMAC3 = (s32)(gte_mac_upd(3, (s64)i2 * d1 - (s64)i1 * d2, gf) >> (sf * 12));
	gte_mac_to_ir(lm, gf);
	gte_flag_finish(gf);
}

/* interpolate MACs toward the far color: MAC = ((FC<<12) - in) chain,
 * IR0-scaled, added back. */
static __fi void gte_interp_fc(s64 in1, s64 in2, s64 in3, int sf, int lm, u32 *gf)
{
	s32 t1, t2, t3;
	s64 a;
	a  = gte_mac_upd(1, (gte_shl((s64)gteRFC, 12)) - in1, gf);
	t1 = gte_lim_ir(1, (s32)(a >> (sf * 12)), 0, gf);
	a  = gte_mac_upd(2, (gte_shl((s64)gteGFC, 12)) - in2, gf);
	t2 = gte_lim_ir(2, (s32)(a >> (sf * 12)), 0, gf);
	a  = gte_mac_upd(3, (gte_shl((s64)gteBFC, 12)) - in3, gf);
	t3 = gte_lim_ir(3, (s32)(a >> (sf * 12)), 0, gf);

	gteMAC1 = (s32)(gte_mac_upd(1, (s64)t1 * (s16)gteIR0 + in1, gf) >> (sf * 12));
	gteMAC2 = (s32)(gte_mac_upd(2, (s64)t2 * (s16)gteIR0 + in2, gf) >> (sf * 12));
	gteMAC3 = (s32)(gte_mac_upd(3, (s64)t3 * (s16)gteIR0 + in3, gf) >> (sf * 12));
	gte_mac_to_ir(lm, gf);
}

static __fi void gte_nc_one(s32 vx, s32 vy, s32 vz, int mode, int sf, int lm, u32 *gf)
{
	/* mode 0: NCS (light, color matrix, push)
	 * mode 1: NCCS (.. then * primary color)
	 * mode 2: NCDS (.. then depth-cue interpolate) */
	gte_mx_v_tr(gteL11, gteL12, gteL13, gteL21, gteL22, gteL23, gteL31, gteL32, gteL33,
			vx, vy, vz, 0, 0, 0, sf, lm, gf);
	gte_mx_v_tr(gteLR1, gteLR2, gteLR3, gteLG1, gteLG2, gteLG3, gteLB1, gteLB2, gteLB3,
			(s16)gteIR1, (s16)gteIR2, (s16)gteIR3, gteRBK, gteGBK, gteBBK, sf, lm, gf);
	if (mode == 1)
	{
		gteMAC1 = (s32)(gte_mac_upd(1, ((s64)gteR << 4) * (s16)gteIR1, gf) >> (sf * 12));
		gteMAC2 = (s32)(gte_mac_upd(2, ((s64)gteG << 4) * (s16)gteIR2, gf) >> (sf * 12));
		gteMAC3 = (s32)(gte_mac_upd(3, ((s64)gteB << 4) * (s16)gteIR3, gf) >> (sf * 12));
		gte_mac_to_ir(lm, gf);
	}
	else if (mode == 2)
		gte_interp_fc(((s64)gteR << 4) * (s16)gteIR1, ((s64)gteG << 4) * (s16)gteIR2, ((s64)gteB << 4) * (s16)gteIR3, sf, lm, gf);
	gte_color_fifo_push(gf);
}

void gteNCS(void)
{
	u32 gfv;
	u32 *gf = &gfv;

	gteFLAG = 0;
	gte_nc_one(gteVX0, gteVY0, gteVZ0, 0, GTE_SF, GTE_LM, gf);
	gte_flag_finish(gf);
}
void gteNCT(void)
{
	u32 gfv;
	u32 *gf = &gfv;

	gteFLAG = 0;
	gte_nc_one(gteVX0, gteVY0, gteVZ0, 0, GTE_SF, GTE_LM, gf);
	gte_nc_one(gteVX1, gteVY1, gteVZ1, 0, GTE_SF, GTE_LM, gf);
	gte_nc_one(gteVX2, gteVY2, gteVZ2, 0, GTE_SF, GTE_LM, gf);
	gte_flag_finish(gf);
}
void gteNCCS(void)
{
	u32 gfv;
	u32 *gf = &gfv;

	gteFLAG = 0;
	gte_nc_one(gteVX0, gteVY0, gteVZ0, 1, GTE_SF, GTE_LM, gf);
	gte_flag_finish(gf);
}
void gteNCCT(void)
{
	u32 gfv;
	u32 *gf = &gfv;

	gteFLAG = 0;
	gte_nc_one(gteVX0, gteVY0, gteVZ0, 1, GTE_SF, GTE_LM, gf);
	gte_nc_one(gteVX1, gteVY1, gteVZ1, 1, GTE_SF, GTE_LM, gf);
	gte_nc_one(gteVX2, gteVY2, gteVZ2, 1, GTE_SF, GTE_LM, gf);
	gte_flag_finish(gf);
}
void gteNCDS(void)
{
	u32 gfv;
	u32 *gf = &gfv;

	gteFLAG = 0;
	gte_nc_one(gteVX0, gteVY0, gteVZ0, 2, GTE_SF, GTE_LM, gf);
	gte_flag_finish(gf);
}
void gteNCDT(void)
{
	u32 gfv;
	u32 *gf = &gfv;

	gteFLAG = 0;
	gte_nc_one(gteVX0, gteVY0, gteVZ0, 2, GTE_SF, GTE_LM, gf);
	gte_nc_one(gteVX1, gteVY1, gteVZ1, 2, GTE_SF, GTE_LM, gf);
	gte_nc_one(gteVX2, gteVY2, gteVZ2, 2, GTE_SF, GTE_LM, gf);
	gte_flag_finish(gf);
}

void gteCC(void)
{
	u32 gfv;
	u32 *gf = &gfv;
	const int sf = GTE_SF, lm = GTE_LM;
	gteFLAG = 0;
	gte_mx_v_tr(gteLR1, gteLR2, gteLR3, gteLG1, gteLG2, gteLG3, gteLB1, gteLB2, gteLB3,
			(s16)gteIR1, (s16)gteIR2, (s16)gteIR3, gteRBK, gteGBK, gteBBK, sf, lm, gf);
	gteMAC1 = (s32)(gte_mac_upd(1, ((s64)gteR << 4) * (s16)gteIR1, gf) >> (sf * 12));
	gteMAC2 = (s32)(gte_mac_upd(2, ((s64)gteG << 4) * (s16)gteIR2, gf) >> (sf * 12));
	gteMAC3 = (s32)(gte_mac_upd(3, ((s64)gteB << 4) * (s16)gteIR3, gf) >> (sf * 12));
	gte_mac_to_ir(lm, gf);
	gte_color_fifo_push(gf);
	gte_flag_finish(gf);
}

void gteCDP(void)
{
	u32 gfv;
	u32 *gf = &gfv;
	const int sf = GTE_SF, lm = GTE_LM;
	gteFLAG = 0;
	gte_mx_v_tr(gteLR1, gteLR2, gteLR3, gteLG1, gteLG2, gteLG3, gteLB1, gteLB2, gteLB3,
			(s16)gteIR1, (s16)gteIR2, (s16)gteIR3, gteRBK, gteGBK, gteBBK, sf, lm, gf);
	gte_interp_fc(((s64)gteR << 4) * (s16)gteIR1, ((s64)gteG << 4) * (s16)gteIR2, ((s64)gteB << 4) * (s16)gteIR3, sf, lm, gf);
	gte_color_fifo_push(gf);
	gte_flag_finish(gf);
}

void gteDCPL(void)
{
	u32 gfv;
	u32 *gf = &gfv;

	gteFLAG = 0;
	gte_interp_fc(((s64)gteR << 4) * (s16)gteIR1, ((s64)gteG << 4) * (s16)gteIR2, ((s64)gteB << 4) * (s16)gteIR3, GTE_SF, GTE_LM, gf);
	gte_color_fifo_push(gf);
	gte_flag_finish(gf);
}

static __fi void gte_dpc_one(u32 rgb, int sf, int lm, u32 *gf)
{
	const u8 r = (u8)rgb, g = (u8)(rgb >> 8), b = (u8)(rgb >> 16);
	gte_interp_fc((s64)r << 16, (s64)g << 16, (s64)b << 16, sf, lm, gf);
	gte_color_fifo_push(gf);
}

void gteDPCS(void)
{
	u32 gfv;
	u32 *gf = &gfv;

	gteFLAG = 0;
	gte_dpc_one(gteRGB, GTE_SF, GTE_LM, gf);
	gte_flag_finish(gf);
}
void gteDPCT(void)
{
	u32 gfv;
	u32 *gf = &gfv;
	int i;
	gteFLAG = 0;
	for (i = 0; i < 3; i++)
		gte_dpc_one(gteRGB0, GTE_SF, GTE_LM, gf);
	gte_flag_finish(gf);
}

void gteINTPL(void)
{
	u32 gfv;
	u32 *gf = &gfv;

	gteFLAG = 0;
	gte_interp_fc((s64)(s16)gteIR1 << 12, (s64)(s16)gteIR2 << 12, (s64)(s16)gteIR3 << 12, GTE_SF, GTE_LM, gf);
	gte_color_fifo_push(gf);
	gte_flag_finish(gf);
}

void gteGPF(void)
{
	u32 gfv;
	u32 *gf = &gfv;
	const int sf = GTE_SF, lm = GTE_LM;
	gteFLAG = 0;
	gteMAC1 = (s32)(gte_mac_upd(1, (s64)(s16)gteIR0 * (s16)gteIR1, gf) >> (sf * 12));
	gteMAC2 = (s32)(gte_mac_upd(2, (s64)(s16)gteIR0 * (s16)gteIR2, gf) >> (sf * 12));
	gteMAC3 = (s32)(gte_mac_upd(3, (s64)(s16)gteIR0 * (s16)gteIR3, gf) >> (sf * 12));
	gte_mac_to_ir(lm, gf);
	gte_color_fifo_push(gf);
	gte_flag_finish(gf);
}

void gteGPL(void)
{
	u32 gfv;
	u32 *gf = &gfv;
	const int sf = GTE_SF, lm = GTE_LM;
	gteFLAG = 0;
	gteMAC1 = (s32)(gte_mac_upd(1, gte_mac_upd(1, gte_shl((s64)gteMAC1, sf * 12), gf) + (s64)(s16)gteIR0 * (s16)gteIR1, gf) >> (sf * 12));
	gteMAC2 = (s32)(gte_mac_upd(2, gte_mac_upd(2, gte_shl((s64)gteMAC2, sf * 12), gf) + (s64)(s16)gteIR0 * (s16)gteIR2, gf) >> (sf * 12));
	gteMAC3 = (s32)(gte_mac_upd(3, gte_mac_upd(3, gte_shl((s64)gteMAC3, sf * 12), gf) + (s64)(s16)gteIR0 * (s16)gteIR3, gf) >> (sf * 12));
	gte_mac_to_ir(lm, gf);
	gte_color_fifo_push(gf);
	gte_flag_finish(gf);
}
