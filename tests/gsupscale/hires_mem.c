/* The software renderer's 2x memory (GSHiresMem.c), against the GS's own
 * addressing.
 *
 * At 2x the software renderer draws each primitive a second time at twice
 * the size, into a memory four times as large, through the same scanline
 * code as at native. That code finds a pixel as the sum of a row part and
 * a column part, and stores four (or eight) horizontally adjacent pixels
 * at the offsets they have in a native column (GSDrawScanline.c,
 * s_offsets: 0, 2, 8, 10, 16, 18, 24, 26 sixteen-bit units). The 2x memory
 * lays a native page out as a square of four pages of the same swizzle.
 *
 * Pinned here, for PSMCT32, PSMCT16, PSMCT16S, PSMZ32 and PSMZ16:
 *  1. the row and column parts add up, for every 2x pixel of a buffer, to
 *     the pixel's place in its native page's four pages, whatever the base
 *     and width, and wrapping with memory;
 *  2. every aligned run of eight 2x pixels sits at the scanline code's
 *     offsets. Placing each native pixel's 2x2 next to one another
 *     (4 * native + 2 * row + column) breaks this for the 16-bit formats,
 *     whose adjacent pixels are two apart;
 *  3. the four pages of a native page hold each 2x pixel of it once;
 *  4. a page taken from native memory holds every native pixel on its 2x2,
 *     only the stale blocks asked for are taken, and a page used in
 *     another layout is taken whole. Taking only the stale blocks
 *     regardless of layout leaves a page drawn in one format read in
 *     another holding the first one's arrangement;
 *  5. the display read gives each native pixel twice in each direction,
 *     expanded as the GS expands the format, across the 2048 wrap;
 *  6. a texture read at 2x gives each native texel's 2x2 from the 2x
 *     memory where it holds the texel's block, expanded with TEXA, and the
 *     native texel twice elsewhere, for textures based at any block;
 *  7. a sprite drawn at 2x covers exactly the 2x2s of the native pixels it
 *     covers, its texture coordinates moved along with its edges;
 *  8. a sprite sampled nearest reads, at each of its 2x pixels, the texel
 *     its native pixel reads, one texel to a pixel, in either direction,
 *     with coordinates on texel centres or edges, and in a 2x texture that
 *     texel's own 2x2; scaled by a hair it reads the texels its native
 *     pixels do; scaled by more it reads no texel beyond the first and last
 *     its native pixels read. The plain 2x coordinates read the next texel
 *     at half the pixels of a sprite with coordinates on texel centres;
 *  9. a 2x texture's wrap doubles the native one, and clamps to the area
 *     the draw samples where that is less than the whole texture.
 *
 * Build and run, from tests/gsupscale:
 *   cc -O2 -std=c89 -pedantic -Wall hires_mem.c -o hires_mem
 *   ./hires_mem
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../pcsx2/GS/Renderers/SW/GSHiresMem.c"

static const unsigned char block32[4][8] = {
   {  0,  1,  4,  5, 16, 17, 20, 21 },
   {  2,  3,  6,  7, 18, 19, 22, 23 },
   {  8,  9, 12, 13, 24, 25, 28, 29 },
   { 10, 11, 14, 15, 26, 27, 30, 31 }
};
static const unsigned char block16[8][4] = {
   {  0,  2,  8, 10 }, {  1,  3,  9, 11 }, {  4,  6, 12, 14 }, {  5,  7, 13, 15 },
   { 16, 18, 24, 26 }, { 17, 19, 25, 27 }, { 20, 22, 28, 30 }, { 21, 23, 29, 31 }
};
static const unsigned char block16s[8][4] = {
   {  0,  2, 16, 18 }, {  1,  3, 17, 19 }, {  8, 10, 24, 26 }, {  9, 11, 25, 27 },
   {  4,  6, 20, 22 }, {  5,  7, 21, 23 }, { 12, 14, 28, 30 }, { 13, 15, 29, 31 }
};
static const unsigned char column32[8][8] = {
   {  0,  1,  4,  5,  8,  9, 12, 13 }, {  2,  3,  6,  7, 10, 11, 14, 15 },
   { 16, 17, 20, 21, 24, 25, 28, 29 }, { 18, 19, 22, 23, 26, 27, 30, 31 },
   { 32, 33, 36, 37, 40, 41, 44, 45 }, { 34, 35, 38, 39, 42, 43, 46, 47 },
   { 48, 49, 52, 53, 56, 57, 60, 61 }, { 50, 51, 54, 55, 58, 59, 62, 63 }
};
static const unsigned char column16[8][16] = {
   {   0,   2,   8,  10,  16,  18,  24,  26,   1,   3,   9,  11,  17,  19,  25,  27 },
   {   4,   6,  12,  14,  20,  22,  28,  30,   5,   7,  13,  15,  21,  23,  29,  31 },
   {  32,  34,  40,  42,  48,  50,  56,  58,  33,  35,  41,  43,  49,  51,  57,  59 },
   {  36,  38,  44,  46,  52,  54,  60,  62,  37,  39,  45,  47,  53,  55,  61,  63 },
   {  64,  66,  72,  74,  80,  82,  88,  90,  65,  67,  73,  75,  81,  83,  89,  91 },
   {  68,  70,  76,  78,  84,  86,  92,  94,  69,  71,  77,  79,  85,  87,  93,  95 },
   {  96,  98, 104, 106, 112, 114, 120, 122,  97,  99, 105, 107, 113, 115, 121, 123 },
   { 100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127 }
};

#define F_CT32  0
#define F_CT16  1
#define F_CT16S 2
#define F_Z32   3
#define F_Z16   4
#define NFMT    5
static const char *fmt_name[NFMT] = { "PSMCT32", "PSMCT16", "PSMCT16S", "PSMZ32", "PSMZ16" };

static int is16(int f) { return f == F_CT16 || f == F_CT16S || f == F_Z16; }

/* The GS's address of pixel (x, y) of a buffer at page bp, bw pages wide,
 * in elements (words, or halfwords for the 16-bit formats). */
