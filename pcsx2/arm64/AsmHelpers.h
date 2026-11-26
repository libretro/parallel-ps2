/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
 * SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
 * SPDX-License-Identifier: GPL-3.0 */

#ifndef ARM64_ASMHELPERS_H
#define ARM64_ASMHELPERS_H

#include <stddef.h>

#include "common/Pcsx2Defs.h"
#include "arm64/ArmCompat.h"
#include "arm64/a64emit.h"

#define RWRET A64_W(0)
#define RXRET A64_X(0)
#define RQRET A64_Q(0)

#define RWARG1 A64_W(0)
#define RWARG2 A64_W(1)
#define RWARG3 A64_W(2)
#define RWARG4 A64_W(3)
#define RXARG1 A64_X(0)
#define RXARG2 A64_X(1)
#define RXARG3 A64_X(2)
#define RXARG4 A64_X(3)

/* x16: the emitter's scratch (its macro forms synthesize into it); x17:
 * address materialization, kept out of the emitter's list */
#define RXSCRATCH A64_X(16)
#define RWSCRATCH A64_W(16)
#define RSCRATCHADDR A64_X(17)

#define RQSCRATCH A64_Q(30)
#define RDSCRATCH A64_D(30)
#define RSSCRATCH A64_S(30)
#define RQSCRATCH2 A64_Q(31)
#define RDSCRATCH2 A64_D(31)
#define RSSCRATCH2 A64_S(31)
#define RQSCRATCH3 A64_Q(29)
#define RDSCRATCH3 A64_D(29)
#define RSSCRATCH3 A64_S(29)

#define RQSCRATCHI A64_V16B(30)
#define RQSCRATCHF A64_V4S(30)
#define RQSCRATCHD A64_V2D(30)

#define RQSCRATCH2I A64_V16B(31)
#define RQSCRATCH2F A64_V4S(31)
#define RQSCRATCH2D A64_V2D(31)

#ifdef __cplusplus
extern "C" {
#endif

static INLINE s64 GetPCDisplacement(const void* current, const void* target)
{
	return (s64)(((ptrdiff_t)target - (ptrdiff_t)current) >> 2);
}

static INLINE a64r armWRegister(int n) { return A64_W(n); }
static INLINE a64r armXRegister(int n) { return A64_X(n); }
static INLINE a64r armSRegister(int n) { return A64_S(n); }
static INLINE a64r armDRegister(int n) { return A64_D(n); }
static INLINE a64r armQRegister(int n) { return A64_Q(n); }

static const u32 SP_SCRATCH_OFFSET = 0;

/* The emitter of the block being compiled on this thread (MTVU compiles VU1
 * on its own), and where the next block goes. */
extern PCSX2_THREAD_LOCAL a64_emit* armAsm PCSX2_TLS_INITIAL_EXEC;
extern PCSX2_THREAD_LOCAL u8* armAsmPtr PCSX2_TLS_INITIAL_EXEC;
extern PCSX2_THREAD_LOCAL size_t armAsmCapacity PCSX2_TLS_INITIAL_EXEC;

static INLINE bool armHasBlock(void) { return armAsm != NULL; }
static INLINE u8* armGetCurrentCodePointer(void) { return a64_cursor(armAsm); }
static INLINE u8* armGetAsmPtr(void) { return armAsmPtr; }

void armSetAsmPtr(void* ptr, size_t capacity);
void armAlignAsmPtr(void);
u8* armStartBlock(void);
u8* armEndBlock(void);

void armEmitJmp(const void* ptr);
void armEmitCall(const void* ptr);
/* In-place patch: overwrite the 4-byte B at code_address with a branch to
 * target. Used by the fastmem SIGSEGV backpatch to redirect a faulting
 * Ldr/Str to its slow-path thunk (code_address is not tied to the current
 * emit cursor). code_address must already hold a single 4-byte instruction;
 * target must be within +/-128 MB (B imm26 range). */
void armEmitJmpPtr(void* code_address, const void* target, bool flush_icache);
void armEmitCbnz(a64r reg, const void* ptr);
void armEmitCondBranch(unsigned cond, const void* ptr);
void armMoveAddressToReg(a64r reg, const void* addr);
void armLoadPtr(a64r reg, const void* addr);
void armStorePtr(a64r reg, const void* addr);
/* C.72: emit an adrp into scratch and return a memory operand with the page
 * offset folded in (or a full materialization fallback). size = access size
 * in bytes; the folded form is used only when the offset encodes for it. */
a64_mem armAbsMemOperand(a64r scratch, const void* addr, unsigned size);
void armBeginStackFrame(bool save_fpr);
void armEndStackFrame(bool save_fpr);
bool armIsCalleeSavedRegister(int reg);

a64_mem armOffsetMemOperand(a64_mem op, s64 offset);
void armGetMemOperandInRegister(a64r addr_reg, a64_mem op, s64 extra_offset);

/* may clobber RSCRATCH/RSCRATCH2. they shouldn't be inputs. */
void armEmitVTBL(a64r dst, a64r src1, a64r src2, a64r tbl);

#ifdef __cplusplus
}
#endif

#endif
