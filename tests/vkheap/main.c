/* GS/Renderers/Vulkan/GSVulkanHeap.c - the suballocator that replaces
 * VMA. No GPU: the Vulkan entry points are stubs that count calls and
 * hand out fake handles, so what is tested is the arithmetic - which is
 * the part that decides whether a frame allocates. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GSVulkanHeap.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)

static int allocations;   /* how many times the driver was asked */

/* A memory type whose driver allocations stop succeeding once this many
 * bytes are taken from it: the PCI BAR window. -1 refuses nothing. */
static int          refuse_type = -1;
static VkDeviceSize refuse_after;
static VkDeviceSize refuse_taken;
static int frees;
static int maps;

/* A memory type the driver allocates but will not map. -1 maps all. */
static int map_fail_type = -1;
/* The type each fake VkDeviceMemory was allocated from, by handle. */
#define TYPE_OF_MAX 4096
static unsigned char type_of[TYPE_OF_MAX];

static VkResult stub_allocate(VkDevice d, const VkMemoryAllocateInfo *ai,
      const VkAllocationCallbacks *cb, VkDeviceMemory *out)
{
   (void)d; (void)cb;
   if ((int)ai->memoryTypeIndex == refuse_type)
   {
      if (refuse_taken + ai->allocationSize > refuse_after)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      refuse_taken += ai->allocationSize;
   }
   allocations++;
   *out = (VkDeviceMemory)(uintptr_t)(0x1000 + allocations);
   if (allocations < TYPE_OF_MAX)
      type_of[allocations] = (unsigned char)ai->memoryTypeIndex;
   return VK_SUCCESS;
}

static void stub_free(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *cb)
{
   (void)d; (void)m; (void)cb;
   frees++;
}

/* What the stub handed out for each fake VkDeviceMemory, so unmap can
 * give it back - otherwise the test leaks and the leak is the test's. */
static VkDeviceMemory mapped_handles[GS_VK_HEAP_MAX_BLOCKS];
static void          *mapped_ptrs[GS_VK_HEAP_MAX_BLOCKS];
static unsigned       mapped_n;

static VkResult stub_map(VkDevice d, VkDeviceMemory m, VkDeviceSize off,
      VkDeviceSize size, VkMemoryMapFlags f, void **out)
{
   (void)d; (void)off; (void)f;
   {
      const uintptr_t n = (uintptr_t)m - 0x1000;
      if (map_fail_type >= 0 && n < TYPE_OF_MAX && type_of[n] == (unsigned char)map_fail_type)
         return VK_ERROR_MEMORY_MAP_FAILED;
   }
   maps++;
   *out = malloc(size == VK_WHOLE_SIZE ? (4u * 1024u * 1024u) : (size_t)size);
   if (!*out)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (mapped_n < GS_VK_HEAP_MAX_BLOCKS)
   {
      mapped_handles[mapped_n] = m;
      mapped_ptrs[mapped_n]    = *out;
      mapped_n++;
   }
   return VK_SUCCESS;
}

static void stub_unmap(VkDevice d, VkDeviceMemory m)
{
   unsigned i;
   (void)d;
   for (i = 0; i < mapped_n; i++)
   {
      if (mapped_handles[i] != m)
         continue;
      free(mapped_ptrs[i]);
      mapped_handles[i] = mapped_handles[mapped_n - 1];
      mapped_ptrs[i]    = mapped_ptrs[mapped_n - 1];
      mapped_n--;
      return;
   }
}

static VkResult stub_flush(VkDevice d, uint32_t n, const VkMappedMemoryRange *r)
{
   (void)d; (void)n; (void)r;
   return VK_SUCCESS;
}

static void fill_props(VkPhysicalDeviceMemoryProperties *p)
{
   memset(p, 0, sizeof(*p));
   p->memoryHeapCount = 1;
   p->memoryHeaps[0].size = 1024u * 1024u * 1024u;
   p->memoryTypeCount = 2;
   /* 0: device local. 1: host visible and coherent. */
   p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   p->memoryTypes[0].heapIndex = 0;
   p->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   p->memoryTypes[1].heapIndex = 0;
}

static void req(VkMemoryRequirements *r, VkDeviceSize size, VkDeviceSize align, uint32_t bits)
{
   r->size = size;
   r->alignment = align;
   r->memoryTypeBits = bits;
}

/* What GSDeviceVK::CreateHeap and CreateBuffers do at startup, in order:
 * two image blocks and one buffer block reserved, then the texture upload
 * buffer, the vertex, index and two uniform stream buffers (host visible,
 * preferring device local), and the expansion index buffer - a host-visible
 * staging copy and its device-local home. Every one must be had, at every
 * upscale, whatever memory types the device offers. */
