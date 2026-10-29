#include <stdlib.h>
#include <string.h>

#include "GSD3D12Heap.h"

#define GS_D3D12_HEAP_MIN_SPANS 16

static uint64_t gs_d3d12_align_up(uint64_t v, uint64_t a)
{
   if (a <= 1)
      return v;
   return (v + (a - 1)) & ~(a - 1);
}

/* DEFAULT is the card's own memory; UPLOAD and READBACK are the ones
 * the CPU can see. Only used to split the byte counters. */
static int gs_d3d12_type_is_host(uint32_t heap_type)
{
   return heap_type != GS_D3D12_HEAP_TYPE_DEFAULT;
}

/* The free spans of a block are kept sorted by offset and never adjacent:
 * a free finds its place with a binary search and merges with the span on
 * either side. Makes room for one more span at index at. */
static int gs_d3d12_block_insert_span(gs_d3d12_block_t *b, unsigned at,
      uint64_t offset, uint64_t size)
{
   if (b->free_count == b->free_capacity)
   {
      const unsigned cap     = b->free_capacity
         ? b->free_capacity * 2u : GS_D3D12_HEAP_MIN_SPANS;
      gs_d3d12_span_t *grown = (gs_d3d12_span_t*)realloc(b->free_spans,
            cap * sizeof(gs_d3d12_span_t));
      if (!grown)
         return 0;
      b->free_spans    = grown;
      b->free_capacity = cap;
   }
   if (at < b->free_count)
      memmove(&b->free_spans[at + 1], &b->free_spans[at],
            (b->free_count - at) * sizeof(gs_d3d12_span_t));
   b->free_spans[at].offset = offset;
   b->free_spans[at].size   = size;
   b->free_count++;
   return 1;
}

static void gs_d3d12_block_remove_span(gs_d3d12_block_t *b, unsigned at)
{
   b->free_count--;
   if (at < b->free_count)
      memmove(&b->free_spans[at], &b->free_spans[at + 1],
            (b->free_count - at) * sizeof(gs_d3d12_span_t));
}

/* One heap from the device, whole. */
static int gs_d3d12_heap_add_block(gs_d3d12_heap_t *heap, uint32_t heap_type,
      uint32_t heap_flags, uint64_t size)
{
   gs_d3d12_block_t *b;
   void             *block_heap = NULL;
   const int         host       = gs_d3d12_type_is_host(heap_type);
   const uint64_t    held       = host ? heap->bytes_reserved_host
      : heap->bytes_reserved - heap->bytes_reserved_host;
   unsigned          i_hole;
   unsigned          slot       = heap->block_count;

   /* A slot trim left empty, if there is one. */
   for (i_hole = 0; i_hole < heap->block_count; i_hole++)
   {
      if (!heap->blocks[i_hole].heap)
      {
         slot = i_hole;
         break;
      }
   }

   if (slot >= GS_D3D12_HEAP_MAX_BLOCKS)
      return 0;

   /* The ceiling. Without one the heap hands out blocks until the card
    * is gone. A heap that refuses is a renderer with a missing texture
    * and a line in the log; a heap that does not is a dead machine.
    * The card's memory and the memory the CPU sees each have it: the
    * upload rings alone fill two blocks, room the targets are owed. */
   if (heap->max_bytes && held + size > heap->max_bytes)
      return 0;

   if (!heap->fns.create_heap(heap->device, size, heap_type, heap_flags,
            &block_heap) || !block_heap)
      return 0;

   b = &heap->blocks[slot];
   memset(b, 0, sizeof(*b));
   b->heap       = block_heap;
   b->size       = size;
   b->max_free   = size;
   b->heap_type  = heap_type;
   b->heap_flags = heap_flags;

   if (!gs_d3d12_block_insert_span(b, 0, 0, size))
   {
      heap->fns.release_heap(heap->device, block_heap);
      memset(b, 0, sizeof(*b));
      return 0;
   }

   if (slot == heap->block_count)
      heap->block_count++;
   heap->bytes_reserved += size;
   if (host)
      heap->bytes_reserved_host += size;
   return 1;
}

