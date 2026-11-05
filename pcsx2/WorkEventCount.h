/*
 * WorkEventCount - the "tell a worker thread there is new work" handshake,
 * as two libretro-common eventcounts and nothing else.
 *
 * This replaces common/Threading.h's WorkSema class. The state it needs is
 * two eventcounts and three ints, and the caller owns them: an MTGS or an
 * MTVU embeds a WorkEventCount and calls these free functions. There is no
 * pcsx2 wrapper class, no common/ helper, no hand-rolled state machine --
 * the whole point of the move is to call straight into libretro-common.
 *
 * Two directions, each an eventcount:
 *
 *   work  (producer -> worker, "work was added"): retro_asym_eventcount.
 *         Its notify is a release store and a relaxed load, no lock
 *         prefix -- that is what the EE pays per GS packet, and it is the
 *         reason for the whole change. Where retro_procbarrier has no
 *         tier the eventcount runs its symmetric protocol for life,
 *         decided at init; the caller never has to know which.
 *
 *   empty (worker -> producer, "I drained everything so far and went
 *         idle"): the plain retro_eventcount. Read once a frame, so its
 *         cost does not matter, and its prepare/commit contract is what
 *         the old STATE_FLAG_WAITING_EMPTY handshake was reinventing.
 *
 * Contract, matching the class it replaces exactly:
 *   - NotifyOfWork:      publish that work was added.
 *   - CheckForWork:      true once per burst of notifies; a false return
 *                        means the worker is now idle and empty-waiters
 *                        are told so.
 *   - WaitForWork:       return at once with work pending; else spin
 *                        SpinBudget() reads and park -- the one place the
 *                        process-wide barrier is paid.
 *   - WaitForWorkTimed:  as WaitForWork, false only on a timeout with
 *                        nothing pending (true means work or dead).
 *   - WaitForEmpty:      block until the worker consumed every notify so
 *                        far AND went idle; false if the worker is dead.
 *   - Kill / Reset:      dead threads process nothing and make every wait
 *                        return; Reset revives.
 */

#ifndef PCSX2_WORK_EVENTCOUNT_H
#define PCSX2_WORK_EVENTCOUNT_H

#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>
#include <rthreads/retro_asym_eventcount.h>

/* CPU relax hint for the bounded pre-park spin. Self-contained on
 * purpose: this header must not pull in common/Threading.h, which is the
 * dependency being removed. */
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#if defined(_MSC_VER)
#define WORK_EVENTCOUNT_RELAX() _mm_pause()
#else
#define WORK_EVENTCOUNT_RELAX() __builtin_ia32_pause()
#endif
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__)
#if defined(_MSC_VER)
#define WORK_EVENTCOUNT_RELAX() __yield()
#else
#define WORK_EVENTCOUNT_RELAX() __asm__ __volatile__("yield" ::: "memory")
#endif
#else
#define WORK_EVENTCOUNT_RELAX() ((void)0)
#endif

typedef struct WorkEventCount
{
	retro_asym_eventcount_t work;    /* producer -> worker            */
	retro_eventcount_t      empty;   /* worker -> producer            */
	/* The epoch the worker has consumed and whether it is idle, in one
	 * word: the low bit is idle, the rest the epoch. They were two
	 * atomics, and a waiter that reads two atomics reads two moments -
	 * it could take idle=1 from before the worker picked work up and the
	 * new epoch from after, and conclude "idle and caught up" of a state
	 * that never existed, returning from WaitForEmpty with work still
	 * outstanding. No ordering between two variables fixes that; one
	 * word does, because every transition publishes both halves at once. */
	retro_atomic_int_t      state;
	retro_atomic_int_t      dead;    /* Kill sets, Reset clears        */
} WorkEventCount;

/* The epoch is kept in 31 bits, which is what the eventcount's own
 * comparisons amount to: it only ever asks whether two epochs are the
 * same one. */
#define WORK_EVENTCOUNT_STATE(epoch, idle) \
	((int)((((uint32_t)(epoch)) << 1) | ((uint32_t)(idle) & 1u)))
#define WORK_EVENTCOUNT_STATE_EPOCH(state) (((uint32_t)(state)) >> 1)
#define WORK_EVENTCOUNT_STATE_IDLE(state)  (((uint32_t)(state)) & 1u)
#define WORK_EVENTCOUNT_EPOCH_BITS(epoch)  ((((uint32_t)(epoch)) << 1) >> 1)

/* Iterations of a read-only spin worth spending to dodge a kernel
 * sleep/wake pair; 0 on single-core hosts. Defined in MTGS.cpp. */
extern s32 WorkEventCount_SpinBudget(void);

static INLINE void work_eventcount_init(WorkEventCount *w)
{
	retro_asym_eventcount_init(&w->work);
	retro_eventcount_init(&w->empty);
	retro_atomic_store_relaxed_int(&w->state,
			WORK_EVENTCOUNT_STATE(
				retro_atomic_load_relaxed_int(&w->work.epoch), 1));
	retro_atomic_store_relaxed_int(&w->dead, 0);
}

