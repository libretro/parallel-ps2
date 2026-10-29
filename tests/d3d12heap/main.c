/* GS/Renderers/DX12/GSD3D12Heap.c - the suballocator that replaces
 * D3D12MemoryAllocator. No GPU: the device entry points are stubs that
 * count calls and hand out fake heaps, so what is tested is the
 * arithmetic - which is the part that decides whether a frame
 * allocates. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "GSD3D12Heap.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)

#define MB (1024ull * 1024ull)

/* Tier 1 keeps buffers and textures apart; these stand in for the real
 * D3D12_HEAP_FLAG values, which this file does not include. */
#define FLAG_BUFFERS       0x80u
#define FLAG_RT_DS         0x40u
#define FLAG_ALL           0x00u

static int creates;
static int releases;
static int create_fails_after; /* -1 for never */

static int stub_create(void *device, uint64_t size, uint32_t heap_type,
      uint32_t heap_flags, void **out_heap)
{
   (void)device; (void)size; (void)heap_type; (void)heap_flags;
   if (create_fails_after >= 0 && creates >= create_fails_after)
      return 0;
   creates++;
   *out_heap = (void*)(size_t)(0x1000 + creates);
   return 1;
}

static void stub_release(void *device, void *heap)
{
   (void)device; (void)heap;
   releases++;
}

static const gs_d3d12_heap_fns_t stub_fns = { stub_create, stub_release };

static void reset_stubs(void)
{
   creates            = 0;
   releases           = 0;
   create_fails_after = -1;
}

static void test_reserve_then_no_device_calls(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t a[8];
   int i;
   int creates_after_reserve;

   printf("reserve, then allocations ask the device for nothing\n");
   reset_stubs();
   CHECK(gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 4096 * MB),
         "init");
   CHECK(gs_d3d12_heap_reserve(&heap, GS_D3D12_HEAP_TYPE_DEFAULT,
            FLAG_RT_DS, 2) == 2, "two blocks reserved");
   creates_after_reserve = creates;

   for (i = 0; i < 8; i++)
      CHECK(gs_d3d12_heap_alloc(&heap, 4 * MB, 64 * 1024,
               GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_RT_DS, &a[i]),
            "allocation fits in reserved blocks");

   CHECK(creates == creates_after_reserve,
         "no new heap was created for those allocations");
   CHECK(heap.bytes_used == 32 * MB, "used total");
   CHECK(heap.bytes_device == 32 * MB, "counted as device memory");
   CHECK(heap.bytes_host == 0, "nothing counted as host memory");

   for (i = 0; i < 8; i++)
      gs_d3d12_heap_free(&heap, &a[i]);
   CHECK(heap.bytes_used == 0, "all given back");
   CHECK(heap.bytes_device == 0, "device counter back to zero");

   gs_d3d12_heap_shutdown(&heap);
   CHECK(releases == creates, "every heap released at shutdown");
}

