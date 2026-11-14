/* The D3D12 descriptor heaps (GS/Renderers/DX12/
 * D3D12DescriptorHeapManager.c) on a fake device: every handle, answer
 * and device call as the C++ classes made them (the hash below was taken
 * from them, scenario.h); then what the classes got wrong: a heap that is
 * full hands out nothing, where the class gave descriptors past its end,
 * a free of an index the heap does not have is nothing, and sampler
 * groups do not outlive the heap their copies were in. */

#include <stdio.h>
#include <string.h>

#include "GS/Renderers/DX12/D3D12DescriptorHeapManager.h"

static void mix(u32 v);
static u32 s_heaps;
static int s_live_heaps;

/* the device, as the classes' calls reached it */
int gs_d3d12_device_create_descriptor_heap(void* device, u32 type, u32 num_descriptors, int shader_visible,
	void** heap, gs_d3d12_cpu_handle* base_cpu, gs_d3d12_gpu_handle* base_gpu, u32* increment)
{
	(void)device;
	mix(0xE0000000u ^ (type << 24) ^ num_descriptors ^ ((u32)shader_visible << 20));
	s_heaps++;
	s_live_heaps++;
	*heap         = (void*)(size_t)(0x100 * s_heaps);
	base_cpu->ptr = (size_t)0x1000000u * s_heaps;
	if (shader_visible)
		base_gpu->ptr = 0x8000000000000000ull + (u64)0x2000000u * s_heaps;
	*increment = type * 16u + 8u;
	return 1;
}
void gs_d3d12_device_release_descriptor_heap(void* heap)
{
	mix(0xEE000000u ^ (u32)(size_t)heap);
	s_live_heaps--;
}
void gs_d3d12_device_copy_sampler(void* device, gs_d3d12_cpu_handle dst, gs_d3d12_cpu_handle src)
{
	(void)device;
	mix(0xC1u);
	mix((u32)dst.ptr);
	mix((u32)src.ptr);
}
void gs_d3d12_device_copy_sampler_group(void* device, gs_d3d12_cpu_handle dst, const gs_d3d12_descriptor_handle* src,
	u32 count)
{
	u32 i;
	(void)device;
	mix(0xC2u ^ (count << 8));
	mix((u32)dst.ptr);
	for (i = 0; i < count; i++)
		mix((u32)src[i].cpu_handle.ptr);
}

#define HANDLE    gs_d3d12_descriptor_handle
#define MANAGER   gs_d3d12_descriptor_heap_manager
#define ALLOCATOR gs_d3d12_descriptor_allocator
#define SAMPLERS  gs_d3d12_sampler_allocator
#define H_INDEX(h) (h)->index
#define H_CPU(h)   ((u64)(h)->cpu_handle.ptr)
#define H_GPU(h)   ((h)->gpu_handle.ptr)
#define M_CREATE(m, t, n, v)  gs_d3d12_descriptor_heap_manager_create(m, (void*)1, t, n, v)
#define M_ALLOC(m, h)         gs_d3d12_descriptor_heap_manager_allocate(m, h)
#define M_FREE(m, h)          gs_d3d12_descriptor_heap_manager_free(m, h)
#define M_FREE_INDEX(m, i)    gs_d3d12_descriptor_heap_manager_free_index(m, i)
#define M_DESTROY(m)          gs_d3d12_descriptor_heap_manager_destroy(m)
#define A_CREATE(a, t, n)     gs_d3d12_descriptor_allocator_create(a, (void*)1, t, n)
#define A_ALLOC(a, n, h)      gs_d3d12_descriptor_allocator_allocate(a, n, h)
#define A_RESET(a)            gs_d3d12_descriptor_allocator_reset(a)
#define A_DESTROY(a)          gs_d3d12_descriptor_allocator_destroy(a)
#define S_CREATE(s, n)        gs_d3d12_sampler_allocator_create(s, (void*)1, n)
#define S_SINGLE(s, g, c)     gs_d3d12_sampler_allocator_lookup_single(s, g, c)
#define S_GROUP(s, g, c)      gs_d3d12_sampler_allocator_lookup_group(s, g, c)
#define S_INVALIDATE(s)       gs_d3d12_sampler_allocator_invalidate(s)
#define S_RESET(s)            gs_d3d12_sampler_allocator_reset(s)
#define S_SHOULD_RESET(s)     gs_d3d12_sampler_allocator_should_reset(s)
#define S_DESTROY(s)          gs_d3d12_sampler_allocator_destroy(s)