#define STARTUP_TEXTURE_UPLOAD (64u * 1024u * 1024u)
#define STARTUP_EXPAND         (2u * 16383u * 6u)

static int startup(const VkPhysicalDeviceMemoryProperties *p, const gs_vk_heap_fns_t *fns,
      unsigned scale)
{
   static const VkDeviceSize streams[4] = {
      32u * 1024u * 1024u, 16u * 1024u * 1024u, 8u * 1024u * 1024u, 8u * 1024u * 1024u };
   const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   const VkDeviceSize block = 64u * 1024u * 1024u;
   const VkDeviceSize fixed = (VkDeviceSize)STARTUP_TEXTURE_UPLOAD + streams[0] + streams[1]
      + streams[2] + streams[3] + 2u * (VkDeviceSize)STARTUP_EXPAND;
   gs_vk_heap_t h;
   gs_vk_alloc_t a;
   VkMemoryRequirements r;
   int ok = 1, i;

   memset(&h, 0, sizeof(h));
   if (!gs_vk_heap_init(&h, (VkDevice)1, p, fns, block, 256,
            gs_vk_heap_ceiling((VkDeviceSize)4194304u * scale * scale, block, fixed)))
      return 0;
   gs_vk_heap_reserve(&h, ~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 2);
   gs_vk_heap_reserve(&h, ~0u, hv, 1, 1);

   req(&r, STARTUP_TEXTURE_UPLOAD, 256, ~0u);
   ok &= gs_vk_heap_alloc(&h, &r, hv, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, &a) != 0;
   for (i = 0; i < 4; i++)
   {
      req(&r, streams[i], 256, ~0u);
      ok &= gs_vk_heap_alloc(&h, &r, hv, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, &a) != 0;
   }
   req(&r, STARTUP_EXPAND, 256, ~0u);
   ok &= gs_vk_heap_alloc(&h, &r, hv, 0, 1, &a) != 0;
   ok &= gs_vk_heap_alloc(&h, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 1, &a) != 0;

   /* And the two image blocks reserved for the first frames are still
    * there: the buffers did not have to give them back to fit. */
   {
      unsigned b, image_blocks = 0;
      for (b = 0; b < h.block_count; b++)
         if (h.blocks[b].memory && !h.blocks[b].linear)
            image_blocks++;
      ok &= image_blocks == 2;
   }
   gs_vk_heap_shutdown(&h);
   return ok;
}

static void check_startup(const gs_vk_heap_fns_t *fns)
{
   const VkMemoryPropertyFlags dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   static const unsigned scales[] = { 1, 2, 3, 4, 8 };
   const char *names[4] = { "unified, private and shared types", "unified, one type",
      "discrete without a BAR", "discrete with a 256 MB BAR" };
   VkPhysicalDeviceMemoryProperties p[4];
   char what[96];
   unsigned l, s;

   memset(p, 0, sizeof(p));
   p[0].memoryHeapCount = 1; p[0].memoryHeaps[0].size = 16384ull * 1024u * 1024u;
   p[0].memoryTypeCount = 2;
   p[0].memoryTypes[0].propertyFlags = dl;
   p[0].memoryTypes[1].propertyFlags = dl | hv;

   p[1].memoryHeapCount = 1; p[1].memoryHeaps[0].size = 16384ull * 1024u * 1024u;
   p[1].memoryTypeCount = 1;
   p[1].memoryTypes[0].propertyFlags = dl | hv;

   p[2].memoryHeapCount = 2;
   p[2].memoryHeaps[0].size = 8192ull * 1024u * 1024u;
   p[2].memoryHeaps[1].size = 16384ull * 1024u * 1024u;
   p[2].memoryTypeCount = 2;
   p[2].memoryTypes[0].propertyFlags = dl;
   p[2].memoryTypes[1].propertyFlags = hv;
   p[2].memoryTypes[1].heapIndex     = 1;

   p[3] = p[2];
   p[3].memoryHeapCount = 3;
   p[3].memoryHeaps[2].size = 256ull * 1024u * 1024u;
   p[3].memoryTypeCount = 3;
   p[3].memoryTypes[2].propertyFlags = dl | hv;
   p[3].memoryTypes[2].heapIndex     = 2;

   for (l = 0; l < 4; l++)
      for (s = 0; s < sizeof(scales) / sizeof(scales[0]); s++)
      {
         sprintf(what, "startup: %s at %ux has every buffer and keeps its image blocks", names[l], scales[s]);
         CHECK(startup(&p[l], fns, scales[s]), what);
      }
}

