/* The D3D12 stream buffer's ring (GS/Renderers/DX12/D3D12StreamBuffer.c)
 * on a simulated GPU (tests/vkstream/scenario.h): the same reservations,
 * offsets, waits and buffer calls as the C++ class it replaced (the
 * hashes below were taken from it), over small and large buffers; no
 * reservation overlaps what a command list still in flight reads; and
 * more command lists in flight than the ring holds move the newest fence
 * along rather than free early. Built with GS_D3D12_STREAM_FENCES large
 * too, as the class's list was: the same hashes. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GS/Renderers/DX12/D3D12StreamBuffer.h"

static void RETRO_CALLCONV quiet(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
retro_log_printf_t log_cb = quiet;

static u8 s_mem[1 << 16];
static int s_live, s_deferred;

static u64 dev_current(void);
static u64 dev_completed(void);
static void dev_wait(u64 counter);
static void mix(u32 v);

int gs_d3d12_device_create_upload_buffer(u32 size, void** resource, gs_d3d12_alloc_t* alloc, u8** host_pointer,
	u64* gpu_pointer)
{
	mix(0xC0000000u ^ size);
	if (size > sizeof(s_mem))
		return 0;
	alloc->heap   = s_mem;
	alloc->offset = 0;
	alloc->size   = size;
	*resource     = s_mem;
	*host_pointer = s_mem;
	*gpu_pointer  = 0x10000000ull;
	s_live++;
	return 1;
}
void gs_d3d12_device_release_upload_buffer(void* resource, const gs_d3d12_alloc_t* alloc, u32 size, int defer)
{
	(void)resource;
	(void)alloc;
	/* (the C++ class's calls: the unmap of all of it, then the deferral
	 * or the span back) */
	mix(0xD0000000u ^ size);
	mix(defer ? 0xDEF00000u : 0xF0000000u);
	s_live--;
	s_deferred += defer;
}
u64 gs_d3d12_device_current_fence(void) { return dev_current(); }
u64 gs_d3d12_device_completed_fence(void) { return dev_completed(); }
void gs_d3d12_device_wait_fence(u64 counter) { dev_wait(counter); }

static gs_d3d12_stream_buffer s_sb;

#define S_CREATE(size)  gs_d3d12_stream_buffer_create(&s_sb, size)
#define S_RESERVE(n, a) gs_d3d12_stream_buffer_reserve(&s_sb, n, a)
/* (the room left and the fences tracked go in the hash with the offset) */
#define S_OFFSET()      (mix(s_sb.current_space), mix(s_sb.fence_count), s_sb.current_offset)
#define S_COMMIT(n)     gs_d3d12_stream_buffer_commit(&s_sb, n)
#define S_DESTROY()     gs_d3d12_stream_buffer_destroy(&s_sb, 1)

#include "../vkstream/scenario.h"

/* The C++ D3D12StreamBuffer's hashes of the scenario at these sizes */
static const u32 s_sizes[3]    = {4096, 1000, 65536};
static const u32 s_expected[3] = {0x539654c4u, 0x29bdb4f2u, 0x3c8c67b4u};

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
	gs_d3d12_stream_buffer_init(&s_sb);
	for (i = 0; i < 3; i++)
	{
		const u32 h = run_scenario(s_sizes[i], 200000);
		printf("size %u: %08x, %u waits\n", (unsigned)s_sizes[i], (unsigned)h, (unsigned)s_waits);
		if (print)
			continue;
		check(h == s_expected[i], "the reservations, offsets, waits and calls match the C++ class");
		check(s_overlaps == 0, "no reservation overlaps a command list in flight");
	}
	if (print)
		return 0;
	check(s_live == 0 && s_deferred == 3, "every buffer released, after the GPU");

	/* past the size: refused; up to it at the start, with no alignment
	 * to pay there, taken */
	check(S_CREATE(4096) && !S_RESERVE(4097, 4), "a reservation the buffer cannot hold is refused");
	check(S_RESERVE(4090, 16) && s_sb.current_offset == 0 && s_sb.current_space == 4096,
		"all but the alignment at the start of an empty buffer");
	S_DESTROY();
	/* a buffer that cannot be made leaves the old one */
	check(S_CREATE(4096), "made");
	check(!S_CREATE(1 << 20) && s_sb.size == 4096 && s_live == 1, "a buffer that cannot be made keeps the old one");
	S_DESTROY();

#if GS_D3D12_STREAM_FENCES < 20
	/* more command lists in flight than fences: the newest moves along */
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
		/* (each reservation leaves the fence of the offset before it) */
		check(s_sb.fence_count == GS_D3D12_STREAM_FENCES, "the fences fill");
		check(s_sb.fences[(s_sb.fence_head + GS_D3D12_STREAM_FENCES - 1) % GS_D3D12_STREAM_FENCES].offset == 19 * 16
				&& s_sb.fences[(s_sb.fence_head + GS_D3D12_STREAM_FENCES - 1) % GS_D3D12_STREAM_FENCES].counter == 119,
			"the newest carries the latest offset and counter: freed later, never early");
		S_DESTROY();
	}
#endif

	if (s_failed)
		return 1;
	printf("d3d12stream: ok\n");
	return 0;
}
