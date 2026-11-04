/* A target leaving the texture cache leaves what it drew in local memory.
 *
 * On the GS a page keeps the last thing written to it -- a draw or a
 * transfer -- until something writes it again, and a game can read it any
 * time later: a buffer drawn into once and sampled frames afterwards. The
 * texture cache holds drawn pages on the GPU only, until a read takes them
 * back. When it drops a target, by age or over its memory budget,
 * GSTextureCache::DropTarget writes back the pages the target drew since
 * its last read, except the pages a transfer has written since, the pages
 * a target drawn later still holds, the pages a later target dropped in
 * the same pass has already taken, and the pages a read has put newer
 * pixels in -- the last of these is how it knows of a newer target that
 * is gone. Drops are batched, and a full batch is written out before the
 * next target is looked at.
 *
 * Pinned here: over random sequences of draws, transfers and drops, every
 * page no live target holds the latest write of reads, in local memory,
 * the latest write -- whatever order the drops come in and wherever the
 * batch boundaries fall. Negative controls: dropping without writing back
 * (pages lost), writing back every drawn page in drop order (an older
 * target's pixels over a newer one's), and the rule without the read
 * stamps (an older target over pixels a newer one, since gone, left).
 *
 * Build and run, from tests/gstc:
 *   cc -O2 -std=c89 -pedantic -Wall drop_writeback.c -o drop_writeback
 *   ./drop_writeback
 */
#include <stdio.h>

#define PAGES 24
#define TARGETS 6
#define BATCH 2

typedef struct
{
   int live;
   int last_draw;
   int drew[PAGES];   /* drew this page at last_draw */
   int dirty[PAGES];  /* a transfer wrote the page after last_draw */
   int unread[PAGES]; /* drawn since the last read */
} target_t;

static unsigned rng = 12345u;
static unsigned next(void)
{
   rng = rng * 1103515245u + 12345u;
   return (rng >> 16) & 0x7fffu;
}

/* The latest write of each page: positive for a target's draw (target
 * index + 1, times 1000, plus the draw), negative for a transfer. */
static int truth[PAGES];
static int mem[PAGES];
/* The last draw of the newest target pixels a read put in each page. */
static int read_draw[PAGES];
static target_t tg[TARGETS];

enum { RULE, NO_WRITEBACK, ALL_IN_ORDER, NO_READ_STAMPS };

typedef struct
{
   int pending_target[BATCH * PAGES];
   int pending_page[BATCH * PAGES];
   int pending;
   int dropped[BATCH];
   int count;
   int page_draw[PAGES];
} drop_t;

static void flush(drop_t* d)
{
   int i;
   for (i = 0; i < d->pending; i++)
   {
      const int t = d->pending_target[i], p = d->pending_page[i];
      mem[p] = (t + 1) * 1000 + tg[t].last_draw;
      if (read_draw[p] < tg[t].last_draw)
         read_draw[p] = tg[t].last_draw;
   }
   d->pending = 0;
   d->count = 0;
}

static int keeps_page(drop_t* d, int t, int p, int mode)
{
   int o;
   if (tg[t].dirty[p])
      return 0;
   for (o = 0; o < TARGETS; o++)
      if (o != t && tg[o].live && tg[o].last_draw > tg[t].last_draw && tg[o].drew[p])
         return 0;
   if (d->page_draw[p] > tg[t].last_draw || (mode != NO_READ_STAMPS && read_draw[p] > tg[t].last_draw))
      return 0;
   d->page_draw[p] = tg[t].last_draw;
   return 1;
}

static void drop(drop_t* d, int t, int mode)
{
   int p;
   if (d->count == BATCH)
      flush(d);
   tg[t].live = 0;
   for (p = 0; p < PAGES; p++)
   {
      if (!tg[t].unread[p] || mode == NO_WRITEBACK)
         continue;
      if (mode == ALL_IN_ORDER ? !tg[t].dirty[p] : keeps_page(d, t, p, mode))
      {
         d->pending_target[d->pending] = t;
         d->pending_page[d->pending] = p;
         d->pending++;
      }
   }
   d->dropped[d->count++] = t;
}

