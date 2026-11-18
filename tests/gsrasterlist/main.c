/* SPDX-License-Identifier: LGPL-3.0+ */
/* The software renderer's drawing threads (GS/Renderers/SW/GSRasterList.c),
 * on draws that record who drew them: each thread draws exactly the
 * draws whose rows touch its bands, in the order they were queued; a
 * whole draw is drawn on the caller by the first thread's rasterizer,
 * after every draw before it; a draw is held while a queue has it and let
 * go of after, drawn or not when the threads stop; and without threads,
 * the caller draws everything in order. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>

#include "GS/Renderers/SW/GSRasterList.h"

/* (MTGS.c's: no spinning before a park) */
s32 WorkEventCount_SpinBudget(void) { return 0; }

#define MAX_THREADS 6
#define MAX_JOBS 4096

typedef struct job
{
	retro_atomic_int_t ref;
	int id;
	gs_recti r;
	bool whole;
} job;

typedef struct rast
{
	int* log;
	int count;
	int whole_seen; /* (the caller's draws of whole ones) */
} rast;

static job s_jobs[MAX_JOBS];
static rast s_rast[MAX_THREADS];
static int* s_expect[MAX_THREADS];
static int s_expect_count[MAX_THREADS];
static int s_threads;
static long s_checks;
static int s_failed;
static int s_slow;

static void fail(const char* what, int a, int b)
{
	if (!s_failed)
		printf("FAIL: %s (%d, %d)\n", what, a, b);
	s_failed = 1;
}

static u32 s_rng = 0x9e3779b9u;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static int rnd_n(int n) { return (int)(rnd() % (u32)n); }

static void op_draw(void* r, void* p)
{
	rast* ra = (rast*)r;
	job* j   = (job*)p;
	volatile int spin;
	/* (held: by the test, and by the queue that had it) */
	if (s_threads && retro_atomic_load_acquire_int(&j->ref) < 2)
		fail("drawn without being held", j->id, retro_atomic_load_acquire_int(&j->ref));
	if (ra->count < 4 * MAX_JOBS)
		ra->log[ra->count] = j->id;
	ra->count++;
	if (s_slow)
		for (spin = 0; spin < 2000; spin++)
			;
}

static void op_draw_whole(void* r, void* p)
{
	rast* ra = (rast*)r;
	job* j   = (job*)p;
	int i;
	if (ra != &s_rast[0])
		fail("a whole draw not by the first rasterizer", (int)(ra - s_rast), j->id);
	/* Everything before it is drawn. */
	for (i = 0; i < s_threads; i++)
		if (s_rast[i].count != s_expect_count[i])
			fail("a whole draw before the threads were done", i, j->id);
	ra->whole_seen++;
}

static void op_retain(void* p)
{
	retro_atomic_inc_int(&((job*)p)->ref);
}

static void op_release(void* p)
{
	job* j = (job*)p;
	if (retro_atomic_fetch_sub_int(&j->ref, 1) <= 1)
		fail("let go of more than held", j->id, 0);
}

static const gs_raster_list_ops s_ops = {op_draw, op_draw_whole, op_retain, op_release};

/* Whose rows these are, as the list was specified */
static void expect(int id, const gs_recti* r, int th)
{
	int top, bottom;
	if (!s_threads)
	{
		s_expect[0][s_expect_count[0]++] = id;
		return;
	}
	top    = r->y >> th;
	bottom = (r->w + (1 << th) - 1) >> th;
	if (bottom > top + s_threads)
		bottom = top + s_threads;
	for (; top < bottom; top++)
	{
		const int t = top % s_threads;
		s_expect[t][s_expect_count[t]++] = id;
	}
}