int gs_d3d12_heap_init(gs_d3d12_heap_t *heap, void *device,
      const gs_d3d12_heap_fns_t *fns, uint64_t block_size,
      uint64_t max_bytes)
{
   if (!heap || !fns || !fns->create_heap || !fns->release_heap)
      return 0;

   memset(heap, 0, sizeof(*heap));
   heap->device     = device;
   heap->fns        = *fns;
   heap->block_size = block_size ? block_size : (64u * 1024u * 1024u);
   heap->max_bytes  = max_bytes;
   return 1;
}

void gs_d3d12_heap_shutdown(gs_d3d12_heap_t *heap)
{
   unsigned i;

   if (!heap)
      return;

   for (i = 0; i < heap->block_count; i++)
   {
      gs_d3d12_block_t *b = &heap->blocks[i];

      if (b->free_spans)
         free(b->free_spans);
      if (b->heap)
         heap->fns.release_heap(heap->device, b->heap);
   }

   memset(heap, 0, sizeof(*heap));
}

unsigned gs_d3d12_heap_reserve(gs_d3d12_heap_t *heap, uint32_t heap_type,
      uint32_t heap_flags, unsigned blocks)
{
   unsigned taken = 0;
   unsigned i;

   if (!heap)
      return 0;

   for (i = 0; i < blocks; i++)
   {
      if (!gs_d3d12_heap_add_block(heap, heap_type, heap_flags,
               heap->block_size))
         break;
      taken++;
   }
   return taken;
}

/* The lowest span of the block that fits, honouring the alignment asked
 * for. The leftovers on either side stay free. */
