/* paraLLEl-GS: super samples carried through copies.
 *
 * A render target read back as a sprite's texture keeps its super samples
 * as an array, one layer per sample. With the full super-sampled textures
 * option every such read goes through triangle_setup's heuristics (blur
 * kernels, up- and downsampling); without it, triangle_setup keeps only
 * the reads that are copies: a sprite, without perspective, point
 * sampled, whose texture coordinates step with its position texel for
 * pixel in the same direction. Sample k of each of its pixels is then
 * sample k of the texel, and a post-processing copy of the picture keeps
 * the picture's super samples. Every other read takes one sample.
 *
 * Pinned here: the copy rule (triangle_setup.comp, SUPER_SAMPLED_COPIES_ONLY)
 * on copies at an offset, within a subpixel of rounding, mirrored, scaled,
 * filtered, in perspective and on triangles; and, read out of
 * slangmosh.hpp, the embedded triangle_setup declaring specialization
 * constant 4 and its reflection's specialization mask including it.
 *
 * Build and run, from tests/pgs:
 *   cc -O2 -std=c89 -pedantic -Wall pgs_ss_copies.c -o pgs_ss_copies
 *   ./pgs_ss_copies ../../pcsx2/GS/parallel-gs/gs/shaders/slangmosh.hpp
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Positions and texture coordinates in sixteenths, as the setup has them. */
struct sprite
{
   int ax, ay, dx, dy; /* corners */
   int u0, v0, u1, v1; /* their coordinates */
   int perspective, linear, is_sprite;
};

static int iabs(int x)
{
   return x < 0 ? -x : x;
}

static int is_copy(const struct sprite *s)
{
   const int ex = (s->u1 - s->u0) - (s->dx - s->ax);
   const int ey = (s->v1 - s->v0) - (s->dy - s->ay);
   return s->is_sprite && !s->perspective && !s->linear && iabs(ex) <= 1 && iabs(ey) <= 1;
}

static int check_rule(void)
{
   static const struct
   {
      struct sprite s;
      int want;
      const char *what;
   } cases[] = {
      { { 0, 0, 640 * 16, 448 * 16, 0, 0, 640 * 16, 448 * 16, 0, 0, 1 }, 1, "whole picture" },
      { { 0, 0, 320 * 16, 224 * 16, 8 * 16, 4 * 16, 328 * 16, 228 * 16, 0, 0, 1 }, 1, "at an offset" },
      { { 0, 0, 640 * 16, 448 * 16, 8, 8, 640 * 16 + 9, 448 * 16 + 8, 0, 0, 1 }, 1, "a subpixel of rounding" },
      { { 0, 0, 640 * 16, 448 * 16, 640 * 16, 0, 0, 448 * 16, 0, 0, 1 }, 0, "mirrored" },
      { { 0, 0, 320 * 16, 224 * 16, 0, 0, 640 * 16, 448 * 16, 0, 0, 1 }, 0, "halved" },
      { { 0, 0, 640 * 16, 448 * 16, 0, 0, 320 * 16, 224 * 16, 0, 0, 1 }, 0, "doubled" },
      { { 0, 0, 640 * 16, 448 * 16, 0, 0, 640 * 16, 448 * 16, 0, 1, 1 }, 0, "bilinear off texel centres" },
      { { 0, 0, 640 * 16, 448 * 16, 0, 0, 640 * 16, 448 * 16, 1, 0, 1 }, 0, "in perspective" },
      { { 0, 0, 640 * 16, 448 * 16, 0, 0, 640 * 16, 448 * 16, 0, 0, 0 }, 0, "a triangle" },
      { { 0, 0, 640 * 16, 448 * 16, 0, 0, 640 * 16 + 2, 448 * 16, 0, 0, 1 }, 0, "two subpixels off" }
   };
   int fail = 0;
   size_t i;
   for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
      if (is_copy(&cases[i].s) != cases[i].want)
      {
         printf("  %s: copy %d, wanted %d\n", cases[i].what, is_copy(&cases[i].s), cases[i].want);
         fail++;
      }
   return fail;
}

