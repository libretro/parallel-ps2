#include <stdlib.h>
#include <string.h>

#include "GSVulkanHeap.h"

#define GS_VK_HEAP_MIN_SPANS 16

static VkDeviceSize gs_vk_align_up(VkDeviceSize v, VkDeviceSize a)
{
   if (a <= 1)
      return v;
   return (v + (a - 1)) & ~(a - 1);
}

/* The memory types an allocation may come from, best first: those that
 * have the preferred flags as well, then the others that meet the
 * requirement - each group in the driver's own order, which is its order
 * of preference. Returns how many; *with_preferred is how many of them
 * are of the first group. */
static unsigned gs_vk_types(const gs_vk_heap_t *heap, uint32_t type_bits,
      VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
      unsigned char *types, unsigned *with_preferred)
{
   const VkMemoryPropertyFlags both = required | preferred;
   unsigned count = 0;
   unsigned i;

   /* AMD's device-coherent types are only allocated from with the
    * deviceCoherentMemory feature (VK_AMD_device_coherent_memory), which
    * the device is made without. */
   for (i = 0; i < heap->props.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; i++)
      if (heap->props.memoryTypes[i].propertyFlags
            & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD))
         type_bits &= ~(1u << i);

   if (preferred)
      for (i = 0; i < heap->props.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; i++)
         if ((type_bits & (1u << i))
               && (heap->props.memoryTypes[i].propertyFlags & both) == both)
            types[count++] = (unsigned char)i;
   *with_preferred = count;
   for (i = 0; i < heap->props.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; i++)
   {
      const VkMemoryPropertyFlags flags = heap->props.memoryTypes[i].propertyFlags;

      if (!(type_bits & (1u << i)) || (flags & required) != required)
         continue;
      if (preferred && (flags & both) == both)
         continue;      /* in the first group already */
      types[count++] = (unsigned char)i;
   }
   return count;
}

/* What one block of a memory type is: the heap's block size, but no more
 * than an eighth of the driver heap that type is in - a block sized for
 * the largest heap can be most of a small one, such as a 256 MB BAR
 * window - and no less than what is asked for. */
static VkDeviceSize gs_vk_block_size(const gs_vk_heap_t *heap, unsigned type,
      VkDeviceSize need)
{
   const unsigned heap_index = heap->props.memoryTypes[type].heapIndex;
   VkDeviceSize size         = heap->block_size;

   if (heap_index < heap->props.memoryHeapCount)
   {
      const VkDeviceSize eighth = heap->props.memoryHeaps[heap_index].size / 8u;

      if (eighth && eighth < size)
         size = eighth;
   }
   if (size < need)
      size = need;
   return gs_vk_align_up(size, (VkDeviceSize)(64u * 1024u));
}

/* The free spans of a block are kept sorted by offset and never adjacent:
 * a free then finds its place with a binary search and merges with the
 * span on either side, where it used to compare every span with every
 * other. Makes room for one more span at index at. */
static int gs_vk_block_insert_span(gs_vk_block_t *b, unsigned at,
      VkDeviceSize offset, VkDeviceSize size)
{
   if (b->free_count == b->free_capacity)
   {
      const unsigned cap = b->free_capacity ? b->free_capacity * 2u : GS_VK_HEAP_MIN_SPANS;
      gs_vk_span_t *grown = (gs_vk_span_t*)realloc(b->free_spans, cap * sizeof(gs_vk_span_t));
      if (!grown)
         return 0;
      b->free_spans    = grown;
      b->free_capacity = cap;
   }
   if (at < b->free_count)
      memmove(&b->free_spans[at + 1], &b->free_spans[at],
            (b->free_count - at) * sizeof(gs_vk_span_t));
   b->free_spans[at].offset = offset;
   b->free_spans[at].size   = size;
   b->free_count++;
   return 1;
}

static void gs_vk_block_remove_span(gs_vk_block_t *b, unsigned at)
{
   b->free_count--;
   if (at < b->free_count)
      memmove(&b->free_spans[at], &b->free_spans[at + 1],
            (b->free_count - at) * sizeof(gs_vk_span_t));
}

