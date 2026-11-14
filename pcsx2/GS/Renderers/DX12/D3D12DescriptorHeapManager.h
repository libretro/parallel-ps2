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

/* A descriptor's addresses: D3D12's own handle types in C++, the same
 * layout in C, which does not include d3d12.h */
#ifdef __cplusplus
#include <d3d12.h>
typedef D3D12_CPU_DESCRIPTOR_HANDLE gs_d3d12_cpu_handle;
typedef D3D12_GPU_DESCRIPTOR_HANDLE gs_d3d12_gpu_handle;
#else
typedef struct gs_d3d12_cpu_handle
{
	size_t ptr;
} gs_d3d12_cpu_handle;
typedef struct gs_d3d12_gpu_handle
{
	u64 ptr;
} gs_d3d12_gpu_handle;
#endif

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

/* The C handle, with the C++ device's ways of comparing and passing it */
struct D3D12DescriptorHandle final : gs_d3d12_descriptor_handle
{
	enum : u32
	{
		INVALID_INDEX = GS_D3D12_DESCRIPTOR_INVALID
	};

	D3D12DescriptorHandle() { Clear(); }

	__fi operator bool() const { return index != INVALID_INDEX; }

	__fi operator D3D12_CPU_DESCRIPTOR_HANDLE() const { return cpu_handle; }
	__fi operator D3D12_GPU_DESCRIPTOR_HANDLE() const { return gpu_handle; }

	__fi bool operator==(const D3D12DescriptorHandle& rhs) const { return (index == rhs.index); }
	__fi bool operator!=(const D3D12DescriptorHandle& rhs) const { return (index != rhs.index); }
	__fi bool operator<(const D3D12DescriptorHandle& rhs) const { return (index < rhs.index); }
	__fi bool operator<=(const D3D12DescriptorHandle& rhs) const { return (index <= rhs.index); }
	__fi bool operator>(const D3D12DescriptorHandle& rhs) const { return (index > rhs.index); }
	__fi bool operator>=(const D3D12DescriptorHandle& rhs) const { return (index >= rhs.index); }

	__fi void Clear() { gs_d3d12_descriptor_handle_clear(this); }
};
static_assert(sizeof(D3D12DescriptorHandle) == sizeof(gs_d3d12_descriptor_handle), "a view, no fields");

class D3D12DescriptorHeapManager final
{
public:
	D3D12DescriptorHeapManager() { gs_d3d12_descriptor_heap_manager_init(&m_m); }
	D3D12DescriptorHeapManager(const D3D12DescriptorHeapManager&) = delete;
	D3D12DescriptorHeapManager& operator=(const D3D12DescriptorHeapManager&) = delete;
	~D3D12DescriptorHeapManager() { Destroy(); }

	ID3D12DescriptorHeap* GetDescriptorHeap() const { return static_cast<ID3D12DescriptorHeap*>(m_m.heap); }
	u32 GetDescriptorIncrementSize() const { return m_m.increment; }

	bool Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 num_descriptors, bool shader_visible)
	{
		return gs_d3d12_descriptor_heap_manager_create(&m_m, device, static_cast<u32>(type), num_descriptors,
				   shader_visible) != 0;
	}
	void Destroy() { gs_d3d12_descriptor_heap_manager_destroy(&m_m); }

	bool Allocate(D3D12DescriptorHandle* handle) { return gs_d3d12_descriptor_heap_manager_allocate(&m_m, handle) != 0; }
	void Free(D3D12DescriptorHandle* handle) { gs_d3d12_descriptor_heap_manager_free(&m_m, handle); }
	void Free(u32 index) { gs_d3d12_descriptor_heap_manager_free_index(&m_m, index); }

private:
	gs_d3d12_descriptor_heap_manager m_m;
};

