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

#include "GSRegs.h"
#include "GSTables.h"
#include "GSVector.h"
#include "MultiISA.h"
#include "gs_block.h"

MULTI_ISA_UNSHARED_START

class GSBlock
{
public:
	/* The GS block swizzle (gs_block.h), for the C++ callers */
	template <int i, int alignment, u32 mask>
	__forceinline static void WriteColumn32(u8* RESTRICT dst, const u8* RESTRICT src, int srcpitch) { gs_block_write_column32(i, alignment, mask, dst, src, srcpitch); }
	template <int i, int alignment>
	__forceinline static void WriteColumn16(u8* RESTRICT dst, const u8* RESTRICT src, int srcpitch) { gs_block_write_column16(i, alignment, dst, src, srcpitch); }
	template <int i, int alignment>
	__forceinline static void WriteColumn8(u8* RESTRICT dst, const u8* RESTRICT src, int srcpitch) { gs_block_write_column8(i, alignment, dst, src, srcpitch); }
	template <int i, int alignment>
	__forceinline static void WriteColumn4(u8* RESTRICT dst, const u8* RESTRICT src, int srcpitch) { gs_block_write_column4(i, alignment, dst, src, srcpitch); }
	template <int alignment, u32 mask>
	static void WriteColumn32(int y, u8* RESTRICT dst, const u8* RESTRICT src, int srcpitch) { gs_block_write_column32_y(y, alignment, mask, dst, src, srcpitch); }
	template <int alignment>
	static void WriteColumn16(int y, u8* RESTRICT dst, const u8* RESTRICT src, int srcpitch) { gs_block_write_column16_y(y, alignment, dst, src, srcpitch); }
	template <int alignment>
	static void WriteColumn8(int y, u8* RESTRICT dst, const u8* RESTRICT src, int srcpitch) { gs_block_write_column8_y(y, alignment, dst, src, srcpitch); }
	template <int alignment>
	static void WriteColumn4(int y, u8* RESTRICT dst, const u8* RESTRICT src, int srcpitch) { gs_block_write_column4_y(y, alignment, dst, src, srcpitch); }
	template <int alignment, u32 mask>
	static void WriteBlock32(u8* RESTRICT dst, const u8* RESTRICT src, int srcpitch) { gs_block_write_block32(alignment, mask, dst, src, srcpitch); }
	template <int alignment>
	static void WriteBlock16(u8* RESTRICT dst, const u8* RESTRICT src, int srcpitch) { gs_block_write_block16(alignment, dst, src, srcpitch); }
	template <int alignment>
	static void WriteBlock8(u8* RESTRICT dst, const u8* RESTRICT src, int srcpitch) { gs_block_write_block8(alignment, dst, src, srcpitch); }
	template <int alignment>
	static void WriteBlock4(u8* RESTRICT dst, const u8* RESTRICT src, int srcpitch) { gs_block_write_block4(alignment, dst, src, srcpitch); }
	template <int i>
	__forceinline static void ReadColumn32(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_column32(i, src, dst, dstpitch); }
	template <int i>
	__forceinline static void ReadColumn16(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_column16(i, src, dst, dstpitch); }
	template <int i>
	__forceinline static void ReadColumn8(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_column8(i, src, dst, dstpitch); }
	template <int i>
	__forceinline static void ReadColumn4(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_column4(i, src, dst, dstpitch); }
	static void ReadColumn32(int y, const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_column32_y(y, src, dst, dstpitch); }
	static void ReadColumn16(int y, const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_column16_y(y, src, dst, dstpitch); }
	static void ReadColumn8(int y, const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_column8_y(y, src, dst, dstpitch); }
	static void ReadColumn4(int y, const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_column4_y(y, src, dst, dstpitch); }
	static void ReadBlock32(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_block32(src, dst, dstpitch); }
	static void ReadBlock16(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_block16(src, dst, dstpitch); }
	static void ReadBlock8(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_block8(src, dst, dstpitch); }
	static void ReadBlock4(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_block4(src, dst, dstpitch); }
	__forceinline static void ReadBlock4P(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_block4p(src, dst, dstpitch); }
	__forceinline static void ReadBlock8HP(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_block8hp(src, dst, dstpitch); }
	__forceinline static void ReadBlock4HLP(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_block4hlp(src, dst, dstpitch); }
	__forceinline static void ReadBlock4HHP(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch) { gs_block_read_block4hhp(src, dst, dstpitch); }

