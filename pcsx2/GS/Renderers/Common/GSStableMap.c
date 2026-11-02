#include <stdlib.h>
#include <string.h>

/* The headers' statics, which this file does not all call. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "GSStableMap.h"

/* Slots in a map's first allocation, and how few of them may be in use:
 * a quarter, so a miss ends after a slot or two. */
#define GS_STABLE_MAP_FIRST 64
#define GS_STABLE_MAP_LOAD  4

/* A node's slot and place in the list, after its value. */
#define GS_STABLE_MAP_NODE_SLOT(map, node) \
   (*(size_t*)((node) + (map)->key_offset - 2 * sizeof(size_t)))
#define GS_STABLE_MAP_NODE_PLACE(map, node) \
   (*(size_t*)((node) + (map)->key_offset - sizeof(size_t)))

static unsigned char *gs_stable_map_node_take(gs_stable_map_t *map)
{
   if (map->nodes && !GS_OBJECT_POOL_SANITIZED)
      return (unsigned char*)gs_node_pool_take(map->nodes);
   return (unsigned char*)malloc(map->key_offset + map->key_size);
}

static void gs_stable_map_node_give(gs_stable_map_t *map, unsigned char *node)
{
   if (map->nodes && !GS_OBJECT_POOL_SANITIZED)
      gs_node_pool_give(map->nodes, node);
   else
      free(node);
}

/* Slots for `slots`, every entry placed again, and a list with room for
 * the most entries they take. */
static int gs_stable_map_resize(gs_stable_map_t *map, size_t slots)
{
   const size_t          old_slots = map->slots ? map->mask + 1 : 0;
   gs_stable_map_slot_t *old       = map->slots;
   gs_stable_map_slot_t *fresh     = (gs_stable_map_slot_t*)calloc(slots, sizeof(*fresh));
   unsigned char       **list;
   size_t                i;

   if (!fresh)
      return 0;
   list = (unsigned char**)realloc(map->list, slots / GS_STABLE_MAP_LOAD * sizeof(*list));
   if (!list)
   {
      free(fresh);
      return 0;
   }
   map->list  = list;
   map->slots = fresh;
   map->mask  = slots - 1;
   for (i = 0; i < old_slots; i++)
   {
      size_t j;
      if (!old[i].hash)
         continue;
      for (j = old[i].hash & map->mask; fresh[j].hash; j = (j + 1) & map->mask)
         ;
      fresh[j] = old[i];
      GS_STABLE_MAP_NODE_SLOT(map, old[i].node) = j;
   }
   free(old);
   return 1;
}

void gs_stable_map_init(gs_stable_map_t *map, size_t key_size, size_t value_size, gs_node_pool_t *nodes)
{
   map->slots      = NULL;
   map->list       = NULL;
   map->mask       = 0;
   map->count      = 0;
   map->nodes      = nodes;
   map->key_size   = key_size;
   map->value_size = value_size;
   map->key_offset = GS_STABLE_MAP_KEY_OFFSET(value_size);
}

void gs_stable_map_clear(gs_stable_map_t *map)
{
   size_t i;
   for (i = 0; i < map->count; i++)
   {
      unsigned char *node = map->list[i];
      map->slots[GS_STABLE_MAP_NODE_SLOT(map, node)].hash = 0;
      gs_stable_map_node_give(map, node);
   }
   map->count = 0;
}

void gs_stable_map_free(gs_stable_map_t *map)
{
   gs_stable_map_clear(map);
   free(map->slots);
   free(map->list);
   map->slots = NULL;
   map->list  = NULL;
   map->mask  = 0;
}

int gs_stable_map_reserve(gs_stable_map_t *map, size_t entries)
{
   size_t slots = map->slots ? map->mask + 1 : GS_STABLE_MAP_FIRST;
   while (entries * GS_STABLE_MAP_LOAD > slots)
      slots *= 2;
   if (map->slots && slots == map->mask + 1)
      return 1;
   return gs_stable_map_resize(map, slots);
}

void *gs_stable_map_insert(gs_stable_map_t *map, const void *key, size_t hash, int *inserted)
{
   const size_t   stored = GS_STABLE_MAP_HASH(hash);
   unsigned char *node;
   size_t         i      = 0;

   *inserted = 0;
   if (map->slots)
   {
      /* one probe: the key's slot, or the free one it would go in */
      for (i = stored & map->mask; map->slots[i].hash; i = (i + 1) & map->mask)
      {
         if (map->slots[i].hash == stored
               && !memcmp(map->slots[i].node + map->key_offset, key, map->key_size))
            return map->slots[i].node;
      }
   }
   if (!map->slots || (map->count + 1) * GS_STABLE_MAP_LOAD > map->mask + 1)
   {
      if (!gs_stable_map_resize(map, map->slots ? (map->mask + 1) * 2 : GS_STABLE_MAP_FIRST))
         return NULL;
      for (i = stored & map->mask; map->slots[i].hash; i = (i + 1) & map->mask)
         ;
   }
   if (!(node = gs_stable_map_node_take(map)))
      return NULL;
   memset(node, 0, map->value_size);
   GS_STABLE_MAP_NODE_SLOT(map, node)  = i;
   GS_STABLE_MAP_NODE_PLACE(map, node) = map->count;
   memcpy(node + map->key_offset, key, map->key_size);
   map->slots[i].hash      = stored;
   map->slots[i].node      = node;
   map->list[map->count++] = node;
   *inserted = 1;
   return node;
}

void gs_stable_map_erase(gs_stable_map_t *map, void *value)
{
   unsigned char *node  = (unsigned char*)value;
   unsigned char *last;
   size_t         hole  = GS_STABLE_MAP_NODE_SLOT(map, node);
   const size_t   place = GS_STABLE_MAP_NODE_PLACE(map, node);
   size_t         i     = hole;

   /* Backward shift: each entry after the hole whose home is not
    * between the hole and it moves into the hole, so no probe that
    * passed through the hole stops short of it. */
   for (;;)
   {
      size_t home;
      i = (i + 1) & map->mask;
      if (!map->slots[i].hash)
         break;
      home = map->slots[i].hash & map->mask;
      if (((i - home) & map->mask) >= ((i - hole) & map->mask))
      {
         map->slots[hole] = map->slots[i];
         GS_STABLE_MAP_NODE_SLOT(map, map->slots[hole].node) = hole;
         hole = i;
      }
   }
   map->slots[hole].hash = 0;
   map->slots[hole].node = NULL;

   /* the last in the list takes its place */
   last              = map->list[--map->count];
   map->list[place]  = last;
   GS_STABLE_MAP_NODE_PLACE(map, last) = place;
   gs_stable_map_node_give(map, node);
}
