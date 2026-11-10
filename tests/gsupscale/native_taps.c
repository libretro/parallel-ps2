/*
 * Native taps for a texture at native size (the PS_NATIVE_TAPS branch of
 * the hardware renderer's pixel shaders: DX11/tfx.fx, which DX12 builds
 * too, OpenGL/tfx_fs.glsl and Vulkan/shaders/tfx.glsl).
 *
 * A bilinear read at scale takes the native pixel's two taps, a native
 * texel apart, with the native pixel's weights. From a target at the same
 * scale each fragment reads its own sample of each tap's texel. A texture
 * at native size has no samples within a texel, and the fragment's place
 * in the pixel, added the same way, carried the right fragments of a pixel
 * across into the next texel: a screen composed for the 2x2 average of a
 * read at texel edges, text and all, came out with each tap half a texel
 * along over half the pixel.
 *
 * The shaders are strings in C++ files, so the arithmetic is modelled here
 * with the expression as each shader carries it, and each shader file is
 * checked to carry it.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The first tap's coordinate, in texels of the texture as it is held (the
 * shader divides by its size after): gi the native tap, f the fragment's
 * place in the native pixel over the pixel's texels (0 .. 1), ts the
 * texture's scale. */
static double tap(double gi, double f, double ts)
{
   const double step_ts = ts >= 1.0001 ? 1.0 : 0.0;
   return (gi + f * step_ts) * ts + 0.5;
}

static double tap_unfixed(double gi, double f, double ts)
{
   return (gi + f) * ts + 0.5;
}

/* The texel of the texture as held that a point read at c takes. */
static int texel(double c)
{
   return (int)floor(c);
}

static int check_taps(double (*fn)(double, double, double), int report)
{
   static const int scales[] = { 2, 3, 4, 8 };
   int fail = 0;
   unsigned s;
   int gi, j;

   for (s = 0; s < sizeof(scales) / sizeof(scales[0]); s++)
   {
      const int S = scales[s];
      for (gi = 0; gi < 8; gi++)
      {
         for (j = 0; j < S; j++)
         {
            const double f = (double)j / S;
            /* At native size: the native tap's texel, whatever the
             * fragment. */
            const int n = texel(fn(gi, f, 1.0));
            /* At the draw's scale: the fragment's own sample of it. */
            const int t = texel(fn(gi, f, (double)S));

            if (n != gi || t != gi * S + j)
            {
               if (report)
                  printf("  %dx: tap %d fragment %d reads %d native, %d scaled\n", S, gi, j, n, t);
               fail++;
            }
         }
      }
   }
   return fail;
}

/* Each shader carries the expression the model takes. */
static int check_shader(const char *dir, const char *file, const char *expr)
{
   char path[1024];
   static char text[1 << 20];
   FILE *f;
   size_t n;

   sprintf(path, "%s/%s", dir, file);
   f = fopen(path, "rb");
   if (!f)
   {
      printf("  %s: cannot open\n", path);
      return 1;
   }
   n = fread(text, 1, sizeof(text) - 1, f);
   fclose(f);
   text[n] = 0;
   if (!strstr(text, expr))
   {
      printf("  %s: the native-taps coordinate is not \"%s\"\n", file, expr);
      return 1;
   }
   return 0;
}

int main(int argc, char **argv)
{
   const char *dir = argc > 1 ? argv[1] : "../../pcsx2/GS/Renderers";
   int fail = 0;

   fail += check_taps(tap, 1);

   /* Negative control: without the step, a texture at native size reads
    * the next texel over half the pixel. */
   if (check_taps(tap_unfixed, 0) == 0)
   {
      printf("  negative control: the unstepped tap reads the native texels\n");
      fail++;
   }

   /* The modelled expression is the arm for everything but a palette
    * read, which takes the native tap's centre: a palette entry is one
    * value per native pixel. */
   fail += check_shader(dir, "DX11/tfx.fx",
         "float2 p0 = (PS_PAL_FMT != 0) ? gi * ts + 0.5f : (gi + (un - un0) / rx * step(1.0001f, ts)) * ts + 0.5f;");
   fail += check_shader(dir, "OpenGL/tfx_fs.glsl",
         "vec2 p0 = ((PS_PAL_FMT != 0)) ? gi * ts + 0.5f : (gi + (un - un0) / rx * step(1.0001f, ts)) * ts + 0.5f;");
   /* Vulkan declares p0 before the palette branch and sets each arm. */
   fail += check_shader(dir, "Vulkan/shaders/tfx.glsl", "p0 = gi * ts + 0.5f;");
   fail += check_shader(dir, "Vulkan/shaders/tfx.glsl",
         "p0 = (gi + (un - un0) / rx * step(1.0001f, ts)) * ts + 0.5f;");

   printf("native taps read a native-size texture's own texels: %s\n", fail ? "FAIL" : "ok");
   return fail != 0;
}
