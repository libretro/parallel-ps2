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

#include <string.h>

#include "../Pcsx2Defs.h"

#define iREGCNT_XMM 16
#define iREGCNT_GPR 16

typedef enum XMMSSEType
{
	XMMT_INT = 0, /* integer (sse2 only) */
	XMMT_FPS = 1  /* floating point */
} XMMSSEType;

#ifdef __cplusplus
extern "C" {
#endif

/* One strong definition each, in iCore.c; every other TU sees only these */
/* externs. Every recompiler emits through the same cursor, so it has to be */
/* one TLS slot per thread across the whole module. An `inline thread_local` */
/* here is a COMDAT the linker must fold across every TU that includes this */
/* header, and MSVC does not do that dependably: unfolded, the EE recompiler */
/* sets its own copy while a COP2 macro op emits through the microVU TU's, */
/* and the rip-relative distance is then measured from a stale cursor -- */
/* which is the FATAL_APP_EXIT in the emitter that survived every placement */
/* fix. A strong definition cannot be duplicated. */
extern PCSX2_THREAD_LOCAL u8* x86Ptr PCSX2_TLS_INITIAL_EXEC;
extern PCSX2_THREAD_LOCAL XMMSSEType g_xmmtypes[iREGCNT_XMM] PCSX2_TLS_INITIAL_EXEC;

#ifdef __cplusplus
}
#endif

/* x86Ptr is the emitter cursor; read and assign it directly. The old
 * x86Ptr/x86Ptr = (u8*)()/x86Ptr wrappers existed against a
 * future change of storage scheme that never came, and the "aligned call
 * target" one did no alignment -- it was the cursor under another name. */

#define xWrite8(val) \
{ \
	*(u8*)x86Ptr = (val); \
	x86Ptr += sizeof(u8); \
}

#define xWrite16(val) \
{ \
	u16 xw_val_ = (u16)(val); \
	memcpy(x86Ptr, &xw_val_, sizeof(u16)); \
	x86Ptr += sizeof(u16); \
}

#define xWrite32(val) \
{ \
	u32 xw_val_ = (u32)(val); \
	memcpy(x86Ptr, &xw_val_, sizeof(u32)); \
	x86Ptr += sizeof(u32); \
}

#define xWrite64(val) \
{ \
	u64 xw_val_ = (u64)(val); \
	memcpy(x86Ptr, &xw_val_, sizeof(u64)); \
	x86Ptr += sizeof(u64); \
}

/* Local-cursor variants of the above. These write through a caller-supplied */
/* u8* cursor (typically a local cached from x86Ptr) and advance it, instead of */
/* touching the thread_local x86Ptr on every byte. Used by emit paths that cache */
/* the cursor across several writes and store it back once. Behaviourally */
/* identical to the global versions for the same cursor value. */
#define xWrite8p(p, val) \
{ \
	*(u8*)(p) = (val); \
	(p) += sizeof(u8); \
}

#define xWrite16p(p, val) \
{ \
	u16 xw_val_ = (u16)(val); \
	memcpy((p), &xw_val_, sizeof(u16)); \
	(p) += sizeof(u16); \
}

#define xWrite32p(p, val) \
{ \
	u32 xw_val_ = (u32)(val); \
	memcpy((p), &xw_val_, sizeof(u32)); \
	(p) += sizeof(u32); \
}

#define xWrite64p(p, val) \
{ \
	u64 xw_val_ = (u64)(val); \
	memcpy((p), &xw_val_, sizeof(u64)); \
	(p) += sizeof(u64); \
}

/* Emitter core: the cursor, the SIMD-type tag, and the Jcc enum. No
 * namespace -- the reference emitter's object world is what needed one, and
 * that lives under tests/emitter/reference now. */
/* Win32 requires 32 bytes of shadow stack in the caller's frame. */
#ifdef _WIN32
#define SHADOW_STACK_SIZE 32
#else
#define SHADOW_STACK_SIZE 0
#endif

/* ModRM 'mod' field enumeration.   Provided mostly for reference: */
typedef enum ModRm_ModField
{
	Mod_NoDisp = 0, /* effective address operation with no displacement, in the form of [reg] (or uses special Disp32-only encoding in the case of [ebp] form) */
	Mod_Disp8, /* effective address operation with 8 bit displacement, in the form of [reg+disp8] */
	Mod_Disp32, /* effective address operation with 32 bit displacement, in the form of [reg+disp32], */
	Mod_Direct /* direct reg/reg operation */
} ModRm_ModField;

/* ---------------------------------------------------------------------------- */
/* JccComparisonType - enumerated possibilities for inspired code branching! */

typedef enum JccComparisonType
{
	Jcc_Unknown = -2,
	Jcc_Unconditional = -1,
	Jcc_Overflow = 0x0,
	Jcc_NotOverflow = 0x1,
	Jcc_Below = 0x2,
	Jcc_Carry = 0x2,
	Jcc_AboveOrEqual = 0x3,
	Jcc_NotCarry = 0x3,
	Jcc_Zero = 0x4,
	Jcc_Equal = 0x4,
	Jcc_NotZero = 0x5,
	Jcc_NotEqual = 0x5,
	Jcc_BelowOrEqual = 0x6,
	Jcc_Above = 0x7,
	Jcc_Signed = 0x8,
	Jcc_Unsigned = 0x9,
	Jcc_ParityEven = 0xa,
	Jcc_ParityOdd = 0xb,
	Jcc_Less = 0xc,
	Jcc_GreaterOrEqual = 0xd,
	Jcc_LessOrEqual = 0xe,
	Jcc_Greater = 0xf
} JccComparisonType;

/* Not supported yet: */
/* E3 cb 	JECXZ rel8 	Jump short if ECX register is 0. */

/* ---------------------------------------------------------------------------- */
/* SSE2_ComparisonType - enumerated possibilities for SIMD data comparison! */

/* Operator selectors for the group-1/2/3 instruction families. These */
/* are API -- recompilers name the values -- so they live here with */
/* the other operand enums rather than in the reference-only headers. */
typedef enum G1Type
{
	G1Type_ADD = 0,
	G1Type_OR,
	G1Type_ADC,
	G1Type_SBB,
	G1Type_AND,
	G1Type_SUB,
	G1Type_XOR,
	G1Type_CMP
} G1Type;

typedef enum G2Type
{
	G2Type_ROL = 0,
	G2Type_ROR,
	G2Type_RCL,
	G2Type_RCR,
	G2Type_SHL,
	G2Type_SHR,
	G2Type_Unused,
	G2Type_SAR
} G2Type;

typedef enum G3Type
{
	G3Type_NOT = 2,
	G3Type_NEG = 3,
	G3Type_MUL = 4,
	G3Type_iMUL = 5, /* partial implementation, iMul has additional forms in ix86.cpp */
	G3Type_DIV = 6,
	G3Type_iDIV = 7
} G3Type;

typedef enum SSE2_ComparisonType
{
	SSE2_Equal = 0,
	SSE2_Less,
	SSE2_LessOrEqual,
	SSE2_Unordered,
	SSE2_NotEqual,
	SSE2_NotLess,
	SSE2_NotLessOrEqual,
	SSE2_Ordered
} SSE2_ComparisonType;

enum
{
	ModRm_UseSib = 4, /* same index value as ESP (used in RM field) */
	ModRm_UseDisp32 = 5, /* same index value as EBP (used in Mod field) */
	Sib_EIZ = 4, /* same index value as ESP (used in Index field) */
	Sib_UseDisp32 = 5 /* same index value as EBP (used in Base field) */
};