int main(void)
{
   VkPhysicalDeviceMemoryProperties props;
   gs_vk_heap_fns_t fns;
   gs_vk_heap_t heap;
   VkMemoryRequirements r;
   gs_vk_alloc_t a[64];
   int i;

   fill_props(&props);
   memset(&fns, 0, sizeof(fns));
   fns.allocate_memory = stub_allocate;
   fns.free_memory     = stub_free;
   fns.map_memory      = stub_map;
   fns.unmap_memory    = stub_unmap;
   fns.flush_ranges    = stub_flush;

   CHECK(gs_vk_heap_init(&heap, (VkDevice)1, &props, &fns, 1024 * 1024, 256, 0) != 0, "init");

   /* Reserving takes blocks now. */
   allocations = 0;
   CHECK(gs_vk_heap_reserve(&heap, 0x3u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 2) == 2,
         "reserve takes the blocks it was asked for");
   CHECK(allocations == 2, "two driver allocations, no more");

   /* And then nothing does. This is the whole point. */
   allocations = 0;
   req(&r, 64 * 1024, 256, 0x1u);
   for (i = 0; i < 16; i++)
      CHECK(gs_vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &a[i]) != 0,
            "allocation out of the reserved blocks");
   CHECK(allocations == 0, "sixteen allocations, nothing asked of the driver");

   /* Offsets do not overlap, and honour alignment. */
   for (i = 0; i < 16; i++)
   {
      int j;
      CHECK((a[i].offset % 256) == 0, "aligned");
      for (j = i + 1; j < 16; j++)
      {
         const int same = (a[i].memory == a[j].memory);
         const int apart = (a[i].offset + a[i].size <= a[j].offset)
            || (a[j].offset + a[j].size <= a[i].offset);
         CHECK(!same || apart, "two allocations never overlap");
      }
   }

   /* Freeing and re-taking the same size reuses the space rather than
    * growing: the churn case, which is what put the card on the floor. */
   for (i = 0; i < 16; i++)
      gs_vk_heap_free(&heap, &a[i]);
   CHECK(heap.bytes_used == 0, "everything given back");

   allocations = 0;
   for (i = 0; i < 16; i++)
      CHECK(gs_vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &a[i]) != 0,
            "re-taken");
   CHECK(allocations == 0, "a free and re-take asks the driver for nothing");

   /* Fragmentation: free every other one, then ask for something that
    * needs two adjacent - it must come from the merged space, still
    * without the driver. */
   for (i = 0; i < 16; i += 2)
      gs_vk_heap_free(&heap, &a[i]);
   for (i = 1; i < 16; i += 2)
      gs_vk_heap_free(&heap, &a[i]);
   allocations = 0;
   req(&r, 512 * 1024, 256, 0x1u);
   CHECK(gs_vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &a[0]) != 0,
         "a large allocation after freeing everything");
   CHECK(allocations == 0, "freed spans merged, so no new block");
   gs_vk_heap_free(&heap, &a[0]);

   /* Host visible memory is mapped once, per block, and an allocation's
    * pointer is inside that mapping. */
   maps = 0;
   req(&r, 4096, 64, 0x2u);
   CHECK(gs_vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, 1, &a[0]) != 0,
         "host visible allocation");
   CHECK(a[0].mapped != NULL, "and it is mapped");
   CHECK(maps == 1, "the block was mapped once");
   maps = 0;
   CHECK(gs_vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, 1, &a[1]) != 0,
         "a second one");
   CHECK(maps == 0, "no second mapping");
   CHECK(a[1].mapped != a[0].mapped, "different offsets, different pointers");

   /* Something bigger than a block gets its own. */
   allocations = 0;
   req(&r, 4u * 1024u * 1024u, 256, 0x1u);
   CHECK(gs_vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &a[2]) != 0,
         "an allocation larger than the block size");
   CHECK(allocations == 1, "which takes exactly one block");

   /* A requirement no memory type satisfies fails rather than pretending. */
   req(&r, 1024, 256, 0x0u);
   CHECK(gs_vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &a[3]) == 0,
         "no type, no allocation");

   /* A ceiling is a ceiling: with one set, the heap refuses rather than
    * taking another block, which is the difference between a missing
    * texture and a card with nothing left on it. */
   {
      gs_vk_heap_t capped;
      gs_vk_alloc_t c;
      int taken = 0;
      memset(&capped, 0, sizeof(capped));
      CHECK(gs_vk_heap_init(&capped, (VkDevice)1, &props, &fns,
            1024 * 1024, 256, 4 * 1024 * 1024) != 0, "init with a ceiling");
      req(&r, 1024 * 1024, 256, 0x1u);
      while (gs_vk_heap_alloc(&capped, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &c))
      {
         taken++;
         if (taken > 64)
            break;
      }
      CHECK(taken == 4, "four megabytes of ceiling is four one-megabyte blocks");
      CHECK(capped.bytes_reserved <= 4u * 1024u * 1024u, "and it never went past it");
      gs_vk_heap_shutdown(&capped);
   }

   /* Trim gives empty blocks back, and the block indices live
    * allocations carry keep pointing at the right memory afterwards -
    * the reason trim leaves holes instead of closing the array up. */
   {
      gs_vk_heap_t t2;
      gs_vk_alloc_t keep[4];
      gs_vk_alloc_t drop[4];
      VkDeviceMemory keep_mem[4];
      VkDeviceSize keep_off[4];
      int k;

      memset(&t2, 0, sizeof(t2));
      CHECK(gs_vk_heap_init(&t2, (VkDevice)1, &props, &fns, 1024 * 1024, 256, 0) != 0, "trim: init");

      /* Four blocks' worth, alternating between ones kept and ones
       * dropped, so trim has holes to make in the middle. */
      req(&r, 1024 * 1024, 256, 0x1u);
      for (k = 0; k < 4; k++)
      {
         CHECK(gs_vk_heap_alloc(&t2, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &keep[k]) != 0, "trim: keep");
         CHECK(gs_vk_heap_alloc(&t2, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &drop[k]) != 0, "trim: drop");
         keep_mem[k] = keep[k].memory;
         keep_off[k] = keep[k].offset;
      }
      for (k = 0; k < 4; k++)
         gs_vk_heap_free(&t2, &drop[k]);

      CHECK(gs_vk_heap_trim(&t2) == 4, "trim gave back the four empty blocks");

      for (k = 0; k < 4; k++)
      {
         CHECK(keep[k].memory == keep_mem[k], "a live allocation still names its own memory");
         CHECK(keep[k].offset == keep_off[k], "at its own offset");
         CHECK(keep[k].block < t2.block_count, "and its block index is still in range");
      }

      /* And the freed room is usable again. */
      CHECK(gs_vk_heap_alloc(&t2, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &drop[0]) != 0,
            "the trimmed room comes back");

      /* Freeing the ones kept still works, which is what a stale block
       * index would break. */
      for (k = 0; k < 4; k++)
         gs_vk_heap_free(&t2, &keep[k]);
      gs_vk_heap_free(&t2, &drop[0]);
      CHECK(t2.bytes_used == 0, "everything given back after a trim");
      gs_vk_heap_shutdown(&t2);
   }

   /* The device-local host-visible type is a preference, not a
    * requirement. Laid out the way a discrete card without resizable BAR
    * reports it - device memory, system memory, and a small BAR window
    * that is device local and host visible - and the GS device's own
    * startup run against it: 64 MB blocks under a 256 MB ceiling, two
    * device blocks and one host block reserved, a 64 MB upload buffer and
    * then the 32 MB vertex buffer, both preferring the BAR. The driver
    * gives the BAR one block and refuses the second; the vertex buffer
    * must land in system memory instead of failing the device. */
   {
      VkPhysicalDeviceMemoryProperties bar;
      gs_vk_heap_t h3;
      gs_vk_alloc_t up, vtx, idx;
      const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
         | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      const VkDeviceSize mb = 1024u * 1024u;

      memset(&bar, 0, sizeof(bar));
      bar.memoryHeapCount = 3;
      bar.memoryHeaps[0].size = 6144u * mb;
      bar.memoryHeaps[1].size = 16384u * mb;
      bar.memoryHeaps[2].size = 214u * mb;
      bar.memoryTypeCount = 3;
      bar.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      bar.memoryTypes[0].heapIndex     = 0;
      bar.memoryTypes[1].propertyFlags = hv;
      bar.memoryTypes[1].heapIndex     = 1;
      bar.memoryTypes[2].propertyFlags = hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      bar.memoryTypes[2].heapIndex     = 2;

      refuse_type  = 2;
      refuse_after = 64u * mb;
      refuse_taken = 0;

      memset(&h3, 0, sizeof(h3));
      CHECK(gs_vk_heap_init(&h3, (VkDevice)1, &bar, &fns, 64u * mb, 256, 256u * mb) != 0, "bar: init");
      CHECK(gs_vk_heap_reserve(&h3, ~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 2) == 2, "bar: device reserve");
      CHECK(gs_vk_heap_reserve(&h3, ~0u, hv, 1, 1) == 1, "bar: host reserve");

      req(&r, 64u * mb, 256, 0x7u);
      CHECK(gs_vk_heap_alloc(&h3, &r, hv, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, &up) != 0, "bar: upload buffer");
      CHECK(up.type == 2, "bar: the upload buffer takes the BAR it prefers");

      req(&r, 32u * mb, 256, 0x7u);
      CHECK(gs_vk_heap_alloc(&h3, &r, hv, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, &vtx) != 0,
            "bar: the vertex buffer is allocated after the BAR refuses");
      CHECK(vtx.type == 1 && vtx.mapped != NULL, "bar: in mapped system memory");
      CHECK(h3.block_count == 4, "bar: out of the reserved host block, with the reservations kept");

      req(&r, 16u * mb, 256, 0x7u);
      CHECK(gs_vk_heap_alloc(&h3, &r, hv, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, &idx) != 0,
            "bar: and the index buffer after it");

      gs_vk_heap_free(&h3, &idx);
      gs_vk_heap_free(&h3, &vtx);
      gs_vk_heap_free(&h3, &up);
      gs_vk_heap_shutdown(&h3);
      refuse_type = -1;
   }


   /* The BAR listed first, the way some drivers order it: the type that
    * has both flags comes before the system memory that only meets the
    * requirement. The driver refuses the BAR outright; the upload must
    * still land in system memory, mapped. */
   {
      VkPhysicalDeviceMemoryProperties o;
      gs_vk_heap_t h;
      gs_vk_alloc_t x;
      const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
         | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

      memset(&o, 0, sizeof(o));
      o.memoryHeapCount = 2;
      o.memoryHeaps[0].size = 256u * 1024u * 1024u;
      o.memoryHeaps[1].size = 1024u * 1024u * 1024u;
      o.memoryTypeCount = 3;
      o.memoryTypes[0].propertyFlags = hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      o.memoryTypes[0].heapIndex     = 0;
      o.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      o.memoryTypes[1].heapIndex     = 0;
      o.memoryTypes[2].propertyFlags = hv;
      o.memoryTypes[2].heapIndex     = 1;

      refuse_type  = 0;
      refuse_after = 0;
      refuse_taken = 0;
      memset(&h, 0, sizeof(h));
      CHECK(gs_vk_heap_init(&h, (VkDevice)1, &o, &fns, 1024 * 1024, 256, 0) != 0, "order: init");
      req(&r, 4096, 256, 0x7u);
      CHECK(gs_vk_heap_alloc(&h, &r, hv, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, &x) != 0,
            "order: refused BAR listed first, the upload still allocates");
      CHECK(x.type == 2 && x.mapped != NULL, "order: in mapped system memory");
      gs_vk_heap_free(&h, &x);
      gs_vk_heap_shutdown(&h);
      refuse_type = -1;
   }

   /* A host-visible type the driver will not map. Memory asked for as
    * host visible is written through its address: it is never handed out
    * unmapped, the block goes back, and a second host-visible type is
    * used. */
   {
      VkPhysicalDeviceMemoryProperties o;
      gs_vk_heap_t h;
      gs_vk_alloc_t x;
      int frees_before;
      const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
         | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

      fill_props(&o);
      o.memoryTypeCount = 3;
      o.memoryTypes[2].propertyFlags = hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      o.memoryTypes[2].heapIndex     = 0;

      map_fail_type = 1;
      memset(&h, 0, sizeof(h));
      CHECK(gs_vk_heap_init(&h, (VkDevice)1, &o, &fns, 1024 * 1024, 256, 0) != 0, "map: init");

      frees_before = frees;
      req(&r, 4096, 64, 0x2u);
      CHECK(gs_vk_heap_alloc(&h, &r, hv, 0, 1, &x) == 0,
            "map: the only type it may use will not map, so it fails");
      CHECK(h.last_error == VK_ERROR_MEMORY_MAP_FAILED, "map: and says why");
      CHECK(frees == frees_before + 1 && h.bytes_reserved == 0,
            "map: the unmappable block went back to the driver");

      req(&r, 4096, 64, 0x6u);
      CHECK(gs_vk_heap_alloc(&h, &r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, 1, &x) != 0,
            "map: with a second host-visible type it allocates");
      CHECK(x.type == 2 && x.mapped != NULL, "map: mapped, in the second type");
      gs_vk_heap_free(&h, &x);

      CHECK(gs_vk_heap_reserve(&h, 0x2u, hv, 1, 2) == 0,
            "map: a reserve takes no unmappable host block");
      gs_vk_heap_shutdown(&h);
      map_fail_type = -1;
   }

   /* On a card where every type is host visible, a block that will not
    * map still serves what never looks at its address, and is passed
    * over for what does. */
   {
      VkPhysicalDeviceMemoryProperties o;
      gs_vk_heap_t h;
      gs_vk_alloc_t img, up;

      memset(&o, 0, sizeof(o));
      o.memoryHeapCount = 1;
      o.memoryHeaps[0].size = 1024u * 1024u * 1024u;
      o.memoryTypeCount = 2;
      o.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
         | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      o.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
         | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

      map_fail_type = 0;
      memset(&h, 0, sizeof(h));
      CHECK(gs_vk_heap_init(&h, (VkDevice)1, &o, &fns, 1024 * 1024, 256, 0) != 0, "uma: init");
      req(&r, 4096, 256, 0x3u);
      CHECK(gs_vk_heap_alloc(&h, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 1, &img) != 0,
            "uma: device-local memory from a block that would not map");
      CHECK(img.type == 0 && img.mapped == NULL, "uma: in the unmapped block");
      CHECK(gs_vk_heap_alloc(&h, &r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, 1, &up) != 0,
            "uma: a host-visible buffer next to it");
      CHECK(up.mapped != NULL && up.memory != img.memory,
            "uma: passed over the unmapped block, and mapped");
      gs_vk_heap_free(&h, &up);
      gs_vk_heap_free(&h, &img);
      gs_vk_heap_shutdown(&h);
      map_fail_type = -1;
   }

   /* Two device-local types and a driver with none left of the first:
    * the second is tried rather than the allocation failed. */
   {
      VkPhysicalDeviceMemoryProperties o;
      gs_vk_heap_t h;
      gs_vk_alloc_t x;

      memset(&o, 0, sizeof(o));
      o.memoryHeapCount = 1;
      o.memoryHeaps[0].size = 1024u * 1024u * 1024u;
      o.memoryTypeCount = 2;
      o.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      o.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

      refuse_type  = 0;
      refuse_after = 0;
      refuse_taken = 0;
      memset(&h, 0, sizeof(h));
      CHECK(gs_vk_heap_init(&h, (VkDevice)1, &o, &fns, 1024 * 1024, 256, 0) != 0, "second: init");
      req(&r, 4096, 256, 0x3u);
      CHECK(gs_vk_heap_alloc(&h, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &x) != 0,
            "second: out of the first type, the second is used");
      CHECK(x.type == 1, "second: in the second type");
      CHECK(gs_vk_heap_reserve(&h, 0x3u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 2) == 2,
            "second: a reserve goes on to the second type too");
      gs_vk_heap_free(&h, &x);
      gs_vk_heap_shutdown(&h);
      refuse_type = -1;
   }

   /* AMD's types, each also offered device-coherent and uncached: those
    * need a feature the device is made without, so with the ordinary
    * types refused nothing comes from them - the request fails instead. */
   {
      const VkMemoryPropertyFlags dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      const VkMemoryPropertyFlags hvc = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
         | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      const VkMemoryPropertyFlags amd = VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD
         | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;
      VkPhysicalDeviceMemoryProperties o;
      gs_vk_heap_t h;
      gs_vk_alloc_t x;
      unsigned t;
      int a, before;

      memset(&o, 0, sizeof(o));
      o.memoryHeapCount = 2;
      o.memoryHeaps[0].size = 1024u * 1024u * 1024u;
      o.memoryHeaps[1].size = 1024u * 1024u * 1024u;
      o.memoryTypeCount = 8;
      o.memoryTypes[0].propertyFlags = dl;
      o.memoryTypes[1].propertyFlags = hvc;
      o.memoryTypes[2].propertyFlags = dl | hvc;
      o.memoryTypes[3].propertyFlags = hvc | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      for (t = 0; t < 4; t++)
      {
         o.memoryTypes[t + 4].propertyFlags = o.memoryTypes[t].propertyFlags | amd;
         o.memoryTypes[t].heapIndex = o.memoryTypes[t + 4].heapIndex = (t & 1) ? 1 : 0;
      }

      memset(&h, 0, sizeof(h));
      CHECK(gs_vk_heap_init(&h, (VkDevice)1, &o, &fns, 1024 * 1024, 256, 0) != 0, "amd: init");
      req(&r, 4096, 256, 0xffu);
      CHECK(gs_vk_heap_alloc(&h, &r, hvc, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1, &x) != 0
            && x.type == 3, "amd: a read-back buffer in the ordinary cached type");
      gs_vk_heap_free(&h, &x);

      refuse_type  = 0;
      refuse_after = 0;
      refuse_taken = 0;
      CHECK(gs_vk_heap_alloc(&h, &r, dl, 0, 0, &x) != 0 && x.type == 2,
            "amd: out of the first device-local type, the next ordinary one");
      gs_vk_heap_free(&h, &x);
      gs_vk_heap_shutdown(&h);

      /* The only ordinary device-local type refused. */
      o.memoryTypes[2].propertyFlags = hvc;
      memset(&h, 0, sizeof(h));
      CHECK(gs_vk_heap_init(&h, (VkDevice)1, &o, &fns, 1024 * 1024, 256, 0) != 0, "amd: init, one device-local type");
      before = allocations;
      CHECK(gs_vk_heap_alloc(&h, &r, dl, 0, 0, &x) == 0,
            "amd: a device-coherent type taken when the ordinary one ran out");
      CHECK(gs_vk_heap_reserve(&h, ~0u, dl, 0, 2) == 0,
            "amd: a reserve went on to a device-coherent type");
      for (a = before + 1; a <= allocations && a < TYPE_OF_MAX; a++)
         CHECK(type_of[a] < 4, "amd: memory allocated from a device-coherent type");
      gs_vk_heap_shutdown(&h);
      refuse_type = -1;
   }

   /* A free of an allocation whose block was trimmed, after the slot
    * has a new block: the new block is left alone, so nothing in it is
    * handed out twice. */
   {
      gs_vk_heap_t h;
      gs_vk_alloc_t old, n1, n2;

      memset(&h, 0, sizeof(h));
      CHECK(gs_vk_heap_init(&h, (VkDevice)1, &props, &fns, 1024 * 1024, 256, 0) != 0, "stale: init");
      req(&r, 1024 * 1024, 256, 0x1u);
      CHECK(gs_vk_heap_alloc(&h, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &old) != 0, "stale: first");
      gs_vk_heap_free(&h, &old);
      CHECK(gs_vk_heap_trim(&h) == 1, "stale: trimmed");
      CHECK(gs_vk_heap_alloc(&h, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &n1) != 0, "stale: slot reused");
      CHECK(n1.block == old.block && n1.memory != old.memory, "stale: same slot, new memory");
      gs_vk_heap_free(&h, &old);
      CHECK(h.bytes_used == r.size, "stale: the old handle changed nothing");
      CHECK(gs_vk_heap_alloc(&h, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &n2) != 0, "stale: another");
      CHECK(n2.memory != n1.memory, "stale: the live block was not handed out again");
      gs_vk_heap_free(&h, &n2);
      gs_vk_heap_free(&h, &n1);
      gs_vk_heap_shutdown(&h);
   }

   /* A second free of one allocation is refused: its span goes on the
    * list once, and nothing is handed out on top of a live allocation. */
   {
      gs_vk_heap_t h;
      gs_vk_alloc_t x, y, fill, p, q;

      memset(&h, 0, sizeof(h));
      CHECK(gs_vk_heap_init(&h, (VkDevice)1, &props, &fns, 4u * 1024u * 1024u, 256, 0) != 0, "twice: init");
      req(&r, 1024 * 1024, 256, 0x1u);
      CHECK(gs_vk_heap_alloc(&h, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &x) != 0, "twice: x");
      CHECK(gs_vk_heap_alloc(&h, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &y) != 0, "twice: y");
      gs_vk_heap_free(&h, &x);
      gs_vk_heap_free(&h, &x);
      CHECK(h.bad_frees == 1 && h.blocks[x.block].free_count == 2,
            "twice: the second free is refused and counted");
      req(&r, 2u * 1024u * 1024u, 256, 0x1u);
      CHECK(gs_vk_heap_alloc(&h, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &fill) != 0, "twice: fill");
      req(&r, 1024 * 1024, 256, 0x1u);
      CHECK(gs_vk_heap_alloc(&h, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &p) != 0, "twice: p");
      CHECK(!gs_vk_heap_alloc(&h, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &q)
            || q.memory != p.memory, "twice: nothing lands on p in its block");
      gs_vk_heap_shutdown(&h);
   }

   /* A block is no more than an eighth of the driver heap its type is
    * in, so a small heap is not mostly one block. */
   {
      VkPhysicalDeviceMemoryProperties o;
      gs_vk_heap_t h;
      gs_vk_alloc_t x;

      fill_props(&o);
      o.memoryHeaps[0].size = 16u * 1024u * 1024u;
      memset(&h, 0, sizeof(h));
      CHECK(gs_vk_heap_init(&h, (VkDevice)1, &o, &fns, 64u * 1024u * 1024u, 256, 0) != 0, "size: init");
      req(&r, 4096, 256, 0x1u);
      CHECK(gs_vk_heap_alloc(&h, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &x) != 0, "size: alloc");
      CHECK(h.bytes_reserved == 2u * 1024u * 1024u, "size: a 2 MB block on a 16 MB heap");
      gs_vk_heap_free(&h, &x);
      gs_vk_heap_shutdown(&h);
   }

   /* Buffers and images never share a block, whatever the memory type:
    * that is how bufferImageGranularity is honoured. */
   {
      gs_vk_heap_t k;
      gs_vk_alloc_t img, buf, img2;

      memset(&k, 0, sizeof(k));
      CHECK(gs_vk_heap_init(&k, (VkDevice)1, &props, &fns, 1024 * 1024, 256, 0) != 0, "kinds: init");
      req(&r, 4096, 256, 0x1u);
      CHECK(gs_vk_heap_alloc(&k, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &img) != 0, "kinds: image");
      CHECK(gs_vk_heap_alloc(&k, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 1, &buf) != 0, "kinds: buffer");
      CHECK(gs_vk_heap_alloc(&k, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &img2) != 0, "kinds: second image");
      CHECK(img.memory != buf.memory, "a buffer does not go in an image's block");
      CHECK(img.memory == img2.memory, "two images share one");
      gs_vk_heap_free(&k, &img);
      gs_vk_heap_free(&k, &buf);
      gs_vk_heap_free(&k, &img2);
      gs_vk_heap_shutdown(&k);
   }

   /* Churn, the way a texture cache does it: thousands of allocations of
    * mixed sizes and alignments, freed in no particular order. Checked
    * against a list kept on the side: nothing overlaps, every offset is
    * aligned, and the block's spans stay sorted, never adjacent, and add
    * up with what is in use to the whole block. */
   {
      enum { LIVE = 600, ROUNDS = 60000 };
      static gs_vk_alloc_t live[LIVE];
      static int used[LIVE];
      gs_vk_heap_t c;
      unsigned seed = 1;
      int n, ok = 1;

      memset(&c, 0, sizeof(c));
      memset(used, 0, sizeof(used));
      CHECK(gs_vk_heap_init(&c, (VkDevice)1, &props, &fns, 8u * 1024u * 1024u, 256, 0) != 0, "churn: init");
      for (n = 0; n < ROUNDS && ok; n++)
      {
         unsigned slot, b, sp;

         seed = seed * 1664525u + 1013904223u;
         slot = (seed >> 8) % LIVE;
         if (used[slot])
         {
            gs_vk_heap_free(&c, &live[slot]);
            used[slot] = 0;
         }
         else
         {
            const VkDeviceSize align = (VkDeviceSize)1 << ((seed >> 20) % 13);
            const VkDeviceSize size  = 16 + ((seed >> 4) % 60000);
            int j;

            req(&r, size, align, 0x1u);
            if (!gs_vk_heap_alloc(&c, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &live[slot]))
            {
               ok = 0;
               break;
            }
            used[slot] = 1;
            if (live[slot].offset % align)
               ok = 0;
            for (j = 0; j < LIVE && ok; j++)
            {
               if (j == (int)slot || !used[j] || live[j].memory != live[slot].memory)
                  continue;
               if (!(live[j].offset + live[j].size <= live[slot].offset
                     || live[slot].offset + live[slot].size <= live[j].offset))
                  ok = 0;
            }
         }
         if (n % 97)
            continue;
         for (b = 0; b < c.block_count && ok; b++)
         {
            const gs_vk_block_t *blk = &c.blocks[b];
            VkDeviceSize free_bytes = 0;

            if (!blk->memory)
               continue;
            for (sp = 0; sp < blk->free_count; sp++)
            {
               free_bytes += blk->free_spans[sp].size;
               if (blk->free_spans[sp].size == 0 || blk->free_spans[sp].size > blk->max_free)
                  ok = 0;
               if (sp + 1 < blk->free_count
                     && blk->free_spans[sp].offset + blk->free_spans[sp].size >= blk->free_spans[sp + 1].offset)
                  ok = 0;
            }
            if (free_bytes + blk->used != blk->size)
               ok = 0;
         }
      }
      CHECK(ok, "churn: allocations aligned and apart, spans sorted, merged and accounted for");
      for (n = 0; n < LIVE; n++)
         if (used[n])
            gs_vk_heap_free(&c, &live[n]);
      CHECK(c.bytes_used == 0, "churn: everything given back");
      for (n = 0; n < (int)c.block_count; n++)
         CHECK(!c.blocks[n].memory || c.blocks[n].free_count == 1,
               "churn: each block is one free span again");
      gs_vk_heap_shutdown(&c);
   }

   check_startup(&fns);

   frees = 0;
   gs_vk_heap_shutdown(&heap);
   CHECK(frees > 0, "shutdown gives the blocks back");
   CHECK(heap.block_count == 0, "and forgets them");
   CHECK(mapped_n == 0, "and unmapped every block it had mapped");

   printf(fails ? "vkheap: FAILED (%d)\n" : "vkheap: ok\n", fails);
   return fails != 0;
}
