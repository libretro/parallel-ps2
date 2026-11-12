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

#include "../../Common.h"
#include "../../HostMem.h"
#include "../../vtlb.h"
#include "../iCore.h"
#include "../iR5900.h"
#include "common/emitter/c89ops.h"
#include "../../Config.h"

/* we need enough for a 32-bit jump forwards (5 bytes) */
static const u32 LOADSTORE_PADDING = 5;

static u32 GetAllocatedGPRBitmask(void)
{
	u32 mask = 0;
	u32 i;
	for (i = 0; i < iREGCNT_GPR; i++)
	{
		if (x86regs[i].inuse)
			mask |= (1u << i);
	}
	return mask;
}

static u32 GetAllocatedXMMBitmask(void)
{
	u32 mask = 0;
	u32 i;
	for (i = 0; i < iREGCNT_XMM; i++)
	{
		if (xmmregs[i].inuse)
			mask |= (1u << i);
	}
	return mask;
}

	/* ------------------------------------------------------------------------
	 * Prepares eax, ecx, and, ebx for Direct or Indirect operations.
	 * Returns the writeback pointer for ebx (return address from indirect handling)
	 */
	static void DynGen_PrepRegs(int addr_reg, int value_reg, u32 sz, int xmm)
	{
		_freeX86reg(XE_ARG1);
		xe_mov32_rr(XE_ARG1, addr_reg);

		if (value_reg >= 0)
		{
			if (sz == 128)
			{
#if PCSX2_MINGW_R128_BY_PTR
				/* MinGW GCC silently drops __vectorcall and passes r128 */
				/* (16-byte aggregate) by-value via the MS x64 ABI's hidden */
				/* pointer in rdx. To match that, spill the value to a 16-byte */
				/* aligned scratch slot at [rsp] (within the dispatcher's */
				/* shadow space on Win64, which we own at this call boundary) */
				/* and pass that pointer in arg2reg. */
				{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, 0); xe_movaps_memxg(xm, value_reg); }
				_freeX86reg(XE_ARG2);
				xe_mov64_rr(XE_ARG2, XE_SP);
#else
				_freeXMMreg(XE_XMM_ARG(1, 0));
				xe_movaps_xx(XE_XMM_ARG(1, 0), value_reg);
#endif
			}
			else if (xmm)
			{
				/* 32bit xmms are passed in GPRs */
				_freeX86reg(XE_ARG2);
				xe_movd_rx(XE_ARG2, value_reg);
			}
			else
			{
				_freeX86reg(XE_ARG2);
				xe_mov64_rr(XE_ARG2, value_reg);
			}
		}

		xe_mov32_rr(XE_AX, XE_ARG1);
		xe_shr32_ri(XE_AX, VTLB_PAGE_BITS);
		{ struct e_mem xm; xe_complexaddr_si(xm, XE_ARG3, vtlbdata.vmap, XE_AX, XE_WORDSIZE); xe_mov64_rmemg(XE_AX, xm); }
		xe_add64_rr(XE_ARG1, XE_AX);
	}

	/* ------------------------------------------------------------------------ */
	static void DynGen_DirectRead(u32 bits, int sign)
	{
		switch (bits)
		{
			case 8:
				if (sign)
					{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_movsx64_rmemg8(XE_AX, xm); }
				else
					{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_movzx32_mem8(XE_AX, xm); }
				break;

			case 16:
				if (sign)
					{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_movsx64_rmemg16(XE_AX, xm); }
				else
					{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_movzx32_mem16(XE_AX, xm); }
				break;

			case 32:
				if (sign)
					{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_movsxd_rmemg(XE_AX, xm); }
				else
					{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_mov32_rmem(XE_AX, xm); }
				break;

			case 64:
				{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_mov64_rmemg(XE_AX, xm); }
				break;

			case 128:
				{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_movaps_xmemg(0, xm); }
				break;
			default:
				break;
		}
	}

	/* ------------------------------------------------------------------------ */
	static void DynGen_DirectWrite(u32 bits)
	{
		switch (bits)
		{
			case 8:
				{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_mov8_memgr(xm, XE_ARG2); }
				break;

			case 16:
				{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_mov16_memgr(xm, XE_ARG2); }
				break;

			case 32:
				{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_mov32_memgr(xm, XE_ARG2); }
				break;

			case 64:
				{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_mov64_memgr(xm, XE_ARG2); }
				break;

			case 128:
#if PCSX2_MINGW_R128_BY_PTR
				/* Value was spilled to [rsp] by PrepRegs (see comment there). */
				{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, 0); xe_movaps_xmemg(0, xm); }
				{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_movaps_memxg(xm, 0); }
#else
				{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_movaps_memxg(xm, XE_XMM_ARG(1, 0)); }
#endif
				break;
		}
	}

