/* A batch read in place is drawn in groups no texel of which is both
 * fetched and written.
 *
 * On a device that samples its render target anywhere behind a texture
 * barrier (GL_ARB_texture_barrier, NV_texture_barrier), the hardware
 * renderer draws a batch auto flush split into runs as one draw reading
 * the target in place, in groups with a barrier before each
 * (GSRendererHW::RenderRunsInPlace). A fetch of a texel the same group
 * writes is undefined there, whichever primitive writes it, so a
 * primitive starts a new group when it reads where the group wrote (the
 * GS would have it see that write) or writes where the group read (the
 * GS would have the earlier read see the old texel); when the pixels
 * blend with what is under them, also where the group wrote. A primitive
 * whose read rect meets its own write rect cannot be drawn so at all,
 * and the batch goes back to a copy of the target.
 *
 * Pinned here over random batches of primitives reading one rect of a
 * small target and writing another: no group fetches a texel it writes
 * (nor, blending, writes one twice), and drawing each group from what the
 * groups before it left -- what a barrier gives -- leaves the target as
 * drawing the primitives one by one does. Negative controls: no new group
 * on a read of the group's writes, and none on a write over its reads.
 *
 * Build and run, from tests/gsautoflush:
 *   cc -O2 -std=c89 -pedantic -Wall runs_groups.c -o runs_groups
 *   ./runs_groups
 */
#include <stdio.h>
#include <string.h>

#define W 32
#define H 16
#define PRIMS 24

enum { RULE, NO_RAW, NO_WAR };

typedef struct { int x, y, z, w; } rect_t;
typedef struct { rect_t rd, wr; } prim_t;

static unsigned rng;
static unsigned next(void)
{
   rng = rng * 1103515245u + 12345u;
   return (rng >> 16) & 0x7fffu;
}

static int overlap(const rect_t* a, const rect_t* b)
{
   return a->x < b->z && b->x < a->z && a->y < b->w && b->y < a->w;
}

static rect_t runion(const rect_t* a, const rect_t* b)
{
   rect_t r;
   r.x = a->x < b->x ? a->x : b->x;
   r.y = a->y < b->y ? a->y : b->y;
   r.z = a->z > b->z ? a->z : b->z;
   r.w = a->w > b->w ? a->w : b->w;
   return r;
}

static rect_t random_rect(void)
{
   rect_t r;
   const int w = 1 + (int)(next() % 6), h = 1 + (int)(next() % 4);
   r.x = (int)(next() % (W - w + 1));
   r.y = (int)(next() % (H - h + 1));
   r.z = r.x + w;
   r.w = r.y + h;
   return r;
}

/* What a primitive writes: from the texels it reads, the same at every
 * pixel it writes, so that a stale or fresh read shows. */
static unsigned shade(const unsigned* mem, const prim_t* p, unsigned id)
{
   unsigned s = id * 2654435761u;
   int x, y;
   for (y = p->rd.y; y < p->rd.w; y++)
      for (x = p->rd.x; x < p->rd.z; x++)
         s = s * 31u + mem[y * W + x];
   return s;
}

static void apply(unsigned* mem, const prim_t* p, unsigned value)
{
   int x, y;
   for (y = p->wr.y; y < p->wr.w; y++)
      for (x = p->wr.x; x < p->wr.z; x++)
         mem[y * W + x] = value;
}

/* RenderRunsInPlace's grouping: groups[] holds each group's count. */
static int group(const prim_t* prims, int n, int blend, int mode, int* groups)
{
   rect_t grd = { 0, 0, 0, 0 }, gwr = { 0, 0, 0, 0 };
   int count = 0, i, raw, war;
   for (i = 0; i < n; i++)
   {
      raw = overlap(&prims[i].rd, &gwr) && mode != NO_RAW;
      war = overlap(&prims[i].wr, &grd) && mode != NO_WAR;
      if (count == 0 || raw || war || (blend && overlap(&prims[i].wr, &gwr)))
      {
         groups[count++] = 0;
         grd = prims[i].rd;
         gwr = prims[i].wr;
      }
      else
      {
         grd = runion(&grd, &prims[i].rd);
         gwr = runion(&gwr, &prims[i].wr);
      }
      groups[count - 1]++;
   }
   return count;
}

