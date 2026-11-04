/* Palettes the GPU drew, read from the target that holds them.
 *
 * A game can draw its palettes: render colours into a page, then load that
 * page as a CLUT. Local memory never sees those colours, so GSClut::Read32
 * takes the palette from the render target holding its blocks
 * (GSTextureCache::LookupPaletteSource), through ps_convert_clut_8/_4.
 *
 * Pinned here, each against the form it replaces as a negative control:
 *
 *  1. An entry is its native pixel's one value. A draw at scale S lays S
 *     fragments across the pixel, and the native sample sits at one place
 *     among them: at the first fragment's centre where the draw keeps the
 *     native grid, at the pixel's middle where the draw is moved half a
 *     pixel (GSTextureCache::NativeSample), which at an even scale is
 *     between two fragments. ps_convert_clut_8/_4 fetch the entry at that
 *     place with a linear filter, so a value linear across the fragments
 *     comes out as the native draw has it. The copy that read the block's
 *     middle for every draw was half a pixel off under the kept grid.
 *
 *  2. Which target serves the palette: one starting at CBP before one
 *     containing it; a containing one only when laid out as the palette is;
 *     and only where it holds the area (valid rect), the EE has not written
 *     it since (dirty rects), and it has drawn some of it since local memory
 *     last took its data (drawn_since_read) -- elsewhere local memory holds
 *     the palette exactly and the target an upscaled, possibly filtered,
 *     copy. The single-pass lookup takes the first target in the list and
 *     neither of the last two tests.
 *
 *  3. The palette is the one the last load put in the CLUT buffer. A change
 *     of CBP alone is outside TEX0's dirty mask (GSState::ApplyTEX0), so a
 *     draw flushed from the saved register state carries an older CBP; the
 *     load's CBP names the palette. Modelled on GSState's snapshot rules.
 *
 *  4. Where a containing target holds the palette: its first block's page on
 *     the target's page grid, its place in the page by its block number, and
 *     only when the palette's blocks cover that area of the page. A target
 *     running past the end of memory wraps, its end block below its base;
 *     it holds the palettes from its base to its unwrapped end. Checked
 *     against the GS's PSMCT32 addressing: the rule places a palette exactly
 *     where every one of its words sits in the target, and refuses it where
 *     no such place exists. The end-block test refused every palette after
 *     the base of a wrapped target (a scratch page at 0x3f00 in a 640-wide
 *     buffer), and local memory's stale copy served them.
 *
 * Build and run, from tests/gsclut:
 *   cc -O2 -std=c89 -pedantic -Wall clut_from_target.c -o clut_from_target
 *   ./clut_from_target
 */
#include <math.h>
#include <stdio.h>

typedef unsigned int u32;

/* ---- 1. the entry's sample ---------------------------------------------- */

/* ps_convert_clut_8's entry position: 8 groups of 16x2, the top-right and
 * bottom-left quadrants swapped, the block clamped at 15. */
static void clut8_pos(u32 clut_x, u32 doffset, u32* px, u32* py)
{
   u32 block = (clut_x >> 4) + (doffset >> 4);
   u32 index, subgroup;
   if (block > 15u)
      block = 15u;
   index = (block << 4) | (clut_x & 15u);
   subgroup = (index / 8u) % 4u;
   *px = (index % 8u) + ((subgroup >= 2u) ? 8u : 0u);
   *py = ((index / 32u) * 2u) + (subgroup % 2u);
}

/* The target drawn at scale S: a shade linear in position, which the draw
 * evaluates at each fragment's centre. A linear fetch at p gives shade(p). */
static double shade(double p)
{
   return 3.0 * p + 7.0;
}

/* GSTextureCache::NativeSample: where the native sample sits among a
 * pixel's fragments, from its first fragment's edge. */
static double native_sample(u32 scale, int shifted)
{
   return (scale > 1u && shifted) ? scale * 0.5 : 0.5;
}

/* ps_convert_clut_8: entry e of a copy from a target at `scale`, read at
 * `at` fragments into its pixel's block. */
static double copy_entry(u32 e, u32 scale, double at, u32 ox, u32 doffset)
{
   u32 px, py;
   clut8_pos(e, doffset, &px, &py);
   return shade((ox + px) * scale + at);
}

/* The native draw's value for the entry's pixel: the shade at the native
 * sample, which at scale 1 is the pixel's one fragment's centre. */
