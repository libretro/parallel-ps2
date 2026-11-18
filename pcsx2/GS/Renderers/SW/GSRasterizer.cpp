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

// TODO: JIT Draw* (flags: depth, texture, color (+iip), scissor)

#include "GSRasterizer.h"
#include <stddef.h>
#include <memalign.h>
#include "GSDrawScanline.h"
#include "../../GSExtra.h"


#include "../../../VMManager.h"

MULTI_ISA_UNSHARED_IMPL;

static int compute_best_thread_height(int threads)
{
	// - for more threads screen segments should be smaller to better distribute the pixels
	// - but not too small to keep the threading overhead low
	// - ideal value between 3 and 5, or log2(64 / number of threads)

	int th = GSConfig.SWExtraThreadsHeight;

	if (th > 0 && th < 9)
		return th;
	return 4;
}

/* (GSRaster.c's view of the vertices) */
static_assert(sizeof(GSVertexSW) == 4 * sizeof(gs_raster_lane) && alignof(GSVertexSW) == 32, "GSVertexSW layout");
static_assert(offsetof(GSVertexSW, p) == 0 && offsetof(GSVertexSW, _pad) == 16 && offsetof(GSVertexSW, t) == 32
	&& offsetof(GSVertexSW, c) == 48, "GSVertexSW layout");

static void DrawRect(const gs_recti* r, const GSVertexSW* v, GSScanlineLocalData* local)
{
	GSDrawScanline::DrawRect(GSVector4i(r->x, r->y, r->z, r->w), *v, *local);
}

GSRasterizer::GSRasterizer(int id, int threads)
{
	if (!gs_raster_init(&m_r, id, threads, compute_best_thread_height(threads)))
		abort();
	m_r.local     = &m_local;
	m_r.tgrid     = m_local.tgrid.F32;
	m_r.tgrid_rcp = m_local.tgrid_rcp.F32;
	m_r.zgrid     = &m_local.zgrid.grid;
	m_r.draw_rect = DrawRect;
}

GSRasterizer::~GSRasterizer()
{
	gs_raster_free(&m_r);
}

void GSRasterizer::Draw(GSRasterizerData& data, bool whole)
{
	gs_raster_draw d;
	GSScanlineSelector sel;

	if ((data.vertex && data.vertex_count == 0) || (data.index && data.index_count == 0))
		return;

	GSDrawScanline::BeginDraw(data, m_local);
	sel = data.global.sel;

	d.scissor       = {data.scissor.x, data.scissor.y, data.scissor.z, data.scissor.w};
	d.bbox          = {data.bbox.x, data.bbox.y, data.bbox.z, data.bbox.w};
	d.primclass     = data.primclass;
	d.vertex        = data.vertex;
	d.vertex_count  = data.vertex_count;
	d.index         = data.index;
	d.index_count   = data.index_count;
	d.scanmsk_value = data.scanmsk_value;
	d.setup_prim    = data.setup_prim;
	d.draw_scanline = data.draw_scanline;
	d.draw_edge     = data.draw_edge;
	d.sel           = ((sel.fb && sel.tfx != TFX_NONE) ? GS_RASTER_SEL_TEX_GRID : 0)
	                | (sel.fst ? GS_RASTER_SEL_FST : 0)
	                | ((sel.zb && sel.prim != GS_SPRITE_CLASS && !sel.zequal) ? GS_RASTER_SEL_Z_GRID : 0)
	                | (sel.prim == GS_TRIANGLE_CLASS ? GS_RASTER_SEL_TRIANGLE : 0)
	                | (sel.hires ? GS_RASTER_SEL_HIRES : 0)
	                | (sel.IsSolidRect() ? GS_RASTER_SEL_SOLID_RECT : 0);

	if (whole)
		gs_raster_draw_whole(&m_r, &d);
	else
		gs_raster_draw_bands(&m_r, &d);

#if _M_SSE >= 0x501
	_mm256_zeroupper();
#endif
}