static INLINE void work_eventcount_free(WorkEventCount *w)
{
	retro_eventcount_free(&w->empty);
	retro_asym_eventcount_free(&w->work);
}

/* The epoch the worker has consumed, for its own comparisons. */
static INLINE uint32_t work_eventcount_seen(const WorkEventCount *w)
{
	return WORK_EVENTCOUNT_STATE_EPOCH(
			retro_atomic_load_relaxed_int((retro_atomic_int_t*)&w->state));
}

/* Worker took everything up to `epoch`; it is processing, not idle. One
 * store, so nobody can see the new epoch without also seeing that the
 * worker is busy with it. */
static INLINE int work_eventcount_consume(WorkEventCount *w, int epoch)
{
	retro_atomic_store_release_int(&w->state, WORK_EVENTCOUNT_STATE(epoch, 0));
	return 1;
}

/* Worker found nothing to do at `epoch`: publish "idle, having seen
 * epoch" for the empty-waiters, again as one store. A notify that lands
 * after the epoch was read and before this store leaves the state idle
 * at the older epoch, which is not empty, and the waiter keeps waiting -
 * which is the point. */
static INLINE void work_eventcount_go_idle(WorkEventCount *w, int epoch)
{
	retro_atomic_store_release_int(&w->state, WORK_EVENTCOUNT_STATE(epoch, 1));
	retro_eventcount_notify(&w->empty);
}

static INLINE void work_eventcount_notify(WorkEventCount *w)
{
	retro_asym_eventcount_notify(&w->work);
}

static INLINE int work_eventcount_check(WorkEventCount *w)
{
	int epoch;
	if (retro_atomic_load_acquire_int(&w->dead))
		return 0;
	/* Reading the epoch is what a non-blocking check is; cheaper than a
	 * prepare/cancel pair, which would pay the barrier. */
	epoch = retro_atomic_load_acquire_int(&w->work.epoch);
	if (WORK_EVENTCOUNT_EPOCH_BITS(epoch) != work_eventcount_seen(w))
		return work_eventcount_consume(w, epoch);
	work_eventcount_go_idle(w, epoch);
	return 0;
}

/* The epoch to hand work_eventcount_drained(), read before the worker
 * looks at its queue. */
static INLINE int work_eventcount_epoch(WorkEventCount *w)
{
	return retro_atomic_load_acquire_int(&w->work.epoch);
}

/* The worker has drained its queue up to a record the producer waits
 * on (MTGS's vsync): go idle now rather than at its next wait, which is
 * what wakes a producer parked in work_eventcount_wait_drained().
 * `pending` is whether the queue still held anything when looked at
 * after `epoch` was read; anything still queued re-arms the work count
 * so the next wait drains it. */
static INLINE void work_eventcount_drained(WorkEventCount *w, int epoch, int pending)
{
	if (pending)
		work_eventcount_notify(w);
	else if (!retro_atomic_load_acquire_int(&w->dead))
		work_eventcount_go_idle(w, epoch);
}

static INLINE void work_eventcount_wait(WorkEventCount *w)
{
	const s32 spin_budget = WorkEventCount_SpinBudget();
	int epoch, key;

	if (retro_atomic_load_acquire_int(&w->dead))
		return;

	epoch = retro_atomic_load_acquire_int(&w->work.epoch);
	if (WORK_EVENTCOUNT_EPOCH_BITS(epoch) != work_eventcount_seen(w))
	{
		work_eventcount_consume(w, epoch);
		return;
	}

	work_eventcount_go_idle(w, epoch);

	if (spin_budget)
	{
		s32 spins = spin_budget;
		while (spins-- > 0)
		{
			epoch = retro_atomic_load_acquire_int(&w->work.epoch);
			if (WORK_EVENTCOUNT_EPOCH_BITS(epoch) != work_eventcount_seen(w))
			{
				work_eventcount_consume(w, epoch);
				return;
			}
			if (retro_atomic_load_relaxed_int(&w->dead))
				return;
			WORK_EVENTCOUNT_RELAX();
		}
	}

	key = retro_asym_eventcount_prepare_wait(&w->work);
	if (WORK_EVENTCOUNT_EPOCH_BITS(key) != work_eventcount_seen(w)
	 || retro_atomic_load_acquire_int(&w->dead))
	{
		retro_asym_eventcount_cancel_wait(&w->work);
		if (!retro_atomic_load_acquire_int(&w->dead))
			work_eventcount_consume(w, key);
		return;
	}
	retro_asym_eventcount_commit_wait(&w->work, key);
	if (!retro_atomic_load_acquire_int(&w->dead))
		work_eventcount_consume(w,
				retro_atomic_load_acquire_int(&w->work.epoch));
}

