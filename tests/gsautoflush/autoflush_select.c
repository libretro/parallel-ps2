/* The auto-flush handlers are chosen by the draw state, and only when it
 * changes.
 *
 * A vertex kick takes the auto-flush handlers only while the draw state
 * reads what it draws (GSState::IsAutoFlushDraw: a texture at its frame's
 * or Z buffer's address, written there). The answer is worked out when a
 * register it reads is written -- TEX0 and TEX2, TEST, FRAME and ZBUF of
 * the current context, PRIM and PRMODE through UpdateContext -- and the
 * handlers change only when it flips (GSState::UpdateAutoFlushKick). Every
 * other draw kicks through the plain handlers, which do not look.
 *
 * Pinned here, over random sequences of writes to both contexts, PRIM and
 * PRMODE: after every write the handlers chosen are the ones evaluating
 * the state at that vertex would choose. The negative control leaves the
 * TEST hook out.
 *
 * It also times what the choice saves. Before, every kick with auto-flush
 * on asked the question; now a register write asks it, and kicks between
 * writes do not. Printed per kick and per write, as relative figures for
 * this host; the frame-level saving is their product with a frame's counts.
 *
 * Build and run, from tests/gsautoflush:
 *   cc -O2 -std=c89 -pedantic -Wall autoflush_select.c -o autoflush_select
 *   ./autoflush_select
 */
#include <stdio.h>
#include <time.h>

typedef unsigned int u32;

typedef struct
{
   u32 tbp0, tpsm;
   u32 fbp, fpsm, fbmsk;
   u32 zbp, zpsm, zmsk;
   u32 ate, atst, afail;
} ctx_t;

typedef struct
{
   u32 tme, ctxt, ac;
   ctx_t c[2];
   int kick; /* the handler set in use */
} gs_t;

/* IsAutoFlushDraw's shape, on the registers it reads. */
static int eligible(const gs_t* g)
{
   const ctx_t* c = &g->c[g->ctxt];
   int frame_hit, zbuf_hit;
   if (!g->tme)
      return 0;
   frame_hit = c->fbp * 32 == c->tbp0 && !(c->ate && c->atst == 0 && c->afail == 2) && c->fbmsk != 0xffffffffu;
   zbuf_hit = c->zbp * 32 == c->tbp0 && !(c->ate && c->atst == 0 && c->afail != 2) && !c->zmsk;
   return (frame_hit || zbuf_hit) && (c->tpsm & 1) == ((frame_hit ? c->fpsm : c->zpsm) & 1);
}

static void update_kick(gs_t* g)
{
   const int e = eligible(g);
   if (e != g->kick)
      g->kick = e;
}

static unsigned rng;
static u32 next(void)
{
   rng = rng * 1103515245u + 12345u;
   return (rng >> 16) & 0x7fffu;
}

/* One register write, with the hooks GSState has for it. */
static void write_reg(gs_t* g, int no_test_hook)
{
   const u32 i = next() & 1;
   ctx_t* c = &g->c[i];
   switch (next() % 7)
   {
      case 0: /* TEX0 */
         c->tbp0 = (next() % 4) * 32;
         c->tpsm = next() % 2;
         if (i == g->ctxt) update_kick(g);
         break;
      case 1: /* TEX2: the PSM only, here */
         c->tpsm = next() % 2;
         if (i == g->ctxt) update_kick(g);
         break;
      case 2: /* TEST */
         c->ate = next() % 2;
         c->atst = next() % 2;
         c->afail = next() % 3;
         if (i == g->ctxt && !no_test_hook) update_kick(g);
         break;
      case 3: /* FRAME, which also sets ZBUF's swizzle */
         c->fbp = next() % 4;
         c->fpsm = next() % 2;
         c->fbmsk = (next() % 3) ? 0 : 0xffffffffu;
         c->zpsm = c->fpsm ^ 1;
         if (i == g->ctxt) update_kick(g);
         break;
      case 4: /* ZBUF */
         c->zbp = next() % 4;
         c->zpsm = next() % 2;
         c->zmsk = next() % 2;
         if (i == g->ctxt) update_kick(g);
         break;
      case 5: /* PRIM (AC = 1): TME and the context, through UpdateContext */
         g->ac = 1;
         g->tme = next() % 2;
         g->ctxt = next() % 2;
         update_kick(g);
         break;
      default: /* PRMODECONT / PRMODE (AC = 0) */
         if (next() % 2)
            g->ac = 0;
         else if (!g->ac)
         {
            g->tme = next() % 2;
            g->ctxt = next() % 2;
            update_kick(g);
         }
         break;
   }
}

static int run(unsigned seed, int no_test_hook)
{
   gs_t g;
   int step, i;
   rng = seed;
   g.tme = 0; g.ctxt = 0; g.ac = 1;
   for (i = 0; i < 2; i++)
   {
      g.c[i].tbp0 = g.c[i].tpsm = 0;
      g.c[i].fbp = 1; g.c[i].fpsm = g.c[i].fbmsk = 0;
      g.c[i].zbp = 2; g.c[i].zpsm = 1; g.c[i].zmsk = 0;
      g.c[i].ate = g.c[i].atst = g.c[i].afail = 0;
   }
   g.kick = eligible(&g);
   for (step = 0; step < 200; step++)
   {
      write_reg(&g, no_test_hook);
      if (g.kick != eligible(&g))
         return 0;
   }
   return 1;
}

/* ISO clock(): builds the same on Windows and Linux, and over two hundred
 * million iterations its resolution does not matter. */
static double now(void)
{
   return (double)clock() / (double)CLOCKS_PER_SEC;
}

int main(void)
{
   unsigned seed;
   int bad = 0, bad_ctl = 0;
   volatile gs_t g;
   gs_t s;
   volatile int sink = 0;
   long n, N = 200000000L;
   double t0, t_ask, t_plain;

   for (seed = 1; seed <= 20000; seed++)
   {
      bad += !run(seed, 0);
      bad_ctl += !run(seed, 1);
   }

   printf("autoflush_select\n");
   printf("  %s: handlers match the state after every write, 20000 sequences: %d wrong\n",
      bad ? "FAIL" : "ok", bad);
   printf("  %s: negative control, no TEST hook: %d wrong\n", bad_ctl ? "ok" : "FAIL", bad_ctl);

   /* The cost the choice moves: one IsAutoFlushDraw-shaped question per
    * kick before, against none. The state is read through a volatile so
    * the question is asked every time. */
   s.tme = 1; s.ctxt = 0; s.ac = 1; s.kick = 0;
   s.c[0].tbp0 = 0; s.c[0].tpsm = 0; s.c[0].fbp = 1; s.c[0].fpsm = 0; s.c[0].fbmsk = 0;
   s.c[0].zbp = 2; s.c[0].zpsm = 1; s.c[0].zmsk = 0; s.c[0].ate = 0; s.c[0].atst = 0; s.c[0].afail = 0;
   s.c[1] = s.c[0];
   g = s;
   t0 = now();
   for (n = 0; n < N; n++)
   {
      s = g;
      sink += eligible(&s);
   }
   t_ask = now() - t0;
   t0 = now();
   for (n = 0; n < N; n++)
   {
      s = g;
      sink += s.kick;
   }
   t_plain = now() - t0;
   printf("  per kick, asking (before): %.2f ns; not asking (now): %.2f ns; per write the question costs the first figure\n",
      t_ask * 1e9 / (double)N, t_plain * 1e9 / (double)N);
   (void)sink;

   if (bad || !bad_ctl)
   {
      printf("autoflush_select: FAILED\n");
      return 1;
   }
   printf("autoflush_select: ok\n");
   return 0;
}
