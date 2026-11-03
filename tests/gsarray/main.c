/* GSArray (GS/Renderers/Common/GSArray.h), against a plain array of what
 * it should hold: a hundred thousand random pushes, erases of one, cuts
 * at the front and clears, over elements of a size that is not a power
 * of two; the model is compared after each. Then reserve, which must not
 * move what is there, and free, after which the array is usable again. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "GS/Renderers/Common/GSArray.h"

#define OPS 100000
#define MOST 4096

typedef struct elem { unsigned a; unsigned char b[9]; } elem_t;

static unsigned long s_rng = 777;
static unsigned rnd(unsigned n)
{
   s_rng = s_rng * 1103515245ul + 12345ul;
   return (unsigned)((s_rng >> 8) % n);
}

static elem_t   s_model[MOST];
static size_t   s_model_count;
static int      s_fails;

#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); s_fails++; } } while (0)

static elem_t make(unsigned i)
{
   elem_t e;
   unsigned j;
   e.a = i * 2654435761u;
   for (j = 0; j < sizeof(e.b); j++)
      e.b[j] = (unsigned char)(i + j);
   return e;
}

static int same(const gs_array_t *a)
{
   size_t i;
   if (a->count != s_model_count)
      return 0;
   for (i = 0; i < s_model_count; i++)
      if (memcmp(&GS_ARRAY_AT(a, elem_t, i), &s_model[i], sizeof(elem_t)) != 0)
         return 0;
   return 1;
}

int main(void)
{
   gs_array_t a;
   unsigned   op, i;
   size_t     grown_to = 0;

   gs_array_init(&a, sizeof(elem_t));
   CHECK(a.count == 0 && a.items == NULL, "an empty array allocates nothing");

   for (op = 0; op < OPS; op++)
   {
      const unsigned what = rnd(100);
      if (what < 80 && s_model_count < MOST)
      {
         elem_t *e = (elem_t*)gs_array_push(&a);
         CHECK(e != NULL, "push");
         *e = make(op);
         s_model[s_model_count++] = *e;
      }
      else if (what < 90 && s_model_count)
      {
         const size_t at = rnd((unsigned)s_model_count);
         gs_array_erase(&a, at);
         memmove(&s_model[at], &s_model[at + 1], (s_model_count - at - 1) * sizeof(elem_t));
         s_model_count--;
      }
      else if (what < 99 && s_model_count)
      {
         const size_t n = rnd((unsigned)s_model_count / 4 + 1);
         gs_array_erase_front(&a, n);
         memmove(&s_model[0], &s_model[n], (s_model_count - n) * sizeof(elem_t));
         s_model_count -= n;
      }
      else
      {
         a.count       = 0;
         s_model_count = 0;
      }
      if (a.capacity > grown_to)
         grown_to = a.capacity;
      if (!same(&a))
      {
         printf("  FAIL: op %u: the array differs from the model\n", op);
         s_fails++;
         break;
      }
   }
   printf("  grew to %lu elements\n", (unsigned long)grown_to);
   CHECK(grown_to >= 256, "the array grew past its first allocations");

   /* reserve keeps what is there, and reserving less changes nothing */
   {
      size_t before = a.capacity;
      CHECK(gs_array_reserve(&a, before + 1000), "reserve");
      CHECK(a.capacity >= before + 1000 && same(&a), "reserve keeps the elements");
      before = a.capacity;
      CHECK(gs_array_reserve(&a, 1), "reserve less");
      CHECK(a.capacity == before, "reserving less changes nothing");
   }

   /* free, then use again */
   gs_array_free(&a);
   CHECK(a.count == 0 && a.capacity == 0 && a.items == NULL, "free empties the array");
   s_model_count = 0;
   for (i = 0; i < 100; i++)
   {
      elem_t *e = (elem_t*)gs_array_push(&a);
      CHECK(e != NULL, "push after free");
      *e = make(i);
      s_model[s_model_count++] = *e;
   }
   CHECK(same(&a), "the array is usable after free");
   CHECK(GS_ARRAY_ITEMS(&a, elem_t) == (elem_t*)(void*)a.items, "the items as a type");
   gs_array_free(&a);

   if (s_fails)
   {
      printf("gsarray: %d failed\n", s_fails);
      return 1;
   }
   printf("gsarray: ok, %u ops, %lu elements at most\n", OPS, (unsigned long)grown_to);
   return 0;
}
