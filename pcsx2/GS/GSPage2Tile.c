#include <string.h>

#include "GSPage2Tile.h"

#define GS_PAGE2TILE_RADIX 9
#define GS_PAGE2TILE_BINS  (1u << GS_PAGE2TILE_RADIX)

size_t gs_page2tile_sort(unsigned *keys, unsigned *scratch, size_t n)
{
   unsigned  count[GS_PAGE2TILE_BINS];
   unsigned *from = keys;
   unsigned *to   = scratch;
   unsigned  shift;
   size_t    i, entries;

   /* Three passes, low digit first, each stable: 27 bits, the 25 a key
    * has. An odd number of passes ends in scratch: copied back. */
   for (shift = 0; shift < 3 * GS_PAGE2TILE_RADIX; shift += GS_PAGE2TILE_RADIX)
   {
      unsigned *t;
      unsigned  sum = 0;
      memset(count, 0, sizeof(count));
      for (i = 0; i < n; i++)
         count[(from[i] >> shift) & (GS_PAGE2TILE_BINS - 1)]++;
      for (i = 0; i < GS_PAGE2TILE_BINS; i++)
      {
         const unsigned c = count[i];
         count[i] = sum;
         sum     += c;
      }
      for (i = 0; i < n; i++)
         to[count[(from[i] >> shift) & (GS_PAGE2TILE_BINS - 1)]++] = from[i];
      t    = from;
      from = to;
      to   = t;
   }
   if (from != keys)
      memcpy(keys, from, n * sizeof(*keys));

   /* an entry for each run of one page and one row */
   for (entries = 0, i = 0; i < n; i++)
   {
      if (!i || (keys[i] >> 5) != (keys[i - 1] >> 5))
         entries++;
   }
   return entries;
}

void gs_page2tile_fill(const unsigned *keys, size_t n, unsigned pages, int *entries, unsigned *first)
{
   size_t   i;
   unsigned at   = 0;
   unsigned page = 0;

   for (i = 0; i < n;)
   {
      const unsigned run  = keys[i] >> 5; /* page and row */
      const unsigned p    = keys[i] >> 16;
      unsigned       mask = 0;
      while (page <= p)
         first[page++] = at;
      for (; i < n && (keys[i] >> 5) == run; i++)
         mask |= 1u << (keys[i] & 31);
      entries[2 * at]     = (int)((keys[i - 1] & 0xffffu) >> 5);
      entries[2 * at + 1] = (int)~mask;
      at++;
   }
   while (page <= pages)
      first[page++] = at;
}
