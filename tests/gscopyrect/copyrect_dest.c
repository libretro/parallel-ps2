/* A rect copy lands at the destination offset it is given, whatever the
 * source offset, on every path a device takes.
 *
 * GSDevice::CopyRect(sTex, dTex, r, destX, destY) copies rect r of the
 * source to (destX, destY) of the destination. Direct3D 11 does it with
 * CopySubresourceRegion at (destX, destY); the OpenGL device with
 * glCopyImageSubData (4.3 or ARB/EXT/OES_copy_image) at (destX, destY),
 * and on drivers with none of those with glBlitFramebuffer, which takes a
 * destination rectangle: (destX, destY) to (destX + w, destY + h). That
 * last one added the source offset again, so a copy from any r not at the
 * origin landed r.x right and r.y down of where it was asked to.
 *
 * Pinned here, over source rects and destination offsets: the pixels each
 * path writes are those of the rect at (destX, destY), and they carry the
 * source's pixels from r. The negative control is the old blit rectangle.
 *
 * Build and run, from tests/gscopyrect:
 *   cc -O2 -std=c89 -pedantic -Wall copyrect_dest.c -o copyrect_dest
 *   ./copyrect_dest
 */
#include <stdio.h>
#include <string.h>

#define W 48
#define H 40

enum { COPY_IMAGE, BLIT, BLIT_OLD };

static unsigned src[H][W];
static unsigned dst[H][W];

/* glBlitFramebuffer with nearest filtering and equal sizes: each
 * destination pixel of [dx0, dx1) x [dy0, dy1) takes the source pixel at
 * the same offset into [sx0, sx1) x [sy0, sy1). */
static void blit(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1)
{
   int x, y;
   (void)sx1; (void)sy1;
   for (y = dy0; y < dy1; y++)
      for (x = dx0; x < dx1; x++)
         if (x >= 0 && x < W && y >= 0 && y < H)
            dst[y][x] = src[sy0 + (y - dy0)][sx0 + (x - dx0)];
}

/* GSDeviceOGL::CopyRect's three ways, as written. */
static void copy_rect(int rx, int ry, int rz, int rw, int destX, int destY, int path)
{
   const int w = rz - rx, h = rw - ry;
   int x, y;
   if (path == COPY_IMAGE)
   {
      for (y = 0; y < h; y++)
         for (x = 0; x < w; x++)
            if (destX + x < W && destY + y < H)
               dst[destY + y][destX + x] = src[ry + y][rx + x];
   }
   else if (path == BLIT)
      blit(rx, ry, rx + w, ry + h, destX, destY, destX + w, destY + h);
   else
      blit(rx, ry, rx + w, ry + h, destX + rx, destY + ry, destX + rx + w, destY + ry + h);
}

static int lands(int rx, int ry, int rz, int rw, int destX, int destY, int path)
{
   int x, y, in;
   memset(dst, 0, sizeof(dst));
   copy_rect(rx, ry, rz, rw, destX, destY, path);
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
      {
         in = x >= destX && x < destX + (rz - rx) && y >= destY && y < destY + (rw - ry);
         if (in && dst[y][x] != src[ry + (y - destY)][rx + (x - destX)])
            return 0;
         if (!in && dst[y][x] != 0)
            return 0;
      }
   return 1;
}

int main(void)
{
   int x, y, rx, ry, dx, dy, path;
   int bad[3] = { 0, 0, 0 };

   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
         src[y][x] = 1u + (unsigned)(y * W + x);

   for (rx = 0; rx < 12; rx += 3)
      for (ry = 0; ry < 12; ry += 4)
         for (dx = 0; dx < 12; dx += 5)
            for (dy = 0; dy < 12; dy += 6)
               for (path = 0; path < 3; path++)
                  bad[path] += !lands(rx, ry, rx + 16, ry + 12, dx, dy, path);

   printf("copyrect_dest\n");
   printf("  %s: glCopyImageSubData lands at the destination offset: %d wrong\n", bad[COPY_IMAGE] ? "FAIL" : "ok", bad[COPY_IMAGE]);
   printf("  %s: glBlitFramebuffer lands at the destination offset: %d wrong\n", bad[BLIT] ? "FAIL" : "ok", bad[BLIT]);
   printf("  %s: negative control, the old blit rectangle: %d wrong\n", bad[BLIT_OLD] ? "ok" : "FAIL", bad[BLIT_OLD]);
   if (bad[COPY_IMAGE] || bad[BLIT] || !bad[BLIT_OLD])
   {
      printf("copyrect_dest: FAILED\n");
      return 1;
   }
   printf("copyrect_dest: ok\n");
   return 0;
}