static double native_entry(u32 e, u32 scale, int shifted, u32 ox, u32 doffset)
{
   u32 px, py;
   clut8_pos(e, doffset, &px, &py);
   return shade((ox + px) * scale + native_sample(scale, shifted));
}

static int check_sampled_palette(void)
{
   static const u32 scales[4] = { 1, 2, 3, 4 };
   const u32 ox = 16, doffset = 0x20;
   int fail = 0;
   u32 si, e;
   int shifted;
   for (si = 0; si < 4; si++)
      for (shifted = 0; shifted < 2; shifted++)
         for (e = 0; e < 256; e++)
         {
            const u32 S = scales[si];
            const double at = native_sample(S, shifted);
            if (copy_entry(e, S, at, ox, doffset) != native_entry(e, S, shifted, ox, doffset))
            {
               printf("  entry %u at scale %u (%s grid) is not the native pixel's value\n", e, S, shifted ? "moved" : "kept");
               return fail + 1;
            }
            /* negative: the block's middle, under the kept grid at scale */
            if (S > 1u && !shifted && copy_entry(e, S, S * 0.5, ox, doffset) == native_entry(e, S, shifted, ox, doffset))
            {
               printf("  negative: the block's middle is the native value at scale %u\n", S);
               return fail + 1;
            }
         }
   return fail;
}

/* ---- 2. which target serves the palette ---------------------------------- */

struct rect
{
   int x0, y0, x1, y1; /* x1, y1 exclusive; empty when x0 >= x1 */
};

struct target
{
   u32 tbp, end, psm, swizzle; /* swizzle: the layout class HasSameSwizzleBits compares */
   int contains_ok;            /* ComputeSurfaceOffset found the palette */
   int ox, oy;                 /* where the palette sits in it */
   struct rect valid, drawn, dirty;
};

static int rect_empty(struct rect r)
{
   return r.x0 >= r.x1 || r.y0 >= r.y1;
}

static struct rect rect_and(struct rect a, struct rect b)
{
   struct rect r;
   r.x0 = a.x0 > b.x0 ? a.x0 : b.x0;
   r.y0 = a.y0 > b.y0 ? a.y0 : b.y0;
   r.x1 = a.x1 < b.x1 ? a.x1 : b.x1;
   r.y1 = a.y1 < b.y1 ? a.y1 : b.y1;
   return r;
}

