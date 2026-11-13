/*
 * A growable array in C89: elements of one size, held in order in one
 * allocation that doubles when it is full. What std::vector was used for
 * where a list is appended to, read back from the end, walked and cut
 * (the draw transfers, the sprite draw list).
 *
 * The elements are bytes to the array: the caller names the type at the
 * access (GS_ARRAY_AT). A push that cannot grow the array returns NULL
 * and leaves it as it was. Alignment is malloc's: sixteen bytes, enough
 * for the vector members of what goes in it. Not thread safe.
 */

#ifndef GS_ARRAY_H
#define GS_ARRAY_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <retro_inline.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gs_array
{
   unsigned char *items;
   size_t         count;
   size_t         capacity; /* elements the allocation holds */
   size_t         elem;     /* bytes an element */
} gs_array_t;

#define GS_ARRAY_FIRST 16

/* An empty array of elements of elem bytes. Allocates nothing. */
static INLINE void gs_array_init(gs_array_t *a, size_t elem)
{
   a->items    = NULL;
   a->count    = 0;
   a->capacity = 0;
   a->elem     = elem;
}

/* Frees the allocation. The array is empty and can be used again. */
static INLINE void gs_array_free(gs_array_t *a)
{
   free(a->items);
   a->items    = NULL;
   a->count    = 0;
   a->capacity = 0;
}

/* Room for at least entries elements, made now: pushes up to that many
 * never allocate. Zero if the allocation could not grow (the array is as
 * it was). */
static INLINE int gs_array_reserve(gs_array_t *a, size_t entries)
{
   size_t         capacity;
   unsigned char *grown;

   if (entries <= a->capacity)
      return 1;
   capacity = a->capacity ? a->capacity : GS_ARRAY_FIRST;
   while (capacity < entries)
      capacity *= 2;
   grown = (unsigned char*)realloc(a->items, capacity * a->elem);
   if (!grown)
      return 0;
   a->items    = grown;
   a->capacity = capacity;
   return 1;
}

/* A new last element, its bytes as they come, or NULL if the array could
 * not grow. */
static INLINE void *gs_array_push(gs_array_t *a)
{
   if (a->count == a->capacity && !gs_array_reserve(a, a->count + 1))
      return NULL;
   return a->items + a->count++ * a->elem;
}

/* Takes element i out; those after it move down one. */
static INLINE void gs_array_erase(gs_array_t *a, size_t i)
{
   memmove(a->items + i * a->elem, a->items + (i + 1) * a->elem, (a->count - i - 1) * a->elem);
   a->count--;
}

/* Takes the first n elements out; the rest move to the front. */
static INLINE void gs_array_erase_front(gs_array_t *a, size_t n)
{
   memmove(a->items, a->items + n * a->elem, (a->count - n) * a->elem);
   a->count -= n;
}

/* Element i of the array, as a T. */
#define GS_ARRAY_AT(a, T, i) (((T*)(void*)(a)->items)[i])
/* The elements, as T*: NULL while the array has had none. */
#define GS_ARRAY_ITEMS(a, T) ((T*)(void*)(a)->items)

#ifdef __cplusplus
}
#endif

#endif
