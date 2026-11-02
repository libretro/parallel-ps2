/* GSStableMap against a model, built as C89: inserts, finds and erases
 * of keys with few distinct hashes (0 among them), every value staying at
 * the address it was first given until erased, keys read back from
 * values, a walk that sees every entry once, clear and free; with the
 * nodes from a pool and from the allocator. */

#include <stdio.h>
#include <string.h>

/* The headers' statics, not all of which this calls. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "GS/Renderers/Common/GSStableMap.h"

#define KEYS 2000
#define OPS  400000

typedef struct key { unsigned w[10]; } key_t_; /* 40 bytes, as the hash cache's */
typedef struct value { void *p; unsigned refcount; unsigned short age; unsigned char b[6]; } value_t_;

static key_t_    s_keys[KEYS];
static value_t_ *s_at[KEYS];   /* where the map put each key's value, or NULL */
static unsigned  s_tag[KEYS];
static int       s_fails;
static unsigned long s_rng = 12345;

static gs_node_pool_t s_pool = GS_NODE_POOL_INIT(GS_STABLE_MAP_NODE_SIZE(sizeof(key_t_), sizeof(value_t_)), 16);

#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); s_fails++; } } while (0)

static unsigned rnd(unsigned n)
{
   s_rng = s_rng * 1103515245UL + 12345UL;
   return (unsigned)((s_rng >> 16) & 0x7fffffffUL) % n;
}

static size_t hash_of(int k, unsigned spread) { return (size_t)((k * 2654435761u) % spread); }

static void run(gs_node_pool_t *nodes, unsigned spread)
{
   gs_stable_map_t m;
   long            i;
   int             k, n, inserted;
   size_t          s;

   gs_stable_map_init(&m, sizeof(key_t_), sizeof(value_t_), nodes);
   memset(s_at, 0, sizeof(s_at));
   for (i = 0; i < OPS; i++)
   {
      value_t_ *v;
      k = (int)rnd(KEYS);
      switch (rnd(3))
      {
         case 0:
            v = (value_t_*)gs_stable_map_insert(&m, &s_keys[k], hash_of(k, spread), &inserted);
            CHECK(v != NULL, "insert gives a value");
            CHECK(inserted == (s_at[k] == NULL), "inserted says whether the key was new");
            if (inserted)
            {
               CHECK(v->p == NULL && v->refcount == 0 && v->age == 0, "a new value is zeroed");
               s_at[k] = v;
               v->refcount = s_tag[k] = (unsigned)i;
            }
            else
               CHECK(v == s_at[k] && v->refcount == s_tag[k], "an insert of a key there gives its value, where it was");
            CHECK(!memcmp(GS_STABLE_MAP_KEY(&m, v), &s_keys[k], sizeof(key_t_)), "the value's key");
            break;
         case 1:
            if (s_at[k])
            {
               gs_stable_map_erase(&m, s_at[k]);
               s_at[k] = NULL;
            }
            CHECK(gs_stable_map_find(&m, &s_keys[k], sizeof(key_t_), hash_of(k, spread)) == NULL, "an erased key is gone");
            break;
         default:
            v = (value_t_*)gs_stable_map_find(&m, &s_keys[k], sizeof(key_t_), hash_of(k, spread));
            CHECK(v == s_at[k], "find gives the value where it was put, and nothing for a key not there");
            if (v)
               CHECK(v->refcount == s_tag[k], "with what was written to it");
            break;
      }
      if (s_fails > 10)
         break;
   }
   for (n = 0, k = 0; k < KEYS; k++)
      n += (s_at[k] != NULL);
   CHECK(GS_STABLE_MAP_COUNT(&m) == (size_t)n, "count is the entries");
   for (n = 0, s = 0; s < GS_STABLE_MAP_COUNT(&m); s++)
   {
      value_t_     *v  = (value_t_*)gs_stable_map_at(&m, s);
      const key_t_ *kp = (const key_t_*)GS_STABLE_MAP_KEY(&m, v);
      k = (int)kp->w[0];
      CHECK(k >= 0 && k < KEYS && s_at[k] == v, "a walk sees each value where it is");
      v->age = 0;
      n++;
   }
   CHECK(GS_STABLE_MAP_COUNT(&m) == (size_t)n, "count is the entries walked");

   /* A walk from the last down, erasing every even key it meets: each
    * entry seen once, the odd ones staying put. */
   for (s = GS_STABLE_MAP_COUNT(&m); s-- > 0;)
   {
      value_t_ *v = (value_t_*)gs_stable_map_at(&m, s);
      k = (int)((const key_t_*)GS_STABLE_MAP_KEY(&m, v))->w[0];
      CHECK(v->age == 0, "a walk downwards that erases sees each entry once");
      v->age = 1;
      if (!(k & 1))
      {
         gs_stable_map_erase(&m, v);
         s_at[k] = NULL;
      }
   }
   for (n = 0, k = 0; k < KEYS; k++)
   {
      CHECK(gs_stable_map_find(&m, &s_keys[k], sizeof(key_t_), hash_of(k, spread)) == s_at[k], "the others where they were");
      CHECK(!s_at[k] || s_at[k]->age == 1, "every one of them seen");
      n += (s_at[k] != NULL);
   }
   CHECK(GS_STABLE_MAP_COUNT(&m) == (size_t)n, "count after");

   CHECK(gs_stable_map_reserve(&m, KEYS), "reserve");
   gs_stable_map_clear(&m);
   CHECK(GS_STABLE_MAP_COUNT(&m) == 0, "clear empties");
   for (k = 0; k < KEYS; k++)
      CHECK(gs_stable_map_find(&m, &s_keys[k], sizeof(key_t_), hash_of(k, spread)) == NULL, "and holds nothing");
   CHECK(gs_stable_map_insert(&m, &s_keys[0], 0, &inserted) != NULL && inserted, "a cleared map takes inserts again");
   gs_stable_map_free(&m);
   CHECK(GS_STABLE_MAP_COUNT(&m) == 0, "free frees");
}

int main(void)
{
   int k, j;
   for (k = 0; k < KEYS; k++)
   {
      s_keys[k].w[0] = (unsigned)k;
      for (j = 1; j < 10; j++)
         s_keys[k].w[j] = (unsigned)(k * 7 + j);
   }
   run(&s_pool, 0xffffffffu);
   run(&s_pool, 7);
   run(NULL, 0xffffffffu);
   run(NULL, 3);
   gs_node_pools_release();
   if (s_fails)
      return 1;
   printf("stable map: ok\n");
   return 0;
}
