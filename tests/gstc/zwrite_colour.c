/* A constant depth write into memory a colour target holds reaches the
 * colour target.
 *
 * A sprite with ZTST ALWAYS and one Z writes that Z into memory in the Z
 * format's layout. When the pages it writes belong to a colour target -- a
 * buffer one page column wide drawn in turn at two neighbouring pages, the
 * draw into one page column writing a constant Z into the other -- the
 * next draw reads those pages as colour. GSRendererHW::
 * WriteDepthIntoColourMemory writes the Z into local memory and refreshes
 * the colour target's pages from it, instead of keeping the write in a
 * depth buffer nothing reads as colour.
 *
 * Pinned here against the GS's own addressing (PSMCT32 and PSMZ32 block
 * tables, the shared column table):
 *   - a PSMZ32 page is a PSMCT32 page with its 32x16 quadrants swapped,
 *     the relation the rule relies on being harmless for whole pages;
 *   - for every page-aligned write the rule takes, the colour pixels it
 *     refreshes are exactly the colour pixels the write changed, and each
 *     reads the Z (PSMZ24 keeps the alpha byte);
 *   - the rects GSRendererHW::FillDepthWriteIntoTarget fills in the
 *     colour target, page by page through the target's own layout and
 *     merged, cover exactly the target pixels the write changed, and the
 *     width they need is the width the target is widened to; filling the
 *     write's own rect instead (no relayout) is the negative control;
 *   - the rule refuses a write sharing a page with the draw's own colour,
 *     and a write covering part of a page; the negative control shows a
 *     half-page write leaves pixels of its page unwritten, which a page
 *     refresh would have reloaded from stale memory.
 *
 * Build and run, from tests/gstc:
 *   cc -O2 -std=c89 -pedantic -Wall zwrite_colour.c -o zwrite_colour
 *   ./zwrite_colour
 */
#include <stdio.h>
#include <stdlib.h>

typedef unsigned int u32;

#define MAX_PAGES 512u
#define WORDS (MAX_PAGES * 2048u)

static const unsigned char block_ct32[4][8] = {
   { 0, 1, 4, 5, 16, 17, 20, 21 },
   { 2, 3, 6, 7, 18, 19, 22, 23 },
   { 8, 9, 12, 13, 24, 25, 28, 29 },
   { 10, 11, 14, 15, 26, 27, 30, 31 }
};