/* One block from the driver, whole and mapped if it can be. need_map: it
 * is for something written through its address, and a block that cannot
 * be mapped is no use to it - it goes back to the driver and this fails.
 * Returns the block's index, or -1 with heap->last_error saying why. */
static int gs_vk_heap_add_block(gs_vk_heap_t *heap, unsigned type, unsigned linear,
      VkDeviceSize size, int need_map)
{
   VkMemoryAllocateInfo mai;
   gs_vk_block_t *b;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   void *mapped          = NULL;
   VkResult result;
   unsigned slot;

   /* A slot trim left empty, if there is one. */
   for (slot = 0; slot < heap->block_count; slot++)
      if (!heap->blocks[slot].memory)
         break;

   /* The ceiling. Without one the heap will hand out blocks until the
    * card is gone. A heap that refuses is a renderer with a missing
    * texture and a line in the log; a heap that does not is a dead
    * machine. */
   if (slot >= GS_VK_HEAP_MAX_BLOCKS
         || (heap->max_bytes && heap->bytes_reserved + size > heap->max_bytes))
   {
      heap->last_error = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      return -1;
   }

   memset(&mai, 0, sizeof(mai));
   mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   mai.allocationSize  = size;
   mai.memoryTypeIndex = type;

   result = heap->fns.allocate_memory(heap->device, &mai, NULL, &memory);
   if (result != VK_SUCCESS)
   {
      heap->last_error = result;
      return -1;
   }

   /* Mapped once, for the life of the block: nothing maps per use. */
   if (heap->props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
   {
      result = heap->fns.map_memory(heap->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
      if (result != VK_SUCCESS)
      {
         mapped = NULL;
         if (need_map)
         {
            heap->fns.free_memory(heap->device, memory, NULL);
            heap->last_error = result;
            return -1;
         }
         /* Kept, for what never looks at its address. */
      }
   }

   b = &heap->blocks[slot];
   memset(b, 0, sizeof(*b));
   b->memory   = memory;
   b->mapped   = mapped;
   b->size     = size;
   b->max_free = size;
   b->type     = type;
   b->linear   = linear;

   if (!gs_vk_block_insert_span(b, 0, 0, size))
   {
      if (mapped)
         heap->fns.unmap_memory(heap->device, memory);
      heap->fns.free_memory(heap->device, memory, NULL);
      memset(b, 0, sizeof(*b));
      heap->last_error = VK_ERROR_OUT_OF_HOST_MEMORY;
      return -1;
   }

   if (slot == heap->block_count)
      heap->block_count++;
   heap->bytes_reserved += size;
   return (int)slot;
}

int gs_vk_heap_init(gs_vk_heap_t *heap, VkDevice device,
      const VkPhysicalDeviceMemoryProperties *props,
      const gs_vk_heap_fns_t *fns, VkDeviceSize block_size,
      VkDeviceSize non_coherent_atom_size, VkDeviceSize max_bytes)
{
   if (!heap || !props || !fns || !fns->allocate_memory)
      return 0;

   memset(heap, 0, sizeof(*heap));
   heap->device     = device;
   heap->props      = *props;
   heap->fns        = *fns;
   heap->block_size = block_size ? block_size : (64u * 1024u * 1024u);
   heap->atom_size  = non_coherent_atom_size ? non_coherent_atom_size : 256u;
   heap->max_bytes  = max_bytes;
   return 1;
}

VkDeviceSize gs_vk_heap_ceiling(VkDeviceSize working_set, VkDeviceSize block,
      VkDeviceSize fixed)
{
   VkDeviceSize ceiling = working_set * 16u;

   if (ceiling < block * 4u)
      ceiling = block * 4u;
   return ceiling + gs_vk_align_up(fixed, block) + block;
}

void gs_vk_heap_shutdown(gs_vk_heap_t *heap)
{
   unsigned i;

   for (i = 0; i < heap->block_count; i++)
   {
      gs_vk_block_t *b = &heap->blocks[i];
      if (!b->memory)
         continue;
      if (b->mapped)
         heap->fns.unmap_memory(heap->device, b->memory);
      heap->fns.free_memory(heap->device, b->memory, NULL);
      if (b->free_spans)
         free(b->free_spans);
   }
   heap->block_count    = 0;
   heap->bytes_reserved = 0;
   heap->bytes_used     = 0;
   heap->bytes_host     = 0;
   heap->bytes_device   = 0;
}

unsigned gs_vk_heap_reserve(gs_vk_heap_t *heap, uint32_t type_bits,
      VkMemoryPropertyFlags flags, int linear, unsigned blocks)
{
   const int need_map = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
   unsigned char types[VK_MAX_MEMORY_TYPES];
   unsigned with_preferred;
   const unsigned count = gs_vk_types(heap, type_bits, flags, 0, types, &with_preferred);
   unsigned made        = 0;
   unsigned i;

   /* The first type that will do; whatever the driver would not give of
    * it comes from the next. */
   for (i = 0; i < count && made < blocks; i++)
      while (made < blocks && gs_vk_heap_add_block(heap, types[i], linear ? 1u : 0u,
               gs_vk_block_size(heap, types[i], 0), need_map) >= 0)
         made++;
   return made;
}

/* The lowest span of the block that fits, honouring the alignment the
 * requirement asks for. The leftovers on either side stay free. */
static int gs_vk_block_alloc(gs_vk_heap_t *heap, gs_vk_block_t *b, unsigned index,
      VkDeviceSize size, VkDeviceSize align, gs_vk_alloc_t *out)
{
   VkDeviceSize largest = 0;
   unsigned i;

   /* No span of this block is larger than max_free, so a block that
    * cannot hold this is passed over without its spans being read. */
   if (size > b->max_free)
      return 0;

   for (i = 0; i < b->free_count; i++)
   {
      const VkDeviceSize start     = b->free_spans[i].offset;
      const VkDeviceSize span_size = b->free_spans[i].size;
      const VkDeviceSize aligned   = gs_vk_align_up(start, align);
      const VkDeviceSize head      = aligned - start;
      VkDeviceSize tail;

      if (span_size > largest)
         largest = span_size;
      if (head + size > span_size)
         continue;

      tail = span_size - head - size;

      if (head && tail)
      {
         /* Split: the head stays where it is, the tail goes in after it. */
         if (!gs_vk_block_insert_span(b, i + 1, aligned + size, tail))
            return 0;
         b->free_spans[i].size = head;
      }
      else if (head)
         b->free_spans[i].size = head;
      else if (tail)
      {
         b->free_spans[i].offset = aligned + size;
         b->free_spans[i].size   = tail;
      }
      else
         gs_vk_block_remove_span(b, i);

      b->used          += size;
      heap->bytes_used += size;
      if (heap->props.memoryTypes[b->type].propertyFlags
            & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
         heap->bytes_host += size;
      else
         heap->bytes_device += size;

      out->memory = b->memory;
      out->offset = aligned;
      out->size   = size;
      out->mapped = b->mapped ? (void*)((unsigned char*)b->mapped + aligned) : NULL;
      out->block  = index;
      out->type   = b->type;
      return 1;
   }

   /* Every span was looked at and none fits: now the bound is exact. */
   b->max_free = largest;
   return 0;
}

unsigned gs_vk_heap_trim(gs_vk_heap_t *heap)
{
   unsigned freed = 0;
   unsigned i;

   for (i = 0; i < heap->block_count; i++)
   {
      gs_vk_block_t *b = &heap->blocks[i];

      if (!b->memory || b->used != 0)
         continue;

      if (b->mapped)
         heap->fns.unmap_memory(heap->device, b->memory);
      heap->fns.free_memory(heap->device, b->memory, NULL);
      if (b->free_spans)
         free(b->free_spans);

      heap->bytes_reserved -= b->size;
      freed++;

      /* A hole, not a gap closed up. Every live allocation carries the
       * index of its block, so the array must not be compacted - moving
       * the last block into this slot would leave those allocations
       * pointing at someone else's memory. The slot is reused by the
       * next block instead. */
      memset(b, 0, sizeof(*b));
   }
   return freed;
}

/* An allocation from the blocks there are of one memory type and kind. */
static int gs_vk_heap_alloc_existing(gs_vk_heap_t *heap, const VkMemoryRequirements *req,
      unsigned type, unsigned linear, int need_map, gs_vk_alloc_t *out)
{
   unsigned i;

   for (i = 0; i < heap->block_count; i++)
   {
      gs_vk_block_t *b = &heap->blocks[i];

      if (!b->memory || b->type != type || b->linear != linear)
         continue;
      if (need_map && !b->mapped)
         continue;
      if (gs_vk_block_alloc(heap, b, i, req->size, req->alignment, out))
         return 1;
   }
   return 0;
}

/* An allocation from a new block of one memory type: the only thing here
 * that asks the driver for memory, and the reason gs_vk_heap_reserve
 * exists - reserve enough at startup and this never runs. */
static int gs_vk_heap_alloc_new(gs_vk_heap_t *heap, const VkMemoryRequirements *req,
      unsigned type, unsigned linear, int need_map, gs_vk_alloc_t *out)
{
   const int slot = gs_vk_heap_add_block(heap, type, linear,
         gs_vk_block_size(heap, type, req->size), need_map);

   if (slot < 0)
      return 0;
   return gs_vk_block_alloc(heap, &heap->blocks[slot], (unsigned)slot,
         req->size, req->alignment, out);
}

int gs_vk_heap_alloc(gs_vk_heap_t *heap, const VkMemoryRequirements *req,
      VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
      int linear, gs_vk_alloc_t *out)
{
   const unsigned kind = linear ? 1u : 0u;
   /* Asked for as a requirement, the memory is written through its
    * address: the allocation is mapped, or it fails. */
   const int need_map  = (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
   unsigned char types[VK_MAX_MEMORY_TYPES];
   unsigned with_preferred;
   const unsigned count = gs_vk_types(heap, req->memoryTypeBits, required, preferred,
         types, &with_preferred);
   unsigned first = 0;
   unsigned end   = with_preferred;
   unsigned i;

   if (!count)
   {
      heap->last_error = VK_ERROR_FEATURE_NOT_PRESENT;
      return 0;
   }

   /* preferred is a preference. A type that has it may not exist, and
    * where it does the driver may have none of it left long before the
    * device runs out: the device-local host-visible type is the PCI BAR
    * window on discrete cards without resizable BAR, 256 MB or less and
    * shared with the driver and the frontend. So: every type that has it,
    * in the blocks there are and then in a new one; then, the same way,
    * every type that only meets the requirement - whatever order the
    * driver lists them in. */
   for (;;)
   {
      for (i = first; i < end; i++)
         if (gs_vk_heap_alloc_existing(heap, req, types[i], kind, need_map, out))
            return 1;
      for (i = first; i < end; i++)
         if (gs_vk_heap_alloc_new(heap, req, types[i], kind, need_map, out))
            return 1;
      if (end == count)
         break;
      first = end;
      end   = count;
   }

   /* No room and no new block anywhere. Empty blocks are given back - a
    * run that filled the ceiling with upload blocks has nothing for an
    * image even with most of it free - and every type is asked once
    * more. */
   if (gs_vk_heap_trim(heap))
      for (i = 0; i < count; i++)
         if (gs_vk_heap_alloc_new(heap, req, types[i], kind, need_map, out))
            return 1;
   return 0;
}

void gs_vk_heap_free(gs_vk_heap_t *heap, const gs_vk_alloc_t *alloc)
{
   gs_vk_block_t *b;
   VkDeviceSize offset;
   VkDeviceSize size;
   unsigned lo;
   unsigned hi;
   int joins_prev;
   int joins_next;

   if (!alloc || alloc->memory == VK_NULL_HANDLE || alloc->block >= heap->block_count)
      return;

   /* The block it came from, and no other: after a trim the slot can hold
    * a new block, and a span of the old one merged into it would be
    * handed out twice. */
   b = &heap->blocks[alloc->block];
   if (b->memory != alloc->memory || alloc->offset > b->size
         || alloc->size > b->size - alloc->offset || !alloc->size)
      return;

   /* Where it goes in the sorted spans: the first that starts after it. */
   offset = alloc->offset;
   size   = alloc->size;
   lo     = 0;
   hi     = b->free_count;
   while (lo < hi)
   {
      const unsigned mid = lo + (hi - lo) / 2u;
      if (b->free_spans[mid].offset < offset)
         lo = mid + 1;
      else
         hi = mid;
   }

   /* Memory that is already free is not freed again: a second free of one
    * allocation would put its span on the list twice, and two resources
    * would then be bound to the same memory. */
   if ((lo > 0 && b->free_spans[lo - 1].offset + b->free_spans[lo - 1].size > offset)
         || (lo < b->free_count && offset + size > b->free_spans[lo].offset))
   {
      heap->bad_frees++;
      return;
   }
   if (b->used >= alloc->size)
      b->used -= alloc->size;
   if (heap->bytes_used >= alloc->size)
      heap->bytes_used -= alloc->size;
   if (heap->props.memoryTypes[b->type].propertyFlags
         & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
   {
      if (heap->bytes_host >= alloc->size)
         heap->bytes_host -= alloc->size;
   }
   else if (heap->bytes_device >= alloc->size)
      heap->bytes_device -= alloc->size;

   /* Joined to whichever neighbours it touches, so a block does not turn
    * into a thousand unusable slivers over a run. */
   joins_prev = lo > 0
      && b->free_spans[lo - 1].offset + b->free_spans[lo - 1].size == offset;
   joins_next = lo < b->free_count && offset + size == b->free_spans[lo].offset;

   if (joins_prev && joins_next)
   {
      b->free_spans[lo - 1].size += size + b->free_spans[lo].size;
      size = b->free_spans[lo - 1].size;
      gs_vk_block_remove_span(b, lo);
   }
   else if (joins_prev)
   {
      b->free_spans[lo - 1].size += size;
      size = b->free_spans[lo - 1].size;
   }
   else if (joins_next)
   {
      b->free_spans[lo].offset = offset;
      b->free_spans[lo].size  += size;
      size = b->free_spans[lo].size;
   }
   else if (!gs_vk_block_insert_span(b, lo, offset, size))
      return;

   if (size > b->max_free)
      b->max_free = size;
}

/* A flush has to name a range that starts and ends on
 * nonCoherentAtomSize, so it is widened to one. Widening is safe: the
 * block around it is ours either way. */
static void gs_vk_heap_range(gs_vk_heap_t *heap, const gs_vk_alloc_t *alloc,
      VkMappedMemoryRange *range)
{
   const VkDeviceSize atom  = heap->atom_size;
   const VkDeviceSize begin = alloc->offset & ~(atom - 1);
   VkDeviceSize end         = gs_vk_align_up(alloc->offset + alloc->size, atom);
   const VkDeviceSize block = heap->blocks[alloc->block].size;

   if (end > block)
      end = block;

   memset(range, 0, sizeof(*range));
   range->sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
   range->memory = alloc->memory;
   range->offset = begin;
   range->size   = end - begin;
}

void gs_vk_heap_flush(gs_vk_heap_t *heap, const gs_vk_alloc_t *alloc)
{
   VkMappedMemoryRange range;

   if (!alloc->mapped || !heap->fns.flush_ranges)
      return;
   if (heap->props.memoryTypes[alloc->type].propertyFlags
         & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
      return;

   gs_vk_heap_range(heap, alloc, &range);
   heap->fns.flush_ranges(heap->device, 1, &range);
}

void gs_vk_heap_invalidate(gs_vk_heap_t *heap, const gs_vk_alloc_t *alloc)
{
   VkMappedMemoryRange range;

   if (!alloc->mapped || !heap->fns.invalidate_ranges)
      return;
   if (heap->props.memoryTypes[alloc->type].propertyFlags
         & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
      return;

   gs_vk_heap_range(heap, alloc, &range);
   heap->fns.invalidate_ranges(heap->device, 1, &range);
}
