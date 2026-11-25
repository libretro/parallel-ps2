// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
// SPDX-License-Identifier: GPL-3.0

#include <cstring>
#include "common/Align.h"
#include "common/Pcsx2Defs.h"
#include "arm64/AsmHelpers.h"
#include "../HostMem.h"

#include "arm64/ArmCompat.h"

#include "common/General.h"

thread_local a64_emit* armAsm PCSX2_TLS_INITIAL_EXEC;
thread_local u8* armAsmPtr PCSX2_TLS_INITIAL_EXEC;
thread_local size_t armAsmCapacity PCSX2_TLS_INITIAL_EXEC;
thread_local ArmConstantPool* armConstantPool PCSX2_TLS_INITIAL_EXEC;

void armSetAsmPtr(void* ptr, size_t capacity, ArmConstantPool* pool)
{
	pxAssert(!armAsm);
	armAsmPtr = static_cast<u8*>(ptr);
	armAsmCapacity = capacity;
	armConstantPool = pool;
}

// Align to 16 bytes, apparently ARM likes that.
void armAlignAsmPtr()
{
	static constexpr uintptr_t ALIGNMENT = 16;
	u8* new_ptr = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(armAsmPtr) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1));
	pxAssert(static_cast<size_t>(new_ptr - armAsmPtr) <= armAsmCapacity);
	armAsmCapacity -= (new_ptr - armAsmPtr);
	armAsmPtr = new_ptr;
}

// The block's emitter lives in thread_local storage: armAsm is one per thread
// (MTVU compiles VU1 on its own thread), one block at a time.
static thread_local a64_emit s_armAsmStorage;

u8* armStartBlock()
{
	armAlignAsmPtr();

	memjit_write_begin();

	pxAssert(!armAsm);
	armAsm = &s_armAsmStorage;
	a64_init(armAsm, armAsmPtr, armAsmCapacity);
	armAsm->vscratch &= ~(1u << 31);
	armAsm->scratch &= ~(1u << A64_CODE(RSCRATCHADDR));
	return armAsmPtr;
}

u8* armEndBlock()
{
	pxAssert(armAsm);

	const u32 size = static_cast<u32>(a64_offset(armAsm));
	pxAssert(size < armAsmCapacity);

	armAsm = nullptr;

	memjit_write_end();

	memsync(armAsmPtr, (u8*)(armAsmPtr) + (size));

	armAsmPtr = armAsmPtr + size;
	armAsmCapacity -= size;
	return armAsmPtr;
}

void armEmitJmp(const void* ptr, bool force_inline)
{
	s64 displacement = GetPCDisplacement(armGetCurrentCodePointer(), ptr);
	bool use_blr = !a64_is_int(displacement, 26);
	if (use_blr && armConstantPool && !force_inline)
	{
		if (u8* trampoline = armConstantPool->GetJumpTrampoline(ptr); trampoline)
		{
			displacement = GetPCDisplacement(armGetCurrentCodePointer(), trampoline);
			use_blr = !a64_is_int(displacement, 26);
		}
	}

	if (use_blr)
	{
		a64_mov_imm(armAsm, RXSCRATCH, reinterpret_cast<uintptr_t>(ptr));
		a64_br(armAsm, RXSCRATCH);
	}
	else
		a64_b_disp(armAsm, displacement);
}

void armEmitCall(const void* ptr, bool force_inline)
{
	s64 displacement = GetPCDisplacement(armGetCurrentCodePointer(), ptr);
	bool use_blr = !a64_is_int(displacement, 26);
	if (use_blr && armConstantPool && !force_inline)
	{
		if (u8* trampoline = armConstantPool->GetJumpTrampoline(ptr); trampoline)
		{
			displacement = GetPCDisplacement(armGetCurrentCodePointer(), trampoline);
			use_blr = !a64_is_int(displacement, 26);
		}
	}

	if (use_blr)
	{
		a64_mov_imm(armAsm, RXSCRATCH, reinterpret_cast<uintptr_t>(ptr));
		a64_blr(armAsm, RXSCRATCH);
	}
	else
		a64_bl_disp(armAsm, displacement);
}