static unsigned pa_ref(int f, int x, int y, unsigned bp, unsigned bw)
{
   unsigned page, blk, pix;
   x &= 2047;
   y &= 2047;
   if (is16(f))
   {
      page = bp + (unsigned)(y >> 6) * bw + (unsigned)(x >> 6);
      blk  = (f == F_CT16S ? block16s : block16)[(y >> 3) & 7][(x >> 4) & 3];
      pix  = column16[y & 7][x & 15];
      if (f == F_Z16)
         blk ^= 0x18;
      return ((page & 511) << 12) + (blk << 7) + pix;
   }
   page = bp + (unsigned)(y >> 5) * bw + (unsigned)(x >> 6);
   blk  = block32[(y >> 3) & 3][(x >> 3) & 7];
   pix  = column32[y & 7][x & 7];
   if (f == F_Z32)
      blk ^= 0x18;
   return ((page & 511) << 11) + (blk << 6) + pix;
}

static gs_hr_layout_t layouts[NFMT];

static void make_layout(int f)
{
   gs_hr_layout_t *l = &layouts[f];
   int i;
   l->pw         = 64;
   l->ph         = is16(f) ? 64 : 32;
   l->elem_bytes = is16(f) ? 2 : 4;
   l->page_elems = 8192 / l->elem_bytes;
   for (i = 0; i < l->ph; i++)
      l->swrow[i] = (int)pa_ref(f, 0, i, 0, 1);
   for (i = 0; i < l->pw; i++)
      l->swcol[i] = (int)(pa_ref(f, i, 0, 0, 1) - pa_ref(f, 0, 0, 0, 1));
   gs_hr_layout_init(l);
}

/* The 2x pixel (X, Y) of a buffer, by definition: its native pixel's page,
 * and in that page's four the quarter and place of (X, Y) in the square. */
static unsigned hr_ref(int f, int X, int Y, unsigned bp, unsigned bw)
{
   const gs_hr_layout_t *l = &layouts[f];
   const int x = (X & 4095) >> 1, y = (Y & 4095) >> 1;
   const unsigned page = (bp + (unsigned)(y / l->ph) * bw + (unsigned)(x / l->pw)) & 511;
   const int PX = 2 * (x % l->pw) + (X & 1), PY = 2 * (y % l->ph) + (Y & 1);
   const int qx = PX >= l->pw, qy = PY >= l->ph;
   return 4u * page * (unsigned)l->page_elems + (unsigned)((qy * 2 + qx) * l->page_elems)
      + pa_ref(f, PX - qx * l->pw, PY - qy * l->ph, 0, 1);
}