static void test_alignment(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t a, b;

   printf("offsets respect the alignment asked for\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 0);

   CHECK(gs_d3d12_heap_alloc(&heap, 1000, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &a), "first");
   CHECK(gs_d3d12_heap_alloc(&heap, 1000, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &b), "second");
   CHECK((a.offset & (64 * 1024 - 1)) == 0, "first is aligned");
   CHECK((b.offset & (64 * 1024 - 1)) == 0, "second is aligned");
   CHECK(b.offset >= a.offset + a.size, "they do not overlap");

   gs_d3d12_heap_shutdown(&heap);
}

static void test_free_merges(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t a[4];
   gs_d3d12_alloc_t big;
   int i;

   printf("freed spans merge, so a block does not fragment away\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 64 * MB);

   for (i = 0; i < 4; i++)
      CHECK(gs_d3d12_heap_alloc(&heap, 16 * MB, 64 * 1024,
               GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &a[i]), "fill the block");

   for (i = 0; i < 4; i++)
      gs_d3d12_heap_free(&heap, &a[i]);

   /* If the four freed spans did not merge back into one, this cannot
    * be satisfied and the ceiling forbids another block. */
   CHECK(gs_d3d12_heap_alloc(&heap, 64 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &big),
         "whole block available again after merge");

   gs_d3d12_heap_free(&heap, &big);
   gs_d3d12_heap_shutdown(&heap);
}

static void test_ceiling(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t a, b, c;
   unsigned taken;

   printf("the ceiling refuses rather than taking the card\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 128 * MB);

   taken = gs_d3d12_heap_reserve(&heap, GS_D3D12_HEAP_TYPE_DEFAULT,
         FLAG_ALL, 8);
   CHECK(taken == 2, "only what the ceiling allows was taken");
   CHECK(heap.bytes_reserved == 128 * MB, "reserved is the ceiling");

   /* Both blocks in use, so trim has nothing to give back and there is
    * no room for a third. An empty block would have been trimmed and
    * the request met, which is the other path and is tested above. */
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &a), "first block in use");
   CHECK(gs_d3d12_heap_alloc(&heap, 64 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &b), "second block in use");
   CHECK(gs_d3d12_heap_alloc(&heap, 64 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &c) == 0,
         "refused past the ceiling");

   gs_d3d12_heap_free(&heap, &a);
   gs_d3d12_heap_free(&heap, &b);

   gs_d3d12_heap_shutdown(&heap);
}

/* The device at native: a four-block ceiling, the upload rings made at
 * startup (64, then 32 + 16 + 8 + 8 MB), a staging buffer and a
 * readback. The targets still get all four blocks, and the CPU's side
 * stays bounded on its own. */
static void test_host_blocks_leave_device_room(void)
{
   static const uint64_t rings[5] = { 64 * MB, 32 * MB, 16 * MB, 8 * MB, 8 * MB };
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t host[8], dev[4], extra;
   int i;

   printf("upload and readback blocks leave the targets their ceiling\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 256 * MB);

   for (i = 0; i < 5; i++)
      CHECK(gs_d3d12_heap_alloc(&heap, rings[i], 64 * 1024,
               GS_D3D12_HEAP_TYPE_UPLOAD, FLAG_ALL, &host[i]), "upload ring");
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_UPLOAD, FLAG_ALL, &host[5]), "staging buffer");
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_READBACK, FLAG_ALL, &host[6]), "readback");
   CHECK(heap.bytes_reserved_host == 256 * MB, "four host blocks");

   for (i = 0; i < 4; i++)
      CHECK(gs_d3d12_heap_alloc(&heap, 64 * MB, 64 * 1024,
               GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_RT_DS, &dev[i]),
            "a target block under the ceiling");
   CHECK(gs_d3d12_heap_alloc(&heap, 64 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_RT_DS, &extra) == 0,
         "targets refused past the ceiling");
   CHECK(gs_d3d12_heap_alloc(&heap, 64 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_UPLOAD, FLAG_ALL, &extra) == 0,
         "uploads refused past the ceiling");

   for (i = 0; i < 7; i++)
      gs_d3d12_heap_free(&heap, &host[i]);
   for (i = 0; i < 4; i++)
      gs_d3d12_heap_free(&heap, &dev[i]);
   gs_d3d12_heap_trim(&heap);
   CHECK(heap.bytes_reserved == 0 && heap.bytes_reserved_host == 0,
         "trim gives both sides back");
   gs_d3d12_heap_shutdown(&heap);
}

static void test_trim_and_hole_reuse(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t keep, a;
   unsigned before;

   printf("trim gives empty blocks back and the slot gets reused\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 128 * MB);
   gs_d3d12_heap_reserve(&heap, GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, 2);

   /* Something live in the first block, nothing in the second. */
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &keep), "live allocation");
   before = heap.block_count;
   CHECK(gs_d3d12_heap_trim(&heap) == 1, "the empty block went back");
   CHECK(heap.block_count == before, "the slot stayed, as a hole");
   CHECK(heap.bytes_reserved == 64 * MB, "reserved dropped by one block");

   /* The live allocation still points at its own block. */
   CHECK(keep.block < heap.block_count, "live allocation still in range");
   CHECK(heap.blocks[keep.block].heap == keep.heap,
         "live allocation still points at its own heap");

   /* A new block lands in the hole rather than past the end. */
   CHECK(gs_d3d12_heap_alloc(&heap, 64 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_UPLOAD, FLAG_BUFFERS, &a),
         "a new block after the trim");
   CHECK(heap.block_count == before, "it reused the hole");

   gs_d3d12_heap_free(&heap, &a);
   gs_d3d12_heap_free(&heap, &keep);
   gs_d3d12_heap_shutdown(&heap);
}

static void test_type_and_flag_segregation(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t buf, rt;

   printf("blocks are kept apart by heap type and heap flags\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 0);

   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_UPLOAD, FLAG_BUFFERS, &buf), "an upload buffer");
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_RT_DS, &rt), "a render target");

   CHECK(buf.heap != rt.heap,
         "a tier 1 device would refuse these in one heap, so they are not");
   CHECK(heap.bytes_host == 1 * MB, "upload counted as host");
   CHECK(heap.bytes_device == 1 * MB, "target counted as device");

   /* Same type, different flags, still a different block. */
   CHECK(creates == 2, "one heap each");

   gs_d3d12_heap_free(&heap, &buf);
   gs_d3d12_heap_free(&heap, &rt);
   gs_d3d12_heap_shutdown(&heap);
}

