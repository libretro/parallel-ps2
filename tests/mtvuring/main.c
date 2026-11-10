/* MTVU ring pointer protocol, modelled exactly, hammered concurrently.
 *
 * This now models the FIXED protocol, which MTVU.c carries; FIXED=0
 * builds the one it replaced, which fails in a few thousand packets.
 *
 * WHAT THIS IS FOR
 *
 * MTVU.c's wait on size carries a FIXME: "there is a bug somewhere in
 * the queue pointer management. It creates a deadlock/corruption in SotC
 * intro. I added a 4KB safety net which seem to avoid to trigger the
 * bug. Note: a wait lock instead of a yield also helps to avoid the
 * bug." The safety net is still there and the yield is still there, and
 * between them the EE thread spins instead of parking whenever MTVU
 * falls behind -- on a four-thread machine, stealing cycles from the
 * very thread it is waiting for.
 *
 * The bug reproduces in one game's intro, which is not a thing a test
 * can run. What a test can run is the protocol itself: the ring is a
 * plain linear buffer with an explicit wrap packet, two positions
 * published as atomics, and four decision points. All of it is
 * reproducible without a GS, a BIOS or a disc.
 *
 * THE PROTOCOL, AS THE CODE HAS IT
 *
 *   reader: while (m_ato_read_pos != m_ato_write_pos) { consume; }
 *           publishes m_ato_read_pos after each whole command
 *   writer: ReserveSpace(size):
 *             if (m_write_pos + size > buffer_size - 1)
 *                 WaitOnSize(1); write NULL; m_write_pos = 0; publish
 *             WaitOnSize(size)
 *           WaitOnSize(size):
 *             readPos = m_ato_read_pos
 *             if (readPos <= m_write_pos)                break
 *             if (readPos > m_write_pos + size + 4096)   break
 *             else yield and retry
 *
 * Equality of the two positions means EMPTY to the reader. So the
 * writer must never advance its position onto the reader's, and the
 * 4096 is what keeps it away. This checks that it does:
 *
 *   - every byte the writer puts in is the byte the reader takes out,
 *     in order, with a per-slot sequence number
 *   - the writer never writes into the region between the published
 *     read position and itself
 *   - the two positions never become equal while data is outstanding,
 *     which is the empty/full ambiguity the 4KB hides
 *
 * WITH_GUARD=0 removes the guard band to show what it is protecting
 * against.
 *
 * WHAT IT FINDS
 *
 * The first test is "readPos <= m_write_pos -> break", read as "the
 * reader is behind us, so everything from here to the end is free".
 * When the two positions are equal that reading is one of two
 * possibilities and the other one is the opposite: a ring whose writer
 * has come all the way around onto the reader is full, not empty. The
 * writer takes the empty branch either way, and because that test comes
 * first, the guard band below it is never consulted.
 *
 * So a writer that gets ahead -- at startup, or any time the reader
 * stalls -- writes the whole ring, wraps to 0, finds the reader still
 * at 0, and goes around again. The reader then reads whatever is under
 * it: this harness sees its first packet already carrying sequence
 * number 118424, and a length field that walks it out of the buffer
 * entirely. The emulator has no bounds check there.
 *
 * The guard band narrows the window rather than closing it, which is
 * exactly what "seem to avoid to trigger the bug" describes. A fix has
 * to make full and empty distinguishable -- a count of outstanding
 * words alongside the positions, or a slot that is never written --
 * and the yield in WaitOnSize can become a park once it is.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_asym_eventcount.h>

#ifndef FIXED
#define FIXED 1
#endif

#ifndef WITH_GUARD
#define WITH_GUARD 1
#endif

/* Small enough that wraps happen constantly, same shape as the real one. */
#define RING_WORDS   4096
#define GUARD_WORDS  (WITH_GUARD ? 64 : 0)   /* the 4KB, to scale */
#define NULL_PACKET  0xFFFFFFFFu

static uint32_t          ring[RING_WORDS];
static retro_atomic_int_t ato_read_pos;
static retro_atomic_int_t ato_write_pos;
static int                write_pos;         /* writer-local */
static int                read_pos;          /* reader-local */
static retro_atomic_int_t done;
static retro_asym_eventcount_t ec_space;   /* MTVU.c ecRingSpace */

/* What the checker needs: the writer's next sequence number and the
 * reader's expected one. A mismatch is corruption. */
static uint32_t next_seq = 1;
static uint32_t expect_seq = 1;
static retro_atomic_int_t corruption;
static retro_atomic_int_t overlaps;
static retro_atomic_int_t equal_while_full;
static retro_atomic_int_t ran_off;
static long long          packets;
/* The reader starts only once the writer has filled the ring: it has
 * wrapped, or found no room and had to wait. That is the stalled reader
 * the old protocol wraps onto, every run rather than by timing. */