static int check_sum(void)
{
   static const unsigned bps[] = { 0, 70, 500 }, bws[] = { 10, 1, 16 };
   int fail = 0, f, c, X, Y;
   for (f = 0; f < NFMT; f++)
   {
      const gs_hr_layout_t *l = &layouts[f];
      const unsigned mask = gs_hr_elem_mask(l);
      for (c = 0; c < 3; c++)
         for (Y = 0; Y < 4096; Y += (c ? 7 : 1))
            for (X = 0; X < 4096; X += (c ? 5 : 1))
            {
               const unsigned got = (gs_hr_row(l, bps[c], bws[c], Y) + gs_hr_col(l, X)) & mask;
               const unsigned want = hr_ref(f, X, Y, bps[c], bws[c]);
               if (got != want && fail++ < 5)
                  printf("  %s bp %u bw %u (%d,%d): %u, want %u\n", fmt_name[f], bps[c], bws[c], X, Y, got, want);
            }
   }
   return fail;
}

/* The runs of eight the scanline code stores, in sixteen-bit units. */
static const int s_offsets[8] = { 0, 2, 8, 10, 16, 18, 24, 26 };

static unsigned naive(int f, int X, int Y)
{
   return 4u * pa_ref(f, X >> 1, Y >> 1, 0, 10) + 2u * (unsigned)(Y & 1) + (unsigned)(X & 1);
}

/* which: 0 the 2x memory, 1 native, 2 the naive 2x2 placement */
static int runs_broken(int f, int which)
{
   const int u = layouts[f].elem_bytes / 2; /* elements to 16-bit units */
   const int lim = which == 1 ? 2048 : 4096;
   int bad = 0, X, Y, i;
   for (Y = 0; Y < lim; Y += 3)
      for (X = 0; X < lim; X += 8)
      {
         unsigned a0 = 0;
         for (i = 0; i < 8; i++)
         {
            const unsigned a = which == 0 ? hr_ref(f, X + i, Y, 0, 10)
               : which == 1 ? pa_ref(f, X + i, Y, 0, 10) : naive(f, X + i, Y);
            if (i == 0)
               a0 = a;
            else if ((int)(a - a0) * u != s_offsets[i])
            {
               bad++;
               break;
            }
         }
      }
   return bad;
}

static int check_runs(void)
{
   int fail = 0, f;
   for (f = 0; f < NFMT; f++)
   {
      const int hr = runs_broken(f, 0), nat = runs_broken(f, 1), nv = runs_broken(f, 2);
      if (hr || nat)
      {
         printf("  %s: %d runs of the 2x memory, %d native, off the scanline offsets\n", fmt_name[f], hr, nat);
         fail++;
      }
      if (is16(f) && !nv)
      {
         printf("  %s: the naive 2x2 placement keeps the scanline offsets (negative control)\n", fmt_name[f]);
         fail++;
      }
   }
   return fail;
}

static int check_bijection(void)
{
   static unsigned char seen[4 * 4096];
   int fail = 0, f, PX, PY, i;
   for (f = 0; f < NFMT; f++)
   {
      const gs_hr_layout_t *l = &layouts[f];
      const int n = 4 * l->page_elems;
      memset(seen, 0, sizeof(seen));
      for (PY = 0; PY < 2 * l->ph; PY++)
         for (PX = 0; PX < 2 * l->pw; PX++)
         {
            const unsigned e = gs_hr_super_elem(l, PX, PY);
            if ((int)e >= n || seen[e]++)
            {
               if (fail++ < 3)
                  printf("  %s: (%d,%d) at %u, taken or outside\n", fmt_name[f], PX, PY, e);
            }
         }
      for (i = 0; i < n; i++)
         if (!seen[i] && fail++ < 3)
            printf("  %s: element %d of the four pages holds no pixel\n", fmt_name[f], i);
   }
   return fail;
}

static unsigned rng = 12345u;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 7; }

