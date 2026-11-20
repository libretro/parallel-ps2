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

#include "../../GSState.h"
#ifdef ARCH_X86 /* the code generators; see ENABLE_JIT_RASTERIZER */
#include "GSSetupPrimCodeGenerator.h"
#include "GSDrawScanlineCodeGenerator.h"
#endif

struct GSScanlineLocalData;

#include "GSRaster.h"

MULTI_ISA_UNSHARED_START

class GSRasterizerData;

#ifdef ARCH_X86
class GSSetupPrimCodeGenerator;
class GSDrawScanlineCodeGenerator;
#endif

class GSDrawScanline : public GSVirtualAlignedClass<32>
{
#ifdef ARCH_X86
	friend GSSetupPrimCodeGenerator;
	friend GSDrawScanlineCodeGenerator;
#endif

public:
	GSDrawScanline();
	~GSDrawScanline() override;

	/// Function pointer types which we call back into.
	using SetupPrimPtr = gs_raster_setup_prim_fn;
	using DrawScanlinePtr = gs_raster_scanline_fn;

	/// Flushes the code cache, forcing everything to be recompiled.
	void ResetCodeCache();

	/// Populates function pointers. If this returns false, we ran out of code space.
	bool SetupDraw(GSRasterizerData& data);

	/// Draw pre-calculations, computed per-thread.
	static void BeginDraw(const GSRasterizerData& data, GSScanlineLocalData& local);


private:
#ifdef ARCH_X86
	/* (made at start for more functions than a game makes: random
	 * selectors average 0.3 to 0.7 KiB of setup and 1.1 to 1.3 KiB of
	 * scanline, so the 64 MiB code reserve fills first) */
	GSCodeGeneratorFunctionMap<GSSetupPrimCodeGenerator, u64, SetupPrimPtr, 2048> m_sp_map;
	GSCodeGeneratorFunctionMap<GSDrawScanlineCodeGenerator, u64, DrawScanlinePtr, 8192> m_ds_map;
#else
	/* The C++ rasterizer, where no code is generated. */
	static void CSetupPrim(const GSVertexSW* vertex, const u16* index, const GSVertexSW* dscan, GSScanlineLocalData* local);
	static void CDrawScanline(int pixels, int left, int top, const GSVertexSW* scan, GSScanlineLocalData* local);
	static void CDrawEdge(int pixels, int left, int top, const GSVertexSW* scan, GSScanlineLocalData* local);
	__ri static void CDrawScanline(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData& local, GSScanlineSelector sel);
#endif
};

MULTI_ISA_UNSHARED_END
