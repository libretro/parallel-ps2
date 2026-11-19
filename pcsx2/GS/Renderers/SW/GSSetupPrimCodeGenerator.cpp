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

#include "GSSetupPrimCodeGenerator.h"
#include "GSSwJitLayout.h"

MULTI_ISA_UNSHARED_IMPL;

GSSetupPrimCodeGenerator::GSSetupPrimCodeGenerator(u64 key, void* code, size_t maxsize)
	: m_code(code)
{
	gs_swj_target t;
	t.key        = key;
	t.avx        = CPU_HAS_AVX;
	t.ymm        = _M_SSE >= 0x501;
	t.win64      = GS_SWJ_WIN64;
	t.shift_128b = g_const.m_shift_128b;
	t.shift_256b = g_const.m_shift_256b;
	m_size = gs_swj_setup_prim(&t, code, maxsize);
}
