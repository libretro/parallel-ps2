/*  The worker-notify handshake in pcsx2/WorkEventCount.h.
 *
 *  This is what replaced common/Threading.h's WorkSema class: two
 *  libretro-common eventcounts and three ints that MTGS and MTVU embed and
 *  drive through free functions. The seven operations keep the contract
 *  the class had, and MTGS and MTVU depend on it exactly, so this pins
 *  each part:
 *
 *    1. NotifyOfWork then CheckForWork returns true once, then false.
 *    2. WaitForWork returns at once when work is pending.
 *    3. WaitForWork parks with nothing pending and wakes on notify.
 *    4. WaitForEmpty returns only after the worker has consumed every
 *       notify so far and gone idle -- including a notify that lands
 *       between the worker's last check and its park.
 *    5. WaitForWorkTimed returns false only on a timeout with nothing
 *       pending, true whenever work arrived.
 *    6. Kill makes every wait return and WaitForEmpty report false;
 *       Reset revives.
 *    7. A producer/consumer pair run hard, with WaitForEmpty interleaved,
 *       loses no wake and never sees WaitForEmpty return while work is
 *       outstanding.
 *
 *  Run under TSan: the two threads share nothing but the primitive.
 *
 *  As with every eventcount test in this series, the lost-wake half of 7
 *  is only load-bearing on more than one core.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/Pcsx2Types.h"
#include "pcsx2/WorkEventCount.h"
extern "C" {
#include <rthreads/rthreads.h>
}
#include <unistd.h>

/* The header wants this from its host; MTGS.cpp defines it in the core.
 * Two here, so the spin path is exercised even on a one-core box. */
s32 WorkEventCount_SpinBudget(void) { return 2000; }

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)
/* For a check inside a loop: one line if it ever fails, not sixty-four. */
#define CHECK_QUIET(c) do { if (!(c)) { if (!quiet_failed) { printf("  FAIL: state looked empty mid-burst\n"); quiet_failed = 1; } fails++; } } while (0)
static int quiet_failed;
static int g_quiet_pair;

/* ---- 1, 2, 5, 6: single-threaded contract ------------------------- */
static void contract_single(void)
{
	WorkEventCount ws;
	work_eventcount_init(&ws);

	CHECK(!work_eventcount_check(&ws), "fresh sema reports no work");
	work_eventcount_notify(&ws);
	CHECK(work_eventcount_check(&ws), "notify makes CheckForWork true");
	CHECK(!work_eventcount_check(&ws), "a second CheckForWork is false");
	work_eventcount_notify(&ws); work_eventcount_notify(&ws); work_eventcount_notify(&ws);
	CHECK(work_eventcount_check(&ws), "three notifies are one check");
	CHECK(!work_eventcount_check(&ws), "and consumed together");

	work_eventcount_notify(&ws);
	work_eventcount_wait(&ws);                       /* must not block */
	CHECK(!work_eventcount_check(&ws), "WaitForWork consumed the pending notify");

	CHECK(!work_eventcount_wait_timed(&ws, 5), "timed wait with nothing pending: false");
	work_eventcount_notify(&ws);
	CHECK(work_eventcount_wait_timed(&ws, 5), "timed wait with work pending: true");

	/* "Empty" is the worker having gone idle after consuming, not merely
	 * having consumed: a worker that took work and has not yet checked
	 * again is still processing, and WaitForEmpty must block for it. The
	 * old state machine did exactly this, checked. So the worker goes
	 * idle first -- one CheckForWork that finds nothing -- and only then
	 * is WaitForEmpty immediate. */
	CHECK(!work_eventcount_check(&ws), "worker drains and finds nothing: idle");
	CHECK(work_eventcount_wait_empty(&ws), "idle worker: WaitForEmpty returns at once, true");

	work_eventcount_kill(&ws);
	CHECK(!work_eventcount_check(&ws), "dead: CheckForWork false");
	work_eventcount_wait(&ws);                       /* must not block */
	CHECK(work_eventcount_wait_timed(&ws, 1000), "dead: timed wait true at once");
	CHECK(!work_eventcount_wait_empty(&ws), "dead: WaitForEmpty false");
	work_eventcount_reset(&ws);
	CHECK(!work_eventcount_check(&ws), "reset: alive and idle");
	CHECK(work_eventcount_wait_empty(&ws), "reset: WaitForEmpty true");
	/* And the first sequence again: a fresh object is idle. */
	{
		WorkEventCount fresh;
		work_eventcount_init(&fresh);
		CHECK(work_eventcount_wait_empty(&fresh), "fresh object: WaitForEmpty true at once");
		work_eventcount_free(&fresh);
	}
	work_eventcount_free(&ws);
	printf("  contract (single thread): %s\n", fails ? "see above" : "ok");
}

