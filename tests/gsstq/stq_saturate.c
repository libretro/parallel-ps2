/* Perspective texture coordinates past the GS's range.
 *
 * With FST=0 a vertex carries S, T and Q, and the texel is S/Q scaled by
 * the texture size. The GS keeps S/Q and T/Q in a signed 12-bit integer
 * range: past +/-2047 the coordinate saturates, it does not wrap. On a
 * power-of-two texture with REPEAT that pins the sample to texel 0, since
 * 2047 << TW has no low bits. parallel-gs models the same clamp
 * (ubershader.comp, before sample_texture).
 *
 * An environment-mapped reflection has back-facing vertices that project
 * to S/Q of several thousand. Real hardware
 * and the software renderer both leave those triangles at texel 0 -- the
 * software renderer because its 16.16 conversion overflows to INT_MIN --
 * while the GPU shaders wrapped the raw value and sprayed the lower body
 * with pseudo-random texels. The three tfx pixel shaders now clamp S/Q
 * and T/Q to +/-2047 before scaling.
 *
 * Two checks:
 *   1. The saturating conversion lands on the same texel as the software
 *      renderer's conversion on vertices captured from that draw, and the
 *      unclamped conversion (kept as a control) does not.
 *   2. Each of the three shader sources carries the clamp on its FST=0
 *      path, so the backends stay in step.
 *
 * Build and run, from tests/gsstq:
 *   cc -O2 -std=c89 -pedantic -Wall stq_saturate.c -o stq_saturate
 *   ./stq_saturate
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

/* GSRendererSW: t = s * (0x10000 << TW); GSDrawScanline: u = cvtt(t / q),
 * then >> 16 and the REPEAT mask. cvttps2dq answers 0x80000000 for
 * anything an int32 cannot hold. */
static int sw_texel(float s, float q, int tw)
{
   float scaled = s * (float)(0x10000 << tw);
   double d = (double)(scaled / q);
   long fixed;
   if (d >= 2147483648.0 || d <= -2147483649.0 || d != d)
      fixed = -2147483647L - 1L;
   else
      fixed = (long)d; /* truncation, as cvtt */
   return (int)((fixed >> 16) & ((1 << tw) - 1));
}

/* tfx pixel shader, FST=0 path, with the clamp. */
static int hw_texel(float s, float q, int tw)
{
   double st = (double)s / (double)q;
   double u;
   if (st > 2047.0)
      st = 2047.0;
   if (st < -2047.0)
      st = -2047.0;
   u = floor(st * (double)(1 << tw));
   return (int)(((long)u) & ((1 << tw) - 1));
}

/* Control: the shader as it was, no clamp. */
static int hw_texel_wrapped(float s, float q, int tw)
{
   double u = floor(((double)s / (double)q) * (double)(1 << tw));
   return (int)(((long)u) & ((1 << tw) - 1));
}

struct vert
{
   float s, t, q;
};

/* S, T, Q of back-facing environment-map vertices from the RR5 draw
 * (TW = TH = 8, REPEAT), plus a few front-facing ones. */
static const struct vert verts[] = {
   { -51952.49f, 136412.70f, 7.3341f },
   { -55167.46f, 135152.08f, 7.3348f },
   { -47926.94f, 136903.75f, 7.2888f },
   { -47910.21f, 136855.94f, 7.2863f },
   { -43953.47f, 137372.27f, 7.2435f },
   { -40687.01f, 138303.80f, 7.2375f },
   { -42927.73f, 141563.20f, 7.2471f },
   { -57043.42f, 138600.17f, 7.3447f },
   { -93426.91f, 107964.83f, 7.2480f },
   { -93398.92f, 107932.48f, 7.2458f },
   { 145477.81f, -145992.60f, 7.1024f },
   { -152415.16f, 148045.89f, 7.4688f },
   { 3.25f, 6.01f, 7.2211f },
   { 3.63f, 5.25f, 7.1892f },
   { 2.70f, 5.95f, 7.2706f },
   { 1.32f, 4.64f, 7.3816f }
};

static int check_source(const char* path, const char* needle)
{
   static char buf[1 << 20];
   FILE* f = fopen(path, "rb");
   size_t n;
   if (!f)
   {
      printf("  cannot open %s\n", path);
      return 1;
   }
   n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = 0;
   if (!strstr(buf, needle))
   {
      printf("  %s: FST=0 path does not clamp S/Q to +/-2047\n", path);
      return 1;
   }
   return 0;
}

int main(void)
{
   const int tw = 8;
   int fail = 0, control_diff = 0, i;
   size_t k;

   for (k = 0; k < sizeof(verts) / sizeof(verts[0]); k++)
   {
      const struct vert* v = &verts[k];
      float st[2];
      st[0] = v->s;
      st[1] = v->t;
      for (i = 0; i < 2; i++)
      {
         int sw = sw_texel(st[i], v->q, tw);
         int hw = hw_texel(st[i], v->q, tw);
         int old = hw_texel_wrapped(st[i], v->q, tw);
         if (sw != hw)
         {
            printf("  vertex %u %c: s/q=%.1f sw texel %d, shader texel %d\n",
               (unsigned)k, i ? 't' : 's', (double)(st[i] / v->q), sw, hw);
            fail++;
         }
         if (old != sw)
            control_diff++;
      }
   }

   printf("captured vertices: %s (%d mismatches); unclamped control differs on %d of %u coordinates\n",
      fail ? "FAIL" : "ok", fail, control_diff, (unsigned)(2 * (sizeof(verts) / sizeof(verts[0]))));
   if (control_diff == 0)
   {
      printf("  control did not differ: the vertex set no longer exercises the clamp\n");
      fail++;
   }

   fail += check_source("../../pcsx2/GS/Renderers/Vulkan/shaders/tfx.glsl",
      "clamp(vsIn.t.xy / vsIn.t.w, vec2(-2047.0f), vec2(2047.0f))");
   fail += check_source("../../pcsx2/GS/Renderers/OpenGL/tfx_fs.glsl",
      "clamp(PSin.t_float.xy / vec2(PSin.t_float.w), vec2(-2047.0f), vec2(2047.0f))");
   fail += check_source("../../pcsx2/GS/Renderers/DX11/tfx.fx",
      "clamp(input.t.xy / input.t.w, -2047.0f, 2047.0f)");
   printf("shader sources: %s\n", fail ? "FAIL" : "ok");

   return fail ? 1 : 0;
}
