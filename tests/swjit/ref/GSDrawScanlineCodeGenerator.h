/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#include "GS/Renderers/SW/GSScanlineEnvironment.h"
#include "GS/Renderers/Common/GSFunctionMap.h"
#include "GS/GSUtil.h"
#include "GS/MultiISA.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif
#if defined(__GNUC__) && !defined(__clang__)
// GCC's -Warray-bounds flags the isMEM()-guarded Operand -> Address downcast
// in opModRM/opModM when a register temporary is passed by const Operand&
// (e.g. test(reg, reg)): the speculative Address::mode_ access is reported
// against the smaller Reg object even though the branch is dead for
// registers. False positive; suppress for this third-party header only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
#ifndef XBYAK_ENABLE_OMITTED_OPERAND
#define XBYAK_ENABLE_OMITTED_OPERAND /* as the core built it */
#endif
#include <xbyak/xbyak.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#if defined(_M_AMD64) || defined(_WIN64)
#define RegLong Xbyak::Reg64
#else
#define RegLong Xbyak::Reg32
#endif

MULTI_ISA_UNSHARED_START

class GSDrawScanlineCodeGenerator : public Xbyak::CodeGenerator
{
	void operator=(const GSDrawScanlineCodeGenerator&);

	GSScanlineSelector m_sel;

public:
	GSDrawScanlineCodeGenerator(u64 key, void* code, size_t maxsize);
};

MULTI_ISA_UNSHARED_END