/* ------------------------------------------------------------------------ */
/* allocate one page for our naked indirect dispatcher function. */
/* this *must* be a full page, since we'll give it execution permission later. */
/* If it were smaller than a page we'd end up allowing execution rights on some */
/* other vars additionally (bad!). */

PCSX2_ALIGN(__pagesize) static u8 m_IndirectDispatchers[__pagesize];

/* ------------------------------------------------------------------------ */
/* mode        - 0 for read, 1 for write! */
/* operandsize - 0 thru 4 represents 8, 16, 32, 64, and 128 bits. */

static u8* GetIndirectDispatcherPtr(int mode, int operandsize, int sign)
{
	/* Each dispatcher is aligned to 64 bytes.  The actual dispatchers are only like */
	/* 20-some bytes each, but 64 byte alignment on functions that are called */
	/* more frequently than a hot sex hotline at 1:15am is probably a good thing. */

	/* 7*64? 5 widths with two sign extension modes for 8 and 16 bit reads */

	/* Gregory: a 32 bytes alignment is likely enough and more cache friendly */
	const int A = 32;
	return &m_IndirectDispatchers[(mode * (8 * A)) + (sign * 5 * A) + (operandsize * A)];
}

/* ------------------------------------------------------------------------ */
/* Generates a JS instruction that targets the appropriate templated instance of */
/* the vtlb Indirect Dispatcher. */

/* Which direct-path generator the caller wants; replaces the lambda the
 * old template took, so this is a plain function again. */
typedef enum DynGenDirect
{
	DYNGEN_DIRECT_READ = 0,
	DYNGEN_DIRECT_WRITE = 1
} DynGenDirect;

static void DynGen_HandlerTest(int direct_kind, u32 direct_bits, int direct_sign, int mode, int bits, int sign)
{
	int szidx = 0;
	uint8_t* to_handler;
	uint8_t* done;
	switch (bits)
	{
		case   8: szidx = 0; break;
		case  16: szidx = 1; break;
		case  32: szidx = 2; break;
		case  64: szidx = 3; break;
		case 128: szidx = 4; break;
		default:
			  break;
	}
	 xe_fwd_jcc8(Jcc_Signed, to_handler);
	if (direct_kind == DYNGEN_DIRECT_WRITE)
		DynGen_DirectWrite(direct_bits);
	else
		DynGen_DirectRead(direct_bits, direct_sign);
	 xe_fwd_jcc8(Jcc_Unconditional, done);
	xe_fwd_set8(to_handler);
	xe_fastcall0(GetIndirectDispatcherPtr(mode, szidx, sign));
	xe_fwd_set8(done);
}

/* ------------------------------------------------------------------------ */
/* Generates the various instances of the indirect dispatchers */
/* In: arg1reg: vtlb entry, arg2reg: data ptr (if mode >= 64), rbx: function return ptr */
/* Out: eax: result (if mode < 64) */
static void DynGen_IndirectTlbDispatcher(int mode, int bits, int sign)
{
	sptr table;
	/* fixup stack */
#ifdef _WIN32
	xe_sub64_ri(XE_SP, 32 + 8);
#else
	xe_sub64_ri(XE_SP, 8);
#endif

	xe_movzx32_rr8(XE_AX, XE_AX);
	if (XE_WORDSIZE != 8)
		xe_sub32_ri(XE_ARG1, 0x80000000);
	xe_sub32_rr(XE_ARG1, XE_AX);

	/* jump to the indirect handler, which is a C++ function. */
	/* [ecx is address, edx is data] */
	table = (sptr)vtlbdata.RWFT[bits][mode];
	if (table == (s32)table)
	{
		{ struct e_mem xm; E_MEM(xm, E_NOREG, XE_AX, XE_WORDSIZE, (intptr_t)table); xe_call_memg(xm); }
	}
	else
	{
		xe_lea64_m(XE_ARG3, (void*)table);
		{ struct e_mem xm; E_MEM(xm, XE_ARG3, XE_AX, XE_WORDSIZE, 0); xe_call_memg(xm); }
	}

	if (!mode)
	{
		if (bits == 0)
		{
			if (sign)
				xe_movsx64_rr8(XE_AX, XE_AX);
			else
				xe_movzx32_rr8(XE_AX, XE_AX);
		}
		else if (bits == 1)
		{
			if (sign)
				xe_movsx64_rr16(XE_AX, XE_AX);
			else
				xe_movzx32_rr16(XE_AX, XE_AX);
		}
		else if (bits == 2)
		{
			if (sign)
				xe_cdqe();
		}
	}

#ifdef _WIN32
	xe_add64_ri(XE_SP, 32 + 8);
#else
	xe_add64_ri(XE_SP, 8);
#endif

	xe_ret();
}

