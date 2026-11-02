#include <stdlib.h>
#include <string.h>

/* The header's find is static, and this file does not call it. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "GSFlatMap.h"

/* Slots in a map's first allocation. */
#define GS_FLAT_MAP_FIRST 32

static size_t gs_flat_map_round(size_t n)
{
   return (n + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1);
}

void gs_flat_map_init(gs_flat_map_t *map, size_t key_size, size_t value_size)
{
   map->slots        = NULL;
   map->key_size     = key_size;
   map->value_size   = value_size;
   map->key_offset   = sizeof(size_t);
   map->value_offset = map->key_offset + gs_flat_map_round(key_size);
   map->stride       = map->value_offset + gs_flat_map_round(value_size);
   map->mask         = 0;
   map->count        = 0;
}

void gs_flat_map_free(gs_flat_map_t *map)
{
   free(map->slots);
   map->slots = NULL;
   map->mask  = 0;
   map->count = 0;
}

void gs_flat_map_clear(gs_flat_map_t *map)
{
   if (map->slots)
      memset(map->slots, 0, (map->mask + 1) * map->stride);
   map->count = 0;
}

/* The slot a stored hash and key go in: the first free one from where
 * the hash lands. The slots must have one free. */
static unsigned char *gs_flat_map_place(gs_flat_map_t *map, size_t stored)
{
   size_t i = stored & map->mask;
   while (*(const size_t*)(map->slots + i * map->stride))
      i = (i + 1) & map->mask;
   return map->slots + i * map->stride;
}

/* Twice the slots (or the first ones), every entry placed again. */
static int gs_flat_map_grow(gs_flat_map_t *map)
{
   const size_t   old_slots = map->slots ? map->mask + 1 : 0;
   const size_t   slots     = old_slots ? old_slots * 2 : GS_FLAT_MAP_FIRST;
   unsigned char *old       = map->slots;
   unsigned char *fresh     = (unsigned char*)calloc(slots, map->stride);
   size_t         i;

   if (!fresh)
      return 0;
   map->slots = fresh;
   map->mask  = slots - 1;
   for (i = 0; i < old_slots; i++)
   {
      const unsigned char *from   = old + i * map->stride;
      const size_t         stored = *(const size_t*)from;
      if (stored)
         memcpy(gs_flat_map_place(map, stored), from, map->stride);
   }
   free(old);
   return 1;
}

void *gs_flat_map_insert(gs_flat_map_t *map, const void *key, size_t hash, int *inserted)
{
   unsigned char *slot;
   void          *found = gs_flat_map_find(map, key, map->key_size, hash);
   const size_t   stored = GS_FLAT_MAP_HASH(hash);

   *inserted = 0;
   if (found)
      return found;

   /* At most half full: probes stay short, and there is always a free
    * slot to end one. */
   if (!map->slots || (map->count + 1) * 2 > map->mask + 1)
   {
      if (!gs_flat_map_grow(map))
         return NULL;
   }
   slot = gs_flat_map_place(map, stored);
   *(size_t*)slot = stored;
   memcpy(slot + map->key_offset, key, map->key_size);
   memset(slot + map->value_offset, 0, map->stride - map->value_offset);
   map->count++;
   *inserted = 1;
   return slot + map->value_offset;
}

int gs_flat_map_erase(gs_flat_map_t *map, const void *key, size_t hash)
{
   unsigned char *value = (unsigned char*)gs_flat_map_find(map, key, map->key_size, hash);
   size_t         hole;
   size_t         i;

   if (!value)
      return 0;
   hole = (size_t)(value - map->value_offset - map->slots) / map->stride;

   /* Backward shift: each entry after the hole that could live in it
    * (the hole lies between where its hash lands and where it is) moves
    * into it, until a free slot ends the run. */
   for (i = (hole + 1) & map->mask;; i = (i + 1) & map->mask)
   {
      unsigned char *slot   = map->slots + i * map->stride;
      const size_t   stored = *(const size_t*)slot;
      size_t         home;
      if (!stored)
         break;
      home = stored & map->mask;
      if (((i - home) & map->mask) >= ((i - hole) & map->mask))
      {
         memcpy(map->slots + hole * map->stride, slot, map->stride);
         hole = i;
      }
   }
   memset(map->slots + hole * map->stride, 0, map->stride);
   map->count--;
   return 1;
}
