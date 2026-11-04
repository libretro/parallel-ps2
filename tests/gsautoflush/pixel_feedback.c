/* Auto flush lets overlaps through for a batch that reads only its own
 * pixels, and for no other.
 *
 * A primitive that samples its own frame where an earlier primitive of the
 * same batch drew has to see that write. Auto flush ends the batch there.
 * The hardware renderer draws a batch whose primitives each read only the
 * pixels they write in place (tex-is-fb, with a barrier between
 * overlapping primitives), which sees every earlier write; for such a
 * batch the flush is not needed. GSState keeps, per batch, whether every
 * primitive so far is one of those (GSState::IsPixelFeedbackPrim, the
 * first primitive of a batch included) and lets an overlap through only
 * then (GSState::AutoFlushOverlap); a primitive that is not, arriving
 * after one was let through, flushes before it.
 *
 * Pinned here over random sequences of primitives, pixel-local or not,
 * overlapping an earlier one of the batch or not: every overlap left
 * inside a batch is in a batch of pixel-local primitives only, and there
 * are never more draws than flushing at every overlap. Negative controls:
 * not classifying a batch's first primitive, and not flushing a primitive
 * that is not pixel-local after an overlap was let through.
 *
 * Build and run, from tests/gsautoflush:
 *   cc -O2 -std=c89 -pedantic -Wall pixel_feedback.c -o pixel_feedback
 *   ./pixel_feedback
 */
#include <stdio.h>

#define PRIMS 64

enum { RULE, FIRST_UNCLASSIFIED, NO_FLUSH_AFTER_SKIP };

typedef struct
{
   int tail;         /* primitives in the batch */
   int pixel_batch;  /* m_af_pixel_batch */
   int skipped;      /* m_af_skipped */
   int all_pixel;    /* truth: every primitive of the batch is pixel-local */
   int overlap_kept; /* truth: an overlap was left in the batch */
   int draws;
   int bad;          /* an overlap kept in a batch with a non-pixel primitive */
} batch_t;

static unsigned rng;
static unsigned next(void)
{
   rng = rng * 1103515245u + 12345u;
   return (rng >> 16) & 0x7fffu;
}

static void flush(batch_t* b)
{
   if (b->tail == 0)
      return;
   if (b->overlap_kept && !b->all_pixel)
      b->bad = 1;
   b->draws++;
   b->tail = 0;
   b->pixel_batch = 1;
   b->skipped = 0;
   b->all_pixel = 1;
   b->overlap_kept = 0;
}

static void add(batch_t* b, int pixel)
{
   b->tail++;
   if (!pixel)
      b->all_pixel = 0;
}

/* HandleAutoFlush's order for one primitive; overlaps says whether it
 * reads where an earlier primitive of the batch drew. */
static void kick(batch_t* b, int pixel, int overlaps, int mode)
{
   if (b->tail == 0)
   {
      if (mode != FIRST_UNCLASSIFIED)
         b->pixel_batch = pixel;
      add(b, pixel);
      return;
   }
   if (!pixel)
   {
      if (b->skipped && mode != NO_FLUSH_AFTER_SKIP)
      {
         flush(b);
         b->pixel_batch = 0;
         add(b, pixel);
         return;
      }
      b->pixel_batch = 0;
   }
   if (overlaps)
   {
      if (pixel && b->pixel_batch)
      {
         b->skipped = 1;
         b->overlap_kept = 1;
         add(b, pixel);
         return;
      }
      flush(b);
      b->pixel_batch = pixel;
   }
   add(b, pixel);
}

static int run(unsigned seed, int mode, int* draws_new, int* draws_old)
{
   batch_t b, o;
   int i, pixel, overlaps;

   rng = seed;
   b.tail = 0; b.pixel_batch = 1; b.skipped = 0; b.all_pixel = 1; b.overlap_kept = 0; b.draws = 0; b.bad = 0;
   o = b;
   for (i = 0; i < PRIMS; i++)
   {
      pixel = (next() % 4) != 0;
      overlaps = (next() % 3) == 0;
      kick(&b, pixel, overlaps, mode);
      /* Before: every overlap flushed. */
      if (o.tail && overlaps)
         flush(&o);
      add(&o, pixel);
   }
   flush(&b);
   flush(&o);
   *draws_new += b.draws;
   *draws_old += o.draws;
   return !b.bad && b.draws <= o.draws;
}

int main(void)
{
   unsigned seed;
   int bad[3] = { 0, 0, 0 };
   int dn = 0, d_old = 0, x = 0, y = 0;
   int m;

   for (seed = 1; seed <= 20000; seed++)
   {
      bad[RULE] += !run(seed, RULE, &dn, &d_old);
      for (m = 1; m < 3; m++)
         bad[m] += !run(seed, m, &x, &y);
   }

   printf("pixel_feedback\n");
   printf("  %s: overlaps kept only in all-pixel batches, never more draws, 20000 sequences: %d wrong\n",
      bad[RULE] ? "FAIL" : "ok", bad[RULE]);
   printf("  draws: %d against %d flushing at every overlap\n", dn, d_old);
   printf("  %s: negative control, first primitive unclassified: %d wrong\n",
      bad[FIRST_UNCLASSIFIED] ? "ok" : "FAIL", bad[FIRST_UNCLASSIFIED]);
   printf("  %s: negative control, no flush after an overlap let through: %d wrong\n",
      bad[NO_FLUSH_AFTER_SKIP] ? "ok" : "FAIL", bad[NO_FLUSH_AFTER_SKIP]);
   if (bad[RULE] || !bad[FIRST_UNCLASSIFIED] || !bad[NO_FLUSH_AFTER_SKIP])
   {
      printf("pixel_feedback: FAILED\n");
      return 1;
   }
   printf("pixel_feedback: ok\n");
   return 0;
}
