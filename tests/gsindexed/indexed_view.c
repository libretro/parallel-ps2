/* An 8-bit view of a 32-bit render target, on the GPU and at scale.
 *
 * Games read a colour buffer back through PSMT8 to run every byte of every
 * pixel through a palette (a colour grade that works one 64x32 block at
 * a time, three channels summed, is one such use). The hardware
 * renderer builds that view with ps_convert_rgba_8i, which maps each texel
 * of the 8-bit page back to the pixel and byte it aliases. Three things
 * about that path are pinned here, each against the swizzle tables rather
 * than against itself:
 *
 *   1. The texel -> (pixel, byte) mapping the shader computes matches what
 *      the GS block and column tables say for PSMT8 over PSMCT32, for every
 *      texel of a page, including a texture that starts on a later page of
 *      the target (PageOffset) and a target pitch that differs from the
 *      texture's (SBW vs DBW).
 *   2. At an integer scale the converted texture is the scaled index
 *      texture: texel index picks the pixel, position inside the texel
 *      picks the sample inside that pixel.
 *   3. A draw that samples such a texture picks the same texels at any
 *      scale as it does at native resolution (the PS_SAMPLE_MAP path takes
 *      the coordinate at the first fragment of the native pixel), so a
 *      3:1 minifying sprite lands on the rows a native draw reads, and a
 *      point-sampled 32-bit block copy offset by the half texel the GS
 *      sample point asks for is a copy at every scale, not one shifted by
 *      half a native texel.
 *
 * Also pinned: the channel shuffle emulation, which fetches by position,
 * is only used for draws that put one channel of each texel on the pixel
 * it aliases (IsChannelShuffleIdentity), or that copy a buffer page by
 * page; a 3:1 block draw and a copy of a block to another page go through
 * the exact conversion instead. The fetch starts at the
 * page the texture begins on, and a draw addressed at a page of a target
 * lands on that page: both use the page's position in the target, which is
 * checked against the GS page addressing, as is a read whose base lies
 * before a target but whose coordinates land on it. A target drawn at
 * another buffer width keeps its pages by moving each to where the new
 * width puts it, a draw within one page goes to that page's place in
 * the target whatever width it was drawn with, and a shuffle's draws are
 * skipped as repeats only where they land on what was drawn already.
 * Every scaled sprite covers the native pixels the rasteriser's ceil()
 * gives it, its coordinates taken along its own line.
 *
 * Build and run, from tests/gsindexed:
 *   cc -O2 -std=c89 -pedantic -Wall indexed_view.c -o indexed_view -lm
 *   ./indexed_view
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

typedef unsigned int u32;

/* ---- GS swizzle tables (GSTables.c) ------------------------------------ */

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

static const unsigned char column_table8[16][16] = {
   { 0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54 },
   { 8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62 },
   { 33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23 },
   { 41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31 },
   { 96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86 },
   { 104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94 },
   { 65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119 },
   { 73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127 },
   { 128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182 },
   { 136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190 },
   { 161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151 },
   { 169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159 },
   { 224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214 },
   { 232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222 },
   { 193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247 },
   { 201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255 }
};

/* Byte address inside a page of PSMCT32 (64x32 pixels, 4 bytes each). */
static u32 addr32(u32 x, u32 y)
{
   return block_table[y / 8][x / 8] * 256u + column_table32[y % 8][x % 8] * 4u;
}

/* Byte address inside a page of PSMT8 (128x64 texels). */
static u32 addr8(u32 u, u32 v)
{
   return block_table[v / 16][u / 16] * 256u + column_table8[v % 16][u % 16];
}

/* ---- ps_convert_rgba_8i, as the three shaders write it ----------------- */

struct fetch
{
   u32 x, y;   /* pixel of the source target */
   u32 byte;   /* 0 = R, 1 = G, 2 = B, 3 = A */
};

static struct fetch shader_convert(u32 pos_x, u32 pos_y, u32 sbw, u32 dbw, u32 page_offset, u32 scale)
{
   struct fetch f;
   u32 sub_x = 0, sub_y = 0;
   u32 block_x, block_y, sub_bx, sub_by, coord_x, coord_y;
   u32 bxy_x, bxy_y, block_num, boff_x, boff_y;
   u32 is_col23, is_col13, is_col12;

   if (scale > 1)
   {
      sub_x = pos_x % scale;
      sub_y = pos_y % scale;
      pos_x /= scale;
      pos_y /= scale;
   }

   block_x = (pos_x & ~15u) >> 1;
   block_y = (pos_y & ~3u) >> 1;
   sub_bx = pos_x & 7u;
   sub_by = pos_y & 1u;
   coord_x = block_x | sub_bx;
   coord_y = block_y | sub_by;

   bxy_x = coord_x / 64u;
   bxy_y = coord_y / 32u;
   block_num = bxy_y * (dbw / 128u) + bxy_x + page_offset;
   boff_x = (block_num % (sbw / 64u)) * 64u;
   boff_y = (block_num / (sbw / 64u)) * 32u;
   coord_x = (coord_x % 64u) + boff_x;
   coord_y = (coord_y % 32u) + boff_y;

   is_col23 = pos_y & 4u;
   is_col13 = pos_y & 2u;
   is_col12 = is_col23 ^ (is_col13 << 1);
   coord_x ^= is_col12;

   f.x = coord_x * scale + sub_x;
   f.y = coord_y * scale + sub_y;
   f.byte = ((pos_y & 2u) ? 1u : 0u) | ((pos_x & 8u) ? 2u : 0u);
   return f;
}

/* GSDevice::IndexedConversionPageOffset */
static u32 page_offset_of(u32 offset_x, u32 offset_y, u32 sbw)
{
   return (offset_y / 32) * ((sbw < 64 ? 64 : sbw) / 64) + (offset_x / 64);
}

/* ---- 1. the swizzle ---------------------------------------------------- */

