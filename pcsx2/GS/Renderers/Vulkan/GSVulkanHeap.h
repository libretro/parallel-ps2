/*
 * Vulkan device memory, allocated in blocks at startup and suballocated
 * from there. In C.
 *
 * What it replaces is the Vulkan Memory Allocator, which is a good
 * library and the wrong shape for this. VMA decides when to ask the
 * driver for a VkDeviceMemory and how big, and those decisions are what
 * put this renderer on the floor: a dedicated allocation per render
 * target until the device ran out of allocation handles, blocks grown
 * on demand in the middle of a frame, and memory freed and re-taken
 * several times a second because the code above it asked for that.
 *
 * Here the rule is in one place and it is simple. Blocks are taken from
 * the driver, are large, and are never given back while the heap lives.
 * An allocation is an offset inside a block, the lowest free span that
 * fits; the free spans are kept sorted, so freeing is a binary search
 * and a merge with the span on either side. A host-visible block is mapped once when it is taken and
 * stays mapped, so nothing maps or unmaps per use either.
 *
 * Reserve at startup with gs_vk_heap_reserve and the frames that follow
 * ask the driver for nothing at all.
 *
 * Not thread safe; the GS is one thread.
 */

#ifndef GS_VULKAN_HEAP_H
#define GS_VULKAN_HEAP_H

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The entry points the heap uses, passed in rather than linked against,
 * because the loader that has them is the caller's. */
typedef struct gs_vk_heap_fns
{
   PFN_vkAllocateMemory              allocate_memory;
   PFN_vkFreeMemory                  free_memory;
   PFN_vkMapMemory                   map_memory;
   PFN_vkUnmapMemory                 unmap_memory;
   PFN_vkFlushMappedMemoryRanges     flush_ranges;
   PFN_vkInvalidateMappedMemoryRanges invalidate_ranges;
} gs_vk_heap_fns_t;

/* Where something ended up. offset is inside memory, not inside the
 * heap; mapped already has offset added, or is NULL for device-local
 * memory. */
typedef struct gs_vk_alloc
{
   VkDeviceMemory memory;
   VkDeviceSize   offset;
   VkDeviceSize   size;
   void          *mapped;
   unsigned       block;
   unsigned       type;
} gs_vk_alloc_t;

/* A span of free memory inside a block. */
typedef struct gs_vk_span
{
   VkDeviceSize offset;
   VkDeviceSize size;
} gs_vk_span_t;

typedef struct gs_vk_block
{
   VkDeviceMemory memory;
   void          *mapped;      /* whole block, or NULL       */
   VkDeviceSize   size;
   VkDeviceSize   used;
   /* No free span is larger than this. Exact after a search that found
    * nothing, an upper bound otherwise. */
   VkDeviceSize   max_free;
   gs_vk_span_t  *free_spans;  /* sorted by offset, never adjacent */
   unsigned       free_count;
   unsigned       free_capacity;
   unsigned       type;
   /* Buffers, or images. The two never share a block: the driver may
    * not have a buffer and an image within bufferImageGranularity of
    * each other in one VkDeviceMemory, and keeping them in blocks of
    * their own is that rule kept without padding anything. */
   unsigned       linear;
   /* A priority class (gs_vk_alloc_request), blocks of one class never
    * holding another's; and whether the block is one allocation's alone,
    * made for it and given back when it is freed. */
   unsigned char  priority;
   unsigned char  dedicated;
   /* free_spans was grown past the heap's own and is the block's to free. */
   unsigned char  spans_owned;
} gs_vk_block_t;

/* Enough that the ceiling is what stops the heap growing, never this
 * array: 64 MB blocks against the 4 GB ceiling at 8x is 64 blocks
 * exactly, and hitting the array first would refuse an allocation the
 * budget allows. */
#define GS_VK_HEAP_MAX_BLOCKS 256

typedef struct gs_vk_heap
{
   VkDevice                         device;
   gs_vk_heap_fns_t                 fns;
   VkPhysicalDeviceMemoryProperties props;
   VkDeviceSize                     block_size;
   VkDeviceSize                     atom_size;      /* nonCoherentAtomSize */
   gs_vk_block_t                    blocks[GS_VK_HEAP_MAX_BLOCKS];
   unsigned                         block_count;
   VkDeviceSize                     max_bytes;      /* never asks past this */
   VkDeviceSize                     bytes_reserved; /* asked of the driver */
   VkDeviceSize                     bytes_used;     /* handed out          */

   /* The same total split by what the memory is for, because the total
    * alone cannot answer the question that matters when an allocation
    * fails: 3199 MB used against a texture cache holding 359 MB says
    * something else has it, and these two lines say which. Uploads are
    * host-visible, images and targets are device-local. */
   VkDeviceSize                     bytes_host;
   VkDeviceSize                     bytes_device;
   VkResult                         last_error;     /* why the last block could not be had */
   VkDeviceSize                     bad_frees;      /* frees of memory already free, refused */

   /* Off unless the caller sets them after gs_vk_heap_init. heap_limit:
    * no block is taken that would put a memory heap past it (0: no
    * limit), for a heap that is small and shared, such as the PCI BAR
    * window. use_priority: blocks carry VkMemoryPriorityAllocateInfoEXT
    * with their class's priority (the device has memoryPriority).
    * linear_flags: VkMemoryAllocateFlags for buffer blocks, such as
    * VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT. */
   VkDeviceSize                     heap_limit[VK_MAX_MEMORY_HEAPS];
   /* Each block slot's first spans, taken once by gs_vk_heap_init: a
    * block taken in play costs the driver call and no allocation here. */
   gs_vk_span_t                    *span_pool;
   VkDeviceSize                     heap_reserved[VK_MAX_MEMORY_HEAPS];
   int                              use_priority;
   VkMemoryAllocateFlags            linear_flags;
} gs_vk_heap_t;