/* One-time initialization procedure.  Multiple subsequent calls during the lifespan of the */
/* process will be ignored. */

void vtlb_DynGenDispatchers(void)
{
	PageProtectionMode mode;
	int dmode;
	static int hasBeenCalled = 0;
	if (hasBeenCalled)
		return;
	hasBeenCalled = 1;

	mode.m_read   = 1;
	mode.m_write  = 1;
	mode.m_exec   = 0;
	/* In case init gets called multiple times: */
	mprotect(m_IndirectDispatchers, __pagesize, host_prot(mode));

	/* clear the buffer to 0xcc (easier debugging). */
	memset(m_IndirectDispatchers, 0xcc, __pagesize);

	for (dmode = 0; dmode < 2; ++dmode)
	{
		int bits;
		for (bits = 0; bits < 5; ++bits)
		{
			int sign;
			for (sign = 0; sign < (!dmode && bits < 3 ? 2 : 1); sign++)
			{
				x86Ptr = (u8*)(GetIndirectDispatcherPtr(dmode, bits, !!sign));

				DynGen_IndirectTlbDispatcher(dmode, bits, !!sign);
			}
		}
	}

	mode.m_write  = 0;
	mode.m_exec   = 1;
	mprotect(m_IndirectDispatchers, __pagesize, host_prot(mode));
}

/*                            Dynarec Load Implementations */
/* ------------------------------------------------------------------------ */
/* Recompiled input registers: */
/*   ecx - source address to read from */
/*   Returns read value in eax. */
int vtlb_DynGenReadNonQuad(u32 bits, int sign, int xmm, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	int x86_dest_reg;
	const u8* codeStart;
	int x86addr;
	u32 padding;
	u32 i;
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
	{
		iFlushCall(FLUSH_FULLVTLB);

		DynGen_PrepRegs(addr_reg, -1, bits, xmm);
		DynGen_HandlerTest(DYNGEN_DIRECT_READ, bits, sign, 0, bits, sign && bits < 64);

		if (!xmm)
		{
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(XE_AX), XE_AX);
			xe_mov64_rr(x86_dest_reg, XE_AX);
		}
		else
		{
			/* we shouldn't be loading any FPRs which aren't 32bit.. */
			/* we use MOVD here despite it being floating-point data, because we're going int->float reinterpret. */
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
			xe_movdzx_xr(x86_dest_reg, XE_AX);
		}

		return x86_dest_reg;
	}

	x86addr = addr_reg;
	if (!xmm)
	{
		int x86reg;
		x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(XE_AX), XE_AX);
		codeStart = x86Ptr;
		x86reg = x86_dest_reg;
		switch (bits)
		{
		case 8:
			{ struct e_mem xm; E_MEM(xm, 5, x86addr, 1, 0); if (sign) xe_movsx64_rmemg8(x86reg, xm); else xe_movzx32_mem8(x86reg, xm); }
			break;
		case 16:
			{ struct e_mem xm; E_MEM(xm, 5, x86addr, 1, 0); if (sign) xe_movsx64_rmemg16(x86reg, xm); else xe_movzx32_mem16(x86reg, xm); }
			break;
		case 32:
			{ struct e_mem xm; E_MEM(xm, 5, x86addr, 1, 0); if (sign) xe_movsxd_rmemg(x86reg, xm); else xe_mov32_rmem(x86reg, xm); }
			break;
		case 64:
			{ struct e_mem xm; E_MEM(xm, 5, x86addr, 1, 0); xe_mov64_rmemg(x86reg, xm); }
			break;
		default:
			break;
		}
	}
	else
	{
		int xmmreg;
		x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		codeStart = x86Ptr;
		xmmreg = x86_dest_reg;
		{ struct e_mem xm; E_MEM(xm, 5, x86addr, 1, 0); xe_movss_xmemg(xmmreg, xm); }
	}

	padding = LOADSTORE_PADDING - MIN_U32((u32)(x86Ptr - codeStart), 5);
	for (i = 0; i < padding; i++)
		xe_nop();

	vtlb_AddLoadStoreInfo((uptr)codeStart, (u32)(x86Ptr - codeStart),
		pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
		(u8)(addr_reg), (u8)(x86_dest_reg),
		(u8)(bits), sign, 1, xmm);

	return x86_dest_reg;
}

