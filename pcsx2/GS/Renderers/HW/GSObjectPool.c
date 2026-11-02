#include <stdlib.h>
#include <memalign.h>

/* The header's take and give are static, so a translation unit that uses
 * neither - this one, which only sets the pool up - would warn about
 * them. Say they are meant to be there. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "GSObjectPool.h"

int gs_object_pool_init(gs_object_pool_t *pool, size_t size, unsigned count)
{
   unsigned i;
   size_t   stride;

   if (pool->slots)
      return 1;
   if (!count)
      return 0;
#if GS_OBJECT_POOL_SANITIZED
   /* An address sanitizer cannot see a slot used after it went back to
    * the pool, so a sanitizer build leaves every object to the
    * allocator, where it can. */
   return 0;
#endif

   /* Slots keep the alignment the objects ask for, so the address of
    * every one of them is as good as the allocator's. */
   stride = (size + (GS_OBJECT_POOL_ALIGN - 1)) & ~((size_t)GS_OBJECT_POOL_ALIGN - 1);

   pool->slots = (unsigned char*)memalign_alloc(GS_OBJECT_POOL_ALIGN, stride * count);
   if (!pool->slots)
      return 0;

   pool->free_indices = (unsigned short*)malloc(count * sizeof(unsigned short));
   if (!pool->free_indices)
   {
      memalign_free(pool->slots);
      pool->slots = NULL;
      return 0;
   }

   /* Zeroed: nothing is out with a caller yet. */
   pool->in_use = (unsigned char*)calloc(count, 1);
   if (!pool->in_use)
   {
      free(pool->free_indices);
      pool->free_indices = NULL;
      memalign_free(pool->slots);
      pool->slots = NULL;
      return 0;
   }
   pool->double_frees = 0;

   /* Handed out from the end, so a run of takes gets the low slots and
    * they stay near each other. */
   for (i = 0; i < count; i++)
      pool->free_indices[i] = (unsigned short)(count - 1 - i);

   pool->stride     = stride;
   pool->count      = count;
   pool->free_count = count;
   return 1;
}

void gs_object_pool_free(gs_object_pool_t *pool)
{
   if (pool->slots)
      memalign_free(pool->slots);
   if (pool->free_indices)
      free(pool->free_indices);
   if (pool->in_use)
      free(pool->in_use);
   pool->slots        = NULL;
   pool->free_indices = NULL;
   pool->in_use       = NULL;
   pool->stride       = 0;
   pool->count        = 0;
   pool->free_count   = 0;
}

/* --- node pools -------------------------------------------------------- */

/* A slab is this many bytes, the first GS_OBJECT_POOL_ALIGN of them the
 * link to the next slab, so nodes keep the slab's alignment. */
#define GS_NODE_SLAB_BYTES 16384

static gs_node_pool_t *gs_node_pools;

void *gs_node_pool_refill(gs_node_pool_t *pool)
{
   size_t         count;
   unsigned char *slab;
   unsigned char *node;

   /* Nodes are handed out from the newest slab in address order, each
    * touched first by whoever takes it: no pass linking a whole slab
    * onto the free list before any of it is used. */
   if (pool->bump != pool->bump_end)
   {
      node        = pool->bump;
      pool->bump += pool->stride;
      return node;
   }

   count = (GS_NODE_SLAB_BYTES - GS_OBJECT_POOL_ALIGN) / pool->stride;
   if (count < 8)
      count = 8;
   slab = (unsigned char*)memalign_alloc(GS_OBJECT_POOL_ALIGN,
         GS_OBJECT_POOL_ALIGN + count * pool->stride);
   /* Out of memory is what operator new does in a build without
    * exceptions: the end. Never NULL, so take needs no check after it. */
   if (!slab)
      abort();

   if (!pool->slabs)
   {
      pool->next_pool = gs_node_pools;
      gs_node_pools   = pool;
   }
   *(void**)slab   = pool->slabs;
   pool->slabs     = slab;
   pool->capacity += count;

   node           = slab + GS_OBJECT_POOL_ALIGN;
   pool->bump     = node + pool->stride;
   pool->bump_end = node + count * pool->stride;
   return node;
}

void gs_node_pools_release(void)
{
   gs_node_pool_t **link = &gs_node_pools;

   while (*link)
   {
      gs_node_pool_t *pool = *link;
      size_t          free_nodes = 0;
      void           *node;

      for (node = pool->free_head; node; node = *(void**)node)
         free_nodes++;
      /* Nodes the newest slab never handed out are not out either. */
      free_nodes += (size_t)(pool->bump_end - pool->bump) / pool->stride;
      if (free_nodes != pool->capacity)
      {
         link = &pool->next_pool;
         continue;
      }
      while (pool->slabs)
      {
         void *slab  = pool->slabs;
         pool->slabs = *(void**)slab;
         memalign_free(slab);
      }
      pool->free_head = NULL;
      pool->bump      = NULL;
      pool->bump_end  = NULL;
      pool->capacity  = 0;
      *link           = pool->next_pool;
      pool->next_pool = NULL;
   }
}