/* What gs_vk_heap_alloc asks, and more. priority is a class, 0 to 3:
 * 0 none, then low, middle and high (0, 0.5 and 1 to the driver); it
 * keeps allocations of different classes in different blocks and means
 * nothing without use_priority. A dedicated image or buffer gets memory
 * of its own, exactly req.size, with VkMemoryDedicatedAllocateInfo; at
 * most one of the two is set. */
typedef struct gs_vk_alloc_request
{
   VkMemoryRequirements  req;
   VkMemoryPropertyFlags required;
   VkMemoryPropertyFlags preferred;
   int                   linear;
   unsigned              priority;
   VkImage               dedicated_image;
   VkBuffer              dedicated_buffer;
} gs_vk_alloc_request_t;

/* block_size is what one VkDeviceMemory is; anything larger than it gets
 * a block of its own. Returns 0 on failure. */
int  gs_vk_heap_init(gs_vk_heap_t *heap, VkDevice device,
      const VkPhysicalDeviceMemoryProperties *props,
      const gs_vk_heap_fns_t *fns, VkDeviceSize block_size,
      VkDeviceSize non_coherent_atom_size, VkDeviceSize max_bytes);

void gs_vk_heap_shutdown(gs_vk_heap_t *heap);

/* The ceiling for a renderer whose working set - everything the console
 * holds at once, at the upscale - is working_set, with fixed bytes of
 * buffers made once at startup. Sixteen working sets, never under four
 * blocks, bound what textures and targets may take; the fixed buffers
 * come on top of that, rounded up to blocks, and one more block for the
 * part of a block each memory type they land in can leave unused. */
VkDeviceSize gs_vk_heap_ceiling(VkDeviceSize working_set, VkDeviceSize block,
      VkDeviceSize fixed);

/* Takes blocks from the driver now, so that later allocations of this
 * kind are offsets. type_bits and flags are as a VkMemoryRequirements
 * would give, linear as for gs_vk_heap_alloc. Every type with flags is
 * tried in turn; host-visible blocks are taken only if they map. Returns
 * the number of blocks taken. */
unsigned gs_vk_heap_reserve(gs_vk_heap_t *heap, uint32_t type_bits,
      VkMemoryPropertyFlags flags, int linear, unsigned blocks);

/* Finds room for something with these requirements. required is what the
 * memory must be; preferred is tried first and dropped when no memory
 * type has it or the driver has none of it left. Every memory type that
 * will do is tried, in that order. linear is non-zero for a buffer or an
 * image with linear tiling, zero for any other image.
 *
 * With VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT in required, out->mapped is an
 * address: memory that could not be mapped is not handed out. Where the
 * bit is only in preferred, or not asked for, out->mapped may be NULL.
 *
 * Returns 0 when there is no room and no block could be taken;
 * heap->last_error is then what the driver last said. */
int  gs_vk_heap_alloc(gs_vk_heap_t *heap, const VkMemoryRequirements *req,
      VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
      int linear, gs_vk_alloc_t *out);

/* gs_vk_heap_alloc with a priority class or a dedicated object. */
int  gs_vk_heap_alloc_request(gs_vk_heap_t *heap, const gs_vk_alloc_request_t *request,
      gs_vk_alloc_t *out);

/* A free of memory that is already free, or of an allocation whose block
 * has since been given back, changes nothing; the first kind is counted
 * in bad_frees. */
void gs_vk_heap_free(gs_vk_heap_t *heap, const gs_vk_alloc_t *alloc);

/* Gives empty blocks back to the driver. Blocks are per memory type and
 * are kept for the life of the heap, so a run that filled the ceiling
 * with upload blocks has nothing left for an image even when most of it
 * is free - "4096 MB reserved, 2687 MB used" and no room for a 128x64
 * texture. paraLLEl-GS meets the same wall and answers it the same way:
 * past the high-water mark it drops the slab and starts over, with the
 * comment that there is no need to be more clever. Returns how many
 * blocks went back. */
unsigned gs_vk_heap_trim(gs_vk_heap_t *heap);

/* For host-visible memory that is not coherent. Both round to
 * nonCoherentAtomSize themselves. */
void gs_vk_heap_flush(gs_vk_heap_t *heap, const gs_vk_alloc_t *alloc);
void gs_vk_heap_invalidate(gs_vk_heap_t *heap, const gs_vk_alloc_t *alloc);

#ifdef __cplusplus
}
#endif

#endif