void armEmitJmpPtr(void* code_address, const void* target, bool flush_icache)
{
	const s64 displacement = GetPCDisplacement(code_address, target);
	pxAssert(a64_is_int(displacement, 26));

	memjit_write_begin();
	a64_patch_b(code_address, target);
	memjit_write_end();

	if (flush_icache)
		memsync(code_address, (u8*)(code_address) + (4));
}

void armEmitCbnz(a64r reg, const void* ptr)
{
	const s64 jump_distance =
		static_cast<s64>(reinterpret_cast<intptr_t>(ptr) - reinterpret_cast<intptr_t>(armGetCurrentCodePointer()));
	//pxAssert(pcsx2_is_aligned_u32(jump_distance, 4));
	if (a64_is_int(jump_distance >> 2, 19))
		a64_cbnz_disp(armAsm, reg, jump_distance >> 2);
	else
	{
		a64_label branch_not_taken = A64_LABEL_INIT;
		a64_cbz(armAsm, reg, &branch_not_taken);

		const s64 new_jump_distance =
			static_cast<s64>(reinterpret_cast<intptr_t>(ptr) - reinterpret_cast<intptr_t>(armGetCurrentCodePointer()));
		a64_b_disp(armAsm, new_jump_distance >> 2);
		a64_bind(armAsm, &branch_not_taken);
	}
}

void armEmitCondBranch(unsigned cond, const void* ptr)
{
	const s64 jump_distance =
		static_cast<s64>(reinterpret_cast<intptr_t>(ptr) - reinterpret_cast<intptr_t>(armGetCurrentCodePointer()));
	//pxAssert(pcsx2_is_aligned_u32(jump_distance, 4));

	if (a64_is_int(jump_distance >> 2, 19))
		a64_b_cond_disp(armAsm, jump_distance >> 2, cond);
	else
	{
		a64_label branch_not_taken = A64_LABEL_INIT;
		a64_b_cond(armAsm, &branch_not_taken, A64_INVERT(cond));

		const s64 new_jump_distance =
			static_cast<s64>(reinterpret_cast<intptr_t>(ptr) - reinterpret_cast<intptr_t>(armGetCurrentCodePointer()));
		a64_b_disp(armAsm, new_jump_distance >> 2);
		a64_bind(armAsm, &branch_not_taken);
	}
}

void armMoveAddressToReg(a64r reg, const void* addr)
{
	// psxAsm->Mov(reg, static_cast<u64>(reinterpret_cast<uintptr_t>(addr)));
	pxAssert(a64_is_x(reg));

	const void* current_code_ptr_page = reinterpret_cast<const void*>(
		reinterpret_cast<uintptr_t>(armGetCurrentCodePointer()) & ~static_cast<uintptr_t>(0xFFF));
	const void* ptr_page =
		reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(addr) & ~static_cast<uintptr_t>(0xFFF));
	const s64 page_displacement = GetPCDisplacement(current_code_ptr_page, ptr_page) >> 10;
	const u32 page_offset = static_cast<u32>(reinterpret_cast<uintptr_t>(addr) & 0xFFFu);
	if (a64_is_int(page_displacement, 21) && a64_is_addsub_imm(page_offset))
	{
		a64_adrp(armAsm, reg, page_displacement);
		a64_add_imm(armAsm, reg, reg, page_offset);
	}
	else if (a64_is_int(page_displacement, 21) && a64_is_logical_imm(page_offset, 64))
	{
		a64_adrp(armAsm, reg, page_displacement);
		a64_orr_imm(armAsm, reg, reg, page_offset);
	}
	else
		a64_mov_imm(armAsm, reg, reinterpret_cast<uintptr_t>(addr));
}

