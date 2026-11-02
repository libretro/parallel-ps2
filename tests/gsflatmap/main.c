/* GSFlatMap (GS/Renderers/Common/GSFlatMap.c), against a plain array of
 * what it should hold: a million random inserts, finds and erases over a
 * few thousand keys. Hashes come from a small set, so keys share home
 * slots, probes run long and wrap past the end of the table, and an
 * erase has entries to shift back; a mistake in the shift leaves an
 * entry no find reaches, which the model catches. Then the grow, clear
 * and free paths, and a walk over every slot. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "GS/Renderers/Common/GSFlatMap.h"

#define KEYS 3000
#define OPS  1000000

typedef struct key { unsigned w[5]; } key_t_;

static unsigned long s_rng = 12345;
static unsigned rnd(unsigned n)
{
   s_rng = s_rng * 1103515245ul + 12345ul;
   return (unsigned)((s_rng >> 8) % n);
}

static key_t_  s_keys[KEYS];
static int     s_in[KEYS];
static unsigned s_val[KEYS];
static int     s_fails;

#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); s_fails++; } } while (0)

/* Few distinct hashes, and 0 among them (the map stores hash | 1). */
static size_t hash_of(int k, unsigned spread) { return (size_t)((k * 2654435761u) % spread); }

static void run(unsigned spread)
{
   gs_flat_map_t m;
   long          i;
   int           k, n, inserted;
   size_t        s;
   gs_flat_map_init(&m, sizeof(key_t_), sizeof(unsigned));
   memset(s_in, 0, sizeof(s_in));
   for (i = 0; i < OPS; i++)
   {
      unsigned *v;
      k = (int)rnd(KEYS);
      switch (rnd(3))
      {
         case 0:
            v = (unsigned*)gs_flat_map_insert(&m, &s_keys[k], hash_of(k, spread), &inserted);
            CHECK(v != NULL, "insert gives a value");
            CHECK(inserted == !s_in[k], "inserted says whether the key was new");
            if (inserted)
               CHECK(*v == 0, "a new value is zeroed");
            else
               CHECK(*v == s_val[k], "an insert of a key there gives its value");
            s_val[k] = *v = (unsigned)i;
            s_in[k] = 1;
            break;
         case 1:
            CHECK(gs_flat_map_erase(&m, &s_keys[k], hash_of(k, spread)) == s_in[k], "erase finds what is there");
            s_in[k] = 0;
            break;
         default:
            v = (unsigned*)gs_flat_map_find(&m, &s_keys[k], sizeof(key_t_), hash_of(k, spread));
            CHECK((v != NULL) == s_in[k], "find finds what is there and nothing else");
            if (v && s_in[k])
               CHECK(*v == s_val[k], "and its value");
            break;
      }
      if (s_fails > 10)
         break;
   }
   /* Every key, and a walk over the slots: count and contents agree. */
   for (n = 0, k = 0; k < KEYS; k++)
   {
      unsigned *v = (unsigned*)gs_flat_map_find(&m, &s_keys[k], sizeof(key_t_), hash_of(k, spread));
      CHECK((v != NULL) == s_in[k], "every key where it should be");
      n += s_in[k];
   }
   CHECK(m.count == (size_t)n, "count is the entries");
   for (n = 0, s = 0; s < GS_FLAT_MAP_SLOTS(&m); s++)
      if (GS_FLAT_MAP_USED(&m, s))
      {
         const key_t_* kp = (const key_t_*)GS_FLAT_MAP_KEY(&m, s);
         k = (int)kp->w[0];
         CHECK(k >= 0 && k < KEYS && s_in[k], "a walk sees only what is there");
         CHECK(*(const unsigned*)GS_FLAT_MAP_VALUE(&m, s) == s_val[k], "with its value");
         n++;
      }
   CHECK(m.count == (size_t)n, "a walk sees every entry once");
   CHECK(m.count * 2 <= GS_FLAT_MAP_SLOTS(&m), "never more than half full");
   gs_flat_map_clear(&m);
   CHECK(m.count == 0 && m.slots != NULL, "clear keeps the slots");
   for (k = 0; k < KEYS; k++)
      CHECK(gs_flat_map_find(&m, &s_keys[k], sizeof(key_t_), hash_of(k, spread)) == NULL, "and holds nothing");
   gs_flat_map_free(&m);
   CHECK(m.slots == NULL && m.count == 0, "free frees");
   CHECK(gs_flat_map_insert(&m, &s_keys[0], 0, &inserted) != NULL && inserted, "a freed map takes inserts again");
   gs_flat_map_free(&m);

   /* Reserved for KEYS: as many inserts move the slots nowhere (none
    * grows them), every one is found after, and a clear keeps them. */
   CHECK(gs_flat_map_reserve(&m, KEYS), "reserve");
   {
      const unsigned char* slots = m.slots;
      for (k = 0; k < KEYS; k++)
         CHECK(gs_flat_map_insert(&m, &s_keys[k], hash_of(k, spread), &inserted) != NULL && inserted, "insert into the reserve");
      CHECK(m.slots == slots, "inserts up to the reserve never grow the slots");
      for (k = 0; k < KEYS; k++)
         CHECK(gs_flat_map_find(&m, &s_keys[k], sizeof(key_t_), hash_of(k, spread)) != NULL, "each found");
      CHECK(gs_flat_map_reserve(&m, KEYS / 2) && m.slots == slots, "a smaller reserve leaves them");
      gs_flat_map_clear(&m);
      CHECK(m.slots == slots, "and so does a clear");
   }
   gs_flat_map_free(&m);
}

int main(void)
{
   int k, j;
   for (k = 0; k < KEYS; k++)
   {
      s_keys[k].w[0] = (unsigned)k;
      for (j = 1; j < 5; j++)
         s_keys[k].w[j] = (unsigned)(k * 7 + j);
   }
   run(7);          /* almost every key shares a home with hundreds   */
   run(1u << 30);   /* hashes spread as XXH3's are                     */
   printf(s_fails ? "flat map: FAILED (%d)\n" : "flat map: ok\n", s_fails);
   return s_fails != 0;
}
