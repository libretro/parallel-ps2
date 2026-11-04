/* A parked target's pixels reach local memory before anything reads them.
 *
 * A target dropped by age keeps its texture, out of the cache's lists:
 * what it drew goes to local memory only when that memory is read. Before
 * a read of a range, GSTextureCache::FlushParked writes back the parked
 * targets over the range, plus every parked target drawn after one of
 * those over its memory, oldest first, each by the drop rule (not pages a
 * transfer has written since, not pages a live target drawn later holds,
 * not pages holding newer pixels). A write over a parked target's pages
 * retires them instead (GSTextureCache::RetireParked).
 *
 * Pinned here: over random histories of draws, transfers, parks and
 * reads, every read of a page that no live target holds the latest write
 * of sees the latest write. Negative controls: reads that flush nothing,
 * a flush without the pass record and the read stamps, and writes that
 * retire nothing.
 *
 * Build and run, from tests/gstc:
 *   cc -O2 -std=c89 -pedantic -Wall park_flush.c -o park_flush
 *   ./park_flush
 */
#include <stdio.h>

#define PAGES 24
#define TARGETS 6

typedef struct
{
   int state; /* 0 never drawn, 1 live, 2 parked, 3 gone */
   int last_draw;
   int lo, hi; /* the page range it covers */
   int dirty[PAGES];
   int unread[PAGES];
} target_t;

enum { RULE, NO_FLUSH, NO_ORDER, NO_RETIRE };

static unsigned rng;
static unsigned next(void)
{
   rng = rng * 1103515245u + 12345u;
   return (rng >> 16) & 0x7fffu;
}

static int truth[PAGES];
static int mem[PAGES];
static int read_draw[PAGES];
static target_t tg[TARGETS];

static int value(int t)
{
   return (t + 1) * 1000 + tg[t].last_draw;
}

static int overlaps(int a, int lo, int hi)
{
   return tg[a].lo <= hi && lo <= tg[a].hi;
}

/* The drop rule for page p of t, as DropKeepsPage has it; page_draw is the
 * pass's record of pages taken. */
static int keeps(int t, int p, int* page_draw, int mode)
{
   int o;
   if (tg[t].dirty[p] || !tg[t].unread[p])
      return 0;
   for (o = 0; o < TARGETS; o++)
      if (o != t && tg[o].state == 1 && tg[o].last_draw > tg[t].last_draw && p >= tg[o].lo && p <= tg[o].hi)
         return 0;
   if (mode != NO_ORDER && (page_draw[p] > tg[t].last_draw || read_draw[p] > tg[t].last_draw))
      return 0;
   page_draw[p] = tg[t].last_draw;
   return 1;
}

static void flush(int lo, int hi, int mode)
{
   int t, p, i, page_draw[PAGES];
   int pend_t[TARGETS * PAGES], pend_p[TARGETS * PAGES], pend = 0;

   for (p = 0; p < PAGES; p++)
      page_draw[p] = -1;
   for (t = 0; t < TARGETS; t++)
   {
      if (tg[t].state != 2 || !overlaps(t, lo, hi))
         continue;
      for (p = tg[t].lo; p <= tg[t].hi; p++)
         if (keeps(t, p, page_draw, mode))
         {
            pend_t[pend] = t;
            pend_p[pend] = p;
            pend++;
         }
      tg[t].state = 3;
   }
   for (i = 0; i < pend; i++)
   {
      mem[pend_p[i]] = value(pend_t[i]);
      if (read_draw[pend_p[i]] < tg[pend_t[i]].last_draw)
         read_draw[pend_p[i]] = tg[pend_t[i]].last_draw;
   }
}

/* A read of [lo, hi]: right where every page either reads its latest write
 * or a live target holds that write. */
static int read_ok(int lo, int hi)
{
   int p, owner;
   for (p = lo; p <= hi; p++)
   {
      owner = truth[p] > 0 ? truth[p] / 1000 - 1 : -1;
      if (owner >= 0 && tg[owner].state == 1)
         continue;
      if (mem[p] != truth[p])
         return 0;
   }
   return 1;
}

static int run(int mode, unsigned seed)
{
   int t, p, step, lo, hi, len, ok = 1;

   rng = seed;
   for (p = 0; p < PAGES; p++)
   {
      truth[p] = 0;
      mem[p] = 0;
      read_draw[p] = -1;
   }
   for (t = 0; t < TARGETS; t++)
   {
      tg[t].state = 0;
      tg[t].last_draw = 0;
      for (p = 0; p < PAGES; p++)
         tg[t].dirty[p] = tg[t].unread[p] = 0;
   }

   for (step = 1; step <= 40; step++)
   {
      switch (next() % 4)
      {
         case 0: /* a target draws once, over a run of pages */
            t = (int)(next() % TARGETS);
            if (tg[t].state != 0)
               break;
            lo = (int)(next() % PAGES);
            len = 1 + (int)(next() % 6);
            hi = lo + len - 1 < PAGES ? lo + len - 1 : PAGES - 1;
            tg[t].state = 1;
            tg[t].last_draw = step;
            tg[t].lo = lo;
            tg[t].hi = hi;
            for (p = lo; p <= hi; p++)
            {
               tg[t].unread[p] = (next() % 4) != 0;
               truth[p] = value(t);
               if (!tg[t].unread[p])
               {
                  mem[p] = truth[p];
                  read_draw[p] = step;
               }
            }
            break;
         case 1: /* a transfer writes a page */
            p = (int)(next() % PAGES);
            truth[p] = -step;
            mem[p] = -step;
            for (t = 0; t < TARGETS; t++)
               if ((tg[t].state == 1 || (tg[t].state == 2 && mode != NO_RETIRE)) && p >= tg[t].lo && p <= tg[t].hi)
                  tg[t].dirty[p] = 1;
            break;
         case 2: /* a live target ages out */
            t = (int)(next() % TARGETS);
            if (tg[t].state == 1)
               tg[t].state = 2;
            break;
         default: /* a read of a range */
            lo = (int)(next() % PAGES);
            hi = lo + (int)(next() % 4);
            if (hi >= PAGES)
               hi = PAGES - 1;
            if (mode != NO_FLUSH)
               flush(lo, hi, mode);
            if (!read_ok(lo, hi))
               ok = 0;
            break;
      }
   }
   return ok;
}

int main(void)
{
   unsigned seed;
   int bad[4] = { 0, 0, 0, 0 };
   int m;

   for (seed = 1; seed <= 20000; seed++)
      for (m = 0; m < 4; m++)
         bad[m] += !run(m, seed);

   printf("park_flush\n");
   printf("  %s: the rule, 20000 histories: %d wrong\n", bad[RULE] ? "FAIL" : "ok", bad[RULE]);
   printf("  %s: negative control, reads flush nothing: %d wrong\n", bad[NO_FLUSH] ? "ok" : "FAIL", bad[NO_FLUSH]);
   printf("  %s: negative control, no pass record or read stamps: %d wrong\n", bad[NO_ORDER] ? "ok" : "FAIL", bad[NO_ORDER]);
   printf("  %s: negative control, writes retire nothing: %d wrong\n", bad[NO_RETIRE] ? "ok" : "FAIL", bad[NO_RETIRE]);
   if (bad[RULE] || !bad[NO_FLUSH] || !bad[NO_ORDER] || !bad[NO_RETIRE])
   {
      printf("park_flush: FAILED\n");
      return 1;
   }
   printf("park_flush: ok\n");
   return 0;
}