static int gs_d3d12_block_alloc(gs_d3d12_heap_t *heap, gs_d3d12_block_t *b,
      unsigned index, uint64_t size, uint64_t align, gs_d3d12_alloc_t *out)
{
   uint64_t largest = 0;
   unsigned i;

   /* No span of this block is larger than max_free, so a block that
    * cannot hold this is passed over without its spans being read. */
   if (size > b->max_free)
      return 0;

   for (i = 0; i < b->free_count; i++)
   {
      const uint64_t start     = b->free_spans[i].offset;
      const uint64_t span_size = b->free_spans[i].size;
      const uint64_t aligned   = gs_d3d12_align_up(start, align);
      const uint64_t head      = aligned - start;
      uint64_t       tail;

      if (span_size > largest)
         largest = span_size;
      if (head > span_size || size > span_size - head)
         continue;

      tail = span_size - head - size;

      if (head && tail)
      {
         /* Split: the head stays where it is, the tail goes in after it. */
         if (!gs_d3d12_block_insert_span(b, i + 1, aligned + size, tail))
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
         gs_d3d12_block_remove_span(b, i);

      b->used          += size;
      heap->bytes_used += size;
      if (gs_d3d12_type_is_host(b->heap_type))
         heap->bytes_host   += size;
      else
         heap->bytes_device += size;

      out->heap       = b->heap;
      out->offset     = aligned;
      out->size       = size;
      out->block      = index;
      out->heap_type  = b->heap_type;
      out->heap_flags = b->heap_flags;
      return 1;
   }

   /* Every span was looked at and none fits: now the bound is exact. */
   b->max_free = largest;
   return 0;
}

unsigned gs_d3d12_heap_trim(gs_d3d12_heap_t *heap)
{
   unsigned freed = 0;
   unsigned i;

   if (!heap)
      return 0;

   for (i = 0; i < heap->block_count; i++)
   {
      gs_d3d12_block_t *b = &heap->blocks[i];

      if (!b->heap || b->used != 0)
         continue;

      heap->fns.release_heap(heap->device, b->heap);
      if (b->free_spans)
         free(b->free_spans);

      heap->bytes_reserved -= b->size;
      if (gs_d3d12_type_is_host(b->heap_type))
         heap->bytes_reserved_host -= b->size;
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

int gs_d3d12_heap_alloc(gs_d3d12_heap_t *heap, uint64_t size,
      uint64_t alignment, uint32_t heap_type, uint32_t heap_flags,
      gs_d3d12_alloc_t *out)
{
   unsigned i;
   uint64_t block_size;

   if (!heap || !out || !size)
      return 0;

   for (i = 0; i < heap->block_count; i++)
   {
      gs_d3d12_block_t *b = &heap->blocks[i];

      if (!b->heap || b->heap_type != heap_type || b->heap_flags != heap_flags)
         continue;
      if (gs_d3d12_block_alloc(heap, b, i, size, alignment, out))
         return 1;
   }

   /* Nothing fits. A new block, which is the only thing here that asks
    * the device for memory, and the reason gs_d3d12_heap_reserve
    * exists: reserve enough at startup and this never runs. */
   block_size = heap->block_size;
   if (block_size < size)
      block_size = gs_d3d12_align_up(size, 64u * 1024u);

   if (!gs_d3d12_heap_add_block(heap, heap_type, heap_flags, block_size))
   {
      /* No room for another block. Empty ones go back first - a
       * ceiling filled with blocks of another kind has nothing for this
       * one even with most of it free - and then one more try. */
      if (!gs_d3d12_heap_trim(heap))
         return 0;
      if (!gs_d3d12_heap_add_block(heap, heap_type, heap_flags, block_size))
         return 0;
   }

   for (i = 0; i < heap->block_count; i++)
   {
      gs_d3d12_block_t *b = &heap->blocks[i];

      if (!b->heap || b->heap_type != heap_type || b->heap_flags != heap_flags)
         continue;
      if (gs_d3d12_block_alloc(heap, b, i, size, alignment, out))
         return 1;
   }
   return 0;
}

void gs_d3d12_heap_free(gs_d3d12_heap_t *heap, const gs_d3d12_alloc_t *alloc)
{
   gs_d3d12_block_t *b;
   uint64_t          offset;
   uint64_t          size;
   unsigned          lo;
   unsigned          hi;
   int               joins_prev;
   int               joins_next;

   if (!heap || !alloc || !alloc->heap || alloc->block >= heap->block_count)
      return;

   /* The block it came from, and no other: after a trim the slot can hold
    * a new block, and a span of the old one merged into it would be
    * handed out twice. */
   b = &heap->blocks[alloc->block];
   if (b->heap != alloc->heap || alloc->offset > b->size
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
    * would then be placed on the same memory. */
   if (lo > 0 && b->free_spans[lo - 1].offset + b->free_spans[lo - 1].size > offset)
   {
      heap->bad_frees++;
      return;
   }
   if (lo < b->free_count && offset + size > b->free_spans[lo].offset)
   {
      heap->bad_frees++;
      return;
   }

   if (b->used >= size)
      b->used -= size;
   if (heap->bytes_used >= size)
      heap->bytes_used -= size;
   if (gs_d3d12_type_is_host(b->heap_type))
   {
      if (heap->bytes_host >= size)
         heap->bytes_host -= size;
   }
   else if (heap->bytes_device >= size)
      heap->bytes_device -= size;

   /* Joined to whichever neighbours it touches, so a block does not turn
    * into a thousand unusable slivers over a run. */
   joins_prev = lo > 0
      && b->free_spans[lo - 1].offset + b->free_spans[lo - 1].size == offset;
   joins_next = lo < b->free_count && offset + size == b->free_spans[lo].offset;

   if (joins_prev && joins_next)
   {
      b->free_spans[lo - 1].size += size + b->free_spans[lo].size;
      size = b->free_spans[lo - 1].size;
      gs_d3d12_block_remove_span(b, lo);
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
   else if (!gs_d3d12_block_insert_span(b, lo, offset, size))
      return;

   if (size > b->max_free)
      b->max_free = size;
}