static MANAGER s_m0, s_m1, s_m2;
static ALLOCATOR s_a;
static SAMPLERS s_s, s_s2;

#include "scenario.h"

/* the C++ classes' hash of the scenario */
#define EXPECTED 0xc7538dedu

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
	u32 h, i;
	gs_d3d12_descriptor_handle d;
	gs_d3d12_descriptor_heap_manager_init(&s_m0);
	gs_d3d12_descriptor_heap_manager_init(&s_m1);
	gs_d3d12_descriptor_heap_manager_init(&s_m2);
	gs_d3d12_descriptor_allocator_init(&s_a);
	gs_d3d12_sampler_allocator_init(&s_s, 2);
	gs_d3d12_sampler_allocator_init(&s_s2, 2);

	h = run_scenario(1);
	printf("d3d12desc: %08x\n", (unsigned)h);
	if (argc > 1 && !strcmp(argv[1], "--print"))
		return 0;
	check(h == EXPECTED, "every handle, answer and device call as the C++ classes made them");
	check(s_live_heaps == 0, "every heap released");

	/* full: nothing more, where the class's last bitset ran past the end */
	check(gs_d3d12_descriptor_heap_manager_create(&s_m0, (void*)1, 2, 37, 0), "made");
	for (i = 0; i < 37; i++)
		check(gs_d3d12_descriptor_heap_manager_allocate(&s_m0, &d) && d.index == i, "the lowest free first");
	check(!gs_d3d12_descriptor_heap_manager_allocate(&s_m0, &d), "a full heap hands out nothing");
	gs_d3d12_descriptor_heap_manager_free_index(&s_m0, 5000);
	gs_d3d12_descriptor_heap_manager_free_index(&s_m0, 36);
	check(gs_d3d12_descriptor_heap_manager_allocate(&s_m0, &d) && d.index == 36
			&& !gs_d3d12_descriptor_heap_manager_allocate(&s_m0, &d),
		"an index past the heap is not freed");
	gs_d3d12_descriptor_heap_manager_destroy(&s_m0);

	/* a group looked up, the allocator destroyed and made again: the
	 * lookup copies the sampler anew */
	{
		gs_d3d12_descriptor_handle cpu, gpu;
		u32 before;
		gs_d3d12_descriptor_handle_clear(&cpu);
		cpu.index          = 3;
		cpu.cpu_handle.ptr = 0x333;
		check(gs_d3d12_sampler_allocator_create(&s_s, (void*)1, 16)
				&& gs_d3d12_sampler_allocator_lookup_single(&s_s, &gpu, &cpu),
			"looked up");
		gs_d3d12_sampler_allocator_destroy(&s_s);
		check(gs_d3d12_sampler_allocator_create(&s_s, (void*)1, 16), "made again");
		before = s_h;
		check(gs_d3d12_sampler_allocator_lookup_single(&s_s, &gpu, &cpu) && s_h != before,
			"no group outlives the heap its copy was in");
		gs_d3d12_sampler_allocator_destroy(&s_s);
	}

	/* reset once half the device's sampler heap is in groups, not before */
	{
		gs_d3d12_descriptor_handle cpu, gpu;
		u32 k;
		gs_d3d12_descriptor_handle_clear(&cpu);
		check(gs_d3d12_sampler_allocator_create(&s_s, (void*)1, 2048), "made");
		for (k = 0; k < 1024; k++)
		{
			check(!gs_d3d12_sampler_allocator_should_reset(&s_s), "not yet");
			cpu.index = k;
			check(gs_d3d12_sampler_allocator_lookup_single(&s_s, &gpu, &cpu), "looked up");
		}
		check(gs_d3d12_sampler_allocator_should_reset(&s_s), "reset at half the heap");
		gs_d3d12_sampler_allocator_destroy(&s_s);
	}

	if (s_failed)
		return 1;
	printf("d3d12desc: ok\n");
	return 0;
}