/* ------------------------------------------------------------------------ */
/* Recompiled input registers: */
/*   ecx - source address to read from */
/*   Returns read value in eax. */

/* TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the */
/* recompiler if the TLB is changed. */

int vtlb_DynGenReadNonQuad_Const(u32 bits, int sign, int xmm, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	int x86_dest_reg;
	const vtlb_virt_t vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (!vtlb_virt_is_handler(vmv, addr_const))
	{
		const uptr ppf = vtlb_virt_ptr(vmv, addr_const);
		if (!xmm)
		{
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(XE_AX), XE_AX);
			switch (bits)
			{
			case 8:
				if (sign) xe_movsx64_rm8(x86_dest_reg, (u8*)ppf); else xe_movzx32_rm8(x86_dest_reg, (u8*)ppf);
				break;

			case 16:
				if (sign) xe_movsx64_rm16(x86_dest_reg, (u16*)ppf); else xe_movzx32_rm16(x86_dest_reg, (u16*)ppf);
				break;

			case 32:
				if (sign) xe_movsxd_rm(x86_dest_reg, (u32*)ppf); else xe_mov32_rm(x86_dest_reg, (u32*)ppf);
				break;

			case 64:
				xe_mov64_rm(x86_dest_reg, (u64*)ppf);
				break;
			}
		}
		else
		{
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
			xe_movss_xm(x86_dest_reg, (float*)ppf);
		}
	}
	else
	{
		/* has to: translate, find function, call function */
		u32 paddr = vtlb_virt_paddr(vmv, addr_const);

		int szidx = 0;
		switch (bits)
		{
			case  8: break;
			case 16: szidx = 1; break;
			case 32: szidx = 2; break;
			case 64: szidx = 3; break;
		}

		/* Shortcut for the INTC_STAT register, which many games like to spin on heavily. */
		if ((bits == 32) && !EmuConfig.Speedhacks.IntcStat && (paddr == INTC_STAT))
		{
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(XE_AX), XE_AX);
			if (!xmm)
			{
				if (sign)
					xe_movsxd_rm(x86_dest_reg, &psHu32(INTC_STAT));
				else
					xe_mov32_rm(x86_dest_reg, &psHu32(INTC_STAT));
			}
			else
			{
				{ struct e_mem xm; XE_MEM_ABS(xm, &psHu32(INTC_STAT)); xe_movdzx_xmemg(x86_dest_reg, xm); }
			}
		}
		else
		{
			iFlushCall(FLUSH_FULLVTLB);
			xe_fastcall1_i(vtlb_virt_handler_raw(vmv, szidx, 0), paddr);

			if (!xmm)
			{
				x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(XE_AX), XE_AX);
				switch (bits)
				{
					/* save REX prefix by using 32bit dest for zext */
				case 8:
					if (sign) xe_movsx64_rr8(x86_dest_reg, XE_AX); else xe_movzx32_rr8(x86_dest_reg, XE_AX);
					break;

				case 16:
					if (sign) xe_movsx64_rr16(x86_dest_reg, XE_AX); else xe_movzx32_rr16(x86_dest_reg, XE_AX);
					break;

				case 32:
					if (sign) xe_movsxd_rr(x86_dest_reg, XE_AX); else xe_mov32_rr(x86_dest_reg, XE_AX);
					break;

				case 64:
					xe_mov64_rr(x86_dest_reg, XE_AX);
					break;
				}
			}
			else
			{
				x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
				xe_movdzx_xr(x86_dest_reg, XE_AX);
			}
		}
	}

	return x86_dest_reg;
}