/* One batch: whether its groups are defined and draw what the GS does. */
static int run(unsigned seed, int mode, int* rejected, int* group_total)
{
   prim_t prims[PRIMS];
   int groups[PRIMS];
   unsigned gs[W * H], gpu[W * H], before[W * H], values[PRIMS];
   unsigned char fetched[W * H], written[W * H];
   int n = 0, i, g, p, count, x, y, blend;

   rng = seed;
   blend = (int)(next() % 2);
   while (n < PRIMS)
   {
      prims[n].rd = random_rect();
      prims[n].wr = random_rect();
      /* A primitive reading what it writes goes back to the copy. */
      if (overlap(&prims[n].rd, &prims[n].wr))
      {
         (*rejected)++;
         continue;
      }
      n++;
   }

   for (i = 0; i < W * H; i++)
      gs[i] = gpu[i] = (unsigned)i * 7u + seed;
   for (i = 0; i < n; i++)
      apply(gs, &prims[i], shade(gs, &prims[i], (unsigned)i));

   count = group(prims, n, blend, mode, groups);
   *group_total += count;
   for (g = 0, p = 0; g < count; g++)
   {
      /* The barrier: the group reads what the groups before it left. */
      memcpy(before, gpu, sizeof(before));
      memset(fetched, 0, sizeof(fetched));
      memset(written, 0, sizeof(written));
      for (i = p; i < p + groups[g]; i++)
      {
         for (y = prims[i].rd.y; y < prims[i].rd.w; y++)
            for (x = prims[i].rd.x; x < prims[i].rd.z; x++)
               fetched[y * W + x] = 1;
         values[i] = shade(before, &prims[i], (unsigned)i);
      }
      for (i = p; i < p + groups[g]; i++)
      {
         for (y = prims[i].wr.y; y < prims[i].wr.w; y++)
            for (x = prims[i].wr.x; x < prims[i].wr.z; x++)
            {
               if (fetched[y * W + x] || (blend && written[y * W + x]))
                  return 0;
               written[y * W + x] = 1;
            }
         apply(gpu, &prims[i], values[i]);
      }
      p += groups[g];
   }
   return memcmp(gs, gpu, sizeof(gs)) == 0;
}

int main(void)
{
   unsigned seed;
   int bad[3] = { 0, 0, 0 };
   int rejected = 0, groups = 0, scratch = 0, m;

   for (seed = 1; seed <= 20000; seed++)
   {
      bad[RULE] += !run(seed, RULE, &rejected, &groups);
      for (m = 1; m < 3; m++)
         bad[m] += !run(seed, m, &scratch, &scratch);
   }

   printf("runs_groups\n");
   printf("  %s: groups fetch no texel they write, and draw what the GS does, 20000 batches: %d wrong\n",
      bad[RULE] ? "FAIL" : "ok", bad[RULE]);
   printf("  groups: %d for %d primitives; %d primitives reading what they write left to the copy\n",
      groups, 20000 * PRIMS, rejected);
   printf("  %s: negative control, no group on a read of the group's writes: %d wrong\n",
      bad[NO_RAW] ? "ok" : "FAIL", bad[NO_RAW]);
   printf("  %s: negative control, no group on a write over the group's reads: %d wrong\n",
      bad[NO_WAR] ? "ok" : "FAIL", bad[NO_WAR]);
   if (bad[RULE] || !bad[NO_RAW] || !bad[NO_WAR])
   {
      printf("runs_groups: FAILED\n");
      return 1;
   }
   printf("runs_groups: ok\n");
   return 0;
}
