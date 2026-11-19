/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef GS_D3D12_DESCRIPTOR_HEAP_MANAGER_H
#define GS_D3D12_DESCRIPTOR_HEAP_MANAGER_H

#include "common/Pcsx2Defs.h"
#include "GS/Renderers/Common/GSFlatMap.h"

/* A descriptor's addresses: D3D12's handle layout, without d3d12.h */
typedef struct gs_d3d12_cpu_handle
{
	size_t ptr;
} gs_d3d12_cpu_handle;
typedef struct gs_d3d12_gpu_handle
{
	u64 ptr;
} gs_d3d12_gpu_handle;

#define GS_D3D12_DESCRIPTOR_INVALID 0xFFFFFFFFu
/* (the values of D3D12_DESCRIPTOR_HEAP_TYPE) */
#define GS_D3D12_DESCRIPTOR_HEAP_SAMPLER 1
/* D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE */
#define GS_D3D12_MAX_SHADER_VISIBLE_SAMPLERS 2048
#define GS_D3D12_SAMPLER_GROUP_MAX 4

typedef struct gs_d3d12_descriptor_handle
{
	gs_d3d12_cpu_handle cpu_handle;
	gs_d3d12_gpu_handle gpu_handle;
	u32 index; /* GS_D3D12_DESCRIPTOR_INVALID for none */
} gs_d3d12_descriptor_handle;

/* A heap whose descriptors are taken and given back one at a time, the
 * lowest free first */
typedef struct gs_d3d12_descriptor_heap_manager
{
	void* heap; /* ID3D12DescriptorHeap* */
	u32 num_descriptors;
	u32 increment;
	int shader_visible;
	gs_d3d12_cpu_handle base_cpu;
	gs_d3d12_gpu_handle base_gpu;
	u32* free_bits; /* a set bit is a free descriptor */
	u32 free_words;
} gs_d3d12_descriptor_heap_manager;

/* A shader-visible heap handed out in runs from the start, all of it
 * back at once */
typedef struct gs_d3d12_descriptor_allocator
{
	void* heap; /* ID3D12DescriptorHeap* */
	u32 increment;
	u32 num_descriptors;
	u32 current_offset;
	gs_d3d12_cpu_handle base_cpu;
	gs_d3d12_gpu_handle base_gpu;
} gs_d3d12_descriptor_allocator;

/* Sampler groups copied into a shader-visible heap once each: the same
 * group of descriptors (by index) gives the same copy until a reset */
typedef struct gs_d3d12_sampler_allocator
{
	gs_d3d12_descriptor_allocator alloc;
	void* device; /* ID3D12Device* */
	u32 num_samplers;
	gs_flat_map_t groups; /* u32[GS_D3D12_SAMPLER_GROUP_MAX] -> handle */
} gs_d3d12_sampler_allocator;

#ifdef __cplusplus
extern "C" {
#endif

void gs_d3d12_descriptor_handle_clear(gs_d3d12_descriptor_handle* h);

void gs_d3d12_descriptor_heap_manager_init(gs_d3d12_descriptor_heap_manager* m);
int gs_d3d12_descriptor_heap_manager_create(gs_d3d12_descriptor_heap_manager* m, void* device, u32 type,
	u32 num_descriptors, int shader_visible);
void gs_d3d12_descriptor_heap_manager_destroy(gs_d3d12_descriptor_heap_manager* m);
int gs_d3d12_descriptor_heap_manager_allocate(gs_d3d12_descriptor_heap_manager* m, gs_d3d12_descriptor_handle* handle);
void gs_d3d12_descriptor_heap_manager_free_index(gs_d3d12_descriptor_heap_manager* m, u32 index);
void gs_d3d12_descriptor_heap_manager_free(gs_d3d12_descriptor_heap_manager* m, gs_d3d12_descriptor_handle* handle);

void gs_d3d12_descriptor_allocator_init(gs_d3d12_descriptor_allocator* a);
int gs_d3d12_descriptor_allocator_create(gs_d3d12_descriptor_allocator* a, void* device, u32 type, u32 num_descriptors);
void gs_d3d12_descriptor_allocator_destroy(gs_d3d12_descriptor_allocator* a);
int gs_d3d12_descriptor_allocator_allocate(gs_d3d12_descriptor_allocator* a, u32 num_handles,
	gs_d3d12_descriptor_handle* out_base_handle);
void gs_d3d12_descriptor_allocator_reset(gs_d3d12_descriptor_allocator* a);

void gs_d3d12_sampler_allocator_init(gs_d3d12_sampler_allocator* s, u32 num_samplers);
int gs_d3d12_sampler_allocator_create(gs_d3d12_sampler_allocator* s, void* device, u32 num_descriptors);
void gs_d3d12_sampler_allocator_destroy(gs_d3d12_sampler_allocator* s);
/* The shader-visible copy of one sampler, or of num_samplers */
int gs_d3d12_sampler_allocator_lookup_single(gs_d3d12_sampler_allocator* s, gs_d3d12_descriptor_handle* gpu_handle,
	const gs_d3d12_descriptor_handle* cpu_handle);
int gs_d3d12_sampler_allocator_lookup_group(gs_d3d12_sampler_allocator* s, gs_d3d12_descriptor_handle* gpu_handle,
	const gs_d3d12_descriptor_handle* cpu_handles);
/* Forgets the copies; the heap is not reused until a reset */
void gs_d3d12_sampler_allocator_invalidate(gs_d3d12_sampler_allocator* s);
void gs_d3d12_sampler_allocator_reset(gs_d3d12_sampler_allocator* s);
/* More than half the heap's worth of groups */
int gs_d3d12_sampler_allocator_should_reset(const gs_d3d12_sampler_allocator* s);

/* What they ask of the device (GSDevice12.cpp) */
int gs_d3d12_device_create_descriptor_heap(void* device, u32 type, u32 num_descriptors, int shader_visible,
	void** heap, gs_d3d12_cpu_handle* base_cpu, gs_d3d12_gpu_handle* base_gpu, u32* increment);
void gs_d3d12_device_release_descriptor_heap(void* heap);
void gs_d3d12_device_copy_sampler(void* device, gs_d3d12_cpu_handle dst, gs_d3d12_cpu_handle src);
void gs_d3d12_device_copy_sampler_group(void* device, gs_d3d12_cpu_handle dst, const gs_d3d12_descriptor_handle* src,
	u32 count);

#ifdef __cplusplus
}
#endif

#endif
