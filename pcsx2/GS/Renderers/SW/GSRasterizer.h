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

#include <retro_atomic.h>
#include <new>
#include <retro_spsc.h>
#include "common/General.h"
#include "WorkEventCount.h"


#include "GSVertexSW.h"
#include "GSDrawScanline.h"
#include "GSRaster.h"
#include "../../GSAlignedClass.h"
#include "../../GSRingHeap.h"
#include "../../MultiISA.h"
#include <rthreads/rthreads.h>

/* SPSC job queue over retro_spsc, carrying real C++ objects: Push
 * placement-constructs the item into the ring's bytes, and the worker
 * runs it and destroys it in place.  The object never moves once
 * constructed - no bitwise relocation, no requirements on T beyond a
 * copy constructor - so a refcounted handle like GSRingHeap::SharedPtr
 * rides through with its count held by the in-ring copy.  The
 * write_end release / read_begin acquire pair orders construction
 * before the worker touches the object, exactly as it orders plain
 * bytes.
 *
 * Records are sizeof(T) bytes: capacity is a whole number of records,
 * so no record straddles the wrap, and every offset is a multiple of
 * sizeof(T) into a malloc'd base - which is where the alignment
 * static_assert below comes from. */
template <class T, int CAPACITY>
class GSJobQueue final
{
	static_assert((CAPACITY & (CAPACITY - 1)) == 0, "capacity must be a power of two");
	static_assert(alignof(T) <= sizeof(T) && (sizeof(T) & (alignof(T) - 1)) == 0,
		"record offsets are sizeof(T) multiples and must land T-aligned");

private:
	sthread_t* m_thread;
	static void ThreadEntry(void* self) { static_cast<GSJobQueue*>(self)->ThreadProc(); }
	/* The worker's callbacks, with the context they are given. */
	void (*m_startup)(void* context);
	void (*m_func)(void* context, T& item);
	void (*m_shutdown)(void* context);
	void* m_context;
	retro_atomic_int_t m_exit;
	retro_spsc_t m_queue;
	bool m_queue_ok;

	WorkEventCount m_sema;

	void ThreadProc()
	{
		if (m_startup)
			m_startup(m_context);

		for (;;)
		{
			work_eventcount_wait(&m_sema);
			if (retro_atomic_load_acquire_int(&m_exit))
				break;
			/* Span drain, batch commit: the object is destroyed
			 * before its record is released, so the producer can
			 * never reuse bytes that still hold a live item.  The
			 * spans loop until the queue reports empty so a wrap
			 * split can't strand records behind a sema park. */
			size_t span;
			const void* span_ptr;
			while (m_queue_ok && (span = retro_spsc_read_begin(&m_queue, &span_ptr)) >= sizeof(T))
			{
				size_t consumed = 0;
				while (consumed < span)
				{
					/* The bytes are ours between read_begin and
					 * read_end; const_cast because retro_spsc's
					 * read side hands out const. */
					T* item = reinterpret_cast<T*>(const_cast<u8*>(
						static_cast<const u8*>(span_ptr) + consumed));
					m_func(m_context, *item);
					item->~T();
					consumed += sizeof(T);
				}
				retro_spsc_read_end(&m_queue, consumed);
			}
		}

		if (m_shutdown)
			m_shutdown(m_context);
	}

public:
	GSJobQueue(void (*startup)(void*), void (*func)(void*, T&), void (*shutdown)(void*), void* context)
		: m_startup(startup)
		, m_func(func)
		, m_shutdown(shutdown)
		, m_context(context)
		, m_exit(RETRO_ATOMIC_INT_INITIALIZER(0))
	{
		/* Fed a job a draw by the GS thread and drained faster than
		 * they come, the worker parks many times a frame: the
		 * symmetric pair, where a park is a kernel wait and no more. */
		work_eventcount_init_symmetric(&m_sema);
		m_queue_ok = retro_spsc_init(&m_queue, (size_t)CAPACITY * sizeof(T));
		if (!m_queue_ok)
			log_cb(RETRO_LOG_ERROR, "GSJobQueue: ring allocation failed; jobs will run on the calling thread\n");
		m_thread = sthread_create(ThreadEntry, this);
	}