int vtlb_DynGenReadQuad(u32 bits, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	int reg;
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
	{
		iFlushCall(FLUSH_FULLVTLB);

		DynGen_PrepRegs(XE_ARG1, -1, bits, 1);
		DynGen_HandlerTest(DYNGEN_DIRECT_READ, bits, 0, 0, bits, 0);

		/* The call here needs to be after the above function calls. */
		reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0); /* Handler returns in xmm0 */

		if (reg >= 0)
			xe_movaps_xx(reg, 0);
	}
	else
	{
		const u8* codeStart = x86Ptr;
		u32 padding;
		u32 i;

		reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0); /* Handler returns in xmm0 */

		{ struct e_mem xm; E_MEM(xm, 5, XE_ARG1, 1, 0); xe_movaps_xmemg(reg, xm); }

		padding = LOADSTORE_PADDING - MIN_U32((u32)(x86Ptr - codeStart), 5);
		for (i = 0; i < padding; i++)
			xe_nop();

		vtlb_AddLoadStoreInfo((uptr)codeStart, (u32)(x86Ptr - codeStart),
				pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
				(u8)(XE_ARG1), (u8)(reg),
				(u8)(bits), 0, 1, 1);
	}
	return reg;
}

/* ------------------------------------------------------------------------ */
/* TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the */
/* recompiler if the TLB is changed. */
int vtlb_DynGenReadQuad_Const(u32 bits, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	int reg  = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
	const vtlb_virt_t vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (!vtlb_virt_is_handler(vmv, addr_const))
	{
		void* ppf = (void*)(vtlb_virt_ptr(vmv, addr_const));
		if (reg >= 0)
			xe_movaps_xm(reg, ppf);
	}
	else
	{
		/* has to: translate, find function, call function */
		u32 paddr = vtlb_virt_paddr(vmv, addr_const);

		const int szidx = 4;
		iFlushCall(FLUSH_FULLVTLB);
		xe_fastcall1_i(vtlb_virt_handler_raw(vmv, szidx, 0), paddr);

		xe_movaps_xx(reg, 0);
	}

	return reg;
}

/*                            Dynarec Store Implementations */

void vtlb_DynGenWrite(u32 sz, int xmm, int addr_reg, int value_reg)
{
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
	{
		iFlushCall(FLUSH_FULLVTLB);

		DynGen_PrepRegs(addr_reg, value_reg, sz, xmm);
		DynGen_HandlerTest(DYNGEN_DIRECT_WRITE, sz, 0, 1, sz, 0);
	}
	else
	{
		const u8* codeStart = x86Ptr;

		const int vaddr_reg = addr_reg;
		u32 padding;
		u32 i;
		if (!xmm)
		{
			switch (sz)
			{
				case 8:
					{ struct e_mem xm; E_MEM(xm, 5, vaddr_reg, 1, 0); xe_mov8_memgr(xm, value_reg); }
					break;
				case 16:
					{ struct e_mem xm; E_MEM(xm, 5, vaddr_reg, 1, 0); xe_mov16_memgr(xm, value_reg); }
					break;
				case 32:
					{ struct e_mem xm; E_MEM(xm, 5, vaddr_reg, 1, 0); xe_mov32_memgr(xm, value_reg); }
					break;
				case 64:
					{ struct e_mem xm; E_MEM(xm, 5, vaddr_reg, 1, 0); xe_mov64_memgr(xm, value_reg); }
					break;
				default:
					break;
			}
		}
		else
		{
			switch (sz)
			{
				case 32:
					{ struct e_mem xm; E_MEM(xm, 5, vaddr_reg, 1, 0); xe_movss_memxg(xm, value_reg); }
					break;
				case 128:
					{ struct e_mem xm; E_MEM(xm, 5, vaddr_reg, 1, 0); xe_movaps_memxg(xm, value_reg); }
					break;
				default:
					break;
			}
		}

		padding = LOADSTORE_PADDING - MIN_U32((u32)(x86Ptr - codeStart), 5);
		for (i = 0; i < padding; i++)
			xe_nop();

		vtlb_AddLoadStoreInfo((uptr)codeStart, (u32)(x86Ptr - codeStart),
				pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
				(u8)(addr_reg), (u8)(value_reg),
				(u8)(sz), 0, 0, xmm);
	}
}