/* One random history; returns whether local memory came out right. */
static int run(int mode, unsigned seed)
{
   drop_t d;
   int t, p, step, n, order[TARGETS], i, j, tmp, owner, ok;

   rng = seed;
   for (p = 0; p < PAGES; p++)
   {
      truth[p] = 0;
      mem[p] = 0;
      read_draw[p] = -1;
      d.page_draw[p] = -1;
   }
   for (t = 0; t < TARGETS; t++)
   {
      tg[t].live = 1;
      tg[t].last_draw = 0;
      for (p = 0; p < PAGES; p++)
         tg[t].drew[p] = tg[t].dirty[p] = tg[t].unread[p] = 0;
   }

   /* Each target draws once, over a run of pages, in a random order of
    * draws; transfers fall between them. */
   for (step = 1; step <= TARGETS * 3; step++)
   {
      if (next() % 3)
      {
         int first = (int)(next() % PAGES), len = 1 + (int)(next() % 6);
         t = (int)(next() % TARGETS);
         if (tg[t].last_draw)
            continue;
         tg[t].last_draw = step;
         for (p = first; p < first + len && p < PAGES; p++)
         {
            tg[t].drew[p] = 1;
            tg[t].unread[p] = (next() % 4) != 0;
            truth[p] = (t + 1) * 1000 + step;
            if (!tg[t].unread[p])
            {
               mem[p] = truth[p];
               read_draw[p] = step;
            }
         }
      }
      else
      {
         p = (int)(next() % PAGES);
         truth[p] = -step;
         mem[p] = -step;
         for (t = 0; t < TARGETS; t++)
            if (tg[t].drew[p] && tg[t].last_draw)
               tg[t].dirty[p] = 1;
      }
   }

   /* Drop a random subset in a random order. */
   for (t = 0; t < TARGETS; t++)
      order[t] = t;
   for (i = TARGETS - 1; i > 0; i--)
   {
      j = (int)(next() % (unsigned)(i + 1));
      tmp = order[i]; order[i] = order[j]; order[j] = tmp;
   }
   n = (int)(next() % (TARGETS + 1));
   d.pending = 0;
   d.count = 0;
   for (i = 0; i < n; i++)
      if (tg[order[i]].last_draw)
         drop(&d, order[i], mode);
   flush(&d);

   ok = 1;
   for (p = 0; p < PAGES; p++)
   {
      owner = truth[p] > 0 ? truth[p] / 1000 - 1 : -1;
      if (owner >= 0 && tg[owner].live)
         continue;
      if (mem[p] != truth[p])
         ok = 0;
   }
   return ok;
}

int main(void)
{
   unsigned seed;
   int rule_bad = 0, none_bad = 0, order_bad = 0, stamp_bad = 0;

   for (seed = 1; seed <= 20000; seed++)
   {
      rule_bad += !run(RULE, seed);
      none_bad += !run(NO_WRITEBACK, seed);
      order_bad += !run(ALL_IN_ORDER, seed);
      stamp_bad += !run(NO_READ_STAMPS, seed);
   }

   printf("drop_writeback\n");
   printf("  %s: the rule, 20000 histories: %d wrong\n", rule_bad ? "FAIL" : "ok", rule_bad);
   printf("  %s: negative control, no write-back: %d wrong\n", none_bad ? "ok" : "FAIL", none_bad);
   printf("  %s: negative control, every drawn page in drop order: %d wrong\n", order_bad ? "ok" : "FAIL", order_bad);
   printf("  %s: negative control, no read stamps: %d wrong\n", stamp_bad ? "ok" : "FAIL", stamp_bad);
   if (rule_bad || !none_bad || !order_bad || !stamp_bad)
   {
      printf("drop_writeback: FAILED\n");
      return 1;
   }
   printf("drop_writeback: ok\n");
   return 0;
}