// C.72: address an absolute global for a load/store of `size` bytes. When the
// page is adrp-reachable and the page offset is size-aligned (i.e. it encodes
// as an unsigned scaled immediate), emit just the `adrp` and fold the offset
// into the returned MemOperand -- one instruction shorter per access than
// armMoveAddressToReg + [scratch], and repeated accesses through the same
// materialization (ldr+op+str patterns) reuse the one adrp. Falls back to the
// full materialization otherwise; the fallback keeps offset 0 so it encodes
// unconditionally (the folded form never needs a second scratch from the
// emitter either -- `scratch` here is usually x17).
a64_mem armAbsMemOperand(a64r scratch, const void* addr, unsigned size)
{
	const void* current_code_ptr_page = reinterpret_cast<const void*>(
		reinterpret_cast<uintptr_t>(armGetCurrentCodePointer()) & ~static_cast<uintptr_t>(0xFFF));
	const void* ptr_page =
		reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(addr) & ~static_cast<uintptr_t>(0xFFF));
	const s64 page_displacement = GetPCDisplacement(current_code_ptr_page, ptr_page) >> 10;
	const u32 page_offset = static_cast<u32>(reinterpret_cast<uintptr_t>(addr) & 0xFFFu);
	if (a64_is_int(page_displacement, 21) && size && (page_offset % size) == 0)
	{
		a64_adrp(armAsm, scratch, page_displacement);
		return a64_mem_off(scratch, page_offset);
	}
	armMoveAddressToReg(scratch, addr);
	return a64_mem_off(scratch, 0);
}

void armLoadPtr(a64r reg, const void* addr)
{
	a64_ldr(armAsm, reg, armAbsMemOperand(RSCRATCHADDR, addr, a64_size_bytes(reg)));
}

void armStorePtr(a64r reg, const void* addr)
{
	a64_str(armAsm, reg, armAbsMemOperand(RSCRATCHADDR, addr, a64_size_bytes(reg)));
}

void armBeginStackFrame(bool save_fpr)
{
	// save x19 through x28, x29 could also be used
	a64_sub_imm(armAsm, A64_SP, A64_SP, save_fpr ? 192 : 144);
	a64_stp(armAsm, A64_X(19), A64_X(20), a64_mem_off(A64_SP, 32));
	a64_stp(armAsm, A64_X(21), A64_X(22), a64_mem_off(A64_SP, 48));
	a64_stp(armAsm, A64_X(23), A64_X(24), a64_mem_off(A64_SP, 64));
	a64_stp(armAsm, A64_X(25), A64_X(26), a64_mem_off(A64_SP, 80));
	a64_stp(armAsm, A64_X(27), A64_X(28), a64_mem_off(A64_SP, 96));
	a64_stp(armAsm, A64_X(29), A64_LR, a64_mem_off(A64_SP, 112));
	if (save_fpr)
	{
		a64_stp(armAsm, A64_D(8), A64_D(9), a64_mem_off(A64_SP, 128));
		a64_stp(armAsm, A64_D(10), A64_D(11), a64_mem_off(A64_SP, 144));
		a64_stp(armAsm, A64_D(12), A64_D(13), a64_mem_off(A64_SP, 160));
		a64_stp(armAsm, A64_D(14), A64_D(15), a64_mem_off(A64_SP, 176));
	}
}

void armEndStackFrame(bool save_fpr)
{
	if (save_fpr)
	{
		a64_ldp(armAsm, A64_D(14), A64_D(15), a64_mem_off(A64_SP, 176));
		a64_ldp(armAsm, A64_D(12), A64_D(13), a64_mem_off(A64_SP, 160));
		a64_ldp(armAsm, A64_D(10), A64_D(11), a64_mem_off(A64_SP, 144));
		a64_ldp(armAsm, A64_D(8), A64_D(9), a64_mem_off(A64_SP, 128));
	}
	a64_ldp(armAsm, A64_X(29), A64_LR, a64_mem_off(A64_SP, 112));
	a64_ldp(armAsm, A64_X(27), A64_X(28), a64_mem_off(A64_SP, 96));
	a64_ldp(armAsm, A64_X(25), A64_X(26), a64_mem_off(A64_SP, 80));
	a64_ldp(armAsm, A64_X(23), A64_X(24), a64_mem_off(A64_SP, 64));
	a64_ldp(armAsm, A64_X(21), A64_X(22), a64_mem_off(A64_SP, 48));
	a64_ldp(armAsm, A64_X(19), A64_X(20), a64_mem_off(A64_SP, 32));
	a64_add_imm(armAsm, A64_SP, A64_SP, save_fpr ? 192 : 144);
}