static char *read_file(const char *path)
{
   FILE *f = fopen(path, "rb");
   char *text;
   long len;
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   len = ftell(f);
   fseek(f, 0, SEEK_SET);
   text = (char *)malloc((size_t)len + 1);
   if (!text || fread(text, 1, (size_t)len, f) != (size_t)len)
   {
      fclose(f);
      free(text);
      return NULL;
   }
   fclose(f);
   text[len] = '\0';
   return text;
}

/* Every hex token between start and end, parsed once: indexing them by
 * rescanning from start each time made the scan of a module quadratic
 * in the bank's size. */
static long *tokens(const char *start, const char *end, unsigned long *count)
{
   const char *p = start;
   unsigned long n = 0, cap = 1024;
   long *t = (long *)malloc(cap * sizeof(*t));
   /* A plain scan: strstr under ASan measures the rest of the text on
    * every call, which is the quadratic cost again. */
   for (; t && p + 1 < end; p++)
   {
      if (p[0] != '0' || p[1] != 'x')
         continue;
      if (n == cap)
      {
         long *grown = (long *)realloc(t, (cap *= 2) * sizeof(*t));
         if (!grown)
         {
            free(t);
            return NULL;
         }
         t = grown;
      }
      t[n++] = (long)strtoul(p, NULL, 16);
      p++;
   }
   *count = n;
   return t;
}

/* The n-th token; -1 past the end. */
static long tok(const long *t, unsigned long count, unsigned long n)
{
   return n < count ? t[n] : -1;
}

static int check_bank(const char *path)
{
   char *text = read_file(path);
   const char *bank, *bank_end, *refl, *refl_end, *call, *layout;
   unsigned long off, bytes, refl_off, i, nb = 0, nr = 0;
   long *bw, *rw;
   int spec4 = 0, fail = 0;
   long mask;
   if (!text)
   {
      printf("  cannot read %s\n", path);
      return 1;
   }
   bank = strstr(text, "spirv_bank[] =");
   refl = strstr(text, "reflection_bank[] =");
   call = strstr(text, "this->triangle_setup = device.request_program(spirv_bank + ");
   if (!bank || !refl || !call ||
       sscanf(call + strlen("this->triangle_setup = device.request_program(spirv_bank + "), "%lu, %lu", &off, &bytes) != 2)
   {
      printf("  no triangle_setup in %s\n", path);
      free(text);
      return 1;
   }
   bank_end = strstr(bank, "};");
   refl_end = strstr(refl, "};");
   /* the layout unserialized for it is the one on the line before */
   layout = call;
   while (layout > text && strncmp(layout, "layout.unserialize(reflection_bank + ", 37) != 0)
      layout--;
   if (sscanf(layout + 37, "%lu", &refl_off) != 1)
   {
      printf("  no reflection for triangle_setup\n");
      free(text);
      return 1;
   }
   bw = tokens(bank, bank_end, &nb);
   rw = tokens(refl, refl_end, &nr);
   if (!bw || !rw)
   {
      printf("  out of memory reading %s\n", path);
      free(bw);
      free(rw);
      free(text);
      return 1;
   }
   /* OpDecorate %x SpecId 4: word count 4, opcode 71, decoration 1 */
   for (i = 5; i < bytes / 4;)
   {
      const long w = tok(bw, nb, off + i);
      const unsigned long wc = (unsigned long)w >> 16;
      if (w < 0 || wc == 0)
         break;
      if ((w & 0xffff) == 71 && wc == 4 && tok(bw, nb, off + i + 2) == 1 &&
          tok(bw, nb, off + i + 3) == 4)
         spec4 = 1;
      i += wc;
   }
   /* spec_constant_mask: the uint32 at byte 340 of the 348-byte entry */
   mask = tok(rw, nr, refl_off + 340) | (tok(rw, nr, refl_off + 341) << 8);
   if (!spec4)
   {
      printf("  the embedded triangle_setup has no specialization constant 4\n");
      fail++;
   }
   if (!(mask & 0x10))
   {
      printf("  triangle_setup's reflection masks specialization constants 0x%lx\n", mask);
      fail++;
   }
   free(bw);
   free(rw);
   free(text);
   return fail;
}

int main(int argc, char **argv)
{
   int fail = check_rule();
   if (argc > 1)
      fail += check_bank(argv[1]);
   printf("super-sampled copies: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
