/* gs_vector against a scalar model of what each operation means.
 *
 * The class cannot be built below SSE4.1 -- it reaches for pminsd and its
 * relatives with no fallback -- so on the SSE2 tier there is no C++ side to
 * compare against and a written-out scalar model is the only reference
 * available. It is also the better reference on the other tiers, because it
 * states the operation rather than restating one instruction in terms of
 * another, so every tier is checked against it here.
 *
 * C, not C++, so this lane also holds gs_vector.h to strict C89.
 */

#include "gs_vector.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint64_t X = 0x243F6A8885A308D3ull;
static uint32_t rnd(void)
{
   X ^= X << 13; X ^= X >> 7; X ^= X << 17;
   return (uint32_t)(X >> 16);
}

static long total = 0;
static long fails = 0;

static void seed(uint32_t *w, int k)
{
   int j;
   for (j = 0; j < 4; j++) w[j] = rnd();
   switch (k & 15)
   {
      case 0: memset(w, 0x00, 16); break;
      case 1: memset(w, 0xff, 16); break;
      case 2: w[0] = 0x80000000u; w[1] = 0x7fffffffu; break;
      case 3: w[0] = 0xffff0000u; w[2] = 0x0000ffffu; break;
      case 4: w[0] = 0x00008000u; w[1] = 0xffff8000u; break;
      /* Sign bit set in every byte but the rest clear, and the mirror of
       * it: the shapes that separate "every byte is negative" from "every
       * bit is set", which a movemask-only test cannot tell apart. */
      case 5: memset(w, 0x80, 16); break;
      case 6: memset(w, 0x7f, 16); break;
      case 7: memset(w, 0x00, 16); w[0] = 1u; break;
      case 8: memset(w, 0xff, 16); w[2] = 0xfffffffeu; break;
      default: break;
   }
}

static int32_t sat16(int32_t v)
{
   if (v >  32767) return  32767;
   if (v < -32768) return -32768;
   return v;
}

static void chk(const char *what, gs_vec4i got, const uint32_t *want)
{
   uint32_t g[4];
   gs_v4i_storeu(g, got);
   total++;
   if (memcmp(g, want, 16) != 0)
   {
      if (fails < 8)
         printf("  MISMATCH %s: %08x %08x %08x %08x vs %08x %08x %08x %08x\n",
                what, g[0], g[1], g[2], g[3], want[0], want[1], want[2], want[3]);
      fails++;
   }
}

static void chk_int(const char *what, int got, int want)
{
   total++;
   if (got != want)
   {
      if (fails < 8) printf("  MISMATCH %s: %d vs %d\n", what, got, want);
      fails++;
   }
}

/* saturating helpers the model needs */
static int16_t sat_s16(int32_t v)
{
   if (v >  32767) return  32767;
   if (v < -32768) return -32768;
   return (int16_t)v;
}

static uint16_t sat_u16(int32_t v)
{
   if (v > 65535) return 65535u;
   if (v < 0)     return 0u;
   return (uint16_t)v;
}