static int rect_eq(struct rect a, struct rect b)
{
   return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

/* LookupPaletteSource. */
static int lookup(const struct target* t, int n, u32 cbp, u32 cpsm, u32 cswizzle)
{
   int pass, i;
   for (pass = 0; pass < 2; pass++)
      for (i = 0; i < n; i++)
      {
         struct rect clut;
         int ox, oy;
         if (pass == 0)
         {
            if (t[i].tbp != cbp || t[i].psm != cpsm)
               continue;
            ox = 0;
            oy = 0;
         }
         else
         {
            if (!(t[i].tbp < cbp && t[i].end >= cbp) || t[i].swizzle != cswizzle || !t[i].contains_ok)
               continue;
            ox = t[i].ox;
            oy = t[i].oy;
         }
         clut.x0 = ox;
         clut.y0 = oy;
         clut.x1 = ox + 16;
         clut.y1 = oy + 16;
         if (!rect_eq(rect_and(t[i].valid, clut), clut) || rect_empty(rect_and(t[i].drawn, clut)))
            continue;
         if (!rect_empty(rect_and(t[i].dirty, clut)))
            continue;
         return i;
      }
   return -1;
}

/* The single-pass form, inside-target mode: first match, dirty test only. */
static int lookup_old(const struct target* t, int n, u32 cbp, u32 cpsm)
{
   int i;
   for (i = 0; i < n; i++)
   {
      struct rect clut;
      int ox, oy;
      if (t[i].tbp == cbp)
      {
         if (t[i].psm != cpsm)
            continue;
         ox = 0;
         oy = 0;
      }
      else if (t[i].tbp < cbp && t[i].end >= cbp && t[i].contains_ok)
      {
         ox = t[i].ox;
         oy = t[i].oy;
      }
      else
         continue;
      clut.x0 = ox;
      clut.y0 = oy;
      clut.x1 = ox + 16;
      clut.y1 = oy + 16;
      if (!rect_empty(rect_and(t[i].dirty, clut)))
         continue;
      return i;
   }
   return -1;
}

static int check_lookup(void)
{
   static const struct rect none = { 0, 0, 0, 0 };
   static const struct rect page = { 0, 0, 640, 32 };
   static const struct rect clut_area = { 16, 0, 32, 16 };
   struct target t[2];
   int fail = 0, got, old;

   /* a containing target listed first, drawn; the palette's own target second */
   t[0].tbp = 0x3a00; t[0].end = 0x3cff; t[0].psm = 0; t[0].swizzle = 0; t[0].contains_ok = 1;
   t[0].ox = 16; t[0].oy = 0; t[0].valid = page; t[0].drawn = page; t[0].dirty = none;
   t[1] = t[0];
   t[1].tbp = 0x3ac4; t[1].ox = 0; t[1].oy = 0;
   got = lookup(t, 2, 0x3ac4, 0, 0);
   old = lookup_old(t, 2, 0x3ac4, 0);
   if (got != 1)
   {
      printf("  an exact target lost to a containing one (%d)\n", got);
      fail++;
   }
   if (old == 1)
   {
      printf("  negative: the single pass took the exact target\n");
      fail++;
   }

   /* one containing target, drawn over the palette: it serves */
   t[0].tbp = 0x3ac0;
   if (lookup(t, 1, 0x3ac4, 0, 0) != 0)
   {
      printf("  a drawn containing target does not serve\n");
      fail++;
   }

   /* never drawn there since the last sync: local memory serves */
   t[0].drawn.x0 = 0; t[0].drawn.y0 = 16; t[0].drawn.x1 = 640; t[0].drawn.y1 = 32;
   if (lookup(t, 1, 0x3ac4, 0, 0) != -1)
   {
      printf("  an undrawn area was read from the target\n");
      fail++;
   }
   if (lookup_old(t, 1, 0x3ac4, 0) != 0)
   {
      printf("  negative: the single pass left the undrawn area alone\n");
      fail++;
   }
   t[0].drawn = clut_area;

   /* laid out otherwise (a 16-bit target under a 32-bit palette) */
   t[0].swizzle = 1;
   if (lookup(t, 1, 0x3ac4, 0, 0) != -1)
   {
      printf("  a target of another layout served the palette\n");
      fail++;
   }
   if (lookup_old(t, 1, 0x3ac4, 0) != 0)
   {
      printf("  negative: the single pass checked the layout\n");
      fail++;
   }
   t[0].swizzle = 0;

   /* the EE wrote over it, or the target does not hold it */
   t[0].dirty = clut_area;
   if (lookup(t, 1, 0x3ac4, 0, 0) != -1)
   {
      printf("  a dirty palette was read from the target\n");
      fail++;
   }
   t[0].dirty = none;
   t[0].valid.x1 = 24;
   if (lookup(t, 1, 0x3ac4, 0, 0) != -1)
   {
      printf("  a palette past the valid area was read from the target\n");
      fail++;
   }
   return fail;
}

/* ---- 3. the palette follows the load ------------------------------------- */

struct tex0
{
   u32 tbp, cbp, csa;
};

struct gs
{
   struct tex0 env[2], prev[2];
   int prim_ctxt, prev_ctxt, backed_up_ctxt, dirty, queued;
   u32 clut_cbp;     /* the last load's CBP: GSClut's m_write */
   u32 drawn_cbp[16]; /* per flushed draw: the CBP its registers carried */
   u32 drawn_load[16]; /* per flushed draw: the CBP of the palette it has */
   int draws;
};

static void flush(struct gs* g)
{
   if (!g->queued)
      return;
   /* GSState::Flush: a dirty state draws from the saved registers */
   g->drawn_cbp[g->draws] = g->dirty ? g->prev[g->prev_ctxt].cbp : g->env[g->prim_ctxt].cbp;
   g->drawn_load[g->draws] = g->clut_cbp;
   g->draws++;
   g->queued = 0;
   if (g->dirty)
      g->backed_up_ctxt = -1;
}

/* ApplyTEX0 with a load: flush, load, then the dirty mask (TBP..TFX, CPSM,
 * CSA; not CBP) when the write is to the drawing context. */
static void write_tex0(struct gs* g, int ctx, struct tex0 t)
{
   flush(g);
   g->clut_cbp = t.cbp;
   g->env[ctx] = t;
   if (ctx == g->prev_ctxt)
      g->dirty = (g->prev[ctx].tbp != t.tbp || g->prev[ctx].csa != t.csa);
}

static void write_other(struct gs* g)
{
   g->dirty = 1;
}

/* VertexKick: the first vertex saves the registers if they changed. */
static void kick(struct gs* g, int ctx)
{
   g->prim_ctxt = ctx;
   if (!g->queued && (g->backed_up_ctxt != ctx || g->dirty))
   {
      g->prev[0] = g->env[0];
      g->prev[1] = g->env[1];
      g->prev_ctxt = ctx;
      g->dirty = 0;
      g->backed_up_ctxt = ctx;
   }
   g->queued = 1;
}

static int check_follows_load(void)
{
   /* eight stripes through eight palettes of one page, then the next page's
    * first pass on the other context, after a register it dirties */
   static const u32 cbp[8] = { 0x3ac0, 0x3ac4, 0x3ad0, 0x3ad4, 0x3ac8, 0x3acc, 0x3ad8, 0x3adc };
   struct gs g;
   struct tex0 t;
   int i, fail = 0, stale = 0;
   g.env[0].tbp = g.env[0].cbp = g.env[0].csa = 0;
   g.env[1] = g.env[0];
   g.prev[0] = g.env[0];
   g.prev[1] = g.env[1];
   g.prim_ctxt = g.prev_ctxt = 0;
   g.backed_up_ctxt = -1;
   g.dirty = g.queued = g.draws = 0;
   g.clut_cbp = 0;
   for (i = 0; i < 8; i++)
   {
      t.tbp = 0x3ae4;
      t.cbp = cbp[i];
      t.csa = 0;
      write_tex0(&g, 1, t);
      kick(&g, 1);
   }
   write_other(&g);
   t.tbp = 0x11a0;
   t.cbp = 0x3ae0;
   t.csa = 0;
   write_tex0(&g, 0, t);
   for (i = 0; i < g.draws; i++)
   {
      if (g.drawn_load[i] != cbp[i])
      {
         printf("  stripe %d has palette %x, wanted %x\n", i, g.drawn_load[i], cbp[i]);
         fail++;
      }
      stale += g.drawn_cbp[i] != cbp[i];
   }
   if (g.draws != 8)
   {
      printf("  %d stripes drawn\n", g.draws);
      fail++;
   }
   /* the registers the last stripe drew from name another palette */
   if (!stale)
   {
      printf("  negative: every draw carried its own CBP\n");
      fail++;
   }
   return fail;
}

/* ---- 4. where a containing target holds the palette ----------------------- */

static const unsigned char block_table[4][8] = {
   { 0, 1, 4, 5, 16, 17, 20, 21 },
   { 2, 3, 6, 7, 18, 19, 22, 23 },
   { 8, 9, 12, 13, 24, 25, 28, 29 },
   { 10, 11, 14, 15, 26, 27, 30, 31 }
};

static const unsigned char column_table32[8][8] = {
   { 0, 1, 4, 5, 8, 9, 12, 13 },
   { 2, 3, 6, 7, 10, 11, 14, 15 },
   { 16, 17, 20, 21, 24, 25, 28, 29 },
   { 18, 19, 22, 23, 26, 27, 30, 31 },
   { 32, 33, 36, 37, 40, 41, 44, 45 },
   { 34, 35, 38, 39, 42, 43, 46, 47 },
   { 48, 49, 52, 53, 56, 57, 60, 61 },
   { 50, 51, 54, 55, 58, 59, 62, 63 }
};

/* PSMCT32 word address of pixel (x, y) in a buffer at block bp, bw pages wide. */
static u32 word32(u32 x, u32 y, u32 bp, u32 bw)
{
   const u32 block = bp + ((y / 32) * bw + x / 64) * 32 + block_table[(y % 32) / 8][(x % 64) / 8];
   return (block & 16383u) * 64 + column_table32[y % 8][x % 8];
}

/* The rule: containment to the unwrapped end (or the wrapped end block, for
 * the negative), then placement by the first block. */
static int place(u32 tbp, u32 tbw, u32 th, u32 cbp, int sw, int sh, int unwrapped, int* ox, int* oy)
{
   const u32 end_unwrapped = tbp + ((th + 31) / 32) * tbw * 32 - 1;
   const u32 end = unwrapped ? end_unwrapped : (end_unwrapped & 16383u);
   const u32 rel = cbp - tbp, page = rel >> 5;
   const u32 nblocks = (u32)(((sw + 7) / 8) * ((sh + 7) / 8));
   int px = -1, py = 0, x, y;
   if (!(tbp < cbp && end >= cbp))
      return 0;
   for (y = 0; y < 32 && px < 0; y += 8)
      for (x = 0; x < 64; x += 8)
         if (block_table[y / 8][x / 8] == (rel & 31))
         {
            px = x;
            py = y;
            break;
         }
   if (px < 0 || px + sw > 64 || py + sh > 32)
      return 0;
   for (y = py; y < py + sh; y += 8)
      for (x = px; x < px + sw; x += 8)
      {
         const u32 blk = block_table[y / 8][x / 8];
         if (blk < (rel & 31) || blk >= (rel & 31) + nblocks)
            return 0;
      }
   *ox = (int)(page % tbw) * 64 + px;
   *oy = (int)(page / tbw) * 32 + py;
   return 1;
}

/* The oracle: the place in the target where every palette word sits, the
 * palette laid out one page wide at cbp. */
static int place_truth(u32 tbp, u32 tbw, u32 th, u32 cbp, int sw, int sh, int* ox, int* oy)
{
   const u32 first = word32(0, 0, cbp, 1);
   u32 x, y;
   for (y = 0; y < th; y++)
      for (x = 0; x < tbw * 64; x++)
         if (word32(x, y, tbp, tbw) == first)
         {
            int i, j;
            if (x + (u32)sw > tbw * 64 || y + (u32)sh > th)
               return 0;
            for (j = 0; j < sh; j++)
               for (i = 0; i < sw; i++)
                  if (word32(x + (u32)i, y + (u32)j, tbp, tbw) != word32((u32)i, (u32)j, cbp, 1))
                     return 0;
            *ox = (int)x;
            *oy = (int)y;
            return 1;
         }
   return 0;
}

static int check_placement(void)
{
   static const struct
   {
      u32 tbp, tbw, th, cbp;
      int sw, sh;
   } cases[] = {
      /* a 640x448 scratch buffer at 0x3f00: it wraps to block 0x0080 */
      { 0x3f00, 10, 448, 0x3f04, 16, 16 },
      { 0x3f00, 10, 448, 0x3f08, 16, 16 },
      { 0x3f00, 10, 448, 0x3f0c, 16, 16 },
      { 0x3f00, 10, 448, 0x3f10, 16, 16 },
      { 0x3f00, 10, 448, 0x3f1c, 16, 16 },
      { 0x3f00, 10, 448, 0x3f20, 16, 16 }, /* the next page */
      { 0x3f00, 10, 448, 0x3f02, 16, 16 }, /* blocks 2-5: not one area */
      { 0x3f00, 10, 448, 0x3f03, 8, 2 },   /* a 16-colour palette */
      /* a target inside memory */
      { 0x1a40, 10, 128, 0x1a44, 16, 16 },
      { 0x1a40, 10, 128, 0x1a40 + 32 * 11 + 12, 16, 16 },
      { 0x1a40, 10, 128, 0x1a40 + 32 * 3 + 30, 16, 16 }
   };
   int fail = 0, refused = 0;
   size_t i;
   for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      int ox = 0, oy = 0, tx = 0, ty = 0;
      const int got = place(cases[i].tbp, cases[i].tbw, cases[i].th, cases[i].cbp, cases[i].sw, cases[i].sh, 1, &ox, &oy);
      const int want = place_truth(cases[i].tbp, cases[i].tbw, cases[i].th, cases[i].cbp, cases[i].sw, cases[i].sh, &tx, &ty);
      if (got != want || (got && (ox != tx || oy != ty)))
      {
         printf("  palette %x in target %x: placed %d at %d,%d, held %d at %d,%d\n", cases[i].cbp, cases[i].tbp,
            got, ox, oy, want, tx, ty);
         fail++;
      }
      if (want && !place(cases[i].tbp, cases[i].tbw, cases[i].th, cases[i].cbp, cases[i].sw, cases[i].sh, 0, &ox, &oy))
         refused++;
   }
   /* negative: the end-block test refuses the wrapped target's palettes */
   if (refused < 6)
   {
      printf("  negative: the end-block test took the wrapped target's palettes\n");
      fail++;
   }
   return fail;
}

int main(void)
{
   int fail = 0;
   fail += check_sampled_palette();
   fail += check_lookup();
   fail += check_follows_load();
   fail += check_placement();
   printf("clut from target: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