static void test_device_refusal_is_not_a_crash(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t a;

   printf("a device that will not create a heap is refused cleanly\n");
   reset_stubs();
   create_fails_after = 0;
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 0);

   CHECK(gs_d3d12_heap_reserve(&heap, GS_D3D12_HEAP_TYPE_DEFAULT,
            FLAG_ALL, 4) == 0, "nothing reserved");
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &a) == 0, "refused");
   CHECK(heap.block_count == 0, "no block recorded");
   CHECK(heap.bytes_reserved == 0, "nothing counted as reserved");

   gs_d3d12_heap_shutdown(&heap);
}

static void test_free_of_a_stale_allocation(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t a;

   printf("freeing something twice, or from a trimmed block, does nothing\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 0);
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &a), "allocate");

   gs_d3d12_heap_free(&heap, &a);
   CHECK(heap.bytes_used == 0, "freed once");
   gs_d3d12_heap_free(&heap, &a);
   CHECK(heap.bytes_used == 0, "freed twice, still zero rather than underflow");

   gs_d3d12_heap_trim(&heap);
   gs_d3d12_heap_free(&heap, &a); /* block is a hole now */
   CHECK(heap.bytes_used == 0, "free against a trimmed block is ignored");

   gs_d3d12_heap_shutdown(&heap);
}

static void test_oversized_gets_its_own_block(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t a;

   printf("something bigger than a block gets a block of its own\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 0);

   CHECK(gs_d3d12_heap_alloc(&heap, 100 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &a), "oversized allocation");
   CHECK(heap.bytes_reserved >= 100 * MB, "a block large enough was taken");
   CHECK(a.offset == 0, "it sits at the start of its own block");

   gs_d3d12_heap_free(&heap, &a);
   gs_d3d12_heap_shutdown(&heap);
}

/* Two live allocations never share memory. */
static int apart(const gs_d3d12_alloc_t *x, const gs_d3d12_alloc_t *y)
{
   return x->heap != y->heap
      || x->offset + x->size <= y->offset
      || y->offset + y->size <= x->offset;
}

static void test_double_free_never_aliases(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t a, b, fill, x, y;

   printf("a second free of one allocation never puts two resources on one span\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 4 * MB, 0);
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &a), "allocate a");
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &b), "allocate b");
   gs_d3d12_heap_free(&heap, &a);
   gs_d3d12_heap_free(&heap, &a);
   CHECK(heap.bad_frees == 1, "the second free is refused and counted");
   CHECK(heap.blocks[0].free_count == 2, "a's span is on the list once");
   CHECK(gs_d3d12_heap_alloc(&heap, 2 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &fill), "fill the tail");
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &x), "take a's room");
   CHECK(!gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &y)
         || (apart(&x, &y) && apart(&y, &b) && apart(&y, &fill)),
         "and nothing else lands on it");
   CHECK(apart(&x, &b) && apart(&x, &fill) && apart(&b, &fill), "three live, apart");
   gs_d3d12_heap_shutdown(&heap);
}

static void test_stale_handle_into_reused_slot(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t old, live, next;

   printf("a free whose block was trimmed leaves the slot's new block alone\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 0);
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &old), "allocate");
   gs_d3d12_heap_free(&heap, &old);
   CHECK(gs_d3d12_heap_trim(&heap) == 1, "trimmed");
   CHECK(gs_d3d12_heap_alloc(&heap, 64 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_UPLOAD, FLAG_BUFFERS, &live), "a new block fills the slot");
   CHECK(live.block == old.block && live.heap != old.heap, "same slot, other heap");
   gs_d3d12_heap_free(&heap, &old);
   CHECK(heap.bytes_used == 64 * MB, "the stale free changed nothing");
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_UPLOAD, FLAG_BUFFERS, &next), "another upload");
   CHECK(apart(&next, &live), "not placed on the live one");
   gs_d3d12_heap_shutdown(&heap);
}

/* Thousands of allocations of mixed sizes and alignments freed in no
 * order, against a list kept on the side: nothing overlaps, offsets are
 * aligned, spans stay sorted and merged and add up with what is in use
 * to the block. */