static retro_atomic_int_t writer_filled;

static int get_read_pos(void)  { return retro_atomic_load_acquire_int(&ato_read_pos); }
static int get_write_pos(void) { return retro_atomic_load_acquire_int(&ato_write_pos); }

#if FIXED
/* One word is never written, so the two positions coincide only when the
 * ring is empty -- which is what the reader's loop already assumes. The
 * writer's whole job is then to never land on the reader:
 *
 *   reader ahead of us: the run ends at readPos, so size must fit
 *                       strictly before it
 *   reader behind us:   the run ends at the buffer, and if size does not
 *                       fit there we must wrap, which puts us at 0 -- so
 *                       the reader must not be at 0, and size must fit
 *                       strictly before it once we are there
 *
 * The shipped code has no equivalent of that last clause, which is why a
 * writer that gets ahead wraps round onto a reader that has not moved. */
static int space_state(int size, int *need_wrap)
{
   int readPos = get_read_pos();
   *need_wrap = 0;
   if (readPos > write_pos)
      return (write_pos + size < readPos);
   if (write_pos + size <= RING_WORDS - 1)
      return 1;
   *need_wrap = 1;
   return (readPos > 0 && size < readPos);
}

/* Returns 1 when the caller must write the wrap packet first. */
#ifndef SPIN_BUDGET
#define SPIN_BUDGET 8
#endif

static int wait_on_size(int size)
{
   int need_wrap;
   int spins = 0;
   for (;;)
   {
      if (space_state(size, &need_wrap))
         return need_wrap;
      if (retro_atomic_load_acquire_int(&ran_off))
         return 0;
      retro_atomic_store_release_int(&writer_filled, 1);
      /* MTVU.c's wait: a spin budget, then park on the eventcount the
       * reader notifies after publishing. A missed notify shows up here
       * as a hang, which is the failure this models. */
      if (spins < SPIN_BUDGET)
      {
         spins++;
         sthread_yield();
      }
      else
      {
         int key = retro_asym_eventcount_prepare_wait(&ec_space);
         if (space_state(size, &need_wrap))
         {
            retro_asym_eventcount_cancel_wait(&ec_space);
            return need_wrap;
         }
         retro_asym_eventcount_commit_wait(&ec_space, key);
      }
   }
}
#else
/* MTVU.c mtvu_wait_on_size, transcribed. */
static void wait_on_size(int size)
{
   for (;;)
   {
      int readPos = get_read_pos();
      if (readPos <= write_pos)
         break;
      if (readPos > write_pos + size + GUARD_WORDS)
         break;
      if (retro_atomic_load_acquire_int(&ran_off))
         break;
      sthread_yield();
   }
}
#endif

/* Equal positions mean empty to the reader, so a writer position about
 * to be published that lands on the reader's loses the whole ring. Read
 * before publishing: the reader cannot pass what is not yet published,
 * so equality here is the full ring, while after publishing the reader
 * may have consumed everything and caught up, which is empty and equal
 * for good reason. */
static void check_landing(void)
{
   if (write_pos == get_read_pos())
      retro_atomic_fetch_add_int(&equal_while_full, 1);
}

static void reserve_space(int size)
{
#if FIXED
   /* One call decides whether a wrap is needed AND waits for the space
    * the wrap will land in, so the two cannot disagree. */
   if (wait_on_size(size))
   {
      ring[write_pos] = NULL_PACKET;
      write_pos = 0;
      check_landing();
      retro_atomic_store_release_int(&ato_write_pos, write_pos);
      retro_atomic_store_release_int(&writer_filled, 1);
   }
#else
   if (write_pos + size > (RING_WORDS - 1))
   {
      wait_on_size(1);
      ring[write_pos] = NULL_PACKET;
      write_pos = 0;
      check_landing();
      retro_atomic_store_release_int(&ato_write_pos, write_pos);
      retro_atomic_store_release_int(&writer_filled, 1);
   }
   wait_on_size(size);
#endif
}

