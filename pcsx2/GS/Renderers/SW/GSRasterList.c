/* SPDX-License-Identifier: LGPL-3.0+ */

#include <stdlib.h>
#include <string.h>

#include <memalign.h>

#include "GSRasterList.h"

/* A worker's ring: this many job pointers, before Push waits for room */
#define GS_RASTER_QUEUE_JOBS 65536

static void gs_raster_worker_proc(void* arg)
{
	gs_raster_worker* w = (gs_raster_worker*)arg;

	/* (worked out on the spawning thread: reading the settings here would
	 * race their being written on the CPU thread) */
	if (w->affinity)
		sthread_set_current_affinity(w->affinity);

	for (;;)
	{
		size_t span;
		const void* ptr;

		work_eventcount_wait(&w->sema);
		if (retro_atomic_load_acquire_int(&w->exit))
			break;
		/* Span drain, batch commit: a job is let go of before its record
		 * is, so the producer never reuses one that holds it. The spans
		 * loop until the queue is empty, so a wrap can't strand records
		 * behind a park. */
		while (w->queue_ok && (span = retro_spsc_read_begin(&w->queue, &ptr)) >= sizeof(void*))
		{
			size_t done;
			span -= span % sizeof(void*);
			for (done = 0; done < span; done += sizeof(void*))
			{
				void* job;
				memcpy(&job, (const u8*)ptr + done, sizeof(job));
				w->ops->draw(w->rasterizer, job);
				w->ops->release(job);
			}
			retro_spsc_read_end(&w->queue, span);
		}
	}
}

static bool gs_raster_worker_start(gs_raster_worker* w, const gs_raster_list_ops* ops, void* rasterizer, u64 affinity)
{
	w->ops        = ops;
	w->rasterizer = rasterizer;
	w->affinity   = affinity;
	retro_atomic_store_relaxed_int(&w->exit, 0);
	/* Fed a job a draw and drained faster than they come, a worker parks
	 * many times a frame: the symmetric pair, where a park is a kernel
	 * wait and no more. */
	work_eventcount_init_symmetric(&w->sema);
	w->queue_ok = retro_spsc_init(&w->queue, (size_t)GS_RASTER_QUEUE_JOBS * sizeof(void*));
	w->thread   = sthread_create(gs_raster_worker_proc, w);
	return w->thread != NULL;
}

static void gs_raster_worker_stop(gs_raster_worker* w)
{
	if (w->thread)
	{
		retro_atomic_store_release_int(&w->exit, 1);
		work_eventcount_notify(&w->sema);
		sthread_join(w->thread);
		w->thread = NULL;
	}
	if (w->queue_ok)
	{
		/* What the worker never drew is let go of undrawn. */
		size_t span;
		const void* ptr;
		while ((span = retro_spsc_read_begin(&w->queue, &ptr)) >= sizeof(void*))
		{
			size_t done;
			span -= span % sizeof(void*);
			for (done = 0; done < span; done += sizeof(void*))
			{
				void* job;
				memcpy(&job, (const u8*)ptr + done, sizeof(job));
				w->ops->release(job);
			}
			retro_spsc_read_end(&w->queue, span);
		}
		retro_spsc_free(&w->queue);
		w->queue_ok = false;
	}
	work_eventcount_free(&w->sema);
}

static void gs_raster_worker_push(gs_raster_worker* w, void* job)
{
	void* dst;

	if (!w->queue_ok)
	{
		/* (no ring, which an allocation at start failing leaves: drawn
		 * here rather than dropped) */
		w->ops->draw(w->rasterizer, job);
		return;
	}
	while (retro_spsc_write_begin(&w->queue, &dst) < sizeof(void*))
		sthread_yield();
	w->ops->retain(job);
	memcpy(dst, &job, sizeof(job));
	retro_spsc_write_end(&w->queue, sizeof(void*));
	work_eventcount_notify(&w->sema);
}

bool gs_raster_list_init(gs_raster_list* l, int threads, int thread_height, void* const* rasterizers,
	const u64* affinity, const gs_raster_list_ops* ops)
{
	int rows, i;

	memset(l, 0, sizeof(*l));
	l->ops           = ops;
	l->thread_height = thread_height;

	if (threads <= 0)
	{
		l->single = rasterizers[0];
		return true;
	}

	/* 4096 rows: the 2x drawing's */
	rows        = (4096 >> thread_height) + 16;
	l->scanline = (u8*)memalign_alloc(64, (size_t)rows);
	l->workers  = (gs_raster_worker*)calloc((size_t)threads, sizeof(gs_raster_worker));
	if (!l->scanline || !l->workers)
	{
		gs_raster_list_deinit(l);
		return false;
	}
	for (i = 0; i < rows; i++)
		l->scanline[i] = (u8)(i % threads);

	for (i = 0; i < threads; i++)
	{
		l->threads = (u32)i + 1;
		if (!gs_raster_worker_start(&l->workers[i], ops, rasterizers[i], affinity ? affinity[i] : 0))
		{
			gs_raster_list_deinit(l);
			return false;
		}
	}
	return true;
}

void gs_raster_list_deinit(gs_raster_list* l)
{
	u32 i;

	for (i = l->threads; i-- > 0;)
		gs_raster_worker_stop(&l->workers[i]);
	free(l->workers);
	memalign_free(l->scanline);
	memset(l, 0, sizeof(*l));
}

void gs_raster_list_queue(gs_raster_list* l, void* job, const gs_recti* r, bool whole)
{
	int top, bottom;

	if (!l->threads)
	{
		l->ops->draw(l->single, job);
		return;
	}

	/* After every queued draw, on this thread, before the next is queued:
	 * the draws before and after it keep their order on every row. */
	if (whole)
	{
		gs_raster_list_sync(l);
		l->ops->draw_whole(l->workers[0].rasterizer, job);
		return;
	}

	top    = r->y >> l->thread_height;
	bottom = (r->w + (1 << l->thread_height) - 1) >> l->thread_height;
	if (bottom > top + (int)l->threads)
		bottom = top + (int)l->threads;

	while (top < bottom)
		gs_raster_worker_push(&l->workers[l->scanline[top++]], job);
}

bool gs_raster_list_is_synced(gs_raster_list* l)
{
	u32 i;

	for (i = 0; i < l->threads; i++)
	{
		gs_raster_worker* w = &l->workers[i];
		if (w->queue_ok && retro_spsc_read_avail(&w->queue) != 0)
			return false;
	}
	return true;
}

void gs_raster_list_sync(gs_raster_list* l)
{
	u32 i;

	if (gs_raster_list_is_synced(l))
		return;
	for (i = 0; i < l->threads; i++)
		work_eventcount_wait_empty(&l->workers[i].sema);
}