static const unsigned char block_z32[4][8] = {
   { 24, 25, 28, 29, 8, 9, 12, 13 },
   { 26, 27, 30, 31, 10, 11, 14, 15 },
   { 16, 17, 20, 21, 0, 1, 4, 5 },
   { 18, 19, 22, 23, 2, 3, 6, 7 }
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

/* Word address of pixel (x, y), bp in blocks, bw in pages. */
static u32 word_at(const unsigned char table[4][8], u32 x, u32 y, u32 bp, u32 bw)
{
   const u32 page = (y / 32) * bw + x / 64;
   const u32 block = bp + page * 32 + table[(y % 32) / 8][(x % 64) / 8];
   return (block & 16383u) * 64 + column_table32[y % 8][x % 8];
}

typedef struct
{
   u32 x, y, z, w; /* left, top, right, bottom, pixels */
} rect_t;

typedef struct
{
   u32 fbp, zbp, fbw; /* pages, pages, pages wide */
   rect_t r;
   int z24;
   int colour_written;
} draw_t;

/* The guards of WriteDepthIntoColourMemory that depend on addresses:
 * whole Z pages within the buffer width, none shared with the colour the
 * draw writes. */
static int rule_takes(const draw_t* d)
{
   static u32 frame_pages[MAX_PAGES / 32];
   u32 px, py, page, i;

   if (d->fbw == 0 || d->r.z > d->fbw * 64 ||
       (d->r.x & 63) || (d->r.z & 63) || (d->r.y & 31) || (d->r.w & 31))
      return 0;
   if (!d->colour_written)
      return 1;
   for (i = 0; i < MAX_PAGES / 32; i++)
      frame_pages[i] = 0;
   for (py = d->r.y / 32; py <= (d->r.w - 1) / 32; py++)
      for (px = d->r.x / 64; px <= (d->r.z - 1) / 64; px++)
      {
         page = (d->fbp + py * d->fbw + px) & (MAX_PAGES - 1);
         frame_pages[page >> 5] |= 1u << (page & 31);
      }
   for (py = d->r.y / 32; py < d->r.w / 32; py++)
      for (px = d->r.x / 64; px < d->r.z / 64; px++)
      {
         page = (d->zbp + py * d->fbw + px) & (MAX_PAGES - 1);
         if (frame_pages[page >> 5] & (1u << (page & 31)))
            return 0;
      }
   return 1;
}


#define MAX_FILL_RECTS 32

/* FillDepthWriteIntoTarget's mapping: each written page placed by its
 * page number in the target's layout, merged below or to the right.
 * Returns the rect count, 0 where it gives up; *need_w the width. */
static u32 fill_rects(const draw_t* d, u32 tpage, u32 tbw, u32 th, rect_t* rects, u32* need_w, int relayout)
{
   u32 n = 0, px, py, zpage, rel, x, y;
   rect_t pr;

   *need_w = 0;
   for (py = d->r.y / 32; py < d->r.w / 32; py++)
      for (px = d->r.x / 64; px < d->r.z / 64; px++)
      {
         zpage = d->zbp + py * d->fbw + px;
         if (zpage < tpage)
            return 0;
         rel = zpage - tpage;
         x = relayout ? (rel % tbw) * 64 : px * 64;
         y = relayout ? (rel / tbw) * 32 : py * 32;
         if (y + 32 > th)
            return 0;
         if (x + 64 > *need_w)
            *need_w = x + 64;
         pr.x = x; pr.y = y; pr.z = x + 64; pr.w = y + 32;
         if (n && rects[n - 1].x == pr.x && rects[n - 1].z == pr.z && rects[n - 1].w == pr.y)
            rects[n - 1].w = pr.w;
         else if (n && rects[n - 1].y == pr.y && rects[n - 1].w == pr.w && rects[n - 1].z == pr.x)
            rects[n - 1].z = pr.z;
         else if (n == MAX_FILL_RECTS)
            return 0;
         else
            rects[n++] = pr;
      }
   return n;
}

/* Whether the fill's rects are exactly the target pixels whose words the
 * write changed, and need_w the rightmost of them. */
static int fill_matches(u32* mem, const draw_t* d, u32 tpage, u32 tbw, u32 th, int relayout)
{
   static rect_t rects[MAX_FILL_RECTS];
   u32 n, need_w, x, y, i, a, max_x = 0;
   int in_fill, changed;

   n = fill_rects(d, tpage, tbw, th, rects, &need_w, relayout);
   if (n == 0)
      return 0;
   for (i = 0; i < WORDS; i++)
      mem[i] = 0;
   for (y = d->r.y; y < d->r.w; y++)
      for (x = d->r.x; x < d->r.z; x++)
         mem[word_at(block_z32, x, y, d->zbp * 32, d->fbw)] = 1;
   for (y = 0; y < th; y++)
      for (x = 0; x < tbw * 64; x++)
      {
         a = word_at(block_ct32, x, y, tpage * 32, tbw);
         changed = mem[a] != 0;
         in_fill = 0;
         for (i = 0; i < n; i++)
            if (x >= rects[i].x && x < rects[i].z && y >= rects[i].y && y < rects[i].w)
               in_fill = 1;
         if (changed != in_fill)
            return 0;
         if (changed && x + 1 > max_x)
            max_x = x + 1;
      }
   return max_x == need_w;
}

static int failures;

static void check(int ok, const char* what)
{
   printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
   if (!ok)
      failures++;
}

/* The quadrant swap: Z pixel (x, y) is colour pixel (x ^ 32, y ^ 16). */
static int quadrants_swap(void)
{
   u32 x, y;
   for (y = 0; y < 32; y++)
      for (x = 0; x < 64; x++)
         if (word_at(block_z32, x, y, 0, 1) != word_at(block_ct32, x ^ 32, y ^ 16, 0, 1))
            return 0;
   return 1;
}

/* Writes the draw's Z as the GS does and compares what the colour target
 * (tbp pages, tbw wide, th tall) reads with what the rule refreshes. */
static int refresh_matches(u32* mem, const draw_t* d, u32 tbp, u32 tbw, u32 th, u32 zval)
{
   const u32 fill = 0x5A5A5A5Au;
   u32 x, y, a, page, i, before, after, want;
   int refreshed, changed;

   for (i = 0; i < WORDS; i++)
      mem[i] = fill;

   for (y = d->r.y; y < d->r.w; y++)
      for (x = d->r.x; x < d->r.z; x++)
      {
         a = word_at(block_z32, x, y, d->zbp * 32, d->fbw);
         mem[a] = d->z24 ? ((mem[a] & 0xFF000000u) | (zval & 0x00FFFFFFu)) : zval;
      }

   want = d->z24 ? ((fill & 0xFF000000u) | (zval & 0x00FFFFFFu)) : zval;
   for (y = 0; y < th; y++)
      for (x = 0; x < tbw * 64; x++)
      {
         a = word_at(block_ct32, x, y, tbp * 32, tbw);
         page = a / 2048;
         refreshed = 0;
         {
            u32 py, px;
            /* Every page the write touches, as a page refresh takes it. */
            for (py = d->r.y / 32; py < (d->r.w + 31) / 32 && !refreshed; py++)
               for (px = d->r.x / 64; px < (d->r.z + 63) / 64; px++)
                  if (((d->zbp + py * d->fbw + px) & (MAX_PAGES - 1)) == page)
                  {
                     refreshed = 1;
                     break;
                  }
         }
         before = fill;
         after = mem[a];
         changed = after != before;
         if (refreshed != changed)
            return 0;
         if (refreshed && after != want)
            return 0;
      }
   return 1;
}

int main(void)
{
   u32* mem = (u32*)malloc(WORDS * sizeof(u32));
   draw_t d;
   u32 h, zbp_off;
   int all;

   if (!mem)
      return 2;

   printf("zwrite_colour\n");
   check(quadrants_swap(), "a PSMZ32 page is a PSMCT32 page with its quadrants swapped");

   /* A two-page-wide buffer at page 0xD2; colour into its first page
    * column, Z into its second, and the reverse. */
   all = 1;
   for (zbp_off = 0; zbp_off < 2; zbp_off++)
      for (h = 32; h <= 480; h += 32)
      {
         d.fbp = 0xD2 + zbp_off;
         d.zbp = 0xD2 + (1 - zbp_off);
         d.fbw = 2;
         d.r.x = 0; d.r.y = 0; d.r.z = 64; d.r.w = h;
         d.z24 = 0;
         d.colour_written = 1;
         if (!rule_takes(&d) || !refresh_matches(mem, &d, 0xD2, 2, 480, 0x00BFBFBFu))
            all = 0;
         d.z24 = 1;
         if (!rule_takes(&d) || !refresh_matches(mem, &d, 0xD2, 2, 480, 0x00BFBFBFu))
            all = 0;
      }
   check(all, "neighbouring page columns: refreshed pixels are the written ones, at the Z");

   /* Wider buffers and offsets: Z two page columns over, two pages wide. */
   all = 1;
   for (h = 32; h <= 256; h += 32)
   {
      d.fbp = 0x100; d.zbp = 0x102; d.fbw = 6;
      d.r.x = 0; d.r.y = 0; d.r.z = 128; d.r.w = h;
      d.z24 = 0; d.colour_written = 1;
      if (!rule_takes(&d) || !refresh_matches(mem, &d, 0x100, 6, 256, 0x12345678u))
         all = 0;
      d.r.x = 64; d.r.z = 128; d.zbp = 0x103;
      if (!rule_takes(&d) || !refresh_matches(mem, &d, 0x100, 6, 256, 0x00ABCDEFu))
         all = 0;
   }
   check(all, "wider buffers and offset rects: refreshed pixels are the written ones");

   d.fbp = 0xD2; d.zbp = 0xD2; d.fbw = 2;
   d.r.x = 0; d.r.y = 0; d.r.z = 64; d.r.w = 448;
   d.z24 = 0; d.colour_written = 1;
   check(!rule_takes(&d), "Z sharing pages with the draw's colour is refused");
   d.colour_written = 0;
   check(rule_takes(&d), "the same Z with no colour written is taken");

   d.fbp = 0xD2; d.zbp = 0xD3; d.fbw = 2;
   d.r.x = 0; d.r.y = 0; d.r.z = 32; d.r.w = 448;
   d.colour_written = 1;
   check(!rule_takes(&d), "a write over half a page's width is refused");
   check(!refresh_matches(mem, &d, 0xD2, 2, 480, 0x00BFBFBFu),
      "negative control: a half-page write leaves pixels of its page unwritten");
   d.r.z = 64; d.r.w = 440;
   check(!rule_takes(&d), "a write ending part way down a page is refused");
   d.r.w = 448; d.r.z = 192;
   check(!rule_takes(&d), "a write wider than the buffer is refused");


   /* The fill: neighbouring page columns, then wider buffers at other
    * widths and offsets, the write's width differing from the target's. */
   all = 1;
   for (h = 32; h <= 480; h += 32)
   {
      d.fbp = 0xD2; d.zbp = 0xD3; d.fbw = 2;
      d.r.x = 0; d.r.y = 0; d.r.z = 64; d.r.w = h;
      if (!fill_matches(mem, &d, 0xD2, 2, 480, 1))
         all = 0;
      d.fbp = 0xD3; d.zbp = 0xD2;
      if (!fill_matches(mem, &d, 0xD2, 2, 480, 1))
         all = 0;
   }
   for (h = 32; h <= 128; h += 32)
   {
      d.zbp = 0x103; d.fbw = 3;
      d.r.x = 0; d.r.y = 0; d.r.z = 128; d.r.w = h;
      if (!fill_matches(mem, &d, 0x100, 6, 256, 1))
         all = 0;
      d.zbp = 0x101; d.fbw = 6; d.r.x = 64; d.r.z = 256;
      if (!fill_matches(mem, &d, 0x100, 6, 256, 1))
         all = 0;
   }
   check(all, "the fill's rects are the target pixels the write changed, widened to their width");
   d.zbp = 0x103; d.fbw = 3; d.r.x = 0; d.r.y = 0; d.r.z = 128; d.r.w = 64;
   check(!fill_matches(mem, &d, 0x100, 6, 256, 0),
      "negative control: the write's own rect, not relaid out, is not what it changed");
   d.zbp = 0xD0; d.fbw = 2; d.r.x = 0; d.r.z = 64; d.r.w = 64;
   check(fill_rects(&d, 0xD2, 2, 480, (rect_t*)mem, &h, 1) == 0, "pages before the target are refused");

   free(mem);
   if (failures)
   {
      printf("zwrite_colour: %d FAILED\n", failures);
      return 1;
   }
   printf("zwrite_colour: ok\n");
   return 0;
}