class D3D12DescriptorAllocator
{
public:
	D3D12DescriptorAllocator() { gs_d3d12_descriptor_allocator_init(&m_a); }
	D3D12DescriptorAllocator(const D3D12DescriptorAllocator&) = delete;
	D3D12DescriptorAllocator& operator=(const D3D12DescriptorAllocator&) = delete;
	/* (taking another's heap, its own released: a command list's
	 * resources reset with = {}) */
	D3D12DescriptorAllocator& operator=(D3D12DescriptorAllocator&& rhs)
	{
		Destroy();
		m_a = rhs.m_a;
		gs_d3d12_descriptor_allocator_init(&rhs.m_a);
		return *this;
	}
	~D3D12DescriptorAllocator() { Destroy(); }

	__fi ID3D12DescriptorHeap* GetDescriptorHeap() const { return static_cast<ID3D12DescriptorHeap*>(m_a.heap); }
	__fi u32 GetDescriptorIncrementSize() const { return m_a.increment; }

	bool Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 num_descriptors)
	{
		return gs_d3d12_descriptor_allocator_create(&m_a, device, static_cast<u32>(type), num_descriptors) != 0;
	}
	void Destroy() { gs_d3d12_descriptor_allocator_destroy(&m_a); }

	bool Allocate(u32 num_handles, D3D12DescriptorHandle* out_base_handle)
	{
		return gs_d3d12_descriptor_allocator_allocate(&m_a, num_handles, out_base_handle) != 0;
	}
	void Reset() { gs_d3d12_descriptor_allocator_reset(&m_a); }

private:
	gs_d3d12_descriptor_allocator m_a;
};

template <u32 NumSamplers>
class D3D12GroupedSamplerAllocator
{
	static_assert(NumSamplers >= 1 && NumSamplers <= GS_D3D12_SAMPLER_GROUP_MAX, "the C key's size");

public:
	D3D12GroupedSamplerAllocator() { gs_d3d12_sampler_allocator_init(&m_s, NumSamplers); }
	D3D12GroupedSamplerAllocator(const D3D12GroupedSamplerAllocator&) = delete;
	D3D12GroupedSamplerAllocator& operator=(const D3D12GroupedSamplerAllocator&) = delete;
	D3D12GroupedSamplerAllocator& operator=(D3D12GroupedSamplerAllocator&& rhs)
	{
		Destroy();
		m_s = rhs.m_s;
		gs_d3d12_sampler_allocator_init(&rhs.m_s, NumSamplers);
		return *this;
	}
	~D3D12GroupedSamplerAllocator() { Destroy(); }

	__fi ID3D12DescriptorHeap* GetDescriptorHeap() const { return static_cast<ID3D12DescriptorHeap*>(m_s.alloc.heap); }
	__fi u32 GetDescriptorIncrementSize() const { return m_s.alloc.increment; }

	bool Create(ID3D12Device* device, u32 num_descriptors)
	{
		return gs_d3d12_sampler_allocator_create(&m_s, device, num_descriptors) != 0;
	}
	void Destroy() { gs_d3d12_sampler_allocator_destroy(&m_s); }

	bool LookupSingle(D3D12DescriptorHandle* gpu_handle, const D3D12DescriptorHandle& cpu_handle)
	{
		return gs_d3d12_sampler_allocator_lookup_single(&m_s, gpu_handle, &cpu_handle) != 0;
	}
	/* (cpu_handles: NumSamplers of them) */
	bool LookupGroup(D3D12DescriptorHandle* gpu_handle, const D3D12DescriptorHandle* cpu_handles)
	{
		return gs_d3d12_sampler_allocator_lookup_group(&m_s, gpu_handle, cpu_handles) != 0;
	}

	void InvalidateCache() { gs_d3d12_sampler_allocator_invalidate(&m_s); }
	void Reset() { gs_d3d12_sampler_allocator_reset(&m_s); }
	bool ShouldReset() const { return gs_d3d12_sampler_allocator_should_reset(&m_s) != 0; }

private:
	gs_d3d12_sampler_allocator m_s;
};
#endif

#endif