/* ---- 3, 4, 7: the pair ---------------------------------------------- */
static WorkEventCount g_ws;
static retro_atomic_int_t  g_queue;      /* items the producer has pushed */
static retro_atomic_int_t  g_taken;      /* items the worker has drained  */
static retro_atomic_int_t  g_stop;
static retro_atomic_int_t  g_bad_empty;  /* WaitForEmpty returned with work outstanding */

static void worker(void*)
{
	while (!retro_atomic_load_relaxed_int(&g_stop))
	{
		work_eventcount_wait(&g_ws);
		/* Drain: take everything that has been pushed. */
		for (;;)
		{
			int q = retro_atomic_load_acquire_int(&g_queue);
			int t = retro_atomic_load_relaxed_int(&g_taken);
			if (t >= q) break;
			retro_atomic_store_release_int(&g_taken, t + 1);
		}
	}
}

/* symmetric: the pair on the symmetric protocol (the rasterizer job
 * queues' choice), which parks without the process-wide barrier and
 * must keep the same contract. */
static void contract_pair(int symmetric)
{
	sthread_t* th;
	int i, empties = 0;

	retro_atomic_store_relaxed_int(&g_queue, 0);
	retro_atomic_store_relaxed_int(&g_taken, 0);
	retro_atomic_store_relaxed_int(&g_stop, 0);
	retro_atomic_store_relaxed_int(&g_bad_empty, 0);
	if (symmetric)
	{
		work_eventcount_init_symmetric(&g_ws);
		CHECK(!retro_asym_eventcount_is_asymmetric(&g_ws.work), "symmetric init runs the symmetric protocol");
	}
	else
		work_eventcount_init(&g_ws);
	th = sthread_create(worker, NULL);

	for (i = 0; i < 20000; i++)
	{
		int q = retro_atomic_load_relaxed_int(&g_queue);
		int burst = 1 + (i % 4), b;
		for (b = 0; b < burst; b++)
		{
			retro_atomic_store_release_int(&g_queue, q + b + 1);
			work_eventcount_notify(&g_ws);
		}
		/* Every so often, the thing MTGS does at vsync. */
		if ((i % 7) == 0)
		{
			CHECK(work_eventcount_wait_empty(&g_ws), "WaitForEmpty true while alive");
			empties++;
			if (retro_atomic_load_acquire_int(&g_taken)
			  != retro_atomic_load_acquire_int(&g_queue))
				retro_atomic_fetch_add_int(&g_bad_empty, 1);
		}
		/* A short pause so the worker actually parks sometimes. */
		if ((i % 50) == 0) usleep(200);
	}
	CHECK(work_eventcount_wait_empty(&g_ws), "final WaitForEmpty");
	CHECK(retro_atomic_load_acquire_int(&g_taken)
	   == retro_atomic_load_acquire_int(&g_queue), "final drain complete");
	CHECK(retro_atomic_load_acquire_int(&g_bad_empty) == 0,
	      "WaitForEmpty never returned with work outstanding");

	retro_atomic_store_relaxed_int(&g_stop, 1);
	work_eventcount_kill(&g_ws);
	sthread_join(th);
	work_eventcount_free(&g_ws);
	if (g_quiet_pair && !fails)
		return;
	printf("  pair%s: %d items, %d WaitForEmpty calls, %d returned early\n",
	       symmetric ? " (symmetric)" : "",
	       retro_atomic_load_relaxed_int(&g_queue), empties,
	       retro_atomic_load_relaxed_int(&g_bad_empty));
}