	/* Expanding, unpacking, and reading to 32-bit colours */
	template <bool AEM>
	static void ExpandBlock24(const u32* RESTRICT src, u8* RESTRICT dst, int dstpitch, const GIFRegTEXA& TEXA) { gs_block_expand_block24(AEM, src, dst, dstpitch, TEXA.TA0); }
	/* (not inlined: too many registers) */
	template <bool AEM>
	static void ExpandBlock16(const u16* RESTRICT src, u8* RESTRICT dst, int dstpitch, const GIFRegTEXA& TEXA) { gs_block_expand_block16(AEM, src, dst, dstpitch, TEXA.TA0, TEXA.TA1); }
	__forceinline static void ExpandBlock8_32(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u32* RESTRICT pal) { gs_block_expand_block8_32(src, dst, dstpitch, pal); }
	__forceinline static void ExpandBlock8_16(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u32* RESTRICT pal) { gs_block_expand_block8_16(src, dst, dstpitch, pal); }
	__forceinline static void ExpandBlock4_32(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u64* RESTRICT pal) { gs_block_expand_block4_32(src, dst, dstpitch, pal); }
	__forceinline static void ExpandBlock4_16(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u64* RESTRICT pal) { gs_block_expand_block4_16(src, dst, dstpitch, pal); }
	__forceinline static void ExpandBlock8H_32(u32* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u32* RESTRICT pal) { gs_block_expand_block8h_32(src, dst, dstpitch, pal); }
	__forceinline static void ExpandBlock8H_16(u32* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u32* RESTRICT pal) { gs_block_expand_block8h_16(src, dst, dstpitch, pal); }
	__forceinline static void ExpandBlock4HL_32(u32* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u32* RESTRICT pal) { gs_block_expand_block4hl_32(src, dst, dstpitch, pal); }
	__forceinline static void ExpandBlock4HL_16(u32* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u32* RESTRICT pal) { gs_block_expand_block4hl_16(src, dst, dstpitch, pal); }
	__forceinline static void ExpandBlock4HH_32(u32* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u32* RESTRICT pal) { gs_block_expand_block4hh_32(src, dst, dstpitch, pal); }
	__forceinline static void ExpandBlock4HH_16(u32* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u32* RESTRICT pal) { gs_block_expand_block4hh_16(src, dst, dstpitch, pal); }
	__forceinline static void UnpackAndWriteBlock24(const u8* RESTRICT src, int srcpitch, u8* RESTRICT dst) { gs_block_unpack_and_write_block24(src, srcpitch, dst); }
	__forceinline static void UnpackAndWriteBlock8H(const u8* RESTRICT src, int srcpitch, u8* RESTRICT dst) { gs_block_unpack_and_write_block8h(src, srcpitch, dst); }
	__forceinline static void UnpackAndWriteBlock4HL(const u8* RESTRICT src, int srcpitch, u8* RESTRICT dst) { gs_block_unpack_and_write_block4hl(src, srcpitch, dst); }
	__forceinline static void UnpackAndWriteBlock4HH(const u8* RESTRICT src, int srcpitch, u8* RESTRICT dst) { gs_block_unpack_and_write_block4hh(src, srcpitch, dst); }
	template <bool AEM>
	__forceinline static void ReadAndExpandBlock24(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch, const GIFRegTEXA& TEXA) { gs_block_read_and_expand_block24(AEM, src, dst, dstpitch, TEXA.TA0); }
	template <bool AEM>
	__forceinline static void ReadAndExpandBlock16(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch, const GIFRegTEXA& TEXA) { gs_block_read_and_expand_block16(AEM, src, dst, dstpitch, TEXA.TA0, TEXA.TA1); }
	__forceinline static void ReadAndExpandBlock8_32HSW(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u32* RESTRICT pal) { gs_block_read_and_expand_block8_32hsw(src, dst, dstpitch, pal); }
	__forceinline static void ReadAndExpandBlock8_32(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u32* RESTRICT pal) { gs_block_read_and_expand_block8_32(src, dst, dstpitch, pal); }
	__forceinline static void ReadAndExpandBlock4_32(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u32* RESTRICT pal) { gs_block_read_and_expand_block4_32(src, dst, dstpitch, pal); }
	__forceinline static void ReadAndExpandBlock8H_32HSW(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u32* RESTRICT pal) { gs_block_read_and_expand_block8h_32hsw(src, dst, dstpitch, pal); }
	__forceinline static void ReadAndExpandBlock8H_32(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u32* RESTRICT pal) { gs_block_read_and_expand_block8h_32(src, dst, dstpitch, pal); }
	__forceinline static void ReadAndExpandBlock4HL_32(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u32* RESTRICT pal) { gs_block_read_and_expand_block4hl_32(src, dst, dstpitch, pal); }
	__forceinline static void ReadAndExpandBlock4HH_32(const u8* RESTRICT src, u8* RESTRICT dst, int dstpitch, const u32* RESTRICT pal) { gs_block_read_and_expand_block4hh_32(src, dst, dstpitch, pal); }
};

MULTI_ISA_UNSHARED_END