static unsigned char *vm, *hr;
static gs_hr_pages_t pages;

static unsigned elem(const gs_hr_layout_t *l, const unsigned char *m, unsigned e)
{
   return l->elem_bytes == 4 ? ((const unsigned*)m)[e] : ((const unsigned short*)m)[e];
}

/* Blocks of the page, in layout f, whose 2x2s do not hold their native
 * pixels. */
static unsigned wrong_blocks(int f, unsigned page)
{
   const gs_hr_layout_t *l = &layouts[f];
   const unsigned epb = 256u / (unsigned)l->elem_bytes;
   unsigned wrong = 0;
   int x, y, s;
   for (y = 0; y < l->ph; y++)
      for (x = 0; x < l->pw; x++)
      {
         const unsigned n = pa_ref(f, x, y, page, 1);
         const unsigned v = elem(l, vm, n);
         for (s = 0; s < 4; s++)
            if (elem(l, hr, hr_ref(f, 2 * x + (s & 1), 2 * y + (s >> 1), page, 1)) != v)
               wrong |= 1u << ((n / epb) & 31);
      }
   return wrong;
}

static void scribble(void)
{
   size_t i;
   for (i = 0; i < 4u << 20; i++)
      vm[i] = (unsigned char)rnd();
}

static int check_refresh(void)
{
   int fail = 0, f;
   const unsigned page = 37;
   size_t i;
   for (f = 0; f < NFMT; f++)
   {
      const gs_hr_layout_t *l = &layouts[f];
      unsigned w;
      scribble();
      memset(hr, 0, 16u << 20);
      gs_hr_pages_reset(&pages);

      /* the blocks asked for, and only those */
      gs_hr_page_refresh(&pages, l, (unsigned)f, page, 0x0000ffffu, vm, hr);
      w = wrong_blocks(f, page);
      if (w != 0xffff0000u)
      {
         printf("  %s: blocks 0-15 taken, wrong %08x\n", fmt_name[f], w);
         fail++;
      }
      gs_hr_page_refresh(&pages, l, (unsigned)f, page, 0xffffffffu, vm, hr);
      if ((w = wrong_blocks(f, page)) != 0)
      {
         printf("  %s: whole page taken, wrong %08x\n", fmt_name[f], w);
         fail++;
      }

      /* a block written natively and marked is taken again; unmarked ones
       * are left as the 2x drawing left them */
      for (i = 0; i < (size_t)l->page_elems * (size_t)l->elem_bytes; i++)
         vm[(size_t)page * 8192u + i] ^= 0x5a;
      gs_hr_mark_block(&pages, page * 32u + 3u);
      gs_hr_page_refresh(&pages, l, (unsigned)f, page, 0xffffffffu, vm, hr);
      if ((w = wrong_blocks(f, page)) != ~(1u << 3))
      {
         printf("  %s: after marking block 3, wrong %08x\n", fmt_name[f], w);
         fail++;
      }
   }

   /* a page held in one layout, used in another */
   {
      const int a = F_CT32, b = F_CT16;
      unsigned w;
      scribble();
      gs_hr_pages_reset(&pages);
      gs_hr_page_refresh(&pages, &layouts[a], (unsigned)a, page, 0xffffffffu, vm, hr);
      gs_hr_page_refresh(&pages, &layouts[b], (unsigned)b, page, 0xffffffffu, vm, hr);
      if ((w = wrong_blocks(b, page)) != 0)
      {
         printf("  a page taken as %s then used as %s: wrong %08x\n", fmt_name[a], fmt_name[b], w);
         fail++;
      }
      /* negative control: stale blocks alone, whatever the layout */
      scribble();
      gs_hr_pages_reset(&pages);
      gs_hr_page_refresh(&pages, &layouts[a], (unsigned)a, page, 0xffffffffu, vm, hr);
      pages.psm[page] = (unsigned char)b;
      gs_hr_page_refresh(&pages, &layouts[b], (unsigned)b, page, 0xffffffffu, vm, hr);
      if (wrong_blocks(b, page) == 0)
      {
         printf("  stale blocks alone kept a page right across layouts (negative control)\n");
         fail++;
      }
   }
   return fail;
}