/* ------------------------------------------------------------------------ */
/* Generates code for a store instruction, where the address is a known constant. */
/* TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the */
/* recompiler if the TLB is changed. */
void vtlb_DynGenWrite_Const(u32 bits, int xmm, u32 addr_const, int value_reg)
{
	const vtlb_virt_t vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (!vtlb_virt_is_handler(vmv, addr_const))
	{
		const uptr ppf = vtlb_virt_ptr(vmv, addr_const);
		if (!xmm)
		{
			switch (bits)
			{
				case 8:
					xe_mov8_mr((void*)ppf, value_reg);
					break;

				case 16:
					xe_mov16_mr((void*)ppf, value_reg);
					break;

				case 32:
					xe_mov32_mr((void*)ppf, value_reg);
					break;

				case 64:
					xe_mov64_mr((void*)ppf, value_reg);
					break;
				default:
					break;
			}
		}
		else
		{
			switch (bits)
			{
				case 32:
					xe_movss_mx((void*)ppf, value_reg);
					break;

				case 128:
					xe_movaps_mx((void*)ppf, value_reg);
					break;
				default:
					break;
			}
		}
	}
	else
	{
		/* has to: translate, find function, call function */
		u32 paddr = vtlb_virt_paddr(vmv, addr_const);

		int szidx = 0;
		switch (bits)
		{
			case 8:
				break;
			case 16:
				szidx = 1;
				break;
			case 32:
				szidx = 2;
				break;
			case 64:
				szidx = 3;
				break;
			case 128:
				szidx = 4;
				break;
		}

		iFlushCall(FLUSH_FULLVTLB);

		_freeX86reg(XE_ARG1);
		xe_mov32_ri(XE_ARG1, paddr);
		if (bits == 128)
		{
#if PCSX2_MINGW_R128_BY_PTR
			/* MinGW: spill to [rsp] and pass pointer in arg2reg. See */
			/* DynGen_PrepRegs for rationale. */
			{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, 0); xe_movaps_memxg(xm, value_reg); }
			_freeX86reg(XE_ARG2);
			xe_mov64_rr(XE_ARG2, XE_SP);
#else
			const int argreg = XE_XMM_ARG(1, 0);
			_freeXMMreg(argreg);
			xe_movaps_xx(argreg, value_reg);
#endif
		}
		else if (xmm)
		{
			_freeX86reg(XE_ARG2);
			xe_movd_rx(XE_ARG2, value_reg);
		}
		else
		{
			_freeX86reg(XE_ARG2);
			xe_mov64_rr(XE_ARG2, value_reg);
		}

		xe_fastcall0(vtlb_virt_handler_raw(vmv, szidx, 1));
	}
}

/* 							Extra Implementations */

/*   ecx - virtual address */
/*   Returns physical address in eax. */
/*   Clobbers edx */
void vtlb_DynV2P(void)
{
	xe_mov32_rr(XE_AX, XE_CX);
	xe_and32_ri(XE_CX, VTLB_PAGE_MASK); /* vaddr & VTLB_PAGE_MASK */

	xe_shr32_ri(XE_AX, VTLB_PAGE_BITS);
	{ struct e_mem xm; xe_complexaddr_si(xm, XE_DX, vtlbdata.ppmap, XE_AX, 4); xe_mov32_rmem(XE_AX, xm); } /* vtlbdata.ppmap[vaddr >> VTLB_PAGE_BITS]; */

	xe_or32_rr(XE_AX, XE_CX);
}

