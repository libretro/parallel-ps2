/*
 * A hash map in C whose values stay where they are: open addressing with
 * linear probing over slots of a stored hash and a node pointer, the
 * nodes from a gs_node_pool, and the nodes listed densely for a walk.
 *
 * For a map whose values are pointed at from elsewhere, as the texture
 * cache's sources point at their hash cache entries. The slots are
 * sixteen bytes, four to a cache line: a probe compares stored hashes
 * there and reads a node only when one matches, and a node holds the
 * key next to the value, so a hit reads the slot and the one node, as a
 * standard map's bucket and node, without its modulo and chain. At most
 * a quarter of the slots are used, so a miss ends at a free slot after a
 * slot or two. A node knows its slot, so an erase neither looks it up
 * nor hashes it again.
 *
 * A node is its value, then its slot, its place in the list and its key:
 * the value's address is the node's, handed out and stable until erased.
 *
 * Walk the entries with GS_STABLE_MAP_COUNT and gs_stable_map_at, last
 * to first: an erase moves the last entry into the place it frees, so a
 * walk downwards may erase the entry it is at and go on. Not thread safe.
 */

#ifndef GS_STABLE_MAP_H
#define GS_STABLE_MAP_H

#include <stddef.h>
#include <string.h>

#include "../HW/GSObjectPool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A node: the value, rounded to sixteen bytes so the key after it keeps
 * any alignment up to that, then the slot, the place in the list, the
 * key. */
#define GS_STABLE_MAP_ROUND(n)               (((n) + 15) & ~(size_t)15)
#define GS_STABLE_MAP_HEAD(value_size)        GS_STABLE_MAP_ROUND(value_size)
#define GS_STABLE_MAP_KEY_OFFSET(value_size)  (GS_STABLE_MAP_HEAD(value_size) + 2 * sizeof(size_t))
/* A node's size, for the pool the caller keeps for the map's nodes:
 * GS_NODE_POOL_INIT(GS_STABLE_MAP_NODE_SIZE(k, v), 16). */
#define GS_STABLE_MAP_NODE_SIZE(key_size, value_size) \
   (GS_STABLE_MAP_KEY_OFFSET(value_size) + (key_size))

typedef struct gs_stable_map_slot
{
   size_t         hash; /* stored hash, never 0; 0 marks a free slot */
   unsigned char *node;
} gs_stable_map_slot_t;

typedef struct gs_stable_map
{
   gs_stable_map_slot_t *slots;
   unsigned char       **list;       /* the nodes, count of them; room for
                                        a quarter of the slots            */
   size_t                mask;       /* slots - 1; slots is a power of two */
   size_t                count;      /* entries                            */
   gs_node_pool_t       *nodes;      /* the caller's, or NULL: allocator   */
   size_t                key_size;
   size_t                value_size;
   size_t                key_offset; /* GS_STABLE_MAP_KEY_OFFSET           */
} gs_stable_map_t;

/* An empty map. nodes is a pool sized GS_STABLE_MAP_NODE_SIZE, which
 * must outlive the map (a static one); NULL, or a build with an address
 * sanitizer, takes every node from the allocator. Allocates nothing. */
void  gs_stable_map_init(gs_stable_map_t *map, size_t key_size, size_t value_size, gs_node_pool_t *nodes);
/* Gives back every node and frees the slots. Can be used again. */
void  gs_stable_map_free(gs_stable_map_t *map);
/* Gives back every node; the slots stay. */
void  gs_stable_map_clear(gs_stable_map_t *map);
/* Slots for this many entries, made now: inserts up to that many never
 * grow them. Zero if they could not be made (the map is as it was). */
int   gs_stable_map_reserve(gs_stable_map_t *map, size_t entries);

/* The value under key, or, if there is none, a new entry's, zeroed;
 * *inserted says which. It stays at that address until erased. NULL
 * only if the slots could not grow or a node could not be had. */
void *gs_stable_map_insert(gs_stable_map_t *map, const void *key, size_t hash, int *inserted);
/* Takes out the entry whose value this is. */
void  gs_stable_map_erase(gs_stable_map_t *map, void *value);

#define GS_STABLE_MAP_HASH(hash)   ((hash) | (size_t)1)
/* The key of the entry whose value this is. */
#define GS_STABLE_MAP_KEY(map, value) \
   ((const void*)((const unsigned char*)(value) + (map)->key_offset))
/* Entries in the map. */
#define GS_STABLE_MAP_COUNT(map)   ((map)->count)

/* The value of entry i, below GS_STABLE_MAP_COUNT. */
static void *gs_stable_map_at(const gs_stable_map_t *map, size_t i)
{
   return map->list[i];
}

/* The value under key, or NULL. key_size is the map's, given again so
 * that it is a constant here and memcmp inlines. */
static void *gs_stable_map_find(const gs_stable_map_t *map, const void *key, size_t key_size, size_t hash)
{
   size_t i;

   if (!map->count)
      return NULL;
   hash = GS_STABLE_MAP_HASH(hash);
   for (i = hash & map->mask;; i = (i + 1) & map->mask)
   {
      const gs_stable_map_slot_t *slot = &map->slots[i];
      if (slot->hash == hash && !memcmp(slot->node + map->key_offset, key, key_size))
         return slot->node;
      if (!slot->hash)
         return NULL;
   }
}

#ifdef __cplusplus
}
#endif

#endif
