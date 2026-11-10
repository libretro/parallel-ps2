/* When the EE is let go at a vsync, what input its next frame reads, and
 * what the GS scans out.
 *
 * The EE posts a vsync to the GS ring and waits in WaitGS until the GS
 * has consumed it; mtgs_main_loop, on the frontend's thread, consumes
 * it, takes its copy of the privileged registers and goes idle, which
 * releases WaitGS. The EE then waits at the frame gate. The next
 * retro_run polls the input, latches it and opens the gate, and the EE
 * runs the frame that retro_run presents: every frame is drawn from the
 * input of the retro_run that shows it, and between two retro_runs the
 * EE starts nothing.
 *
 * Modelled here on the real work eventcount (pcsx2/WorkEventCount.h)
 * and a gate built as pcsx2/MTGS.c builds it, with a producer thread
 * as the EE and this thread as the frontend. Every frame must hold:
 *   - the scanout reads the vsync's registers;
 *   - the frame presented was drawn from the input polled in the same
 *     call;
 *   - the producer starts no frame while the frontend is between calls.
 *
 * RULE selects the frontend's order:
 *   0  the tree's: poll, latch and open the gate, wait for the vsync,
 *      copy the registers, go idle, scan out and present
 *   1  no gate: latch at the vsync and let the producer run on through
 *      the present, so a frame reads the input of the call before
 *   2  open the gate first, then poll: the latch takes the input of the
 *      call before
 * build.sh runs 0 and requires 1 and 2 to fail.
 *
 * Build, from tests/mtgsvsync: see build.sh. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <retro_inline.h>
#include <retro_timers.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#include <retro_atomic.h>
#include "common/Pcsx2Types.h"
#include "WorkEventCount.h"

s32 WorkEventCount_SpinBudget(void) { return 0; } /* every wait parks */

#ifndef RULE
#define RULE 0
#endif

#define FRAMES 64
/* How long the frontend stays away between two calls, in milliseconds:
 * long enough for a producer that is free to run to start its next
 * frame. */
#define BETWEEN_MS 2

static WorkEventCount ev;

/* The privileged registers: written by the producer, read by the
 * consumer through its copy. One word stands for the display register a
 * game rewrites every frame. */
static retro_atomic_int_t live_regs;
static int regs_copy;

/* The input: the frontend's polled copy and the latched one the
 * producer reads, and for each frame the input it was drawn from. */
static int input_staged;
static int input_live;
static int drawn_from[FRAMES + 2];

/* The vsync record in the ring: posted by the producer, committed by
 * the consumer. WaitGS waits for the ring to drain. */
static retro_atomic_int_t ring_vsyncs;

/* The frame gate. */
static retro_eventcount_t gate_ec;
static retro_atomic_int_t gate_granted;
static retro_atomic_int_t gate_quit;
static int gate_taken;

static int ring_drained(void *ctx)
{
   (void)ctx;
   return retro_atomic_load_acquire_int(&ring_vsyncs) == 0;
}

#define GATE_OPEN() \
   (retro_atomic_load_acquire_int(&gate_granted) != gate_taken \
    || retro_atomic_load_acquire_int(&gate_quit))

#if RULE != 1
static void gate_wait(void)
{
   while (!GATE_OPEN())
   {
      int key = retro_eventcount_prepare_wait(&gate_ec);
      if (GATE_OPEN())
      {
         retro_eventcount_cancel_wait(&gate_ec);
         break;
      }
      retro_eventcount_commit_wait(&gate_ec, key);
   }
   if (retro_atomic_load_acquire_int(&gate_granted) != gate_taken)
      gate_taken++;
}
#endif

/* Latch the polled input, then let the producer through. */
static void gate_open(void)
{
   input_live = input_staged;
   retro_atomic_fetch_add_int(&gate_granted, 1);
   retro_eventcount_notify(&gate_ec);
}

static void producer_main(void *arg)
{
   int frame;
   (void)arg;
#if RULE != 1
   gate_wait(); /* paused until the first call lets it go */
#endif
   for (frame = 1; frame <= FRAMES + 1; frame++)
   {
      /* The frame: it reads the pad, and writes its registers. */
      drawn_from[frame] = input_live;
      retro_atomic_store_release_int(&live_regs, frame);
      /* The vsync, then WaitGS(false), then the gate. */
      retro_atomic_fetch_add_int(&ring_vsyncs, 1);
      work_eventcount_notify(&ev);
      work_eventcount_wait_drained(&ev, ring_drained, NULL);
      if (retro_atomic_load_acquire_int(&gate_quit))
         break;
#if RULE != 1
      gate_wait();
#endif
   }
}

int main(void)
{
   sthread_t *producer;
   int frame;
   int fail = 0;

   work_eventcount_init(&ev);
   retro_eventcount_init(&gate_ec);
   retro_atomic_store_relaxed_int(&live_regs, 0);
   retro_atomic_store_relaxed_int(&ring_vsyncs, 0);
   retro_atomic_store_relaxed_int(&gate_granted, 0);
   retro_atomic_store_relaxed_int(&gate_quit, 0);
   input_staged = input_live = 1000;

   producer = sthread_create(producer_main, NULL);
   if (!producer)
   {
      printf("cannot start the producer\n");
      return 1;
   }

   for (frame = 1; frame <= FRAMES; frame++)
   {
      const int polled = 1000 + frame;
      int scanned, ahead;

#if RULE == 0
      input_staged = polled;
      gate_open();
#elif RULE == 2
      gate_open();
      input_staged = polled;
#else
      input_staged = polled;
#endif

      /* MainLoop: wait for the vsync (the frame's only packet here). */
      while (!work_eventcount_wait_timed(&ev, 100))
         ;
      regs_copy = retro_atomic_load_acquire_int(&live_regs);
#if RULE == 1
      /* The latch at the vsync, for the frame the producer goes on to. */
      input_live = input_staged;
#endif
      retro_atomic_fetch_sub_int(&ring_vsyncs, 1);
      work_eventcount_drained(&ev, work_eventcount_epoch(&ev), 0);
      scanned = regs_copy;

      /* The present, the frontend's pacing, the time between calls. */
      retro_sleep(BETWEEN_MS);
      ahead = retro_atomic_load_acquire_int(&live_regs) != frame;

      if (scanned != frame)
      {
         if (fail < 3)
            printf("  frame %d: the scanout read the registers of frame %d\n", frame, scanned);
         fail++;
      }
      else if (drawn_from[frame] != polled)
      {
         if (fail < 3)
            printf("  frame %d: drawn from the input of call %d\n", frame, drawn_from[frame] - 1000);
         fail++;
      }
      else if (ahead)
      {
         if (fail < 3)
            printf("  frame %d: the EE started the next frame between calls\n", frame);
         fail++;
      }
   }

   /* The producer is at the gate, or in a WaitGS: let both return. */
   retro_atomic_store_release_int(&gate_quit, 1);
   retro_eventcount_notify(&gate_ec);
   work_eventcount_kill(&ev);
   sthread_join(producer);
   work_eventcount_free(&ev);
   retro_eventcount_free(&gate_ec);

   printf("a frame on its own call's input: %s (%d of %d frames wrong)\n", fail ? "FAIL" : "ok", fail, FRAMES);
   return fail ? 1 : 0;
}