void vtlb_DynBackpatchLoadStore(uptr code_address, u32 code_size, u32 guest_pc, u32 guest_addr, u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register, u8 size_in_bits, int is_signed, int is_load, int is_xmm)
{
	static const u32 GPR_SIZE = 8;
	static const u32 XMM_SIZE = 16;

	/* on win32, we need to reserve an additional 32 bytes shadow space when calling out to C */
#ifdef _WIN32
	static const u32 SHADOW_SIZE = 32;
#else
	static const u32 SHADOW_SIZE = 0;
#endif
	u8* thunk = recBeginThunk();

	/* save regs */
	u32 num_gprs = 0;
	u32 num_fprs = 0;

	const u32 rbxid = (u32)(3 /* rbx */);
	const u32 arg1id = (u32)(XE_ARG1);
	const u32 arg2id = (u32)(XE_ARG2);
	const u32 arg3id = (u32)(XE_ARG3);
	u32 i;
	u32 stack_size;

	for (i = 0; i < iREGCNT_GPR; i++)
	{
		if ((gpr_bitmask & (1u << i)) && (i == rbxid || i == arg1id || i == arg2id || XE_IS_CALLER_SAVED(i)) && (!is_load || is_xmm || data_register != i))
			num_gprs++;
	}
	for (i = 0; i < iREGCNT_XMM; i++)
	{
		if (fpr_bitmask & (1u << i) && XE_XMM_CALLER_SAVED(i) && (!is_load || !is_xmm || data_register != i))
			num_fprs++;
	}

	stack_size = (((num_gprs + 1) & ~1u) * GPR_SIZE) + (num_fprs * XMM_SIZE) + SHADOW_SIZE;

	if (stack_size > 0)
	{
		u32 stack_offset;
		u32 i;
		xe_sub64_ri(XE_SP, stack_size);

		stack_offset = SHADOW_SIZE;
		for (i = 0; i < iREGCNT_XMM; i++)
		{
			if (fpr_bitmask & (1u << i) && XE_XMM_CALLER_SAVED(i) && (!is_load || !is_xmm || data_register != i))
			{
				{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, stack_offset); xe_movaps_memxg(xm, i); }
				stack_offset += XMM_SIZE;
			}
		}

		for (i = 0; i < iREGCNT_GPR; i++)
		{
			if ((gpr_bitmask & (1u << i)) && (i == arg1id || i == arg2id || i == arg3id || XE_IS_CALLER_SAVED(i)) && (!is_load || is_xmm || data_register != i))
			{
				{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, stack_offset); xe_mov64_memgr(xm, i); }
				stack_offset += GPR_SIZE;
			}
		}
	}

	if (is_load)
	{
		DynGen_PrepRegs(address_register, -1, size_in_bits, is_xmm);
		DynGen_HandlerTest(DYNGEN_DIRECT_READ, size_in_bits, is_signed, 0, size_in_bits, is_signed && size_in_bits <= 32);

		if (size_in_bits == 128)
		{
			if (data_register != 0 /* xmm0 */)
				xe_movaps_xx(data_register, 0);
		}
		else
		{
			if (is_xmm)
			{
				xe_movq_xr(data_register, XE_AX);
			}
			else
			{
				if (data_register != 0 /* rax */)
					xe_mov64_rr(data_register, XE_AX);
			}
		}
	}
	else
	{
		if (address_register != XE_ARG1)
			xe_mov32_rr(XE_ARG1, address_register);

		if (size_in_bits == 128)
		{
#if !PCSX2_MINGW_R128_BY_PTR
			const int argreg = XE_XMM_ARG(1, 0);
			if (data_register != argreg)
				xe_movaps_xx(argreg, data_register);
#endif
		}
		else
		{
			if (is_xmm)
			{
				xe_movq_rx(XE_ARG2, data_register);
			}
			else
			{
				if (data_register != XE_ARG2)
					xe_mov64_rr(XE_ARG2, data_register);
			}
		}

		DynGen_PrepRegs(address_register, data_register, size_in_bits, is_xmm);
		DynGen_HandlerTest(DYNGEN_DIRECT_WRITE, size_in_bits, 0, 1, size_in_bits, 0);
	}

	/* restore regs */
	if (stack_size > 0)
	{
		u32 stack_offset = SHADOW_SIZE;
		u32 i;
		for (i = 0; i < iREGCNT_XMM; i++)
		{
			if (fpr_bitmask & (1u << i) && XE_XMM_CALLER_SAVED(i) && (!is_load || !is_xmm || data_register != i))
			{
				{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, stack_offset); xe_movaps_xmemg(i, xm); }
				stack_offset += XMM_SIZE;
			}
		}

		for (i = 0; i < iREGCNT_GPR; i++)
		{
			if ((gpr_bitmask & (1u << i)) && (i == arg1id || i == arg2id || i == arg3id || XE_IS_CALLER_SAVED(i)) && (!is_load || is_xmm || data_register != i))
			{
				{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, stack_offset); xe_mov64_rmemg(i, xm); }
				stack_offset += GPR_SIZE;
			}
		}

		xe_add64_ri(XE_SP, stack_size);
	}

	xe_jmp_to((void*)(code_address + code_size));

	recEndThunk();

	/* backpatch to a jump to the slowmem handler */
	x86Ptr = (u8*)((u8*)code_address);
	xe_jmp_to(thunk);

	/* fill the rest of it with nops, if any */
	for (i = (u32)((uptr)x86Ptr - code_address); i < code_size; i++)
		xe_nop();
}
