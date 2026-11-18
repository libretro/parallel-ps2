/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GS_RASTER_LIST_H
#define GS_RASTER_LIST_H

/* The software renderer's drawing threads: each owns the bands of rows
 * one in every threads, drawn by its own rasterizer, and takes the draws
 * that touch them from a queue in the order they came. Without threads
 * the caller draws. Lock-free: a ring and an eventcount a thread. */

#include <boolean.h>
#include <retro_atomic.h>
#include <retro_spsc.h>
#include <rthreads/rthreads.h>

#include "common/Pcsx2Defs.h"
#include "../../../WorkEventCount.h"
#include "../../gs_rect.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a draw is to the list: drawn by a rasterizer, all of it or its
 * bands, and held while a queue has it */
typedef struct gs_raster_list_ops
{
	void (*draw)(void* rasterizer, void* job);
	void (*draw_whole)(void* rasterizer, void* job);
	void (*retain)(void* job);
	void (*release)(void* job);
} gs_raster_list_ops;

typedef struct gs_raster_worker
{
	sthread_t* thread;
	const gs_raster_list_ops* ops;
	void* rasterizer;
	u64 affinity; /* 0: the system's choice */
	retro_spsc_t queue; /* of job pointers */
	bool queue_ok;
	retro_atomic_int_t exit;
	WorkEventCount sema;
} gs_raster_worker;

typedef struct gs_raster_list
{
	const gs_raster_list_ops* ops;
	gs_raster_worker* workers;
	void* single; /* the rasterizer when there are no threads */
	u8* scanline; /* each band's thread */
	u32 threads;
	int thread_height;
} gs_raster_list;

/* threads threads (0: draws on the caller, rasterizers[0]), one of
 * rasterizers and affinity each, bands of 1 << thread_height rows */
bool gs_raster_list_init(gs_raster_list* l, int threads, int thread_height, void* const* rasterizers,
	const u64* affinity, const gs_raster_list_ops* ops);
/* Stops the threads; what they had not drawn is let go of */
void gs_raster_list_deinit(gs_raster_list* l);

/* The job to the threads whose bands rows r.y to r.w touch; whole: every
 * row drawn on the caller by the first thread's rasterizer, once the
 * threads are done */
void gs_raster_list_queue(gs_raster_list* l, void* job, const gs_recti* r, bool whole);
/* Waits for the threads to draw all they have */
void gs_raster_list_sync(gs_raster_list* l);
bool gs_raster_list_is_synced(gs_raster_list* l);

#ifdef __cplusplus
}
#endif

#endif