bool armIsCalleeSavedRegister(int reg)
{
	// same on both linux and windows
	return (reg >= 19);
}

a64_mem armOffsetMemOperand(const a64_mem& op, s64 offset)
{
	pxAssert(A64_ISREG(op.base) && op.mode == A64_OFFSET);
	return a64_mem_off(op.base, op.offset + offset);
}

void armGetMemOperandInRegister(a64r addr_reg, const a64_mem& op, s64 extra_offset /*= 0*/)
{
	pxAssert(a64_is_x(addr_reg));
	pxAssert(A64_ISREG(op.base) && op.mode == A64_OFFSET);
	a64_add_imm(armAsm, addr_reg, op.base, op.offset + extra_offset);
}

void armEmitVTBL(a64r dst, a64r src1, a64r src2, a64r tbl)
{
	pxAssert(A64_CODE(src1) != A64_CODE(RQSCRATCH) && A64_CODE(src2) != A64_CODE(RQSCRATCH2));
	pxAssert(A64_CODE(tbl) != A64_CODE(RQSCRATCH) && A64_CODE(tbl) != A64_CODE(RQSCRATCH2));

	// must be consecutive
	if (A64_CODE(src2) == (A64_CODE(src1) + 1))
	{
		a64_tbl2(armAsm, a64_as(dst, A64K_16B), a64_as(src1, A64K_16B), a64_as(src2, A64K_16B), a64_as(tbl, A64K_16B));
		return;
	}

	a64_mov(armAsm, a64_as(RQSCRATCH, A64K_Q), a64_as(src1, A64K_Q));
	a64_mov(armAsm, a64_as(RQSCRATCH2, A64K_Q), a64_as(src2, A64K_Q));
	a64_tbl2(armAsm, a64_as(dst, A64K_16B), a64_as(RQSCRATCH, A64K_16B), a64_as(RQSCRATCH2, A64K_16B), a64_as(tbl, A64K_16B));
}


void ArmConstantPool::Init(void* ptr, u32 capacity)
{
	m_base_ptr = static_cast<u8*>(ptr);
	m_capacity = capacity;
	m_used = 0;
	m_jump_targets.clear();
}

void ArmConstantPool::Destroy()
{
	m_base_ptr = nullptr;
	m_capacity = 0;
	m_used = 0;
	m_jump_targets.clear();
}

void ArmConstantPool::Reset()
{
	m_used = 0;
	m_jump_targets.clear();
}

u8* ArmConstantPool::GetJumpTrampoline(const void* target)
{
	auto it = m_jump_targets.find(target);
	if (it != m_jump_targets.end())
		return m_base_ptr + it->second;

	// align to 16 bytes?
	const u32 offset = pcsx2_align_up_pow2_u32(m_used, 16);

	// 4 movs plus a jump
	if ((m_capacity - offset) < 20)
	{
		log_cb(RETRO_LOG_ERROR, "Ran out of space in constant pool\n");
		return nullptr;
	}

	a64_emit masm;
	a64_init(&masm, m_base_ptr + offset, m_capacity - offset);
	a64_mov_imm(&masm, RXSCRATCH, reinterpret_cast<intptr_t>(target));
	a64_br(&masm, RXSCRATCH);

	pxAssert(a64_offset(&masm) < 20);
	m_jump_targets.emplace(target, offset);
	m_used = offset + static_cast<u32>(a64_offset(&masm));

	memsync(reinterpret_cast<void*>(m_base_ptr + offset), (u8*)(reinterpret_cast<void*>(m_base_ptr + offset)) + (m_used - offset));

	return m_base_ptr + offset;
}

// Definitions for the common/Perf.h stub (see that header).
#include "common/Perf.h"
namespace Perf
{
	Group any, ee, iop, vu, vu0, vu1, vif;
}

// COP2-macro-mode analysis pointer (aR5900Analysis.h). The full aR5900
// analysis pass is not ported; VU0 macro mode is not wired on this fork yet,
// so the aVU_Macro.inl paths that read this are never invoked. Null keeps the
// linker happy and any accidental use loudly segfaults.
#include "arm64/aR5900Analysis.h"
EEINST* g_pCurInstInfo = nullptr;
