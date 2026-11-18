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

GSSingleRasterizer::GSSingleRasterizer()
	: m_r(0, 1)
{
}

GSSingleRasterizer::~GSSingleRasterizer() = default;

void GSSingleRasterizer::Queue(const GSRingHeap::SharedPtr<GSRasterizerData>& data)
{
	Draw(*data.get());
}

void GSSingleRasterizer::Draw(GSRasterizerData& data)
{
	/* Out of room for code: start the reserve again; a draw whose
	 * functions still cannot be made is dropped. */
	if (!unlikely(m_ds.SetupDraw(data)))
	{
		m_ds.ResetCodeCache();
		if (!m_ds.SetupDraw(data))
			return;
	}

	m_r.Draw(data);
}

void GSSingleRasterizer::Sync()
{
}

bool GSSingleRasterizer::IsSynced() const
{
	return true;
}

GSRasterizerList::GSRasterizerList(int threads)
{
	m_r         = static_cast<GSRasterizer**>(calloc(static_cast<size_t>(threads), sizeof(GSRasterizer*)));
	m_workers   = static_cast<GSWorker**>(calloc(static_cast<size_t>(threads), sizeof(GSWorker*)));
	m_worker_ctx = static_cast<Worker*>(calloc(static_cast<size_t>(threads), sizeof(Worker)));
	m_threads   = static_cast<u32>(threads);
	if (!m_r || !m_workers || !m_worker_ctx)
		abort();
	m_thread_height = compute_best_thread_height(threads);

	const int rows = (4096 >> m_thread_height) + 16;
	m_scanline = static_cast<u8*>(memalign_alloc(64, rows));

	for (int i = 0; i < rows; i++)
		m_scanline[i] = static_cast<u8>(i % threads);
}

GSRasterizerList::~GSRasterizerList()
{
	for (u32 i = m_threads; i-- > 0;)
		delete m_workers[i];
	for (u32 i = m_threads; i-- > 0;)
		delete m_r[i];
	free(m_workers);
	free(m_r);
	free(m_worker_ctx);
	memalign_free(m_scanline);
}

void GSRasterizerList::OnWorkerStartup(void* context)
{
	const u64 affinity = static_cast<const Worker*>(context)->affinity;
	/* The affinity mask is computed on the spawning thread at Create()
	 * and captured by value: reading EmuConfig (AffinityControlMode,
	 * THREAD_VU1) from inside the freshly started worker raced
	 * vm_apply_settings' whole-struct assignment on the CPU
	 * thread (TSan).  Create() runs in the GS-open path, which shares
	 * the ordering discipline of every other GSopen-time config read. */
	if (affinity)
	{
		sthread_set_current_affinity(affinity);
	}
}

void GSRasterizerList::OnWorkerDraw(void* context, GSRingHeap::SharedPtr<GSRasterizerData>& item)
{
	static_cast<Worker*>(context)->r->Draw(*item.get());
}

void GSRasterizerList::Queue(const GSRingHeap::SharedPtr<GSRasterizerData>& data)
{
	GSVector4i r = data->bbox.rintersect(data->scissor);

	/* As in GSSingleRasterizer::Draw. */
	if (unlikely(!m_ds.SetupDraw(*data.get())))
	{
		Sync();
		m_ds.ResetCodeCache();
		if (!m_ds.SetupDraw(*data.get()))
			return;
	}

	/* After every queued draw, on this thread, before the next is queued:
	 * the draws before and after it keep their order on every row. */
	if (data->whole)
	{
		Sync();
		m_r[0]->DrawWhole(*data.get());
		return;
	}

	int top = r.top >> m_thread_height;
	int bottom = pcsx2_min_i((r.bottom + (1 << m_thread_height) - 1) >> m_thread_height, top + static_cast<int>(m_threads));

	while (top < bottom)
	{
		m_workers[m_scanline[top++]]->Push(data);
	}
}

void GSRasterizerList::Sync()
{
	if (!IsSynced())
	{
		for (u32 i = 0; i < m_threads; i++)
			m_workers[i]->Wait();
	}
}

bool GSRasterizerList::IsSynced() const
{
	for (u32 i = 0; i < m_threads; i++)
	{
		if (!m_workers[i]->IsEmpty())
		{
			return false;
		}
	}

	return true;
}

IRasterizer* GSRasterizerList::Create(int threads)
{
	threads = pcsx2_max_i(threads, 0);

	if (threads == 0)
		return new GSSingleRasterizer();

	GSRasterizerList* rl = new GSRasterizerList(threads);

	const bool affinity_on = EmuConfig.Cpu.AffinityControlMode != 0;
	u32 proc_count;
	const u32* procs = vm_get_sorted_processor_list(&proc_count);
	const u32 worker_proc_base = THREAD_VU1 ? 3 : 2;
	for (int i = 0; i < threads; i++)
	{
		u64 affinity = 0;
		if (affinity_on && (worker_proc_base + i) < proc_count)
			affinity = static_cast<u64>(1) << procs[worker_proc_base + i];
		rl->m_r[i] = new GSRasterizer(i, threads);
		rl->m_worker_ctx[i].r        = rl->m_r[i];
		rl->m_worker_ctx[i].affinity = affinity;
		rl->m_workers[i] = new GSWorker(GSRasterizerList::OnWorkerStartup, GSRasterizerList::OnWorkerDraw, nullptr, &rl->m_worker_ctx[i]);
	}

	return rl;
}
