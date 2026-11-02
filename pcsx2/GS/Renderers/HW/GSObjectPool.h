/*
 * A fixed pool of equally sized slots, in C.
 *
 * The texture cache makes and destroys sources and targets inside a
 * draw. Each one through the allocator is a cost that varies frame to
 * frame, and it is the reason a heap block header sits next to every
 * one of them. A pool is one allocation at startup and an index off a
 * free list per take and give: no allocator, no lock, no variation.
 *
 * Not thread safe and not meant to be: the GS runs on one thread.
 *
 * Past the pool's size, gs_object_pool_take returns NULL and the caller
 * falls back to the allocator, so a pool size is a budget rather than a
 * limit.
 */

#ifndef GS_OBJECT_POOL_H
#define GS_OBJECT_POOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Slots are aligned to this, and so is anything the caller falls back to
 * the allocator for. Sixty-four because the GS compares and copies with
 * aligned SSE loads - GSVector4i::compare64 and friends cast a pointer
 * and index it - and the buffers those run over live inside pooled
 * objects. Sixteen would do for SSE; sixty-four is the cache line and is
 * what the allocations these pools replaced asked for. */
#define GS_OBJECT_POOL_ALIGN 64

/* Built with an address sanitizer: the pool hands out nothing. */
#if defined(__SANITIZE_ADDRESS__)
#define GS_OBJECT_POOL_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GS_OBJECT_POOL_SANITIZED 1
#endif
#endif
#ifndef GS_OBJECT_POOL_SANITIZED
#define GS_OBJECT_POOL_SANITIZED 0
#endif

typedef struct gs_object_pool
{
   unsigned char  *slots;        /* one allocation, count * stride     */
   unsigned short *free_indices; /* the slots nobody holds             */
   /* One byte a slot: out with a caller, or on the free list. What
    * makes a second free of the same object detectable instead of
    * fatal. */
   unsigned char  *in_use;
   size_t          stride;       /* bytes per slot, 32-byte aligned    */
   unsigned        count;        /* how many slots there are           */
   unsigned        free_count;   /* how many of them are free          */
   unsigned        double_frees; /* callers that freed twice           */
} gs_object_pool_t;

/* Sizes the pool for objects of `size` bytes and allocates it. Returns 0
 * if the allocation failed, in which case take always returns NULL and
 * the caller uses the allocator for everything. Calling it again on a
 * pool that is already set up does nothing. Cold: once per pool. */
int  gs_object_pool_init(gs_object_pool_t *pool, size_t size, unsigned count);

void gs_object_pool_free(gs_object_pool_t *pool);

/* Take and give are here rather than in the .c on purpose: they are what
 * a draw pays, they are a handful of instructions each, and out of line
 * they would be a call the C++ they replaced did not make. Static in the
 * header is what C89 has instead of inline; the generated code for both
 * is the same as the version this replaced (tests/gspool checks the
 * behaviour, and the commit that added them compares the assembly). */

/* A slot, or NULL when the pool is empty or `size` does not fit one. */
static void *gs_object_pool_take(gs_object_pool_t *pool, size_t size)
{
   unsigned index;

   if (!pool->free_count || size > pool->stride)
      return NULL;
   index = pool->free_indices[--pool->free_count];
   if (pool->in_use)
      pool->in_use[index] = 1;
   return pool->slots + (size_t)index * pool->stride;
}

/* Returns a slot to the pool. Tells the caller whether `p` was one:
 * nonzero if it was and has been taken back, zero if it came from
 * somewhere else and is still theirs to free. */
static int gs_object_pool_give(gs_object_pool_t *pool, void *p)
{
   size_t off;
   unsigned index;

   if (!pool->slots)
      return 0;
   if ((unsigned char*)p < pool->slots)
      return 0;
   off = (size_t)((unsigned char*)p - pool->slots);
   if (off >= pool->stride * pool->count)
      return 0;

   index = (unsigned)(off / pool->stride);

   /* Already back. Something freed this object twice, and putting the
    * slot on the free list again hands it to two callers at once - a
    * live object overwritten by an unrelated one, which is the same
    * address destroyed over and over in the log and then a lockup.
    *
    * paraLLEl-GS does not need this: it owns images through a refcounted
    * handle, so a second free is not something a call site can express.
    * Sources and targets here are raw pointers freed by whoever
    * remembers to. Until that is a handle, the pool refuses. */
   if (pool->in_use && !pool->in_use[index])
   {
      pool->double_frees++;
      return 1;
   }

   if (pool->in_use)
      pool->in_use[index] = 0;

   if (pool->free_count < pool->count)
      pool->free_indices[pool->free_count++] = (unsigned short)index;
   return 1;
}

/* --- node pools --------------------------------------------------------
 * The texture cache's hash maps and sets allocate a node per entry and
 * give it back on erase: a texture or palette made and dropped every
 * frame was a heap allocation and a free every frame. A node pool keeps
 * nodes of one size on a free list, linked through their first word, and
 * carves them out of slabs: filling a cache costs a heap call a slab
 * rather than a node, and nodes made together sit together.
 *
 * Take and give are the inlined part and are three and two instructions
 * past the call; refilling is out of line and cold. GS thread only, like
 * the object pool. With an address sanitizer the C++ side does not use
 * these (see GSNodeAllocator), so the sanitizer sees every node. */
typedef struct gs_node_pool
{
   void                *free_head; /* first free node; its first word links on */
   unsigned char       *bump;      /* next never-used node of the newest slab */
   unsigned char       *bump_end;  /* end of the newest slab                  */
   void                *slabs;     /* slab chain, linked through first words  */
   size_t               stride;    /* node size, a multiple of its alignment  */
   size_t               capacity;  /* nodes in all the slabs                  */
   struct gs_node_pool *next_pool; /* every pool that has a slab              */
} gs_node_pool_t;

/* A pool for nodes of `size` bytes aligned to `align` (a power of two,
 * at most GS_OBJECT_POOL_ALIGN); a constant expression, so a pool can be
 * initialised statically. */
#define GS_NODE_POOL_STRIDE(size, align) \
   ((((size) < sizeof(void*) ? sizeof(void*) : (size)) + ((align) - 1)) & ~((size_t)(align) - 1))
#define GS_NODE_POOL_INIT(size, align) { NULL, NULL, NULL, NULL, GS_NODE_POOL_STRIDE(size, align), 0, NULL }

/* Cold, out of line: the free list is empty. The next node the newest
 * slab has never handed out, or a new slab's first. Out of memory it
 * aborts, as operator new does without exceptions. */
void *gs_node_pool_refill(gs_node_pool_t *pool);

/* Frees the slabs of every pool whose nodes are all back. A pool with
 * any still out keeps its slabs. Cold: when the texture cache goes. */
void gs_node_pools_release(void);

static void *gs_node_pool_take(gs_node_pool_t *pool)
{
   void *node = pool->free_head;
   if (!node)
      return gs_node_pool_refill(pool);
   pool->free_head = *(void**)node;
   return node;
}

static void gs_node_pool_give(gs_node_pool_t *pool, void *node)
{
   *(void**)node   = pool->free_head;
   pool->free_head = node;
}

#ifdef __cplusplus
}
#endif

#endif