static void check_drawn(int njobs, bool prefix)
{
	int i, k;
	for (i = 0; i < (s_threads ? s_threads : 1); i++)
	{
		const rast* ra = &s_rast[i];
		s_checks++;
		if (prefix ? ra->count > s_expect_count[i] : ra->count != s_expect_count[i])
		{
			fail("a thread drew a different number of draws", i, ra->count - s_expect_count[i]);
			return;
		}
		for (k = 0; k < ra->count; k++)
			if (ra->log[k] != s_expect[i][k])
			{
				fail("a thread drew a different draw", i, k);
				return;
			}
	}
	for (i = 0; i < njobs; i++)
	{
		s_checks++;
		if (retro_atomic_load_acquire_int(&s_jobs[i].ref) != 1)
			fail("a draw still held", i, retro_atomic_load_acquire_int(&s_jobs[i].ref));
	}
}

static void run(int threads, int th, int rounds, bool stop_early)
{
	gs_raster_list l;
	void* rasterizers[MAX_THREADS];
	u64 affinity[MAX_THREADS];
	int i, round, n = 0, wholes = 0;

	s_threads = threads;
	for (i = 0; i < MAX_THREADS; i++)
	{
		s_rast[i].count      = 0;
		s_rast[i].whole_seen = 0;
		s_expect_count[i]    = 0;
		rasterizers[i]       = &s_rast[i];
		affinity[i]          = 0;
	}
	if (!gs_raster_list_init(&l, threads, th, rasterizers, affinity, &s_ops))
	{
		fail("init", threads, th);
		return;
	}

	for (round = 0; round < rounds && !s_failed; round++)
	{
		const int count = 1 + rnd_n(MAX_JOBS / 4);
		s_slow = rnd_n(4) == 0;
		for (i = 0; i < count; i++, n++)
		{
			job* j = &s_jobs[n % MAX_JOBS];
			if (n >= MAX_JOBS)
				break;
			j->id = n;
			retro_atomic_store_release_int(&j->ref, 1);
			j->r.x = 0;
			j->r.z = 640;
			j->r.y = rnd_n(4) ? rnd_n(4096) : rnd_n(64);
			j->r.w = j->r.y + (rnd_n(8) ? rnd_n(64) : rnd_n(4096 - j->r.y + 1));
			if (rnd_n(16) == 0)
				j->r.w = j->r.y - rnd_n(8); /* (no rows) */
			j->whole = rnd_n(32) == 0;
			if (j->whole && threads)
				wholes++;
			else
				expect(n, &j->r, th);
			gs_raster_list_queue(&l, j, &j->r, j->whole);
		}
		if (stop_early && round == rounds - 1)
			break;
		gs_raster_list_sync(&l);
		s_checks++;
		if (!gs_raster_list_is_synced(&l))
			fail("not synced after a sync", threads, round);
		check_drawn(n < MAX_JOBS ? n : MAX_JOBS, false);
		s_checks++;
		if (s_rast[0].whole_seen != wholes)
			fail("whole draws", s_rast[0].whole_seen, wholes);
		if (n >= MAX_JOBS)
			break;
	}

	gs_raster_list_deinit(&l);
	/* Stopped with draws queued: some drawn, the rest let go of. */
	check_drawn(n < MAX_JOBS ? n : MAX_JOBS, true);
}

int main(void)
{
	int i, threads, th;

	for (i = 0; i < MAX_THREADS; i++)
	{
		s_rast[i].log = (int*)malloc(sizeof(int) * 4 * MAX_JOBS);
		s_expect[i]   = (int*)malloc(sizeof(int) * 4 * MAX_JOBS);
	}

	for (threads = 0; threads <= MAX_THREADS && !s_failed; threads++)
		for (th = 1; th <= 8 && !s_failed; th++)
		{
			run(threads, th, 8, false);
			run(threads, th, 2, true);
		}

	for (i = 0; i < MAX_THREADS; i++)
	{
		free(s_rast[i].log);
		free(s_expect[i]);
	}
	printf("gsrasterlist: %ld checks, %d differed\n", s_checks, s_failed);
	return s_failed;
}