//

/* (the list's view of the draws and the rasterizers) */
static void ListDraw(void* r, void* job)
{
	static_cast<GSRasterizer*>(r)->Draw(*static_cast<GSRasterizerData*>(job));
}

static void ListDrawWhole(void* r, void* job)
{
	static_cast<GSRasterizer*>(r)->DrawWhole(*static_cast<GSRasterizerData*>(job));
}

static void ListRetain(void* job)
{
	GSRingHeap::SharedPtr<GSRasterizerData>::Retain(static_cast<GSRasterizerData*>(job));
}

static void ListRelease(void* job)
{
	GSRingHeap::SharedPtr<GSRasterizerData>::Release(static_cast<GSRasterizerData*>(job));
}

static const gs_raster_list_ops s_list_ops = {ListDraw, ListDrawWhole, ListRetain, ListRelease};

GSRasterizerList::~GSRasterizerList()
{
	/* The threads first: each draws on its rasterizer. */
	gs_raster_list_deinit(&m_list);
	for (int i = m_count; i-- > 0;)
		delete m_r[i];
	free(m_r);
}

bool GSRasterizerList::SetupDraw(GSRasterizerData& data)
{
	/* Out of room for code: start the reserve again, once the threads are
	 * done with what is in it; a draw whose functions still cannot be
	 * made is dropped. */
	if (likely(m_ds.SetupDraw(data)))
		return true;
	Sync();
	m_ds.ResetCodeCache();
	return m_ds.SetupDraw(data);
}

void GSRasterizerList::Draw(GSRasterizerData& data)
{
	if (SetupDraw(data))
		m_r[0]->Draw(data);
}

void GSRasterizerList::Queue(const GSRingHeap::SharedPtr<GSRasterizerData>& data)
{
	gs_recti r = {data->bbox.x, data->bbox.y, data->bbox.z, data->bbox.w};
	const gs_recti scissor = {data->scissor.x, data->scissor.y, data->scissor.z, data->scissor.w};

	if (!SetupDraw(*data.get()))
		return;

	gs_recti_clamp_into(&r, &scissor);
	gs_raster_list_queue(&m_list, data.get(), &r, data->whole);
}

void GSRasterizerList::Sync()
{
	gs_raster_list_sync(&m_list);
}

bool GSRasterizerList::IsSynced()
{
	return gs_raster_list_is_synced(&m_list);
}

GSRasterizerList* GSRasterizerList::Create(int threads)
{
	GSRasterizerList* rl = new GSRasterizerList();
	const bool affinity_on = EmuConfig.Cpu.AffinityControlMode != 0;
	const u32 worker_proc_base = THREAD_VU1 ? 3 : 2;
	u32 proc_count;
	const u32* procs = vm_get_sorted_processor_list(&proc_count);
	u64* affinity;
	int i;

	threads    = pcsx2_max_i(threads, 0);
	rl->m_count = pcsx2_max_i(threads, 1);
	rl->m_r    = static_cast<GSRasterizer**>(calloc(static_cast<size_t>(rl->m_count), sizeof(GSRasterizer*)));
	affinity   = static_cast<u64*>(calloc(static_cast<size_t>(rl->m_count), sizeof(u64)));
	if (!rl->m_r || !affinity)
		abort();

	/* (the masks worked out here, on the spawning thread: reading the
	 * settings on a fresh worker raced their being written) */
	for (i = 0; i < rl->m_count; i++)
	{
		rl->m_r[i] = new GSRasterizer(threads ? i : 0, pcsx2_max_i(threads, 1));
		if (affinity_on && threads && (worker_proc_base + i) < proc_count)
			affinity[i] = static_cast<u64>(1) << procs[worker_proc_base + i];
	}

	if (!gs_raster_list_init(&rl->m_list, threads, compute_best_thread_height(threads),
			reinterpret_cast<void* const*>(rl->m_r), affinity, &s_list_ops))
		abort();
	free(affinity);
	return rl;
}