static unsigned expand(int f, unsigned v)
{
   if (!is16(f))
      return f == F_CT32 ? v : ((v & 0xffffffu) | 0x80000000u);
   return ((v << 3) & 0xf8u) | ((v << 6) & 0xf800u) | ((v << 9) & 0xf80000u) | ((v & 0x8000u) ? 0x80000000u : 0u);
}

static int check_read(void)
{
   static unsigned out[400 * 300], colbuf[400];
   static const int fmts[3] = { F_CT32, F_CT16, F_CT16S };
   /* the second one runs past 2048 in both directions */
   static const int x0s[2] = { 0, 1960 }, y0s[2] = { 8, 1990 };
   int fail = 0, fi, c, X, Y;
   for (fi = 0; fi < 3; fi++)
   {
      const int f = fmts[fi];
      const gs_hr_layout_t *l = &layouts[f];
      const unsigned bp = 100, bw = 10;
      int p;
      scribble();
      gs_hr_pages_reset(&pages);
      for (p = 0; p < 512; p++)
         gs_hr_page_refresh(&pages, l, (unsigned)f, (unsigned)p, 0xffffffffu, vm, hr);
      for (c = 0; c < 2; c++)
      {
         gs_hr_read(l, hr, bp, bw, x0s[c], y0s[c], 400, 300,
            f == F_CT32 ? GS_HR_READ_32 : GS_HR_READ_16, 0, 0x80, out, 400 * 4, colbuf);
         for (Y = 0; Y < 300; Y++)
            for (X = 0; X < 400; X++)
            {
               const unsigned v = elem(l, vm, pa_ref(f, x0s[c] + X / 2, y0s[c] + Y / 2, bp, bw));
               if (out[Y * 400 + X] != expand(f, v) && fail++ < 4)
                  printf("  %s read from (%d,%d): (%d,%d) %08x, want %08x\n", fmt_name[f], x0s[c], y0s[c], X, Y, out[Y * 400 + X], expand(f, v));
            }
      }
   }
   return fail;
}

/* The GS's address of texel (x, y) of a texture at block bp (any block),
 * bw pages wide: its blocks run on from bp across page boundaries. */
static unsigned pa_blocks(int f, int x, int y, unsigned bp, unsigned bw)
{
   const unsigned page0 = pa_ref(f, x, y, 0, bw) / (unsigned)layouts[f].page_elems;
   const unsigned in    = pa_ref(f, x, y, 0, bw) % (unsigned)layouts[f].page_elems;
   const unsigned epb   = 256u / (unsigned)layouts[f].elem_bytes;
   const unsigned blk   = (bp + page0 * 32u + in / epb) & 16383u;
   return blk * epb + in % epb;
}

/* The 2x texels of a texture are each native texel's 2x2 from the 2x
 * memory where it holds the texel's block, expanded with TEXA, and the
 * native texel twice where it does not, for textures based anywhere. */