static void test_churn(void)
{
   enum { LIVE = 600, ROUNDS = 60000 };
   static gs_d3d12_alloc_t live[LIVE];
   static int used[LIVE];
   gs_d3d12_heap_t heap;
   unsigned seed = 1;
   int n, ok = 1;

   printf("churn: aligned, apart, sorted, merged and accounted for\n");
   reset_stubs();
   memset(used, 0, sizeof(used));
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 8 * MB, 0);
   for (n = 0; n < ROUNDS && ok; n++)
   {
      unsigned slot, b, sp;

      seed = seed * 1664525u + 1013904223u;
      slot = (seed >> 8) % LIVE;
      if (used[slot])
      {
         gs_d3d12_heap_free(&heap, &live[slot]);
         used[slot] = 0;
      }
      else
      {
         const uint64_t align = (uint64_t)1 << ((seed >> 20) % 17);
         const uint64_t size  = 16 + ((seed >> 4) % 60000);
         int j;

         if (!gs_d3d12_heap_alloc(&heap, size, align,
                  GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &live[slot]))
         {
            ok = 0;
            break;
         }
         used[slot] = 1;
         if (live[slot].offset % align)
            ok = 0;
         for (j = 0; j < LIVE && ok; j++)
            if (j != (int)slot && used[j] && !apart(&live[j], &live[slot]))
               ok = 0;
      }
      if (n % 97)
         continue;
      for (b = 0; b < heap.block_count && ok; b++)
      {
         const gs_d3d12_block_t *blk = &heap.blocks[b];
         uint64_t free_bytes = 0;

         if (!blk->heap)
            continue;
         for (sp = 0; sp < blk->free_count; sp++)
         {
            free_bytes += blk->free_spans[sp].size;
            if (blk->free_spans[sp].size == 0 || blk->free_spans[sp].size > blk->max_free)
               ok = 0;
            if (sp + 1 < blk->free_count
                  && blk->free_spans[sp].offset + blk->free_spans[sp].size
                     >= blk->free_spans[sp + 1].offset)
               ok = 0;
         }
         if (free_bytes + blk->used != blk->size)
            ok = 0;
      }
   }
   CHECK(ok, "churn invariants held");
   for (n = 0; n < LIVE; n++)
      if (used[n])
         gs_d3d12_heap_free(&heap, &live[n]);
   CHECK(heap.bytes_used == 0 && heap.bad_frees == 0, "everything back, no bad frees");
   for (n = 0; n < (int)heap.block_count; n++)
      CHECK(!heap.blocks[n].heap || heap.blocks[n].free_count == 1,
            "each block is one span again");
   gs_d3d12_heap_shutdown(&heap);
}

/* 4000 small allocations freed in a shuffled order: each free is a
 * search and a merge, so the whole run is quick and ends as one span. */
static void test_fragmented_free(void)
{
   enum { N = 4000 };
   static gs_d3d12_alloc_t a[N];
   static unsigned order[N];
   gs_d3d12_heap_t heap;
   unsigned seed = 7;
   clock_t t0;
   int i;

   printf("4000 shuffled frees merge back to one span\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 1 * MB, 0);
   for (i = 0; i < N; i++)
   {
      CHECK(gs_d3d12_heap_alloc(&heap, 64, 64,
               GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &a[i]), "small allocation");
      order[i] = (unsigned)i;
   }
   for (i = N - 1; i > 0; i--)
   {
      unsigned j, t;
      seed = seed * 1664525u + 1013904223u;
      j        = (seed >> 8) % (unsigned)(i + 1);
      t        = order[i];
      order[i] = order[j];
      order[j] = t;
   }
   t0 = clock();
   for (i = 0; i < N; i++)
      gs_d3d12_heap_free(&heap, &a[order[i]]);
   printf("  (%.3f ms)\n", (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
   CHECK(heap.blocks[0].free_count == 1 && heap.blocks[0].used == 0, "one span, empty");
   gs_d3d12_heap_shutdown(&heap);
}

int main(void)
{
   test_reserve_then_no_device_calls();
   test_alignment();
   test_free_merges();
   test_ceiling();
   test_host_blocks_leave_device_room();
   test_trim_and_hole_reuse();
   test_type_and_flag_segregation();
   test_device_refusal_is_not_a_crash();
   test_free_of_a_stale_allocation();
   test_oversized_gets_its_own_block();
   test_double_free_never_aliases();
   test_stale_handle_into_reused_slot();
   test_churn();
   test_fragmented_free();

   if (fails)
   {
      printf("d3d12heap: %d failed\n", fails);
      return 1;
   }
   printf("d3d12heap: ok\n");
   return 0;
}
