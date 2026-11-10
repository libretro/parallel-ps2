/* Who drains the GS ring: the rule in pcsx2/MTGSOwner.h, run with the
 * threads a frontend really has, on the real work eventcount.
 *
 * The roles are the ones from the hang this was written after (threaded
 * video, Vulkan HW renderer):
 *
 *   video    opens the GS from create_device, then resumes the EE from
 *            context_reset, and never touches the ring again
 *   ee       the producer: already running, and already waiting on the
 *            GS, before the frontend's first retro_run
 *   frontend whose first act in its first retro_run is to wait on the GS
 *            (update_av_info), and which only then gets to anything else
 *
 * Two things must hold. Nobody may be left parked: every wait returns.
 * And the ring's work -- GS commands, the video callback -- must never
 * run on the producer.
 *
 * RULE selects what decides a wait:
 *   0  MTGSOwner.h, the rule in the tree
 *   1  "the owner drains": owner is whoever opened the GS until
 *      retro_run claims it, and retro_run claims after its first wait.
 *      Hangs: the video thread owns the ring, so the frontend parks
 *      beside the EE.
 *   2  "anyone who can drain, does". Does not hang, and runs the GS on
 *      the EE.
 * build.sh runs 0 and requires 1 and 2 to fail. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <retro_inline.h>
#include <rthreads/rthreads.h>

typedef int32_t  s32;
typedef uint32_t u32;
#include "WorkEventCount.h"
#include "MTGSOwner.h"

#ifndef RULE
#define RULE 0
#endif
#define FRAMES 2000

s32 WorkEventCount_SpinBudget(void) { return 0; } /* every wait parks */

static WorkEventCount     ev;
static retro_atomic_int_t pending;        /* entries in the "ring"          */
static retro_atomic_int_t ee_resumed;     /* context_reset resumed the EE   */
static retro_atomic_int_t ee_waiting;     /* the EE has reached its wait    */
static retro_atomic_int_t ee_done;
static retro_atomic_int_t ran_on_producer;
static uintptr_t producer, owner;         /* ordered by the flags above     */

static void on_alarm(int sig)
{
	static const char msg[] = "  FAIL: hang -- a wait on the GS never returned\n";
	(void)sig;
	if (write(1, msg, sizeof(msg) - 1) < 0) { }
	_exit(1);
}

/* mtgs_main_loop: consume what the eventcount says is there. */
static void drain(int flush_all)
{
	for (;;)
	{
		if (flush_all)
		{
			if (!work_eventcount_check(&ev))
				return;
		}
		else if (!work_eventcount_wait_timed(&ev, 100))
			return;
		if (sthread_get_current_thread_id() == producer)
			retro_atomic_store_release_int(&ran_on_producer, 1);
		while (retro_atomic_load_acquire_int(&pending) > 0)
			retro_atomic_fetch_sub_int(&pending, 1);
		if (!flush_all)
			return; /* a vsync ends the frontend's pass */
	}
}

static int wait_drains(void)
{
	uintptr_t self = sthread_get_current_thread_id();
#if RULE == 0
	return MTGS_WAIT_DRAINS(self, producer, 0);
#elif RULE == 1
	return owner == 0 || owner == self;
#else
	(void)self;
	return 1;
#endif
}

/* mtgs_wait_gs(false) */
static void wait_gs(void)
{
	work_eventcount_notify(&ev);
	if (wait_drains())
		drain(1);
	else
		work_eventcount_wait_empty(&ev);
}

static void produce(int n)
{
	retro_atomic_fetch_add_int(&pending, n);
	work_eventcount_notify(&ev);
}

static void ee_thread(void *u)
{
	int f;
	(void)u;
	producer = sthread_get_current_thread_id();
	while (!retro_atomic_load_acquire_int(&ee_resumed))
		sthread_yield();
	produce(100);
	retro_atomic_store_release_int(&ee_waiting, 1);
	wait_gs();                            /* before any retro_run */
	for (f = 0; f < FRAMES; f++)
	{
		produce(8);
		if ((f % 16) == 0)
			wait_gs();                    /* a full path buffer, a readback */
	}
	retro_atomic_store_release_int(&ee_done, 1);
}

static void video_thread(void *u)
{
	(void)u;
	owner = sthread_get_current_thread_id();            /* create_device: TryOpenGS */
	retro_atomic_store_release_int(&ee_resumed, 1);     /* context_reset            */
}

int main(void)
{
	sthread_t *ee, *video;
	int first = 1;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("mtgsown (rule %d)\n", RULE);
	signal(SIGALRM, on_alarm);
	alarm(5);

	work_eventcount_init(&ev);
	ee    = sthread_create(ee_thread, NULL);
	video = sthread_create(video_thread, NULL);
	sthread_join(video);
	/* The hang needs the EE parked first; on a loaded host it might not
	 * be yet, and then the old rule would pass by luck. */
	while (!retro_atomic_load_acquire_int(&ee_waiting))
		sthread_yield();

	/* retro_run, until the EE has finished */
	while (!retro_atomic_load_acquire_int(&ee_done))
	{
#if RULE != 1
		owner = sthread_get_current_thread_id();        /* claim, first     */
#endif
		if (first)
			wait_gs();                                  /* update_av_info   */
#if RULE == 1
		owner = sthread_get_current_thread_id();        /* claim, late      */
#endif
		first = 0;
		drain(0);
	}
	sthread_join(ee);
	alarm(0);

	if (retro_atomic_load_acquire_int(&ran_on_producer))
	{
		printf("  FAIL: the ring was drained on the producer thread\n");
		return 1;
	}
	printf("  ok: every wait returned, %d frames; the ring never ran on the producer\n", FRAMES);
	return 0;
}
