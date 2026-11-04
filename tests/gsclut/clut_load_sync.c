/* A CLUT load the GPU palette path will not take reads what the GPU drew.
 *
 * When a palette is loaded, GSState asks the renderer to bring local
 * memory up to date for the palette's blocks, then GSClut::Write reads
 * memory. The hardware renderer skips that for a load the GPU palette path
 * takes -- GSClut::Read32 then reads the palette from the target holding
 * it, on the GPU -- and reads the target back for any other load, so the
 * CPU palette it falls back on holds what the GPU drew.
 *
 * Read32 takes the GPU path for CSM1 with the CSA, masked to the palette's
 * reach (15 for a 32-bit palette, 31 for a 16-bit one), within the first
 * sixteen blocks. Pinned here over every CSM, CPSM and CSA: a load skips
 * the readback exactly when Read32 will take the GPU path. The negative
 * control skips it for every load, which leaves CSM2 palettes and 16-bit
 * palettes past block 15 reading memory the GPU has not written.
 *
 * Build and run, from tests/gsclut:
 *   cc -O2 -std=c89 -pedantic -Wall clut_load_sync.c -o clut_load_sync
 *   ./clut_load_sync
 */
#include <stdio.h>

#define PSMCT32 0x00
#define PSMCT24 0x01
#define PSMCT16 0x02
#define PSMCT16S 0x0A

static int is16(unsigned cpsm)
{
   return cpsm == PSMCT16 || cpsm == PSMCT16S;
}

/* GSClut::Read32: whether the GPU palette path is tried. */
static int read32_takes_gpu(unsigned csm, unsigned cpsm, unsigned csa)
{
   const unsigned masked = csa & (is16(cpsm) ? 31u : 15u);
   return !csm && masked <= 15;
}

/* GSState's CLUT load: whether it skips the readback (passes clut). */
static int load_skips_readback(unsigned csm, unsigned cpsm, unsigned csa, int old_rule)
{
   const unsigned load_csa = csa & (is16(cpsm) ? 31u : 15u);
   if (old_rule)
      return 1;
   return csm == 0 && load_csa <= 15;
}

int main(void)
{
   static const unsigned cpsms[4] = { PSMCT32, PSMCT24, PSMCT16, PSMCT16S };
   unsigned csm, c, csa;
   int rule_bad = 0, old_bad = 0;

   for (csm = 0; csm < 2; csm++)
      for (c = 0; c < 4; c++)
         for (csa = 0; csa < 32; csa++)
         {
            const int gpu = read32_takes_gpu(csm, cpsms[c], csa);
            /* Skipping is right only where the GPU path reads the target. */
            if (load_skips_readback(csm, cpsms[c], csa, 0) != gpu)
               rule_bad++;
            if (load_skips_readback(csm, cpsms[c], csa, 1) != gpu)
               old_bad++;
         }

   printf("clut_load_sync\n");
   printf("  %s: loads skip the readback exactly where the GPU path reads: %d wrong of 256\n",
      rule_bad ? "FAIL" : "ok", rule_bad);
   printf("  %s: negative control, every load skipping it: %d wrong\n", old_bad ? "ok" : "FAIL", old_bad);
   if (rule_bad || !old_bad)
   {
      printf("clut_load_sync: FAILED\n");
      return 1;
   }
   printf("clut_load_sync: ok\n");
   return 0;
}
