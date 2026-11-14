/* The Vulkan stream buffer's ring (GS/Renderers/Vulkan/VKStreamBuffer.c)
 * on a simulated GPU: the same reservations, offsets and waits as the
 * C++ class it replaced (the hashes below were taken from it), over
 * small and large buffers; no reservation overlaps what a command buffer
 * still in flight reads; and more command buffers in flight than it
 * tracks fences merge rather than free early. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GS/Renderers/Vulkan/VKStreamBuffer.h"

static void RETRO_CALLCONV quiet(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
retro_log_printf_t log_cb = quiet;

static u8 s_mem[1 << 16];
static int s_live, s_deferred, s_flushes;

static u64 dev_current(void);
static u64 dev_completed(void);
static void dev_wait(u64 counter);

gs_vk_heap_t* gs_vk_device_heap(void) { return NULL; }
void gs_vk_heap_flush(gs_vk_heap_t* heap, const gs_vk_alloc_t* alloc) { (void)heap; (void)alloc; s_flushes++; }
int gs_vk_device_create_host_buffer(VkBufferUsageFlags usage, u32 size, VkBuffer* buffer, gs_vk_alloc_t* alloc, void** mapped)
{
	(void)usage;
	(void)alloc;
	if (size > sizeof(s_mem))
		return 0;
	*buffer = (VkBuffer)(uintptr_t)0x1000;
	*mapped = s_mem;
	s_live++;
	return 1;
}
void gs_vk_device_release_buffer(VkBuffer buffer, const gs_vk_alloc_t* alloc, int defer)
{
	(void)buffer;
	(void)alloc;
	s_live--;
	s_deferred += defer;
}
u64 gs_vk_device_current_fence(void) { return dev_current(); }
u64 gs_vk_device_completed_fence(void) { return dev_completed(); }
void gs_vk_device_wait_fence(u64 counter) { dev_wait(counter); }

static gs_vk_stream_buffer s_sb;

#define S_CREATE(size)        gs_vk_stream_buffer_create(&s_sb, 0, size)
#define S_RESERVE(n, a)       gs_vk_stream_buffer_reserve(&s_sb, n, a)
#define S_OFFSET()            s_sb.current_offset
#define S_COMMIT(n)           gs_vk_stream_buffer_commit(&s_sb, n)
#define S_DESTROY()           gs_vk_stream_buffer_destroy(&s_sb, 1)

#include "scenario.h"

/* The C++ VKStreamBuffer's hashes of the scenario at these sizes */
static const u32 s_sizes[3]    = {4096, 1000, 65536};
static const u32 s_expected[3] = {0xf5a9886fu, 0x03d96cc4u, 0xfe5b266bu};

static int s_failed;

static void check(int ok, const char* what)
{
	if (!ok)
	{
		printf("FAIL: %s\n", what);
		s_failed = 1;
	}
}

int main(int argc, char** argv)
{
	int i;
	const int print = argc > 1 && !strcmp(argv[1], "--print");
	gs_vk_stream_buffer_init(&s_sb);
	for (i = 0; i < 3; i++)
	{
		const u32 h = run_scenario(s_sizes[i], 200000);
		printf("size %u: %08x, %u waits\n", (unsigned)s_sizes[i], (unsigned)h, (unsigned)s_waits);
		if (print)
			continue;
		check(h == s_expected[i], "the reservations, offsets and waits match the C++ class");
		check(s_overlaps == 0, "no reservation overlaps a command buffer in flight");
	}
	if (print)
		return 0;
	check(s_live == 0 && s_deferred == 3, "every buffer released, after the GPU");

	/* past the size: refused, nothing waited for */
	check(S_CREATE(4096) && !S_RESERVE(4096, 4) && !S_RESERVE(4093, 4), "a reservation the buffer cannot hold is refused");
	S_DESTROY();
	/* a buffer that cannot be made leaves the old one */
	check(S_CREATE(4096), "made");
	check(!S_CREATE(1 << 20) && s_sb.size == 4096 && s_live == 1, "a buffer that cannot be made keeps the old one");
	S_DESTROY();

	/* more command buffers in flight than fences: the newest merge */
	{
		u32 k;
		s_current   = 100;
		s_completed = 99;
		check(S_CREATE(4096), "made");
		for (k = 0; k < 20; k++)
		{
			check(S_RESERVE(16, 4), "room");
			S_COMMIT(16);
			s_current++;
		}
		check(s_sb.fence_count == GS_VK_STREAM_FENCES, "the fences fill");
		check(s_sb.fences[(s_sb.fence_head + GS_VK_STREAM_FENCES - 1) % GS_VK_STREAM_FENCES].offset == 20 * 16
				&& s_sb.fences[(s_sb.fence_head + GS_VK_STREAM_FENCES - 1) % GS_VK_STREAM_FENCES].counter == 119,
			"the newest carries the latest offset and counter: freed later, never early");
		S_DESTROY();
	}

	if (s_failed)
		return 1;
	printf("vkstream: ok\n");
	return 0;
}
