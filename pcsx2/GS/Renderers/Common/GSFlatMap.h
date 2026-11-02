/*
 * A hash map in C: open addressing, linear probing, keys and values of a
 * fixed size held in one array of slots.
 *
 * It replaces std::unordered_map where a lookup sits on the draw path
 * (pipelines, samplers, render passes). A node map follows a bucket
 * pointer to a node it allocated on its own; here a lookup reads the
 * slot its hash lands on, the hash stored there turns away almost every
 * key that is not the one asked for, and the next slot is the next few
 * bytes. Nothing is allocated except when the table doubles.
 *
 * A slot is the hash, the key and the value, in that order, each at a
 * multiple of sizeof(size_t). A stored hash is never 0: 0 marks a free
 * slot. Erasing moves the entries after it back (backward shift), so
 * there are no tombstones and a probe stops at the first free slot.
 *
 * The caller hashes the key (gs_key_hash, at its constant size, where
 * XXH3 inlines) and passes the hash with it, so the map need not know
 * how. Keys compare with memcmp. Find is static in the header, as the
 * object pool's take is, so it inlines with the key size a constant;
 * the rest is in GSFlatMap.c. Not thread safe.
 */

#ifndef GS_FLAT_MAP_H
#define GS_FLAT_MAP_H

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gs_flat_map
{
   unsigned char *slots;
   size_t         key_size;
   size_t         value_size;
   size_t         key_offset;   /* sizeof(size_t): after the hash        */
   size_t         value_offset; /* after the key, rounded                */
   size_t         stride;       /* bytes a slot                          */
   size_t         mask;         /* slots - 1; slots is a power of two    */
   size_t         count;        /* entries                               */
} gs_flat_map_t;

/* An empty map for keys and values of these sizes. Allocates nothing. */
void gs_flat_map_init(gs_flat_map_t *map, size_t key_size, size_t value_size);
/* Frees the slots. The map is empty and can be used again. */
void gs_flat_map_free(gs_flat_map_t *map);
/* Empties the map and keeps its slots. */
void gs_flat_map_clear(gs_flat_map_t *map);
/* Slots for this many entries, made now: inserts up to that many never
 * allocate. Zero if the slots could not grow (the map is as it was). */
int  gs_flat_map_reserve(gs_flat_map_t *map, size_t entries);

/* The value under key, or, if there is none, a new entry's value, zeroed;
 * *inserted says which. NULL only if the slots could not grow. */
void *gs_flat_map_insert(gs_flat_map_t *map, const void *key, size_t hash, int *inserted);
/* Takes key out. Nonzero if it was there. */
int   gs_flat_map_erase(gs_flat_map_t *map, const void *key, size_t hash);

/* A stored hash is never 0, which marks a free slot. */
#define GS_FLAT_MAP_HASH(hash) ((hash) | (size_t)1)

#define GS_FLAT_MAP_SLOT(map, i)      ((map)->slots + (i) * (map)->stride)
#define GS_FLAT_MAP_USED(map, i)      (*(const size_t*)GS_FLAT_MAP_SLOT(map, i) != 0)
#define GS_FLAT_MAP_KEY(map, i)       ((void*)(GS_FLAT_MAP_SLOT(map, i) + (map)->key_offset))
#define GS_FLAT_MAP_VALUE(map, i)     ((void*)(GS_FLAT_MAP_SLOT(map, i) + (map)->value_offset))
/* Slots to walk to visit every entry: those with GS_FLAT_MAP_USED. */
#define GS_FLAT_MAP_SLOTS(map)        ((map)->slots ? (map)->mask + 1 : 0)

/* The value under key, or NULL. key_size is the map's, given again by
 * the caller so that it is a constant here and memcmp inlines. */
static void *gs_flat_map_find(const gs_flat_map_t *map, const void *key, size_t key_size, size_t hash)
{
   size_t i;

   if (!map->count)
      return NULL;
   hash = GS_FLAT_MAP_HASH(hash);
   for (i = hash & map->mask;; i = (i + 1) & map->mask)
   {
      const unsigned char *slot = map->slots + i * map->stride;
      const size_t         h    = *(const size_t*)slot;
      if (h == hash && !memcmp(slot + sizeof(size_t), key, key_size))
         return (void*)(slot + map->value_offset);
      if (!h)
         return NULL;
   }
}

#ifdef __cplusplus
}
#endif

#endif