static INLINE int work_eventcount_wait_timed(WorkEventCount *w, u32 timeout_ms)
{
	const s32 spin_budget = WorkEventCount_SpinBudget();
	int epoch, key;

	if (retro_atomic_load_acquire_int(&w->dead))
		return 1;

	epoch = retro_atomic_load_acquire_int(&w->work.epoch);
	if (WORK_EVENTCOUNT_EPOCH_BITS(epoch) != work_eventcount_seen(w))
		return work_eventcount_consume(w, epoch);

	work_eventcount_go_idle(w, epoch);

	/* The same bounded spin as work_eventcount_wait before the park:
	 * the bound is for a wedged producer, and a park is the barrier
	 * and a kernel sleep/wake pair for work that is a few packets
	 * away. */
	if (spin_budget)
	{
		s32 spins = spin_budget;
		while (spins-- > 0)
		{
			epoch = retro_atomic_load_acquire_int(&w->work.epoch);
			if (WORK_EVENTCOUNT_EPOCH_BITS(epoch) != work_eventcount_seen(w))
				return work_eventcount_consume(w, epoch);
			if (retro_atomic_load_relaxed_int(&w->dead))
				return 1;
			WORK_EVENTCOUNT_RELAX();
		}
	}

	key = retro_asym_eventcount_prepare_wait(&w->work);
	if (WORK_EVENTCOUNT_EPOCH_BITS(key) != work_eventcount_seen(w)
	 || retro_atomic_load_acquire_int(&w->dead))
	{
		retro_asym_eventcount_cancel_wait(&w->work);
		if (!retro_atomic_load_acquire_int(&w->dead))
			work_eventcount_consume(w, key);
		return 1;
	}
	if (!retro_asym_eventcount_commit_wait_timeout(&w->work, key,
			(int64_t)timeout_ms * 1000))
	{
		epoch = retro_atomic_load_acquire_int(&w->work.epoch);
		if (epoch == key && !retro_atomic_load_acquire_int(&w->dead))
			return 0;   /* timed out with nothing pending */
	}
	if (!retro_atomic_load_acquire_int(&w->dead))
		work_eventcount_consume(w,
				retro_atomic_load_acquire_int(&w->work.epoch));
	return 1;
}

/* Empty is one state, read as one word: the worker is idle AND the epoch
 * it went idle at is still the current one. Reading the state first and
 * the epoch second is what makes a late notify count as not-empty: a
 * notify after this load leaves the epoch ahead of the state, and the
 * caller waits. */
static INLINE int work_eventcount_is_empty(WorkEventCount *w)
{
	const int state = retro_atomic_load_acquire_int(&w->state);
	if (!WORK_EVENTCOUNT_STATE_IDLE(state))
		return 0;
	return WORK_EVENTCOUNT_STATE_EPOCH(state)
		== WORK_EVENTCOUNT_EPOCH_BITS(
			retro_atomic_load_acquire_int(&w->work.epoch));
}

static INLINE int work_eventcount_wait_empty(WorkEventCount *w)
{
	for (;;)
	{
		int key;
		if (retro_atomic_load_acquire_int(&w->dead))
			return 0;
		/* Idle alone is not empty: a notify can land after the worker's
		 * last check and before its park, and that work is outstanding.
		 * Empty is idle AND the worker went idle having seen everything. */
		if (work_eventcount_is_empty(w))
			return 1;
		key = retro_eventcount_prepare_wait(&w->empty);
		if (retro_atomic_load_acquire_int(&w->dead))
		{
			retro_eventcount_cancel_wait(&w->empty);
			return 0;
		}
		if (work_eventcount_is_empty(w))
		{
			retro_eventcount_cancel_wait(&w->empty);
			return 1;
		}
		retro_eventcount_commit_wait(&w->empty, key);
	}
}

/* Block until `drained(ctx)` reports that the worker has consumed
 * everything the producer queued; 0 if the worker is dead. For a
 * producer whose queue can answer that itself, which is a stronger
 * answer than the idle epoch: a worker can take a record before the
 * notify that announces it and go idle an epoch behind, and
 * work_eventcount_wait_empty() would then hold the producer until the
 * worker's next wait. Woken by the worker going idle, which every drain
 * that empties the queue ends with. */
static INLINE int work_eventcount_wait_drained(WorkEventCount *w,
		int (*drained)(void *ctx), void *ctx)
{
	for (;;)
	{
		int key;
		if (retro_atomic_load_acquire_int(&w->dead))
			return 0;
		if (drained(ctx))
			return 1;
		key = retro_eventcount_prepare_wait(&w->empty);
		if (retro_atomic_load_acquire_int(&w->dead))
		{
			retro_eventcount_cancel_wait(&w->empty);
			return 0;
		}
		if (drained(ctx))
		{
			retro_eventcount_cancel_wait(&w->empty);
			return 1;
		}
		retro_eventcount_commit_wait(&w->empty, key);
	}
}

static INLINE void work_eventcount_kill(WorkEventCount *w)
{
	retro_atomic_store_release_int(&w->dead, 1);
	retro_asym_eventcount_notify(&w->work);
	retro_eventcount_notify(&w->empty);
}

static INLINE void work_eventcount_reset(WorkEventCount *w)
{
	retro_atomic_store_release_int(&w->dead, 0);
	retro_atomic_store_release_int(&w->state,
			WORK_EVENTCOUNT_STATE(
				retro_atomic_load_acquire_int(&w->work.epoch), 1));
}

#endif