static int check_swizzle(void)
{
   /* Target: 640 wide (SBW 640), texture TBW 20 (DBW 1280), a common
    * pairing; the texture starts `page` pages into the target. */
   static const u32 pages[] = { 0, 1, 2, 9, 10, 23, 69 };
   int fail = 0;
   size_t pi;

   for (pi = 0; pi < sizeof(pages) / sizeof(pages[0]); pi++)
   {
      const u32 page = pages[pi];
      const u32 sbw = 640, dbw = 1280;
      u32 u, v;
      /* the target pixel this page starts on (10 pages per row) */
      const u32 base_x = (page % 10) * 64, base_y = (page / 10) * 32;
      const u32 poff = page_offset_of(base_x, base_y, sbw);

      if (poff != page)
      {
         printf("  page offset: pixel offset %u,%u gives page %u, want %u\n", base_x, base_y, poff, page);
         fail++;
      }

      for (v = 0; v < 64; v++)
      {
         for (u = 0; u < 128; u++)
         {
            const u32 a = addr8(u, v);           /* byte the texel aliases */
            struct fetch f = shader_convert(u, v, sbw, dbw, poff, 1);
            u32 ax;

            if (f.x < base_x || f.x >= base_x + 64 || f.y < base_y || f.y >= base_y + 32)
            {
               if (fail++ < 5)
                  printf("  page %u texel %u,%u fetched outside its page: %u,%u\n", page, u, v, f.x, f.y);
               continue;
            }
            ax = addr32(f.x - base_x, f.y - base_y) + f.byte;
            if (ax != a)
            {
               if (fail++ < 5)
                  printf("  page %u texel %u,%u: shader byte %u, tables byte %u\n", page, u, v, ax, a);
            }
         }
      }
   }
   printf("swizzle against the block tables: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* ---- 2. the scaled conversion ------------------------------------------ */

static int check_scaled(void)
{
   static const u32 scales[] = { 2, 3, 4, 8 };
   int fail = 0;
   size_t si;

   for (si = 0; si < sizeof(scales) / sizeof(scales[0]); si++)
   {
      const u32 s = scales[si];
      u32 X, Y;
      for (Y = 0; Y < 64 * s; Y++)
      {
         for (X = 0; X < 128 * s; X++)
         {
            struct fetch a = shader_convert(X, Y, 640, 1280, 3, s);
            struct fetch b = shader_convert(X / s, Y / s, 640, 1280, 3, 1);
            if (a.byte != b.byte || a.x != b.x * s + X % s || a.y != b.y * s + Y % s)
            {
               if (fail++ < 5)
                  printf("  scale %u texel %u,%u: %u,%u byte %u; native %u,%u byte %u\n", s, X, Y, a.x, a.y, a.byte, b.x, b.y, b.byte);
            }
         }
      }
   }
   printf("scaled conversion is the scaled index texture: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* ---- 3. texel choice at scale ------------------------------------------ */

/* A sprite as the game gives it: pixel box and texel box, 1/16 units. */
struct sprite
{
   int x0, y0, x1, y1;   /* pixels, 1/16 */
   int u0, v0, u1, v1;   /* texels, 1/16 */
};

/* The texel row a native draw reads for pixel row Y: the coordinate at the
 * row itself, stepping by the sprite's ratio (what GSRendererHW's
 * rasterisation resolves to, and what the software renderer matches). */
static int native_row_texel(const struct sprite* s, int Y)
{
   const double dv = (double)(s->v1 - s->v0) / (double)(s->y1 - s->y0);
   const double v = (double)s->v0 / 16.0 + dv * (double)(Y * 16 - s->y0) / 16.0;
   return (int)v;
}

/* PS_SAMPLE_MAP: fragment row Y at scale S -> texel row and sample row.
 * The hardware renderer lands the sprite's first texel coordinate on the
 * centre of its first fragment row and steps by the ratio per fragment;
 * the shader takes that interpolant and moves it to the first fragment of
 * the native pixel (d = floor(frag / S) * S + 0.5 - frag). */
static void mapped_row(const struct sprite* s, int S, int Y, int* texel, int* sub)
{
   const double dv = (double)(s->v1 - s->v0) / (double)(s->y1 - s->y0) / (double)S;
   const double y0 = (double)s->y0 / 16.0 * S;
   const double frag = (double)Y + 0.5;
   const double v_frag = (double)s->v0 / 16.0 + dv * (frag - 0.5 - y0);
   const double d = (double)((Y / S) * S) + 0.5 - frag;
   *texel = (int)(v_frag + dv * d);
   *sub = Y % S;
}

static int check_sample_map(void)
{
   /* Draw A: 8x6 pixel sprite from an 8x18 texel box (3:1), draw B: 16x2
    * from 16x2, 0.5-texel offsets as a game sends them, and the
    * 32-bit block copy that sums the three tinted views back into the
    * frame: 64x32 from 64x32 at 0.5,0.5 to 64.5,32.5, point sampled; and
    * the night noise tile, 64x32 pixels from 65x65 texels of a native-size
    * texture, whose 65th row and column no native pixel reads. */
   static const struct sprite sprites[] = {
      { 0 * 16, 2 * 16, 8 * 16, 8 * 16, 72, 8, 200, 296 },
      { 0 * 16, 0 * 16, 16 * 16, 2 * 16, 136, 8, 392, 40 },
      { 8 * 16, 10 * 16, 16 * 16, 16 * 16, 328, 264, 456, 552 },
      { 0, 0, 64 * 16, 32 * 16, 8, 8, 1032, 520 },
      { 16, 192 * 16, 1040, 224 * 16, 4416, 4640, 5456, 5680 }
   };
   static const int scales[] = { 2, 3, 4, 8 };
   int fail = 0;
   size_t si, pi;

   for (pi = 0; pi < sizeof(sprites) / sizeof(sprites[0]); pi++)
   {
      const struct sprite* s = &sprites[pi];
      for (si = 0; si < sizeof(scales) / sizeof(scales[0]); si++)
      {
         const int S = scales[si];
         int Y;
         for (Y = (s->y0 / 16) * S; Y < (s->y1 / 16) * S; Y++)
         {
            int texel, sub;
            const int native = native_row_texel(s, Y / S);
            mapped_row(s, S, Y, &texel, &sub);
            if (texel != native || sub != Y % S)
            {
               if (fail++ < 8)
                  printf("  sprite %u scale %d row %d: texel %d sub %d, native row %d reads texel %d\n",
                     (unsigned)pi, S, Y, texel, sub, Y / S, native);
            }
         }
      }
   }
   /* GSRendererHW::IsSampleMapDraw and the sample_map flag: the map is
    * for a point-sampled sprite with pixel coordinates at an integer
    * scale, and not for a shuffle, which moves bits between the halves of
    * each pixel and fetches the pixel by its own rule, nor for a read of
    * the target being drawn, which is fetched at the fragment's own place
    * (the map's fetch went to a texture that was not bound, and a
    * vignette's shuffled alpha came out zero at 2x and 4x, not at 1x). */
   {
      static const struct { int fst, sprite, linear, scale, tshuf, cshuf, tex_is_fb, map; } cases[] = {
         { 1, 1, 0, 4, 0, 0, 0, 1 },
         { 1, 1, 0, 1, 0, 0, 0, 0 },
         { 0, 1, 0, 4, 0, 0, 0, 0 },
         { 1, 0, 0, 4, 0, 0, 0, 0 },
         { 1, 1, 1, 4, 0, 0, 0, 0 },
         { 1, 1, 0, 4, 1, 0, 0, 0 },
         { 1, 1, 0, 4, 0, 1, 0, 0 },
         { 1, 1, 0, 2, 0, 0, 1, 0 },
         { 1, 1, 0, 4, 1, 0, 1, 0 }
      };
      size_t ci;
      for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++)
      {
         const int map = cases[ci].fst && cases[ci].sprite && !cases[ci].linear && cases[ci].scale > 1 &&
            !cases[ci].tshuf && !cases[ci].cshuf && !cases[ci].tex_is_fb;
         if (map != cases[ci].map)
         {
            printf("  sample map case %u: %d, expected %d\n", (unsigned)ci, map, cases[ci].map);
            fail++;
         }
      }
   }
   printf("sample map picks the native draw's texels: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* ---- 3b. sprite edges at scale ----------------------------------------- */

/* GSRendererHW::SnapSpriteEdges: an edge in 1/16 pixel moves up to the
 * pixel the rasteriser's ceil() lands it on. */
static int ceil16(int e)
{
   return -((-e) & ~15);
}

/* A sprite with its coordinates in pixels and texels, as the vertex
 * carries them after the snap (the texture coordinate then travels as a
 * float ST). */
struct fsprite
{
   double y0, y1, v0, v1;
};

static void snap_sprite(const struct sprite* s, struct fsprite* f)
{
   const int ny0 = ceil16(s->y0);
   const int ny1 = ceil16(s->y1);
   const double dv = (double)(s->v1 - s->v0) / (double)(s->y1 - s->y0);
   f->y0 = ny0 / 16.0;
   f->y1 = ny1 / 16.0;
   f->v0 = (s->v0 + (ny0 - s->y0) * dv) / 16.0;
   f->v1 = (s->v1 + (ny1 - s->y1) * dv) / 16.0;
}

/* The software rasteriser: pixel row Y belongs to the sprite when
 * ceil(y0) <= Y < ceil(y1). */
static int sw_covers(const struct sprite* s, int Y)
{
   return Y >= (s->y0 + 15) / 16 && Y < (s->y1 + 15) / 16;
}

/* The hardware renderer at scale S: fragment row I belongs to the sprite
 * when its edges, scaled, enclose it: y0 * S <= I < y1 * S. */
static int hw_covers(double y0, double y1, int S, int I)
{
   return (double)I >= y0 * S && (double)I < y1 * S;
}

static int check_sprite_edges(void)
{
   /* Two noise tiles of 64x32 pixels from 65x65 texels, one above the
    * other, with their rows at half pixels. */
   static const struct sprite tiles[2] = {
      { 16, 2568, 1040, 3080, 4416, 4640, 5456, 5680 },
      { 16, 3080, 1040, 3592, 4416, 4640, 5456, 5680 }
   };
   static const int scales[] = { 1, 2, 4, 8 };
   struct fsprite snapped[2];
   int fail = 0, split = 0;
   size_t si, t;
   int Y, I;

   if (ceil16(8) != 16 || ceil16(16) != 16 || ceil16(0) != 0 || ceil16(-8) != 0 || ceil16(-17) != -16 || ceil16(3080) != 3088)
   {
      printf("  ceil16 is not the rasteriser's ceil\n");
      fail++;
   }

   snap_sprite(&tiles[0], &snapped[0]);
   snap_sprite(&tiles[1], &snapped[1]);

   for (si = 0; si < sizeof(scales) / sizeof(scales[0]); si++)
   {
      const int S = scales[si];
      for (I = 150 * S; I < 240 * S; I++)
      {
         for (t = 0; t < 2; t++)
         {
            const int sw = sw_covers(&tiles[t], I / S);
            const int hw = hw_covers(snapped[t].y0, snapped[t].y1, S, I);
            if (hw != sw)
            {
               if (fail++ < 8)
                  printf("  tile %u scale %d fragment row %d: drawn %d, native row %d drawn %d\n", (unsigned)t, S, I, hw, I / S, sw);
            }
            if (hw_covers(tiles[t].y0 / 16.0, tiles[t].y1 / 16.0, S, I) != sw)
               split++;
         }
      }
   }
   if (!split)
   {
      printf("  the tiles' own edges never split a pixel between them\n");
      fail++;
   }

   /* Two bloom tiles magnified 4:1 from a 160x112 buffer, laid out by
    * page with a half-pixel gap between them (rows -1/16 .. 503/16 and
    * 511/16 .. 1015/16): natively the first covers rows 0..31 and the
    * second 32..63, so no row is left without its bloom; scaled, the
    * gap between the tiles' own edges would leave part of row 31 bare
    * on every page boundary. */
   {
      static const struct sprite bloom[2] = {
         { -1, -1, 1015, 503, 2, 2, 250, 122 },
         { -1, 511, 1015, 1015, 2, 130, 250, 250 }
      };
      struct fsprite bs[2];
      int bare = 0, gap = 0;
      snap_sprite(&bloom[0], &bs[0]);
      snap_sprite(&bloom[1], &bs[1]);
      if (bs[0].y0 != 0.0 || bs[0].y1 != 32.0 || bs[1].y0 != 32.0 || bs[1].y1 != 64.0)
      {
         printf("  bloom tiles snap to %g..%g and %g..%g, not 0..32 and 32..64\n", bs[0].y0, bs[0].y1, bs[1].y0, bs[1].y1);
         fail++;
      }
      for (I = 0; I < 64 * 8; I++)
      {
         if (!hw_covers(bs[0].y0, bs[0].y1, 8, I) && !hw_covers(bs[1].y0, bs[1].y1, 8, I))
            bare++;
         if (!hw_covers(bloom[0].y0 / 16.0, bloom[0].y1 / 16.0, 8, I) && !hw_covers(bloom[1].y0 / 16.0, bloom[1].y1 / 16.0, 8, I))
            gap++;
      }
      /* their own edges: half a pixel at the seam, and below the second */
      if (bare != 0 || gap != 8)
      {
         printf("  bloom tiles at 8x leave %d fragment rows bare (their own edges leave %d)\n", bare, gap);
         fail++;
      }
   }

   /* The texel a row reads is the one the native draw reads. */
   for (t = 0; t < 2; t++)
   {
      for (Y = (tiles[t].y0 + 15) / 16; Y < (tiles[t].y1 + 15) / 16; Y++)
      {
         const double dv = (snapped[t].v1 - snapped[t].v0) / (snapped[t].y1 - snapped[t].y0);
         const double v = snapped[t].v0 + dv * (Y - snapped[t].y0);
         const int native = native_row_texel(&tiles[t], Y);
         if ((int)v != native || v - (double)native < 1e-9)
         {
            if (fail++ < 8)
               printf("  tile %u row %d: texel %.6f, native reads %d\n", (unsigned)t, Y, v, native);
         }
      }
   }
   /* A sprite with float coordinates and a perspective divide (FST off):
    * S and Q both move along the sprite's line, so S/Q at every native
    * pixel is what it was. Edges at 3/16 and 5+9/16 pixels. */
   {
      const double x0 = 3.0 / 16.0, x1 = 5.0 + 9.0 / 16.0, s0 = 0.25, s1 = 0.75, q0 = 1.0, q1 = 2.0;
      const double nx0 = 1.0, nx1 = 6.0;
      const double f0 = (nx0 - x0) / (x1 - x0), f1 = (nx1 - x1) / (x1 - x0);
      const double ns0 = s0 + f0 * (s1 - s0), ns1 = s1 + f1 * (s1 - s0);
      const double nq0 = q0 + f0 * (q1 - q0), nq1 = q1 + f1 * (q1 - q0);
      int X;
      for (X = 1; X < 6; X++)
      {
         const double a = (X - x0) / (x1 - x0), b = (X - nx0) / (nx1 - nx0);
         const double before = (s0 + a * (s1 - s0)) / (q0 + a * (q1 - q0));
         const double after = (ns0 + b * (ns1 - ns0)) / (nq0 + b * (nq1 - nq0));
         if (fabs(before - after) > 1e-12)
         {
            if (fail++ < 8)
               printf("  pixel %d: S/Q %.9f before the snap, %.9f after\n", X, before, after);
         }
      }
   }
   printf("sprite edges land on native pixels: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* A sprite read from local memory, minified 33:16 vertically, its top
 * edge on a half line (an odd field drawn half a line down): natively
 * its first row is 12 and reads texel row 161, so row 160 is never
 * shown. Drawn scaled from its own edges, the fragments between 11.5 and
 * 12 read row 160; snapped, every fragment row belongs to the native row
 * the GS draws and reads between that row's texel and the next row's.
 *
 * The half-pixel offsets that centre a native pixel's samples on it put
 * fragment row I at (I + 1/2) / S - 1/2: a snapped edge there would take
 * only half of its first native row, so a snapped sprite is drawn without
 * them (SetupIA). */
static int check_minified_edge(void)
{
   static const struct sprite s = { 8688, 184, 9200, 440, 0, 2560, 528, 3088 };
   static const int scales[] = { 2, 3, 4, 5, 8 };
   const double dv_own = (double)(s.v1 - s.v0) / (double)(s.y1 - s.y0);
   struct fsprite f;
   double dv;
   int fail = 0, shown = 0, centred = 0;
   size_t si;

   snap_sprite(&s, &f);
   dv = (f.v1 - f.v0) / (f.y1 - f.y0);
   if (native_row_texel(&s, 12) != 161)
   {
      printf("  native first row reads texel %d, not 161\n", native_row_texel(&s, 12));
      fail++;
   }
   for (si = 0; si < sizeof(scales) / sizeof(scales[0]); si++)
   {
      const int S = scales[si];
      int I;
      for (I = 8 * S; I < 32 * S; I++)
      {
         const double y = (double)I / S;
         /* the sprite's own edges */
         if (hw_covers(s.y0 / 16.0, s.y1 / 16.0, S, I) && (int)(s.v0 / 16.0 + dv_own * (y - s.y0 / 16.0)) == 160)
            shown++;
         /* snapped, under a centring offset */
         {
            const double yc = ((double)I + 0.5) / S - 0.5;
            if ((yc >= f.y0 && yc < f.y1) != sw_covers(&s, I / S))
               centred++;
         }
         /* snapped */
         {
            const int hw = hw_covers(f.y0, f.y1, S, I);
            const int sw = sw_covers(&s, I / S);
            if (hw != sw)
            {
               if (fail++ < 8)
                  printf("  scale %d fragment row %d: drawn %d, native row %d drawn %d\n", S, I, hw, I / S, sw);
            }
            else if (hw)
            {
               const int t = (int)(f.v0 + dv * (y - f.y0));
               const int lo = native_row_texel(&s, I / S);
               const int hi = native_row_texel(&s, I / S + 1);
               if (t < lo || t > hi)
               {
                  if (fail++ < 8)
                     printf("  scale %d fragment row %d reads texel %d, native row %d reads %d, the next %d\n", S, I, t, I / S, lo, hi);
               }
            }
         }
      }
   }
   if (!shown)
   {
      printf("  the sprite's own edges never read texel row 160\n");
      fail++;
   }
   if (!centred)
   {
      printf("  snapped edges under a centring offset cover the native rows\n");
      fail++;
   }
   printf("a minified sprite on a half line shows the native rows' texels: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* ---- 3c. the region clamp's ends ---------------------------------------- */

/* GSRendererHW's REGION_CLAMP bounds, in texels: a coordinate past an end
 * lands on the outermost sample of the end texel at the texture's scale S
 * (under the native half-pixel offset, the low end on the texel's last
 * sample, the first ones being undrawn). The sample map reads the ends as
 * integer texels. */
static void clamp_ends(int minu, int maxu, int S, int native_hpo, double* lo, double* hi)
{
   const double last = 1.0 - 0.5 / S;
   const double first = native_hpo ? last : 1.0 - last;
   *lo = minu + first;
   *hi = maxu + last;
}

/* GSRendererHW: STRange.zw = rect end * scale - 1. */
static int region_rect_end(int rect_end, int S)
{
   return rect_end * S - 1;
}

static int check_region_clamp(void)
{
   static const int scales[] = { 1, 2, 4, 8 };
   int fail = 0;
   size_t si;
   int hpo;

   for (hpo = 0; hpo < 2; hpo++)
   {
      for (si = 0; si < sizeof(scales) / sizeof(scales[0]); si++)
      {
         const int S = scales[si];
         double lo, hi, u;
         clamp_ends(0, 127, S, hpo, &lo, &hi);
         /* The sample map's integer ends are the register's. */
         if ((int)lo != 0 || (int)hi != 127)
         {
            printf("  scale %d hpo %d: integer ends %d..%d\n", S, hpo, (int)lo, (int)hi);
            fail++;
         }
         /* A 128-texel strip: pixel 127 reads texel 127.5, which must
          * stay texel 127, and a coordinate past the end reads the
          * texel's last sample. */
         u = 127.5 < hi ? 127.5 : hi;
         if ((int)u != 127)
         {
            printf("  scale %d hpo %d: texel 127.5 clamps to %g\n", S, hpo, u);
            fail++;
         }
         u = 200.0 < hi ? 200.0 : hi;
         if ((int)(u * S) != 127 * S + S - 1)
         {
            printf("  scale %d hpo %d: past the end reads sample %d, not %d\n", S, hpo, (int)(u * S), 127 * S + S - 1);
            fail++;
         }
         /* Below the start: the first sample, or the last one under the
          * native half-pixel offset, where the first S-1 are undrawn. */
         u = -5.0 > lo ? -5.0 : lo;
         if ((int)(u * S) != (hpo && S > 1 ? S - 1 : 0))
         {
            printf("  scale %d hpo %d: before the start reads sample %d\n", S, hpo, (int)(u * S));
            fail++;
         }
      }
   }
   /* The region rect's bound for texelFetch, from its exclusive end at
    * the texture's scale: the end texel's last sample is inside it. */
   for (si = 0; si < sizeof(scales) / sizeof(scales[0]); si++)
   {
      const int S = scales[si];
      if (127 * S + S - 1 > region_rect_end(128, S))
      {
         printf("  scale %d: region rect end %d leaves out the last sample\n", S, region_rect_end(128, S));
         fail++;
      }
   }
   printf("region clamp ends: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* ---- 3d. bilinear taps on a scaled target ------------------------------ */

/* PS_NATIVE_TAPS, one axis: a fragment's coordinate u (native texels), the
 * coordinate u0 at its native pixel's first fragment and the texels the
 * coordinate advances per fragment, du, give the two taps (scaled texels,
 * S per native texel) and the weight of the second. The fragment reads its
 * own sample of the pixel's texel, its place in the pixel: a read of more
 * than one texel per pixel (a minifying blur) has the fragments' samples
 * spread over one texel, not run into the next. The taps are for sprites,
 * whose coordinates run in screen space; a triangle's run in perspective. */
static void native_taps(double u, double u0, double du, int S, double* tap0, double* tap1, double* w)
{
   const double g = u0 - 0.5;
   const double gi = floor(g);
   const double rx = du * S > 1.0 ? du * S : 1.0;
   *w = g - gi;
   *tap0 = (gi + (u - u0) / rx) * S;
   *tap1 = *tap0 + S;
}

/* The same taps through a palette (an indexed view of a scaled target,
 * or its alpha byte): an index is a native texel's, one per texel, so
 * every fragment reads the texel's first sample, the one a native draw
 * writes; the palette colours are what the weights blend. The taps apply
 * to a magnifying read as well, since the scaled texture's own filter
 * over the indices between has no meaning. */
static void palette_taps(double u0, int S, double* tap0, double* tap1, double* w)
{
   const double g = u0 - 0.5;
   const double gi = floor(g);
   *w = g - gi;
   *tap0 = gi * S;
   *tap1 = *tap0 + S;
}

/* GSRendererHW::MagnifiesTexture: the taps are for a read that keeps,
 * shrinks or barely stretches the texture (a copy, a blur, a zoom blur
 * pass a texel or two larger); a read that enlarges it by half again or
 * more is left to the scaled texture's own filter. */
static int magnifies(double px_w, double px_h, double tx_w, double tx_h)
{
   return tx_w * 1.5 < px_w || tx_h * 1.5 < px_h;
}

/* The rule it replaces: any stretch past half a texel. */
static int magnifies_old(double px_w, double px_h, double tx_w, double tx_h)
{
   return tx_w + 0.5 < px_w || tx_h + 0.5 < px_h;
}

/* The weight a pixel's bilinear read gives the texel past the last one a
 * zoom blur pass wrote, for the last pixel of a pass drawing tx texels over
 * px pixels from u = 0.5. The GS samples at the pixel's corner; the taps
 * keep that weight at any scale, and the scaled texture's own filter,
 * sampling at each fragment of the pixel, takes up to the last
 * fragment's. */
static double edge_weight(double px, double tx, int S, int taps)
{
   const double n = px - 1.0;
   const double f = taps ? 0.0 : (S - 0.5) / S;
   const double g = 0.5 + (n + f) * tx / px - 0.5;
   const double w = g - floor(g);
   return (floor(g) + 1.0 >= tx - 0.5) ? w : 0.0;
}

/* The taps for a read that magnifies: not for a colour read, which is
 * left to the scaled texture's own filter, but for a palette read. */
static int native_taps_allowed(int magnifying, int palette)
{
   return !magnifying || palette;
}

static int check_native_taps(void)
{
   static const int scales[] = { 2, 4, 8 };
   int fail = 0;
   size_t si;

   for (si = 0; si < sizeof(scales) / sizeof(scales[0]); si++)
   {
      const int S = scales[si];
      int k;
      for (k = 0; k < S; k++)
      {
         const int x = 37;
         double t0, t1, w;
         /* A buffer read a quarter texel in: the native pixel blends
          * texels x-1 and x, three to one; the fragment k of that pixel
          * reads sample k of each. */
         native_taps(x + 0.25 + (double)k / S, x + 0.25, 1.0 / S, S, &t0, &t1, &w);
         if ((int)t0 != (x - 1) * S + k || (int)t1 != x * S + k || w < 0.75 - 1e-9 || w > 0.75 + 1e-9)
         {
            printf("  scale %d fragment %d: quarter-texel read taps %g,%g weight %g\n", S, k, t0, t1, w);
            fail++;
         }
         /* A copy, half a texel in: the pixel reads texel x alone, and
          * the fragment its own sample of it. */
         native_taps(x + 0.5 + (double)k / S, x + 0.5, 1.0 / S, S, &t0, &t1, &w);
         if ((int)t0 != x * S + k || w != 0.0)
         {
            printf("  scale %d fragment %d: copy taps %g,%g weight %g\n", S, k, t0, t1, w);
            fail++;
         }
         /* Reading two texels per pixel, the coordinate advances twice
          * as fast, but the fragment still reads sample k of the pixel's
          * texel (2k would be the next texel's for k >= S/2); the weight
          * is still the pixel's. */
         native_taps(2 * x + 0.25 + 2.0 * k / S, 2 * x + 0.25, 2.0 / S, S, &t0, &t1, &w);
         if ((int)t0 != (2 * x - 1) * S + k || w < 0.75 - 1e-9 || w > 0.75 + 1e-9)
         {
            printf("  scale %d fragment %d: two-texel read taps %g,%g weight %g\n", S, k, t0, t1, w);
            fail++;
         }
      }
   }
   /* A glare buffer's alpha read through a palette, blurred 1:1 and then
    * drawn at twice its size: every fragment of a pixel reads the same
    * two indices, the texels' first samples, with the pixel's weights;
    * the fragment's own sample would be one of a magnified palette read's
    * sixteen, and a pixel's box came out a square, brighter by the
    * palette's curve. */
   for (si = 0; si < sizeof(scales) / sizeof(scales[0]); si++)
   {
      const int S = scales[si];
      int k;
      for (k = 0; k < S; k++)
      {
         const int x = 12;
         double t0, t1, w, m0, m1, mw;
         palette_taps(x + 0.25, S, &t0, &t1, &w);
         if ((int)t0 != (x - 1) * S || (int)t1 != x * S || w < 0.75 - 1e-9 || w > 0.75 + 1e-9)
         {
            printf("  scale %d fragment %d: palette taps %g,%g weight %g\n", S, k, t0, t1, w);
            fail++;
         }
         /* magnified 2:1, the pixel's coordinate advances half a texel */
         palette_taps(x + 0.25 + 0.5 * (k / S), S, &m0, &m1, &mw);
         if (m0 != t0 || m1 != t1)
         {
            printf("  scale %d fragment %d: magnified palette taps %g,%g\n", S, k, m0, m1);
            fail++;
         }
      }
   }
   /* a 1:1 copy and a 2:1 blur keep the taps; a bloom drawn 4:1 from a
    * 160x112 buffer, and a logo drawn larger than its texture, do not,
    * unless read through a palette */
   if (magnifies(640, 448, 640, 448) || magnifies(320, 224, 640, 448) || !magnifies(640, 448, 160, 112) || !magnifies(256, 64, 128, 64))
   {
      printf("  the magnification rule is wrong\n");
      fail++;
   }
   if (!(magnifies(62, 66, 31, 33) && native_taps_allowed(1, 1)) || native_taps_allowed(1, 0))
   {
      printf("  a magnified read keeps the taps only through a palette\n");
      fail++;
   }
   /* A zoom blur bouncing between two 256x128 buffers draws each pass a
    * texel or two larger, and never writes the last row and column; a
    * brightening pass amplifies whatever the reads take from there. The
    * GS takes next to nothing, the taps as little at 4x, and those passes
    * keep the taps; the rule they replace left them to the scaled filter,
    * which takes most of the stale texel into the last pixel. */
   if (magnifies(256, 128, 255, 127) || magnifies(256, 128, 255.44, 126.94) ||
      edge_weight(128, 126.94, 4, 1) > 0.01)
   {
      printf("  a zoom blur pass lost the taps, or they read past the edge\n");
      fail++;
   }
   if (!magnifies_old(256, 128, 255.44, 126.94) || edge_weight(128, 126.94, 4, 0) < 0.25)
   {
      printf("  negative: the old rule kept the taps for a zoom blur pass\n");
      fail++;
   }
   printf("bilinear taps on a scaled target: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* ---- 4. a page's position in its target -------------------------------- */

/* GS page addressing for a buffer of width bw pages: the page a pixel is
 * on, counted from the base pointer (GSOffset::bn, in pages). */
static u32 page_of_pixel(u32 x, u32 y, u32 bw)
{
   return (y / 32u) * bw + x / 64u;
}

/* Where the channel shuffle fetch starts, and where a draw addressed at
 * that page goes, as GSRendererHW computes it. */
static void page_position(u32 page, u32 bw, u32* x, u32* y)
{
   *x = (page % bw) * 64u;
   *y = (page / bw) * 32u;
}

static int check_page_position(void)
{
   static const u32 widths[] = { 1, 2, 4, 8, 10 };
   int fail = 0;
   size_t wi;

   for (wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++)
   {
      const u32 bw = widths[wi];
      u32 page;
      /* a 448-row buffer: 14 rows of pages */
      for (page = 0; page < bw * 14u; page++)
      {
         u32 x, y, px, py;
         page_position(page, bw, &x, &y);
         if (page_of_pixel(x, y, bw) != page || (x % 64u) || (y % 32u))
         {
            if (fail++ < 8)
               printf("  width %u page %u: position %u,%u is page %u\n", bw, page, x, y, page_of_pixel(x, y, bw));
         }
         /* every pixel of the page is on it */
         for (py = y; py < y + 32u; py += 31u)
            for (px = x; px < x + 64u; px += 63u)
               if (page_of_pixel(px, py, bw) != page)
               {
                  if (fail++ < 8)
                     printf("  width %u page %u: pixel %u,%u is page %u\n", bw, page, px, py, page_of_pixel(px, py, bw));
               }
      }
   }
   /* A texture whose base is before a target but whose coordinates land
    * on it (a glow drawn into a scratch buffer at 0x1a40 and read back
    * through the display buffer's address 0x9a0, texel 733,478, both 10
    * pages wide): the texel's page is a page of the target, and
    * the offset puts the texel on the target pixel with the same place in
    * its page. GSTextureCache::LookupSource, the read-before-target case. */
   {
      const u32 bp = 0x9a0, tbp = 0x1a40, bw = 10, u = 733, v = 478;
      const u32 page_x = u & ~63u, page_y = v & ~31u;
      const u32 rect_bp = bp + page_of_pixel(page_x, page_y, bw) * 32u;
      u32 x, y;
      int tx, ty;
      if (rect_bp < tbp || (rect_bp - tbp) % 32u)
      {
         printf("  read-before-target: texel %u,%u of 0x%x is block 0x%x, not a page of 0x%x\n", u, v, bp, rect_bp, tbp);
         fail++;
      }
      page_position((rect_bp - tbp) / 32u, bw, &x, &y);
      tx = (int)x - (int)page_x;
      ty = (int)y - (int)page_y;
      if (tx != -192 || ty != -416 || (int)u + tx != 541 || (int)v + ty != 62 ||
          tbp + page_of_pixel((u32)((int)u + tx), (u32)((int)v + ty), bw) * 32u != rect_bp)
      {
         printf("  read-before-target: offset %d,%d puts texel %u,%u at %d,%d\n", tx, ty, u, v, (int)u + tx, (int)v + ty);
         fail++;
      }
   }
   /* The same read through a region: a 64x56 buffer blurred at 0x2f60
    * and read back through 0x2ec0, four pages wide,
    * clamped to texels 64,32-128,88; those texels are page 2 of the
    * 256x224 buffer at 0x2f20 that the draw went to, at 128,0. The clamp
    * moves the rect, not the rule. */
   {
      const u32 bp = 0x2ec0, tbp = 0x2f20, bw = 4, u = 64, v = 32;
      const u32 rect_bp = bp + page_of_pixel(u, v, bw) * 32u;
      u32 x, y;
      page_position((rect_bp - tbp) / 32u, bw, &x, &y);
      if (rect_bp != 0x2f60 || (int)x - (int)u != 64 || (int)y - (int)v != -32 || x != 128 || y != 0)
      {
         printf("  read-before-target through a region: block 0x%x, offset %d,%d\n", rect_bp, (int)x - (int)u, (int)y - (int)v);
         fail++;
      }
   }
   /* The read-inside-target case, the same rule with the base after the
    * target's: a glow blurred in a 256x448 buffer at 0x3380, four pages
    * wide, whose lower half is read back by its own base 0x3700 as a
    * 256x224 texture. Texel 0,0 of the read is page 28 of the target, at
    * 0,224; the whole read lands on rows 224-448 of the target. And the
    * buffer at 0x3380 is itself the lower half of one at 0x3000: read
    * through 0x3380 while only 0x3000 is held, the rows are 224 down. */
   {
      const u32 bw = 4;
      u32 x, y;
      page_position((0x3700 - 0x3380) / 32u, bw, &x, &y);
      if (x != 0 || y != 224 || y + 224 > 448)
      {
         printf("  read-inside-target: 0x3700 in 0x3380 at %u,%u, not 0,224\n", x, y);
         fail++;
      }
      page_position((0x3380 - 0x3000) / 32u, bw, &x, &y);
      if (x != 0 || y != 224)
      {
         printf("  read-inside-target: 0x3380 in 0x3000 at %u,%u, not 0,224\n", x, y);
         fail++;
      }
      /* the same base read a page wider is not the same pages */
      if ((0x3700 - 0x3380) / 32u % 5u == 0 || page_of_pixel(0, 224, 5) == (0x3700 - 0x3380) / 32u)
      {
         printf("  read-inside-target: a 5-wide read of 0x3700 must not be taken for rows of a 4-wide target\n");
         fail++;
      }
   }
   /* A read of a target's pages at another width (GSTextureCache::
    * PagesInTarget and the gathered source): page n of the read, counted
    * in the read's width, is the page at block bp + 32n, and it sits in
    * the target where the target's width puts that block's page. A bloom
    * pyramid drawn two pages wide at 0x2f20 (128x128) and read back one
    * page wide through 0x2ee0, rows 64-128 of the read: its pages 2 and 3
    * are the target's pages 0 and 1, side by side at 0,0 and 64,0, not
    * stacked as the read has them. A read wider than its own width
    * aliases: column 64 of row 64 is page 3 as well. A page past the
    * target's end is not the target's. */
   {
      const u32 tbp = 0x2f20, tbw = 2, bp = 0x2ee0, bw = 1;
      const u32 pages_in_target = 8; /* 128x128, 32-bit */
      static const struct { u32 px, py; int inside; u32 x, y; } reads[] = {
         { 0, 2, 1, 0, 0 },
         { 0, 3, 1, 64, 0 },
         { 1, 2, 1, 64, 0 },
         { 0, 5, 1, 64, 32 },
         { 0, 1, 0, 0, 0 },
         { 0, 10, 0, 0, 0 }
      };
      size_t ri;
      u32 x, y;
      for (ri = 0; ri < sizeof(reads) / sizeof(reads[0]); ri++)
      {
         const u32 n = reads[ri].py * bw + reads[ri].px;
         const u32 block = bp + n * 32u;
         const int inside = block >= tbp && block < tbp + pages_in_target * 32u && ((block - tbp) % 32u) == 0;
         x = y = 0;
         if (inside)
            page_position((block - tbp) / 32u, tbw, &x, &y);
         if (inside != reads[ri].inside || x != reads[ri].x || y != reads[ri].y)
         {
            printf("  gather: read page %u,%u -> block 0x%x inside %d at %u,%u\n", reads[ri].px, reads[ri].py, block, inside, x, y);
            fail++;
         }
      }
      /* the same read at the target's own width is the identity offset */
      page_position((0x2f40 - tbp) / 32u, tbw, &x, &y);
      if (x != 64 || y != 0)
      {
         printf("  gather: a same-width page lands at %u,%u\n", x, y);
         fail++;
      }
   }
   /* A display 8 pages wide processed in strips: a strip starting two
    * pages in sits 128 pixels across. */
   {
      u32 x, y;
      page_position(2, 8, &x, &y);
      if (x != 128 || y != 0)
      {
         printf("  page 2 of an 8-wide buffer at %u,%u, not 128,0\n", x, y);
         fail++;
      }
   }
   printf("page position in a target: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* ---- 4b. a target drawn at another width ------------------------------- */

/* GSTextureCache::RelayoutTarget: page n of a buffer sits at
 * (n % bw, n / bw) in pages whatever bw the buffer is drawn with, so a
 * target drawn at a new width moves each page to where the new width puts
 * it; relayout_rect is the box around a rect's pages once moved. */
static void relayout_page(u32 n, u32 old_bw, u32 new_bw, u32* sx, u32* sy, u32* dx, u32* dy)
{
   page_position(n, old_bw, sx, sy);
   page_position(n, new_bw, dx, dy);
}

static void relayout_rect(const u32 r[4], u32 old_bw, u32 new_bw, u32 out[4])
{
   const u32 px0 = r[0] / 64u, py0 = r[1] / 32u, px1 = (r[2] - 1) / 64u, py1 = (r[3] - 1) / 32u;
   u32 px, py, x, y;
   out[0] = out[1] = 0xffffffffu;
   out[2] = out[3] = 0;
   for (py = py0; py <= py1; py++)
      for (px = px0; px <= px1; px++)
      {
         page_position(py * old_bw + px, new_bw, &x, &y);
         if (x < out[0]) out[0] = x;
         if (y < out[1]) out[1] = y;
         if (x + 64u > out[2]) out[2] = x + 64u;
         if (y + 32u > out[3]) out[3] = y + 32u;
      }
}

/* GSTextureCache::FindPageOwner for a draw within one page: the page's
 * index in the draw's own layout, added to the page the draw is addressed
 * at, is its index in the target; the draw goes to that page's place. */
static void place_page(u32 fbp, u32 fbw, const u32 r[4], u32 tbp, u32 tbw, u32* x, u32* y)
{
   const u32 in_draw = page_of_pixel(r[0], r[1], fbw);
   page_position((fbp - tbp) / 32u + in_draw, tbw, x, y);
}

static int check_relayout(void)
{
   /* A 320x224 blur buffer (5 pages wide, 35 pages) then drawn 1 and 3
    * pages wide, as the colour grade and the ping-pong blur do. */
   static const u32 widths[] = { 5, 1, 3, 5 };
   int fail = 0;
   u32 n, wi;

   for (n = 0; n < 35u; n++)
   {
      u32 sx, sy, dx, dy, back_x, back_y;
      for (wi = 0; wi + 1 < sizeof(widths) / sizeof(widths[0]); wi++)
      {
         relayout_page(n, widths[wi], widths[wi + 1], &sx, &sy, &dx, &dy);
         if (page_of_pixel(sx, sy, widths[wi]) != n || page_of_pixel(dx, dy, widths[wi + 1]) != n)
         {
            if (fail++ < 8)
               printf("  page %u: %u,%u at width %u -> %u,%u at width %u is not the page\n", n, sx, sy, widths[wi], dx, dy, widths[wi + 1]);
         }
      }
      /* and it comes back where it started */
      relayout_page(n, 5, 1, &sx, &sy, &dx, &dy);
      relayout_page(n, 1, 5, &dx, &dy, &back_x, &back_y);
      if (back_x != sx || back_y != sy)
      {
         if (fail++ < 8)
            printf("  page %u: 5 -> 1 -> 5 lands at %u,%u, not %u,%u\n", n, back_x, back_y, sx, sy);
      }
   }
   /* The valid rect of the whole buffer, 5 wide, is 64x1120 at 1 wide
    * and 192x384 (12 rows of 3, the last one a page short) at 3. */
   {
      static const u32 whole[4] = { 0, 0, 320, 224 };
      static const u32 page7[4] = { 128, 32, 192, 64 };
      u32 out[4];
      relayout_rect(whole, 5, 1, out);
      if (out[0] != 0 || out[1] != 0 || out[2] != 64 || out[3] != 1120)
      {
         printf("  320x224 at width 1: %u,%u-%u,%u\n", out[0], out[1], out[2], out[3]);
         fail++;
      }
      relayout_rect(whole, 5, 3, out);
      if (out[0] != 0 || out[1] != 0 || out[2] != 192 || out[3] != 384)
      {
         printf("  320x224 at width 3: %u,%u-%u,%u\n", out[0], out[1], out[2], out[3]);
         fail++;
      }
      /* one page (page 7 of the 5-wide layout) is one page after */
      relayout_rect(page7, 5, 3, out);
      if (out[0] != 64 || out[1] != 64 || out[2] != 128 || out[3] != 96)
      {
         printf("  page 7 at width 3: %u,%u-%u,%u, not 64,64-128,96\n", out[0], out[1], out[2], out[3]);
         fail++;
      }
   }
   /* A target fitted wider than its width (a 28-page buffer, 4 pages
    * wide, moved to 1 wide and then fitted to 256 pixels across by a
    * draw's size) still stands for its 28 pages: what lies beyond the
    * width's one page column is nothing the width can address, so it is
    * left out of the count, the moves and the valid rect, and the buffer
    * comes back 4 wide as 7 rows of 4, not 28 rows of 4 (which would
    * grow it fourfold at every change of width until it wrapped around
    * memory and over the display buffer). */
   {
      const u32 pages = 28, old_tbw = 1, fitted_cols = 4, rows = 28;
      const u32 counted = pages < old_tbw * rows ? pages : old_tbw * rows;
      const u32 rows4 = (counted + 3) / 4;
      static const u32 valid_wide[4] = { 0, 0, 256, 896 };
      u32 clipped[4], out[4];
      (void)fitted_cols;
      clipped[0] = valid_wide[0]; clipped[1] = valid_wide[1];
      clipped[2] = valid_wide[2] < old_tbw * 64u ? valid_wide[2] : old_tbw * 64u;
      clipped[3] = valid_wide[3] < rows * 32u ? valid_wide[3] : rows * 32u;
      relayout_rect(clipped, old_tbw, 4, out);
      if (counted != 28 || rows4 != 7 || out[2] != 256 || out[3] != 224)
      {
         printf("  a 28-page buffer fitted 4 pages wide at width 1 comes back at width 4 as %u pages, %u rows, valid %u,%u-%u,%u\n", counted, rows4, out[0], out[1], out[2], out[3]);
         fail++;
      }
   }
   /* The pages the box stands for cannot shrink under it: 35 pages 3
    * wide fill 12 rows, so the box holds 36; a target keeps its own page
    * count (from its end block) so the extra page does not become a
    * 13th row at the next width. */
   {
      const u32 pages = 35, rows3 = (pages + 2) / 3, box3 = rows3 * 3, rows5 = (box3 + 4) / 5;
      if (rows3 != 12 || box3 != 36 || rows5 != 8 || (pages + 4) / 5 != 7)
      {
         printf("  35 pages: %u rows of 3 (%u pages), %u rows of 5 from the box, %u from the count\n", rows3, box3, rows5, (pages + 4) / 5);
         fail++;
      }
   }
   /* A page copied from the scratch page to the k-th page of the 12-page
    * 160x112 buffer at 0x34a0 (3 wide), drawn with FBW 1 at row 32k: it
    * goes to page k's place in the 3-wide target, not to a 1-wide copy of
    * it; the scratch page itself, page 0 of the 5-wide buffer, stays at
    * 0,0 whatever width it is drawn with. */
   {
      u32 k, x, y;
      for (k = 0; k < 12u; k++)
      {
         u32 r[4];
         r[0] = 0; r[1] = 32u * k; r[2] = 64; r[3] = 32u * k + 32u;
         place_page(0x34a0, 1, r, 0x34a0, 3, &x, &y);
         if (x != (k % 3u) * 64u || y != (k / 3u) * 32u)
         {
            if (fail++ < 8)
               printf("  page %u copied at width 1 lands at %u,%u in the 3-wide buffer\n", k, x, y);
         }
         /* the same page addressed at its own base, drawn at row 0 */
         r[1] = 0; r[3] = 32;
         place_page(0x34a0 + 0x20u * k, 1, r, 0x34a0, 3, &x, &y);
         if (x != (k % 3u) * 64u || y != (k / 3u) * 32u)
         {
            if (fail++ < 8)
               printf("  page %u addressed at its base lands at %u,%u in the 3-wide buffer\n", k, x, y);
         }
      }
      {
         static const u32 r0[4] = { 0, 2, 64, 32 };
         place_page(0x3620, 1, r0, 0x3620, 5, &x, &y);
         if (x != 0 || y != 0)
         {
            printf("  the scratch page lands at %u,%u\n", x, y);
            fail++;
         }
         place_page(0x3620, 1, r0, 0x3620, 3, &x, &y);
         if (x != 0 || y != 0)
         {
            printf("  the scratch page lands at %u,%u in the 3-wide layout\n", x, y);
            fail++;
         }
      }
   }
   printf("target pages at another width: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* ---- 4c. the shuffle's repeated draws ----------------------------------- */

/* GSRendererHW::ChannelShuffleCovered: a draw of the shuffle in progress
 * is a repeat when its rect, placed by the page it is addressed at, lies
 * within what the shuffle has drawn; otherwise it is drawn and joins
 * (GSRendererHW::JoinedRect) when the two make a rect, else stands alone,
 * so nothing is taken as drawn for lying in the box around draws that
 * left it out. */
struct written
{
   u32 x0, y0, x1, y1;
   int any;
};

static int rect_within(const u32 r[4], const struct written* w)
{
   return w->any && r[0] >= w->x0 && r[1] >= w->y0 && r[2] <= w->x1 && r[3] <= w->y1;
}

static void rect_join(const u32 r[4], struct written* w)
{
   const int same_x = w->any && r[0] == w->x0 && r[2] == w->x1 && r[1] <= w->y1 && w->y0 <= r[3];
   const int same_y = w->any && r[1] == w->y0 && r[3] == w->y1 && r[0] <= w->x1 && w->x0 <= r[2];
   if (rect_within(r, w))
      return;
   if (!w->any || !(same_x || same_y))
   {
      w->x0 = r[0]; w->y0 = r[1]; w->x1 = r[2]; w->y1 = r[3];
      w->any = 1;
      return;
   }
   if (r[0] < w->x0) w->x0 = r[0];
   if (r[1] < w->y0) w->y0 = r[1];
   if (r[2] > w->x1) w->x1 = r[2];
   if (r[3] > w->y1) w->y1 = r[3];
}

/* A draw of the shuffle: its rect placed in the target, and whether it
 * was skipped as a repeat. */
static int shuffle_draw(u32 fbp, u32 first_fbp, u32 tbw, const u32 r[4], struct written* w)
{
   u32 x, y, placed[4];
   page_position((fbp - first_fbp) / 32u, tbw, &x, &y);
   placed[0] = r[0] + x; placed[1] = r[1] + y; placed[2] = r[2] + x; placed[3] = r[3] + y;
   if (rect_within(placed, w))
      return 1;
   rect_join(placed, w);
   return 0;
}

static int check_shuffle_repeats(void)
{
   /* The colour grade: per channel, one pass over rows 2..32 of the
    * scratch page and one over rows 0..30, both 64 wide. The second is
    * not within the first, so both are drawn and the page is whole; a
    * third pass over rows 0..30 again would be a repeat. */
   static const u32 pass_a[4] = { 0, 2, 64, 32 };
   static const u32 pass_b[4] = { 0, 0, 64, 30 };
   struct written w;
   int fail = 0;

   w.any = 0;
   if (shuffle_draw(0x3620, 0x3620, 5, pass_a, &w) || shuffle_draw(0x3620, 0x3620, 5, pass_b, &w))
   {
      printf("  the second pass over the page was skipped as a repeat\n");
      fail++;
   }
   if (!w.any || w.x0 != 0 || w.y0 != 0 || w.x1 != 64 || w.y1 != 32)
   {
      printf("  the two passes cover %u,%u-%u,%u, not the page\n", w.x0, w.y0, w.x1, w.y1);
      fail++;
   }
   if (!shuffle_draw(0x3620, 0x3620, 5, pass_b, &w))
   {
      printf("  a third pass over the page was drawn again\n");
      fail++;
   }
   /* A shuffle that advances a page per draw over a 10-wide display:
    * each page is new until drawn, then a repeat; page 12 of the second
    * row sits at 128,32. */
   {
      static const u32 page[4] = { 0, 0, 64, 32 };
      u32 k;
      w.any = 0;
      for (k = 0; k < 20u; k++)
      {
         if (shuffle_draw(0x20u * k, 0, 10, page, &w))
         {
            if (fail++ < 8)
               printf("  page %u of the display was skipped before being drawn\n", k);
         }
      }
      /* the second row's first page does not make a rect with the first
       * row, so the first row is let go: its pages would be drawn again,
       * which is safe; the second row, whole, stands as drawn */
      if (!shuffle_draw(0x20u * 12u, 0, 10, page, &w) || w.y0 != 32 || w.x1 != 640)
      {
         printf("  page 12, drawn already, was drawn again (%u,%u-%u,%u)\n", w.x0, w.y0, w.x1, w.y1);
         fail++;
      }
      if (shuffle_draw(0x20u * 3u, 0, 10, page, &w))
      {
         printf("  page 3 was taken as drawn from the box around the rows\n");
         fail++;
      }
   }
   /* Two whole rows of pages, one page at a time, do make a rect, and a
    * page of either is then a repeat. */
   {
      static const u32 row[4] = { 0, 0, 640, 32 };
      static const u32 page[4] = { 0, 0, 64, 32 };
      w.any = 0;
      shuffle_draw(0, 0, 10, row, &w);
      shuffle_draw(0x20u * 10u, 0, 10, row, &w);
      if (w.x1 != 640 || w.y1 != 64 || !shuffle_draw(0x20u * 12u, 0, 10, page, &w) || !shuffle_draw(0x20u * 3u, 0, 10, page, &w))
      {
         printf("  two rows of pages: %u,%u-%u,%u\n", w.x0, w.y0, w.x1, w.y1);
         fail++;
      }
   }
   printf("shuffle repeats: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* ---- 5. the channel shuffle gate --------------------------------------- */

/* The pixel and byte a texel of the 8-bit view aliases, by the tables. */
static void aliased_pixel(u32 u, u32 v, int* x, int* y, int* byte)
{
   const u32 a = addr8(u, v);
   u32 px, py;
   for (py = 0; py < 32; py++)
      for (px = 0; px < 64; px++)
         if (addr32(px, py) == (a & ~3u))
         {
            *x = (int)px;
            *y = (int)py;
            *byte = (int)(a & 3u);
            return;
         }
   *x = *y = *byte = -1;
}

/* The same, as GSRendererHW::IsChannelShuffleIdentity computes it. */
static void renderer_pixel(int u, int v, int* x, int* y)
{
   const int flip = (((v >> 1) ^ (v >> 2)) & 1) << 2;
   *x = ((u & 7) ^ flip) | ((u >> 4) << 3);
   *y = ((v >> 2) << 1) | (v & 1);
}

static int check_alias_formula(void)
{
   int fail = 0;
   u32 u, v;
   for (v = 0; v < 64; v++)
      for (u = 0; u < 128; u++)
      {
         int tx, ty, tb, rx, ry;
         aliased_pixel(u, v, &tx, &ty, &tb);
         renderer_pixel((int)u, (int)v, &rx, &ry);
         if (tx != rx || ty != ry)
         {
            if (fail++ < 8)
               printf("  texel %u,%u: renderer says pixel %d,%d, tables say %d,%d\n", u, v, rx, ry, tx, ty);
         }
      }
   printf("gate's texel-to-pixel formula matches the tables: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

/* GSRendererHW::IsChannelShuffleIdentity over a sprite list, with the
 * position asked about or not (a page-by-page copy is not asked). */
static int shuffle_identity(const struct sprite* v, size_t n, int wms, int minu, int maxu, int wmt, int minv, int maxv, int position)
{
   const int u_masked = wms == 3 && (minu & 8) == 0;
   const int v_masked = wmt == 3 && (minv & 2) == 0;
   size_t i;

   for (i = 0; i < n; i++)
   {
      const int w = abs(v[i].x1 - v[i].x0) >> 4;
      const int h = abs(v[i].y1 - v[i].y0) >> 4;
      const int uw = abs(v[i].u1 - v[i].u0) >> 4;
      const int vh = abs(v[i].v1 - v[i].v0) >> 4;
      const int umin = v[i].u0 < v[i].u1 ? v[i].u0 : v[i].u1;
      const int vmin = v[i].v0 < v[i].v1 ? v[i].v0 : v[i].v1;
      const int x0 = (v[i].x0 < v[i].x1 ? v[i].x0 : v[i].x1) >> 4;
      const int y0 = (v[i].y0 < v[i].y1 ? v[i].y0 : v[i].y1) >> 4;
      int u_first, u_last, v_first, v_last, u, vv, sx, sy, byte;

      if (w == 0 || h == 0 || uw != w || vh != h)
         return 0;
      /* the sprite's first coordinate is what its first pixel reads */
      u_first = umin >> 4;
      u_last = (umin + ((w - 1) << 4)) >> 4;
      v_first = vmin >> 4;
      v_last = (vmin + ((h - 1) << 4)) >> 4;
      if (!u_masked && (u_first >> 3) != (u_last >> 3))
         return 0;
      if (!v_masked && (v_first >> 1) != (v_last >> 1))
         return 0;
      if (!position)
         continue;

      u = u_first;
      vv = v_first;
      if (wms == 3)
         u = (u & minu) | maxu;
      if (wmt == 3)
         vv = (vv & minv) | maxv;
      aliased_pixel((u32)u, (u32)vv, &sx, &sy, &byte);
      if (sx != x0 || sy != y0)
         return 0;
   }
   return 1;
}

static int check_shuffle_gate(void)
{
   /* Draw A (WMS 3 MINU 1015, WMT 3 MINV 1017): 3:1 in V. */
   static const struct sprite draw_a[] = {
      { 0, 32, 128, 128, 72, 8, 200, 296 },
      { 128, 32, 256, 128, 328, 8, 456, 296 }
   };
   /* Draw B (WMS 3 MINU 1015, WMT 1): 16x2 sprites, each on the pixels
    * its texels alias, and a single-channel copy, which is the same draw
    * with rows of 8x2 sprites between (its two kinds of row, then a row
    * that reads the flipped columns). */
   static const struct sprite copy[] = {
      { 0, 0, 256, 32, 136, 8, 392, 40 },
      { 256, 0, 512, 32, 648, 8, 904, 40 },
      { 0, 32, 128, 64, 72, 72, 200, 104 },
      { 128, 32, 256, 64, 328, 72, 456, 104 },
      { 0, 64, 256, 96, 136, 136, 392, 168 }
   };
   /* The same page put at x 64 of the frame (a block's second and third
    * copies): not where the texels alias. */
   static const struct sprite moved[] = {
      { 1024, 0, 1280, 32, 136, 8, 392, 40 }
   };
   /* An alpha pass: green texels copied 16 rows down. */
   static const struct sprite shifted[] = {
      { 512, 288, 768, 320, 136, 104, 392, 136 },
      { 0, 288, 256, 320, 1160, 104, 1416, 136 }
   };
   /* A channel shuffle drawn as 8x2 pixel sprites, each from its own
    * 8x2 texel box on one channel. Pixel rows 2-3 of a block hold their
    * red bytes in the flipped column order, so the sprite there starts
    * its box at texel 4 and needs bit 3 of U masked to stay on red. */
   static const struct sprite real[] = {
      { 0, 0, 128, 32, 0, 0, 128, 32 },
      { 128, 0, 256, 32, 256, 0, 384, 32 },
      { 0, 32, 128, 64, 64, 64, 192, 96 }
   };
   /* The same box put on rows 2-3 unflipped reads red bytes of other
    * pixels: what the game gets is not what the copy would give it. */
   static const struct sprite unflipped[] = {
      { 0, 32, 128, 64, 0, 64, 128, 96 }
   };
   /* A colour grade through a palette: each 8x16 pixel sprite reads an
    * 8x64 texel column, one texel row in four, the odd columns a row on --
    * a byte of every pixel of the page, laid out as palettes. Eight wide,
    * it passes the size test; drawn as a channel copy it would move one
    * channel, not grade three. */
   static const struct sprite grade[] = {
      { 0, 0, 128, 256, 0, 0, 128, 1024 },
      { 128, 0, 256, 256, 0, 16, 128, 1040 }
   };
   int fail = 0;

   if (shuffle_identity(draw_a, 2, 3, 1015, 0, 3, 1017, 0, 1))
   {
      printf("  draw A taken for a channel shuffle\n");
      fail++;
   }
   if (!shuffle_identity(copy, 5, 3, 1015, 0, 1, 0, 0, 1))
   {
      printf("  a block copy onto the pixels it aliases refused\n");
      fail++;
   }
   if (!shuffle_identity(copy, 5, 3, 1015, 0, 0, 0, 0, 1))
   {
      printf("  the single-channel copy refused\n");
      fail++;
   }
   if (shuffle_identity(moved, 1, 3, 1015, 0, 1, 0, 0, 1))
   {
      printf("  a block copied elsewhere taken for a channel shuffle\n");
      fail++;
   }
   if (shuffle_identity(shifted, 2, 3, 1015, 0, 0, 0, 0, 1))
   {
      printf("  the shifted alpha pass taken for an identity\n");
      fail++;
   }
   if (!shuffle_identity(shifted, 2, 3, 1015, 0, 0, 0, 0, 0))
   {
      printf("  the shifted alpha pass refused as a page copy\n");
      fail++;
   }
   if (!shuffle_identity(real, 2, 0, 0, 0, 0, 0, 0, 1))
   {
      printf("  a 1:1 single-channel read refused\n");
      fail++;
   }
   if (!shuffle_identity(real, 3, 3, 1015, 0, 3, 1021, 0, 1))
   {
      printf("  a masked single-channel read refused\n");
      fail++;
   }
   if (shuffle_identity(unflipped, 1, 3, 1015, 0, 3, 1021, 0, 1))
   {
      printf("  a read of the flipped columns taken for an identity\n");
      fail++;
   }
   if (shuffle_identity(grade, 2, 0, 0, 0, 0, 0, 0, 1) || shuffle_identity(grade, 2, 0, 0, 0, 0, 0, 0, 0))
   {
      printf("  the palette grade taken for a channel shuffle\n");
      fail++;
   }
   /* negative: IsPossibleChannelShuffle's size test, on its own, lets it by */
   if ((abs(grade[0].x1 - grade[0].x0) >> 4) != 8)
   {
      printf("  negative: the grade fails the size test by itself\n");
      fail++;
   }
   printf("channel shuffle gate: %s\n", fail ? "FAIL" : "ok");
   return fail;
}

int main(void)
{
   int fail = 0;
   fail += check_swizzle();
   fail += check_scaled();
   fail += check_sample_map();
   fail += check_sprite_edges();
   fail += check_minified_edge();
   fail += check_region_clamp();
   fail += check_native_taps();
   fail += check_page_position();
   fail += check_relayout();
   fail += check_shuffle_repeats();
   fail += check_alias_formula();
   fail += check_shuffle_gate();
   return fail ? 1 : 0;
}