/*  8. The state a WaitForEmpty waiter reads is one state, not two.
 *
 *  The one that shipped broken: "the worker has consumed everything" and
 *  "the worker is idle" were two atomics, so a waiter could read idle=1
 *  from before the worker picked the work up and the epoch from after,
 *  and return with work outstanding. It takes two cores and the right
 *  timing to see that as a failure, and this box may have one, so the
 *  property is checked directly instead: whatever the worker is doing,
 *  every intermediate state a waiter can observe is one the worker was
 *  actually in.
 *
 *  Drive the worker's transitions one at a time, from this thread, and
 *  read the state after each. The interleaving that used to lie -
 *  notify, worker consumes, waiter looks - must never report empty. */
static void contract_state_is_one_word(void)
{
	printf("  state is one word: ");
	WorkEventCount ws;
	int epoch;

	work_eventcount_init(&ws);
	CHECK(work_eventcount_is_empty(&ws), "fresh: empty");

	/* Work arrives. Not empty, whatever else is true. */
	work_eventcount_notify(&ws);
	CHECK(!work_eventcount_is_empty(&ws), "notified, worker has not looked: not empty");

	/* The worker picks it up. Still not empty: it is processing. This is
	 * the moment the two-atomic version could report empty, having taken
	 * idle from before this and the epoch from after. */
	CHECK(work_eventcount_check(&ws), "worker sees the work");
	CHECK(!work_eventcount_is_empty(&ws), "worker processing: not empty");

	/* It finishes and finds nothing more. Now empty. */
	CHECK(!work_eventcount_check(&ws), "worker finds nothing more");
	CHECK(work_eventcount_is_empty(&ws), "worker idle and caught up: empty");

	/* A notify landing while the worker is idle: not empty again, and the
	 * worker must be able to see it. */
	work_eventcount_notify(&ws);
	CHECK(!work_eventcount_is_empty(&ws), "notify to an idle worker: not empty");
	CHECK(work_eventcount_check(&ws), "idle worker sees the new work");

	/* Many notifies while processing collapse into one epoch, and none of
	 * them may make the state look empty. */
	for (epoch = 0; epoch < 64; epoch++)
	{
		work_eventcount_notify(&ws);
		CHECK_QUIET(!work_eventcount_is_empty(&ws));
	}
	CHECK(work_eventcount_check(&ws), "worker sees the burst");
	CHECK(!work_eventcount_is_empty(&ws), "burst consumed, processing: not empty");
	CHECK(!work_eventcount_check(&ws), "worker finishes the burst");
	CHECK(work_eventcount_is_empty(&ws), "after the burst: empty");

	work_eventcount_free(&ws);
	printf("ok\n");
}

int main(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	/* The pair is a race: one run of it proves little, and the failure it
	 * exists to catch showed up a dozen times in fifty thousand items on
	 * a busy multi-core host and not at all on a quiet one. WORKSEMA_REPS
	 * repeats it; CI sets it, and anyone chasing a suspected lost wake
	 * can set it higher. */
	const char* reps_env = getenv("WORKSEMA_REPS");
	int reps = reps_env ? atoi(reps_env) : 1;
	int i;
	if (reps < 1)
		reps = 1;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("work_eventcount\n  cpus: %ld%s\n", n,
	       n > 1 ? "" : "  (lost-wake half not load-bearing on one core)");
	contract_single();
	contract_state_is_one_word();
	for (i = 0; i < reps; i++)
	{
		/* One line per repetition is noise at two hundred: the interesting
		 * run is the one that fails, and contract_pair prints its own
		 * counts, so only the last is shown when all of them pass. */
		g_quiet_pair = (reps > 1 && i + 1 < reps);
		contract_pair(0);
		if (fails)
			break;
		contract_pair(1);
		if (fails)
			break;
	}
	printf(fails ? "worksema: FAILED (%d)\n" : "worksema: ok\n", fails);
	return fails != 0;
}
