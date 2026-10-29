/*
 * D3D12 heap memory, taken in blocks and suballocated from there, with
 * resources placed into it. In C.
 *
 * What it replaces is D3D12MemoryAllocator, for the same reasons VMA
 * went from the Vulkan backend: the library decides when to ask the
 * driver for a heap and how big, and those decisions are the ones that
 * put the renderer on the floor. Render targets were being created
 * committed, which is a heap apiece - the D3D12 spelling of Vulkan's
 * dedicated allocation, and the same mistake.
 *
 * The rule here is the same one and it is in one place. Blocks are
 * taken from the device, are large, and are not given back while the
 * heap lives unless a trim finds one empty. An allocation is an offset
 * inside a block, the lowest free span that fits; the free spans are
 * kept sorted, so freeing is a binary search and a merge with the span
 * on either side. The caller places a
 * resource at that offset with CreatePlacedResource.
 *
 * Unlike the Vulkan heap nothing is mapped here: D3D12 maps resources,
 * not heaps, so mapping stays with whoever created the resource.
 *
 * Blocks are segregated by heap type and heap flags together, because
 * on a resource-heap-tier-1 device a heap may hold buffers, or render
 * targets and depth stencils, or other textures, but not a mix. On tier
 * 2 the caller passes ALLOW_ALL_BUFFERS_AND_TEXTURES for everything and
 * they all land in the same blocks.
 *
 * The device calls are passed in rather than linked against, so this
 * file needs no d3d12.h and the arithmetic - the part that decides
 * whether a frame allocates - can be built and run without a GPU.
 *
 * Reserve at startup with gs_d3d12_heap_reserve and the frames that
 * follow ask the device for nothing at all.
 *
 * Not thread safe; the GS is one thread.
 */

#ifndef GS_D3D12_HEAP_H
#define GS_D3D12_HEAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The same values as D3D12_HEAP_TYPE, so a caller passes the enum
 * straight through. Repeated rather than included because this file
 * does not include d3d12.h. */
#define GS_D3D12_HEAP_TYPE_DEFAULT  1
#define GS_D3D12_HEAP_TYPE_UPLOAD   2
#define GS_D3D12_HEAP_TYPE_READBACK 3

/* What the heap needs of the device. create_heap returns nonzero and
 * fills out_heap on success. */
typedef struct gs_d3d12_heap_fns
{
   int  (*create_heap)(void *device, uint64_t size, uint32_t heap_type,
         uint32_t heap_flags, void **out_heap);
   void (*release_heap)(void *device, void *heap);
} gs_d3d12_heap_fns_t;

/* Where something ended up. heap is an ID3D12Heap* to place into, and
 * offset is inside it. */
typedef struct gs_d3d12_alloc
{
   void    *heap;
   uint64_t offset;
   uint64_t size;
   unsigned block;
   uint32_t heap_type;
   uint32_t heap_flags;
} gs_d3d12_alloc_t;

/* A span of free memory inside a block. */
typedef struct gs_d3d12_span
{
   uint64_t offset;
   uint64_t size;
} gs_d3d12_span_t;

typedef struct gs_d3d12_block
{
   void            *heap;         /* ID3D12Heap*, NULL when the slot is a hole */
   uint64_t         size;
   uint64_t         used;
   /* No free span is larger than this. Exact after a search that found
    * nothing, an upper bound otherwise. */
   uint64_t         max_free;
   gs_d3d12_span_t *free_spans;   /* sorted by offset, never adjacent */
   unsigned         free_count;
   unsigned         free_capacity;
   uint32_t         heap_type;
   uint32_t         heap_flags;
} gs_d3d12_block_t;

/* Enough that the ceiling is what stops the heap growing, never this
 * array, as on the Vulkan side: 64 MB blocks against a 4 GB ceiling is
 * 64 blocks, and hitting the array first would refuse an allocation the
 * budget allows. */
#define GS_D3D12_HEAP_MAX_BLOCKS 256

typedef struct gs_d3d12_heap
{
   void               *device;
   gs_d3d12_heap_fns_t fns;
   uint64_t            block_size;
   gs_d3d12_block_t    blocks[GS_D3D12_HEAP_MAX_BLOCKS];
   unsigned            block_count;
   uint64_t            max_bytes;      /* never asks past this, for
                                        * DEFAULT and for UPLOAD and
                                        * READBACK each */
   uint64_t            bytes_reserved; /* asked of the device  */
   uint64_t            bytes_reserved_host; /* the UPLOAD and READBACK part */
   uint64_t            bytes_used;     /* handed out           */

   /* The same total split by what the memory is for. The total alone
    * cannot answer the question that matters when an allocation fails -
    * memory used against what the texture cache admits to holding - and
    * these two lines say which side has it. */
   uint64_t            bytes_host;     /* UPLOAD and READBACK */
   uint64_t            bytes_device;   /* DEFAULT             */
   uint64_t            bad_frees;      /* frees of memory already free, refused */
} gs_d3d12_heap_t;

/* block_size is what one ID3D12Heap is; anything larger than it gets a
 * block of its own. Returns 0 on failure. */
int  gs_d3d12_heap_init(gs_d3d12_heap_t *heap, void *device,
      const gs_d3d12_heap_fns_t *fns, uint64_t block_size,
      uint64_t max_bytes);

void gs_d3d12_heap_shutdown(gs_d3d12_heap_t *heap);

/* Takes blocks from the device now, so that later allocations of this
 * kind are offsets. Returns the number of blocks taken. */
unsigned gs_d3d12_heap_reserve(gs_d3d12_heap_t *heap, uint32_t heap_type,
      uint32_t heap_flags, unsigned blocks);

/* Finds room for a resource of this size and alignment, as
 * GetResourceAllocationInfo gives them. Returns 0 if there is no room
 * and no block could be taken. */
int  gs_d3d12_heap_alloc(gs_d3d12_heap_t *heap, uint64_t size,
      uint64_t alignment, uint32_t heap_type, uint32_t heap_flags,
      gs_d3d12_alloc_t *out);

/* A free of memory that is already free, or of an allocation whose block
 * has since been given back, changes nothing; the first kind is counted
 * in bad_frees. */
void gs_d3d12_heap_free(gs_d3d12_heap_t *heap, const gs_d3d12_alloc_t *alloc);

/* Gives empty blocks back to the device. Blocks are per type and per
 * flags and are otherwise kept for the life of the heap, so a run that
 * filled the ceiling with upload blocks has nothing left for a target
 * even when most of it is free. Returns how many blocks went back. */
unsigned gs_d3d12_heap_trim(gs_d3d12_heap_t *heap);

#ifdef __cplusplus
}
#endif

#endif