/* A packet is [len][seq][payload...], len words total. */
static void writer(void *data)
{
   long long n = *(long long*)data;
   long long i;
   unsigned r = 12345;
   for (i = 0; i < n; i++)
   {
      int size, k, before;
      r = r * 1103515245u + 12345u;
      size = 3 + (int)((r >> 16) % 61);        /* 3..63 words */
      reserve_space(size);
      before = get_read_pos();
      /* The writer is about to occupy [write_pos, write_pos + size).
       * If the published read position is inside that, the reader is
       * still reading what we are about to overwrite. */
      if (before > write_pos && before < write_pos + size)
      {
         if (retro_atomic_fetch_add_int(&overlaps, 1) == 0)
            printf("    first overlap: readPos=%d writePos=%d size=%d (packet %lld)\n",
                   before, write_pos, size, (long long)i);
      }
      {
         uint32_t seq = next_seq++;
         ring[write_pos] = (uint32_t)size;
         ring[write_pos + 1] = seq;
         for (k = 2; k < size; k++)
            ring[write_pos + k] = seq * 7u + (uint32_t)k;
      }
      write_pos += size;
      check_landing();
      retro_atomic_store_release_int(&ato_write_pos, write_pos);
   }
   retro_atomic_store_release_int(&writer_filled, 1);
   retro_atomic_store_release_int(&done, 1);
}

static void reader(void *data)
{
   (void)data;
   while (!retro_atomic_load_acquire_int(&writer_filled)
       && !retro_atomic_load_acquire_int(&done))
      sthread_yield();
   for (;;)
   {
      while (retro_atomic_load_acquire_int(&ato_read_pos) != get_write_pos())
      {
         uint32_t len;
         if (read_pos < 0 || read_pos >= RING_WORDS)
         {
            /* Following a corrupted length walked out of the ring. In
             * the emulator this is a wild read into whatever follows
             * the buffer; here it is a reported failure. */
            retro_atomic_fetch_add_int(&corruption, 1);
            retro_atomic_store_release_int(&ran_off, 1);
            return;
         }
         len = ring[read_pos];
         if (len != NULL_PACKET && (len < 3 || read_pos + (int)len > RING_WORDS))
         {
            retro_atomic_fetch_add_int(&corruption, 1);
            retro_atomic_store_release_int(&ran_off, 1);
            return;
         }
         if (len == NULL_PACKET)
         {
            read_pos = 0;
         }
         else
         {
            uint32_t seq = ring[read_pos + 1];
            uint32_t k;
            if (seq != expect_seq)
            {
               if (retro_atomic_fetch_add_int(&corruption, 1) == 0)
                  printf("    first bad seq: got %u want %u at read_pos=%d writePos=%d len=%u\n",
                         seq, expect_seq, read_pos, get_write_pos(), len);
            }
            for (k = 2; k < len; k++)
            {
               if (ring[read_pos + k] != seq * 7u + k)
               {
                  retro_atomic_fetch_add_int(&corruption, 1);
                  break;
               }
            }
            expect_seq = seq + 1;
            read_pos += (int)len;
            packets++;
         }
         retro_atomic_store_release_int(&ato_read_pos, read_pos);
         retro_asym_eventcount_notify(&ec_space);
      }
      retro_asym_eventcount_notify(&ec_space);
      if (retro_atomic_load_acquire_int(&done) &&
          retro_atomic_load_acquire_int(&ato_read_pos) == get_write_pos())
         break;
      sthread_yield();
   }
}

int main(int argc, char **argv)
{
   long long n = (argc > 1) ? atoll(argv[1]) : 2000000;
   sthread_t *w, *rd;
   int bad;

   setvbuf(stdout, NULL, _IONBF, 0);
   printf("mtvu ring: %lld packets, ring %d words, %s\n",
          n, RING_WORDS, FIXED ? "one slot reserved, wrap checked" : (GUARD_WORDS ? "positions + guard band" : "positions, no guard"));

   if (!retro_asym_eventcount_init(&ec_space))
   {
      printf("  FAIL: eventcount init\n");
      return 1;
   }
   rd = sthread_create(reader, NULL);
   w  = sthread_create(writer, &n);
   if (!w || !rd)
   {
      printf("  FAIL: thread creation\n");
      return 1;
   }
   sthread_join(w);
   sthread_join(rd);

   if (retro_atomic_load_acquire_int(&ran_off))
      printf("  the reader followed a corrupted length out of the ring\n");
   printf("  %lld packets read, %d corrupt, %d writer-into-unread, %d positions equal while full\n",
          packets, retro_atomic_load_acquire_int(&corruption),
          retro_atomic_load_acquire_int(&overlaps),
          retro_atomic_load_acquire_int(&equal_while_full));
   bad = retro_atomic_load_acquire_int(&corruption)
       | retro_atomic_load_acquire_int(&overlaps)
       | retro_atomic_load_acquire_int(&equal_while_full);
   if (packets != n)
   {
      printf("  FAIL: %lld packets written, %lld read\n", n, packets);
      bad = 1;
   }
   retro_asym_eventcount_free(&ec_space);
   printf(bad ? "mtvu ring: FAILED\n" : "mtvu ring: ok\n");
   return bad ? 1 : 0;
}