int main(int argc, char **argv)
{
   int iters = argc > 1 ? atoi(argv[1]) : 200000;
   int k;

   printf("tier: ");
#if defined(GS_VEC_CAN_AVX)
   printf("avx\n");
#elif defined(GS_VEC_CAN_SSE41)
   printf("sse4.1\n");
#elif defined(GS_VEC_X86)
   printf("sse2\n");
#else
   printf("neon\n");
#endif

   for (k = 0; k < iters; k++)
   {
      uint32_t ra[4];
      uint32_t rb[4];
      uint32_t w[4];
      gs_vec4i a;
      gs_vec4i b;
      int j;

      seed(ra, k);
      seed(rb, k >> 4);
      a = gs_v4i_loadu(ra);
      b = gs_v4i_loadu(rb);

      /* ---- arithmetic and logic ---- */
      for (j = 0; j < 4; j++) w[j] = ra[j] + rb[j];
      chk("add32", gs_v4i_add32(a, b), w);
      for (j = 0; j < 4; j++) w[j] = ra[j] - rb[j];
      chk("sub32", gs_v4i_sub32(a, b), w);
      for (j = 0; j < 4; j++) w[j] = ra[j] & rb[j];
      chk("and", gs_v4i_and(a, b), w);
      for (j = 0; j < 4; j++) w[j] = ra[j] | rb[j];
      chk("or", gs_v4i_or(a, b), w);
      for (j = 0; j < 4; j++) w[j] = ra[j] ^ rb[j];
      chk("xor", gs_v4i_xor(a, b), w);
      for (j = 0; j < 4; j++) w[j] = ~ra[j] & rb[j];
      chk("andnot", gs_v4i_andnot(a, b), w);
      for (j = 0; j < 4; j++) w[j] = ra[j] == rb[j] ? 0xffffffffu : 0u;
      chk("eq32", gs_v4i_eq32(a, b), w);
      for (j = 0; j < 4; j++)
         w[j] = (int32_t)ra[j] > (int32_t)rb[j] ? 0xffffffffu : 0u;
      chk("gt32", gs_v4i_gt32(a, b), w);

      /* 16-bit add/sub, lane by lane */
      {
         uint16_t g[8];
         const uint16_t *pa = (const uint16_t *)ra;
         const uint16_t *pb = (const uint16_t *)rb;
         for (j = 0; j < 8; j++) g[j] = (uint16_t)(pa[j] + pb[j]);
         memcpy(w, g, 16);
         chk("add16", gs_v4i_add16(a, b), w);
         for (j = 0; j < 8; j++) g[j] = (uint16_t)(pa[j] - pb[j]);
         memcpy(w, g, 16);
         chk("sub16", gs_v4i_sub16(a, b), w);

         /* The three the reverb FIR is made of. */
         for (j = 0; j < 8; j++)
            g[j] = (uint16_t)sat16((int32_t)(int16_t)pa[j] + (int16_t)pb[j]);
         memcpy(w, g, 16);
         chk("adds16", gs_v4i_adds16(a, b), w);

         for (j = 0; j < 4; j++)
         {
            g[j]     = (uint16_t)sat16((int32_t)(int16_t)pa[j * 2]
                                     + (int16_t)pa[j * 2 + 1]);
            g[j + 4] = (uint16_t)sat16((int32_t)(int16_t)pb[j * 2]
                                     + (int16_t)pb[j * 2 + 1]);
         }
         memcpy(w, g, 16);
         chk("hadds16", gs_v4i_hadds16(a, b), w);

         for (j = 0; j < 8; j++)
         {
            int32_t p = (int32_t)(int16_t)pa[j] * (int16_t)pb[j];
            g[j] = (uint16_t)((p + 0x4000) >> 15);
         }
         memcpy(w, g, 16);
         chk("mul16hrs", gs_v4i_mul16hrs(a, b), w);
      }

      /* ---- min and max ---- */
      for (j = 0; j < 4; j++)
         w[j] = (int32_t)ra[j] < (int32_t)rb[j] ? ra[j] : rb[j];
      chk("min_i32", gs_v4i_min_i32(a, b), w);
      for (j = 0; j < 4; j++)
         w[j] = (int32_t)ra[j] > (int32_t)rb[j] ? ra[j] : rb[j];
      chk("max_i32", gs_v4i_max_i32(a, b), w);
      for (j = 0; j < 4; j++) w[j] = ra[j] < rb[j] ? ra[j] : rb[j];
      chk("min_u32", gs_v4i_min_u32(a, b), w);
      for (j = 0; j < 4; j++) w[j] = ra[j] > rb[j] ? ra[j] : rb[j];
      chk("max_u32", gs_v4i_max_u32(a, b), w);

      /* ---- unpack ---- */
      {
         const uint16_t *pa = (const uint16_t *)ra;
         const uint16_t *pb = (const uint16_t *)rb;
         uint16_t g[8];
         for (j = 0; j < 4; j++) { g[2 * j] = pa[j]; g[2 * j + 1] = pb[j]; }
         memcpy(w, g, 16); chk("upl16v", gs_v4i_upl16v(a, b), w);
         for (j = 0; j < 4; j++) { g[2 * j] = pa[j + 4]; g[2 * j + 1] = pb[j + 4]; }
         memcpy(w, g, 16); chk("uph16v", gs_v4i_uph16v(a, b), w);
         for (j = 0; j < 4; j++) { g[2 * j] = pa[j]; g[2 * j + 1] = 0; }
         memcpy(w, g, 16); chk("upl16", gs_v4i_upl16(a), w);
         for (j = 0; j < 4; j++) { g[2 * j] = pa[j + 4]; g[2 * j + 1] = 0; }
         memcpy(w, g, 16); chk("uph16", gs_v4i_uph16(a), w);
      }
      {
         const uint8_t *pa = (const uint8_t *)ra;
         const uint8_t *pb = (const uint8_t *)rb;
         uint8_t g[16];
         for (j = 0; j < 8; j++) { g[2 * j] = pa[j]; g[2 * j + 1] = pb[j]; }
         memcpy(w, g, 16); chk("upl8v", gs_v4i_upl8v(a, b), w);
         for (j = 0; j < 8; j++) { g[2 * j] = pa[j + 8]; g[2 * j + 1] = pb[j + 8]; }
         memcpy(w, g, 16); chk("uph8v", gs_v4i_uph8v(a, b), w);
         for (j = 0; j < 8; j++) { g[2 * j] = pa[j]; g[2 * j + 1] = 0; }
         memcpy(w, g, 16); chk("upl8", gs_v4i_upl8(a), w);
         for (j = 0; j < 8; j++) { g[2 * j] = pa[j + 8]; g[2 * j + 1] = 0; }
         memcpy(w, g, 16); chk("uph8", gs_v4i_uph8(a), w);
      }
      w[0] = ra[0]; w[1] = rb[0]; w[2] = ra[1]; w[3] = rb[1];
      chk("upl32v", gs_v4i_upl32v(a, b), w);
      w[0] = ra[2]; w[1] = rb[2]; w[2] = ra[3]; w[3] = rb[3];
      chk("uph32v", gs_v4i_uph32v(a, b), w);
      w[0] = ra[0]; w[1] = 0; w[2] = ra[1]; w[3] = 0;
      chk("upl32", gs_v4i_upl32(a), w);
      w[0] = ra[2]; w[1] = 0; w[2] = ra[3]; w[3] = 0;
      chk("uph32", gs_v4i_uph32(a), w);
      w[0] = ra[0]; w[1] = ra[1]; w[2] = rb[0]; w[3] = rb[1];
      chk("upl64v", gs_v4i_upl64v(a, b), w);
      w[0] = ra[2]; w[1] = ra[3]; w[2] = rb[2]; w[3] = rb[3];
      chk("uph64v", gs_v4i_uph64v(a, b), w);

      /* ---- the block swizzle's: byte shuffle (an index's top bit gives
       * 0), bit select, high half store, 16-bit lanes of both halves and
       * shufps on integers by one immediate ---- */
      {
         const uint8_t *pa = (const uint8_t *)ra;
         const uint8_t *pb = (const uint8_t *)rb;
         uint8_t g[16];
         uint8_t m[16];
         uint32_t h[4];
         for (j = 0; j < 16; j++) m[j] = (uint8_t)(pb[j] & 0x8f);
         for (j = 0; j < 16; j++) g[j] = (m[j] & 0x80) ? 0 : pa[m[j] & 15];
         memcpy(w, g, 16);
         chk("shuffle8", gs_v4i_shuffle8(a, gs_v4i_loadu(m)), w);
         for (j = 0; j < 4; j++) h[j] = ra[j] ^ rb[j] ^ 0x5a5a5a5au;
         for (j = 0; j < 4; j++) w[j] = (ra[j] & ~h[j]) | (rb[j] & h[j]);
         chk("select", gs_v4i_select(a, b, gs_v4i_loadu(h)), w);
         h[0] = h[1] = h[2] = h[3] = 0;
         gs_v4i_storeh(h, a);
         w[0] = ra[2]; w[1] = ra[3]; w[2] = 0; w[3] = 0;
         chk("storeh", gs_v4i_loadu(h), w);
      }
      {
         const uint16_t *pa = (const uint16_t *)ra;
         uint16_t g[8];
         static const int sh = 0x72; /* ZXWY */
         for (j = 0; j < 4; j++) g[j] = pa[(sh >> (2 * j)) & 3];
         for (j = 0; j < 4; j++) g[j + 4] = pa[4 + ((sh >> (2 * j)) & 3)];
         memcpy(w, g, 16);
         chk("shuffle16lh", GS_V4I_SHUFFLE16LH(a, 0x72), w);
         w[0] = ra[0]; w[1] = ra[2]; w[2] = rb[1]; w[3] = rb[3];
         chk("shuffle2", GS_V4I_SHUFFLE2(a, b, 0xD8), w);
      }
      {
         const uint8_t *pa = (const uint8_t *)ra;
         const uint8_t *pb = (const uint8_t *)rb;
         const uint16_t *ha = (const uint16_t *)ra;
         uint8_t g[16];
         uint64_t q[2];
         for (j = 0; j < 16; j++) g[j] = j + 12 < 16 ? pa[j + 12] : pb[j - 4];
         memcpy(w, g, 16); chk("alignr12", GS_V4I_ALIGNR(b, a, 12), w);
         for (j = 0; j < 16; j++) g[j] = j + 8 < 16 ? pa[j + 8] : pb[j - 8];
         memcpy(w, g, 16); chk("alignr8", GS_V4I_ALIGNR(b, a, 8), w);
         memcpy(w, ra, 16);
         chk("set16", gs_v4i_set16(ha[0], ha[1], ha[2], ha[3], ha[4], ha[5], ha[6], ha[7]), w);
         memcpy(q, rb, 16);
         memcpy(w, rb, 16);
         chk("set64", gs_v4i_set64(q[0], q[1]), w);
      }

      /* ---- pack ---- */
      {
         int16_t g[8];
         for (j = 0; j < 4; j++) g[j]     = sat_s16((int32_t)ra[j]);
         for (j = 0; j < 4; j++) g[j + 4] = sat_s16((int32_t)rb[j]);
         memcpy(w, g, 16); chk("ps32", gs_v4i_ps32(a, b), w);
      }
      {
         uint16_t g[8];
         for (j = 0; j < 4; j++) g[j]     = sat_u16((int32_t)ra[j]);
         for (j = 0; j < 4; j++) g[j + 4] = sat_u16((int32_t)rb[j]);
         memcpy(w, g, 16); chk("pu32", gs_v4i_pu32(a, b), w);
      }
      {
         const int16_t *sa = (const int16_t *)ra;
         const int16_t *sb = (const int16_t *)rb;
         uint8_t g[16];
         for (j = 0; j < 8; j++) g[j]     = (uint8_t)(sa[j] < 0 ? 0 : sa[j] > 255 ? 255 : sa[j]);
         for (j = 0; j < 8; j++) g[j + 8] = (uint8_t)(sb[j] < 0 ? 0 : sb[j] > 255 ? 255 : sb[j]);
         memcpy(w, g, 16); chk("pu16", gs_v4i_pu16(a, b), w);
      }

      /* ---- blend: a byte comes from b where the mask byte is negative ---- */
      {
         const uint8_t *pa = (const uint8_t *)ra;
         const uint8_t *pb = (const uint8_t *)rb;
         uint8_t g[16];
         for (j = 0; j < 16; j++) g[j] = (pb[j] & 0x80u) ? pb[j] : pa[j];
         memcpy(w, g, 16);
         chk("blend8", gs_v4i_blend8(a, b, b), w);
      }

      /* ---- immediate blends: a set bit takes that lane, or that 16-bit
       * lane, from b ---- */
      {
         static const int m32 = 12;
         for (j = 0; j < 4; j++) w[j] = (m32 & (1 << j)) ? rb[j] : ra[j];
         chk("blend32", GS_V4I_BLEND32(a, b, 12), w);
      }
      {
         uint16_t g[8];
         const uint16_t *pa = (const uint16_t *)ra;
         const uint16_t *pb = (const uint16_t *)rb;
         for (j = 0; j < 8; j++) g[j] = (0xa5 & (1 << j)) ? pb[j] : pa[j];
         memcpy(w, g, 16);
         chk("blend16", GS_V4I_BLEND16(a, b, 0xa5), w);
      }

      /* ---- shifts ---- */
      for (j = 0; j < 4; j++) w[j] = ra[j] << 7;
      chk("sll32", GS_V4I_SLL32(a, 7), w);
      for (j = 0; j < 4; j++) w[j] = ra[j] >> 11;
      chk("srl32", GS_V4I_SRL32(a, 11), w);
      for (j = 0; j < 4; j++) w[j] = (uint32_t)((int32_t)ra[j] >> 4);
      chk("sra32", GS_V4I_SRA32(a, 4), w);
      {
         uint16_t g[8];
         const uint16_t *pa = (const uint16_t *)ra;
         for (j = 0; j < 8; j++) g[j] = (uint16_t)(pa[j] << 3);
         memcpy(w, g, 16); chk("sll16", GS_V4I_SLL16(a, 3), w);
         for (j = 0; j < 8; j++) g[j] = (uint16_t)(pa[j] >> 5);
         memcpy(w, g, 16); chk("srl16", GS_V4I_SRL16(a, 5), w);
         for (j = 0; j < 8; j++) g[j] = (uint16_t)((int16_t)pa[j] >> 2);
         memcpy(w, g, 16); chk("sra16", GS_V4I_SRA16(a, 2), w);
      }
      {
         /* whole-register byte shifts */
         uint8_t g[16];
         const uint8_t *pa = (const uint8_t *)ra;
         memset(g, 0, 16);
         for (j = 0; j < 11; j++) g[j + 5] = pa[j];
         memcpy(w, g, 16); chk("sll", GS_V4I_SLL(a, 5), w);
         memset(g, 0, 16);
         for (j = 0; j < 8; j++) g[j] = pa[j + 8];
         memcpy(w, g, 16); chk("srl", GS_V4I_SRL(a, 8), w);
      }

      /* ---- lane access ---- */
      chk_int("extract32<0>", GS_V4I_EXTRACT32(a, 0), (int)ra[0]);
      chk_int("extract32<1>", GS_V4I_EXTRACT32(a, 1), (int)ra[1]);
      chk_int("extract32<2>", GS_V4I_EXTRACT32(a, 2), (int)ra[2]);
      chk_int("extract32<3>", GS_V4I_EXTRACT32(a, 3), (int)ra[3]);
      memcpy(w, ra, 16); w[1] = 0x1234abcdu;
      chk("insert32<1>", GS_V4I_INSERT32(a, 0x1234abcd, 1), w);
      memcpy(w, ra, 16); w[3] = (uint32_t)-7;
      chk("insert32<3>", GS_V4I_INSERT32(a, -7, 3), w);
      {
         uint8_t g[16];
         memcpy(g, ra, 16); g[5] = 0x5au;
         memcpy(w, g, 16); chk("insert8<5>", GS_V4I_INSERT8(a, 0x5a, 5), w);
         memcpy(g, ra, 16); g[10] = 0xc3u;
         memcpy(w, g, 16); chk("insert8<10>", GS_V4I_INSERT8(a, 0xc3, 10), w);
      }

      /* ---- tests ---- */
      chk_int("alltrue", gs_v4i_alltrue(a),
              (ra[0] & ra[1] & ra[2] & ra[3]) == 0xffffffffu);
      chk_int("allfalse", gs_v4i_allfalse(a),
              (ra[0] | ra[1] | ra[2] | ra[3]) == 0u);

      /* ---- constants ---- */
      memset(w, 0x00, 16); chk("zero", gs_v4i_zero(), w);
      memset(w, 0xff, 16); chk("ones", gs_v4i_ones(), w);
      for (j = 0; j < 4; j++) w[j] = ra[0];
      chk("set32", gs_v4i_set32((int32_t)ra[0]), w);

      /* ---- rectangles ---- */
      for (j = 0; j < 4; j++)
      {
         int32_t va = (int32_t)ra[j];
         int32_t lim = (int32_t)(j < 2 ? rb[j] : rb[j]);
         /* rintersect: lanes 0,1 take max against b.xy; lanes 2,3 take
          * max against b.xy then min against b.zw */
         (void)va; (void)lim;
      }
      {
         int32_t t[4];
         int32_t xyxy[4];
         int32_t zwzw[4];
         xyxy[0] = (int32_t)rb[0]; xyxy[1] = (int32_t)rb[1];
         xyxy[2] = (int32_t)rb[0]; xyxy[3] = (int32_t)rb[1];
         zwzw[0] = (int32_t)rb[2]; zwzw[1] = (int32_t)rb[3];
         zwzw[2] = (int32_t)rb[2]; zwzw[3] = (int32_t)rb[3];
         for (j = 0; j < 4; j++)
         {
            int32_t v = (int32_t)ra[j];
            v = v > xyxy[j] ? v : xyxy[j];
            v = v < zwzw[j] ? v : zwzw[j];
            t[j] = v;
         }
         memcpy(w, t, 16);
         chk("rintersect", gs_v4i_rintersect(a, b), w);
      }
      {
         int32_t lo[4];
         int32_t hi[4];
         int32_t t[4];
         for (j = 0; j < 4; j++)
         {
            int32_t va = (int32_t)ra[j];
            int32_t vb = (int32_t)rb[j];
            lo[j] = va < vb ? va : vb;
            hi[j] = va > vb ? va : vb;
         }
         /* upl64 of lo with hi shifted down two lanes */
         t[0] = lo[0]; t[1] = lo[1]; t[2] = hi[2]; t[3] = hi[3];
         memcpy(w, t, 16);
         chk("runion", gs_v4i_runion(a, b), w);
      }

      /* ---- what the GIF path takes ---- */
      w[0] = ra[0]; w[1] = w[2] = w[3] = 0;
      chk("load32", gs_v4i_load32((int32_t)ra[0]), w);
      w[0] = ra[0]; w[1] = ra[1]; w[2] = rb[2]; w[3] = rb[3];
      chk("loadlh", gs_v4i_loadlh(ra, &rb[2]), w);
      for (j = 0; j < 4; j++) w[j] = (int32_t)ra[j] < (int32_t)rb[j] ? 0xffffffffu : 0u;
      chk("lt32", gs_v4i_lt32(a, b), w);
      for (j = 0; j < 4; j += 2)
         w[j] = w[j + 1] = (ra[j] == rb[j] && ra[j + 1] == rb[j + 1]) ? 0xffffffffu : 0u;
      chk("eq64", gs_v4i_eq64(a, b), w);
      {
         uint32_t m[4];
         m[0] = ra[0]; m[1] = rb[1]; m[2] = ra[2]; m[3] = ra[3] ^ (k & 1);
         for (j = 0; j < 4; j += 2)
            w[j] = w[j + 1] = (ra[j] == m[j] && ra[j + 1] == m[j + 1]) ? 0xffffffffu : 0u;
         chk("eq64 (half equal)", gs_v4i_eq64(a, gs_v4i_loadu(m)), w);
         chk_int("eq (one lane off)", gs_v4i_eq(a, gs_v4i_loadu(m)), !memcmp(ra, m, 16));
      }
      {
         const uint8_t *pa = (const uint8_t *)ra;
         int mk = 0;
         for (j = 0; j < 16; j++) mk |= (pa[j] >> 7) << j;
         chk_int("mask", gs_v4i_mask(a), mk);
      }
      {
         const uint16_t *pa = (const uint16_t *)ra;
         for (j = 0; j < 4; j++) w[j] = pa[j];
         chk("u16to32", gs_v4i_u16to32(a), w);
      }
      chk_int("eq (self)", gs_v4i_eq(a, a), 1);
      chk_int("eq", gs_v4i_eq(a, b), !memcmp(ra, rb, 16));
      chk_int("rempty", gs_v4i_rempty(a),
              !((int32_t)ra[0] < (int32_t)ra[2] && (int32_t)ra[1] < (int32_t)ra[3]));
      for (j = 0; j < 4; j++)
      {
         float f;
         memcpy(&f, &ra[j], 4);
         w[j] = (f == f) ? ra[j] : rb[j];
      }
      chk("replace_nan_f32", gs_v4i_replace_nan_f32(a, b), w);
   }

   printf("%s: gs_vector oracle, %ld comparisons, %ld mismatches\n",
          fails ? "FAIL" : "PASS", total, fails);
   return fails != 0;
}