static int check_texels(void)
{
   static unsigned addr[64], native[64], out[128];
   static const int fmts[3] = { F_CT32, F_CT16, F_Z16 };
   static const unsigned bps[3] = { 0, 3 * 32 + 5, 1000 * 16 + 17 };
   int fail = 0, fi, c, x, y, sy, aem, held = 0, other = 0;
   for (fi = 0; fi < 3; fi++)
   {
      const int f = fmts[fi];
      const gs_hr_layout_t *l = &layouts[f];
      const unsigned epb = 256u / (unsigned)l->elem_bytes;
      int p;
      scribble();
      /* some zero texels, for AEM */
      for (p = 0; p < 4096; p += 7)
         vm[(size_t)p * 97u % (4u << 20)] = 0;
      gs_hr_pages_reset(&pages);
      for (p = 0; p < 512; p++)
         gs_hr_page_refresh(&pages, l, (unsigned)f, (unsigned)p, 0xffffffffu, vm, hr);
      /* some blocks stale, some pages in another layout */
      for (p = 0; p < 16384; p += 5)
         gs_hr_mark_block(&pages, (unsigned)p);
      for (p = 0; p < 512; p += 9)
         pages.psm[p] = (unsigned char)(f == F_CT32 ? F_Z32 : F_CT32);
      for (c = 0; c < 3; c++)
         for (aem = 0; aem < 2; aem++)
            for (y = 0; y < 70; y++)
               for (sy = 0; sy < 2; sy++)
               {
                  for (x = 0; x < 64; x++)
                  {
                     addr[x] = pa_blocks(f, 30 + x, y, bps[c], 4);
                     native[x] = 0x5a000000u | (unsigned)(x * 1000 + y);
                  }
                  gs_hr_read_texels(l, &pages, (unsigned)f, hr, addr, native, 64, sy,
                     is16(f) ? GS_HR_READ_16 : GS_HR_READ_32, aem, 0x40, 0x80, out);
                  for (x = 0; x < 128; x++)
                  {
                     const unsigned a = addr[x / 2];
                     const unsigned page = a / (unsigned)l->page_elems;
                     const int in = pages.psm[page] == (unsigned)f && !(pages.stale[page] & (1u << ((a / epb) & 31)));
                     const unsigned v = elem(l, vm, a);
                     unsigned want = f == F_CT32 ? v : expand(f, v);
                     if (is16(f))
                     {
                        want = (want & 0xffffffu) | ((v & 0x8000u) ? 0x80000000u : 0x40000000u);
                        if (aem && v == 0)
                           want &= 0xffffffu;
                     }
                     if (!in)
                        want = native[x / 2];
                     held += in;
                     other += !in;
                     if (out[x] != want && fail++ < 4)
                        printf("  %s texels bp %u aem %d (%d,%d) sub %d: %08x, want %08x\n", fmt_name[f], bps[c], aem, 30 + x / 2, y, sy, out[x], want);
                  }
               }
   }
   if (!held || !other)
   {
      printf("  texels: %d from the 2x memory, %d native; both kinds wanted\n", held, other);
      fail++;
   }
   return fail;
}

/* The texel a native sprite pixel reads, nearest: pixel k of a sprite from
 * p0 (t0) to p1 (t1), sampled at k. */
static int texel_at(float p0, float p1, float t0, float t1, double x)
{
   return (int)floor((t0 + (x - p0) * (t1 - t0) / (p1 - p0)) / 65536.0);
}