	~GSJobQueue()
	{
		retro_atomic_store_release_int(&m_exit, 1);
		work_eventcount_notify(&m_sema);
		sthread_join(m_thread);
		m_thread = NULL;
		if (m_queue_ok)
		{
			/* The worker is gone; anything still buffered is
			 * destroyed without being run, which is what the old
			 * ringbuffer's destructor did with leftover items. */
			size_t span;
			const void* span_ptr;
			while ((span = retro_spsc_read_begin(&m_queue, &span_ptr)) >= sizeof(T))
			{
				size_t consumed = 0;
				while (consumed < span)
				{
					reinterpret_cast<T*>(const_cast<u8*>(
						static_cast<const u8*>(span_ptr) + consumed))->~T();
					consumed += sizeof(T);
				}
				retro_spsc_read_end(&m_queue, consumed);
			}
			retro_spsc_free(&m_queue);
		}
		work_eventcount_free(&m_sema);
	}

	bool IsEmpty()
	{
		return !m_queue_ok || retro_spsc_read_avail(&m_queue) == 0;
	}

	void Push(const T& item)
	{
		void* dst;
		if (!m_queue_ok)
		{
			/* Degraded mode for an impossible 512K allocation
			 * failure: run the job synchronously rather than
			 * dropping a draw. */
			T local(item);
			m_func(m_context, local);
			return;
		}
		while (retro_spsc_write_begin(&m_queue, &dst) < sizeof(T))
			sthread_yield();
		new (dst) T(item);
		retro_spsc_write_end(&m_queue, sizeof(T));
		work_eventcount_notify(&m_sema);
	}

	void Wait()
	{
		work_eventcount_wait_empty(&m_sema);
	}

	void operator()(T& item)
	{
		m_func(item);
	}
};

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

class IRasterizer : public GSVirtualAlignedClass<32>
{
public:
	virtual ~IRasterizer() {}

	virtual void Queue(const GSRingHeap::SharedPtr<GSRasterizerData>& data) = 0;
	virtual void Sync() = 0;
	virtual bool IsSynced() const = 0;
};

class GSSingleRasterizer final : public IRasterizer
{
public:
	GSSingleRasterizer();
	~GSSingleRasterizer() override;

	void Queue(const GSRingHeap::SharedPtr<GSRasterizerData>& data) override;
	void Sync() override;
	bool IsSynced() const override;

	void Draw(GSRasterizerData& data);

private:
	GSDrawScanline m_ds;
	GSRasterizer m_r;
};

class GSRasterizerList final : public IRasterizer
{
protected:
	using GSWorker = GSJobQueue<GSRingHeap::SharedPtr<GSRasterizerData>, 65536>;

	GSDrawScanline m_ds;

	/* One rasterizer and one worker thread per slice; a worker draws on
	 * its rasterizer, so the workers go first when the list is torn down. */
	GSRasterizer** m_r;
	GSWorker** m_workers;
	u32 m_threads;
	u8* m_scanline;
	int m_thread_height;

	GSRasterizerList(int threads);

	/* A worker's context: its rasterizer and the CPU affinity it takes. */
	struct Worker
	{
		GSRasterizer* r;
		u64 affinity;
	};
	Worker* m_worker_ctx;
	static void OnWorkerStartup(void* context);
	static void OnWorkerDraw(void* context, GSRingHeap::SharedPtr<GSRasterizerData>& item);

public:
	~GSRasterizerList() override;

	/* A rasterizer on threads worker threads (none: a single one); the caller deletes it. */
	static IRasterizer* Create(int threads);

	// IRasterizer

	void Queue(const GSRingHeap::SharedPtr<GSRasterizerData>& data) override;
	void Sync() override;
	bool IsSynced() const override;
};

MULTI_ISA_UNSHARED_END
