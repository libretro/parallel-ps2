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
#include "common/Pcsx2Defs.h"

#include "GSVertexSW.h"
#include "GSDrawScanline.h"
#include "GSRaster.h"
#include "GSRasterList.h"
#include "../../GSAlignedClass.h"
#include "../../GSRingHeap.h"
#include "../../MultiISA.h"

MULTI_ISA_UNSHARED_START

class GSDrawScanline;

class alignas(32) GSRasterizerData : public GSAlignedClass<32>
{
public:
	GSVector4i scissor;
	GSVector4i bbox;
	GS_PRIM_CLASS primclass;
	u8* buff;
	GSVertexSW* vertex;
	int vertex_count;
	u16* index;
	int index_count;
	u8 scanmsk_value;
	/* Drawn by one thread, every row, in order, the others idle: a draw
	 * whose frame or depth buffer wraps local memory, or which writes
	 * one over the other, has rows far apart at one address, which the
	 * threads' bands would write in no set order (GSRendererSW). */
	bool whole;

	GSScanlineGlobalData global;

	GSDrawScanline::SetupPrimPtr setup_prim;
	GSDrawScanline::DrawScanlinePtr draw_scanline;
	GSDrawScanline::DrawScanlinePtr draw_edge;

	GSRasterizerData()
		: scissor(GSVector4i::zero())
		, bbox(GSVector4i::zero())
		, primclass(GS_INVALID_CLASS)
		, buff(nullptr)
		, vertex(NULL)
		, vertex_count(0)
		, index(NULL)
		, index_count(0)
		, scanmsk_value(0)
		, whole(false)
	{
	}

	virtual ~GSRasterizerData()
	{
		if (buff != NULL)
			GSRingHeap::free(buff);
	}
};

/* A drawing thread's rasterizer (GSRaster.c), with the scanline code's
 * data for it */
class alignas(32) GSRasterizer final : public GSVirtualAlignedClass<32>
{
protected:
	gs_raster m_r;
	gs_raster_fill m_fill = {};
	GSScanlineLocalData m_local = {};

	void Draw(GSRasterizerData& data, bool whole);

public:
	GSRasterizer(int id, int threads);
	~GSRasterizer();

	void Draw(GSRasterizerData& data) { Draw(data, false); }
	/* Every row of it, on the calling thread, while no other thread
	 * draws (GSRasterizerData::whole). */
	void DrawWhole(GSRasterizerData& data) { Draw(data, true); }
};

/* The drawing threads (GSRasterList.c), each with its rasterizer, and
 * the scanline code they draw with; without threads, one rasterizer the
 * caller draws on */
class GSRasterizerList final : public GSVirtualAlignedClass<32>
{
protected:
	GSDrawScanline m_ds;
	GSRasterizer** m_r = nullptr;
	int m_count = 0;
	gs_raster_list m_list = {};

	GSRasterizerList() = default;
	bool SetupDraw(GSRasterizerData& data);

public:
	~GSRasterizerList() override;

	/* A rasterizer on threads threads (none: on the caller); the caller
	 * deletes it. */
	static GSRasterizerList* Create(int threads);

	void Queue(const GSRingHeap::SharedPtr<GSRasterizerData>& data);
	/* Now, on the caller: of one without threads */
	void Draw(GSRasterizerData& data);
	void Sync();
	bool IsSynced();
};

MULTI_ISA_UNSHARED_END