static int check_sprites(void)
{
   /* edges, coordinates in texels: 1:1 on texel edges and centres, both
    * directions, 121 over 120, 65 over 64, 2:1, 1:2, and half-pixel edges */
   static const float cases[][4] = {
      { 501.0f, 621.0f, 128.0f, 248.0f },
      { 64.0f, 128.0f, 0.5f, 64.5f },
      { 10.0f, 40.0f, 40.5f, 10.5f },
      { 10.0f, 40.0f, 40.0f, 10.0f },
      { 501.0f, 621.0f, 128.0f, 249.0f },
      { 527.0f, 591.0f, 192.0f, 257.0f },
      { 0.0f, 100.0f, 0.0f, 200.0f },
      { 0.0f, 100.0f, 3.5f, 53.5f },
      { 0.5f, 223.5f, 0.5f, 223.5f },
      { 10.5f, 110.5f, 0.0f, 100.0f }
   };
   int fail = 0, c, X, s2, negative = 0;
   for (c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++)
   {
      const float P0 = cases[c][0], P1 = cases[c][1];
      const float T0 = cases[c][2] * 65536.0f, T1 = cases[c][3] * 65536.0f;
      const int k0 = (int)ceil(P0), k1 = (int)ceil(P1); /* native pixels k0 .. k1 - 1 */
      const double du = (T1 - T0) / (P1 - P0) / 65536.0;
      const int one = fabs(fabs(du) - 1.0) < 1e-9;
      const int hair = !one && fabs(fabs(du) - 1.0) < 0.02;
      const int lo_t = du >= 0 ? texel_at(P0, P1, T0, T1, k0) : texel_at(P0, P1, T0, T1, k1 - 1);
      const int hi_t = du >= 0 ? texel_at(P0, P1, T0, T1, k1 - 1) : texel_at(P0, P1, T0, T1, k0);
      float p0 = P0, p1 = P1, t0 = T0, t1 = T1;

      gs_hr_snap_sprite(&p0, &p1, &t0, &t1);
      if (p0 != (float)k0 || p1 != (float)k1 || texel_at(p0, p1, t0, t1, k0 + 0.25) != texel_at(P0, P1, T0, T1, k0 + 0.25))
      {
         printf("  sprite %d: snapped to %g..%g, coordinates off the line\n", c, p0, p1);
         fail++;
      }
      gs_hr_fit_nearest(p0, p1, &t0, &t1);

      for (s2 = 1; s2 <= 2; s2++) /* a native texture, a 2x one */
         for (X = 2 * k0; X < 2 * k1; X++)
         {
            /* 2x pixel X, drawn from vertices at 2 p0 and 2 p1 */
            const double tx = (t0 + (X - 2.0 * p0) * (t1 - t0) / (2.0 * (p1 - p0))) * s2 / 65536.0;
            const int got = (int)floor(tx);
            const int nat = texel_at(P0, P1, T0, T1, X / 2);
            int want_lo, want_hi;
            if (one || hair)
            {
               want_lo = s2 * nat;
               want_hi = s2 * nat + s2 - 1;
               if (one && s2 == 2)
                  want_lo = want_hi = 2 * nat + ((du > 0) ? (X & 1) : 1 - (X & 1));
            }
            else
            {
               want_lo = s2 * lo_t;
               want_hi = s2 * hi_t + s2 - 1;
            }
            if ((got < want_lo || got > want_hi) && fail++ < 8)
               printf("  sprite %d at %s: 2x pixel %d reads texel %d, want %d..%d\n", c, s2 == 1 ? "1x" : "2x", X, got, want_lo, want_hi);
         }

      /* negative control: the plain 2x coordinates, 1:1 on texel centres */
      if (c == 1)
         for (X = 2 * k0; X < 2 * k1; X++)
            if ((int)floor((T0 + (X - 2.0 * P0) * (T1 - T0) / (2.0 * (P1 - P0))) / 65536.0) != texel_at(P0, P1, T0, T1, X / 2))
               negative++;
   }
   if (negative < 60)
   {
      printf("  the plain 2x coordinates read the native texels (negative control, %d off)\n", negative);
      fail++;
   }
   return fail;
}

static int check_wrap(void)
{
   unsigned short mn, mx;
   unsigned mask, mode;
   int fail = 0;

   mode = gs_hr_wrap_axis(GS_HR_REPEAT, 256, 0, 0, 0, 256, &mn, &mx, &mask);
   if (mode != GS_HR_REPEAT || mn != 511 || mx != 0 || mask != 0xffffffffu)
      fail++, printf("  whole repeat: mode %u %u %u %x\n", mode, mn, mx, mask);
   mode = gs_hr_wrap_axis(GS_HR_REPEAT, 512, 0, 0, 476, 482, &mn, &mx, &mask);
   if (mode != GS_HR_REGION_CLAMP || mn != 952 || mx != 963 || mask != 0)
      fail++, printf("  repeat within an area: mode %u %u %u\n", mode, mn, mx);
   mode = gs_hr_wrap_axis(GS_HR_CLAMP, 1024, 0, 0, 0, 641, &mn, &mx, &mask);
   if (mode != GS_HR_REGION_CLAMP || mn != 0 || mx != 1281)
      fail++, printf("  clamp: mode %u %u %u\n", mode, mn, mx);
   mode = gs_hr_wrap_axis(GS_HR_REGION_CLAMP, 256, 10, 20, 0, 256, &mn, &mx, &mask);
   if (mode != GS_HR_REGION_CLAMP || mn != 20 || mx != 41)
      fail++, printf("  region clamp: mode %u %u %u\n", mode, mn, mx);
   return fail;
}

int main(void)
{
   int fail = 0, f;
   vm = (unsigned char*)malloc(4u << 20);
   hr = (unsigned char*)malloc(16u << 20);
   if (!vm || !hr)
      return 2;
   for (f = 0; f < NFMT; f++)
      make_layout(f);
   fail += check_sum();
   fail += check_runs();
   fail += check_bijection();
   fail += check_refresh();
   fail += check_read();
   fail += check_texels();
   fail += check_sprites();
   fail += check_wrap();
   free(vm);
   free(hr);
   printf("software 2x memory: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
