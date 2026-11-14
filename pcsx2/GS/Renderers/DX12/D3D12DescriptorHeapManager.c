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

#include <stdlib.h>
#include <string.h>

#include "D3D12DescriptorHeapManager.h"
#include "GS/Renderers/Common/GSKeyHash.h"

void gs_d3d12_descriptor_handle_clear(gs_d3d12_descriptor_handle* h)
{
	memset(h, 0, sizeof(*h));
	h->index = GS_D3D12_DESCRIPTOR_INVALID;
}

static void gs_d3d12_descriptor_at(gs_d3d12_descriptor_handle* h, u32 index, gs_d3d12_cpu_handle base_cpu,
	gs_d3d12_gpu_handle base_gpu, u32 increment, int shader_visible)
{
	h->index          = index;
	h->cpu_handle.ptr = base_cpu.ptr + (size_t)index * increment;
	h->gpu_handle.ptr = shader_visible ? base_gpu.ptr + (u64)index * increment : 0;
}

/* --- one at a time --- */

void gs_d3d12_descriptor_heap_manager_init(gs_d3d12_descriptor_heap_manager* m)
{
	memset(m, 0, sizeof(*m));
}

int gs_d3d12_descriptor_heap_manager_create(gs_d3d12_descriptor_heap_manager* m, void* device, u32 type,
	u32 num_descriptors, int shader_visible)
{
	const u32 words = (num_descriptors + 31u) / 32u;
	u32* bits;
	u32 i;

	gs_d3d12_descriptor_heap_manager_destroy(m);
	bits = (u32*)malloc((words ? words : 1u) * sizeof(u32));
	if (!bits)
		return 0;
	if (!gs_d3d12_device_create_descriptor_heap(device, type, num_descriptors, shader_visible, &m->heap,
			&m->base_cpu, &m->base_gpu, &m->increment))
	{
		free(bits);
		gs_d3d12_descriptor_heap_manager_init(m);
		return 0;
	}
	if (!shader_visible)
		m->base_gpu.ptr = 0;

	/* All free, and nothing past the heap's end: the last word has only
	 * the descriptors there are. */
	for (i = 0; i < words; i++)
		bits[i] = 0xFFFFFFFFu;
	if (num_descriptors % 32u)
		bits[words - 1] = (1u << (num_descriptors % 32u)) - 1u;

	m->num_descriptors = num_descriptors;
	m->shader_visible  = shader_visible;
	m->free_bits       = bits;
	m->free_words      = words;
	return 1;
}

void gs_d3d12_descriptor_heap_manager_destroy(gs_d3d12_descriptor_heap_manager* m)
{
	if (m->heap)
		gs_d3d12_device_release_descriptor_heap(m->heap);
	free(m->free_bits);
	gs_d3d12_descriptor_heap_manager_init(m);
}

int gs_d3d12_descriptor_heap_manager_allocate(gs_d3d12_descriptor_heap_manager* m, gs_d3d12_descriptor_handle* handle)
{
	u32 w;

	for (w = 0; w < m->free_words; w++)
	{
		const u32 word = m->free_bits[w];
		u32 bit        = 0;

		if (!word)
			continue;
		while (!(word & (1u << bit)))
			bit++;
		m->free_bits[w] = word & ~(1u << bit);
		gs_d3d12_descriptor_at(handle, w * 32u + bit, m->base_cpu, m->base_gpu, m->increment, m->shader_visible);
		return 1;
	}
	return 0;
}

void gs_d3d12_descriptor_heap_manager_free_index(gs_d3d12_descriptor_heap_manager* m, u32 index)
{
	if (index < m->num_descriptors)
		m->free_bits[index / 32u] |= 1u << (index % 32u);
}

void gs_d3d12_descriptor_heap_manager_free(gs_d3d12_descriptor_heap_manager* m, gs_d3d12_descriptor_handle* handle)
{
	if (handle->index == GS_D3D12_DESCRIPTOR_INVALID)
		return;
	gs_d3d12_descriptor_heap_manager_free_index(m, handle->index);
	gs_d3d12_descriptor_handle_clear(handle);
}

/* --- in runs --- */

void gs_d3d12_descriptor_allocator_init(gs_d3d12_descriptor_allocator* a)
{
	memset(a, 0, sizeof(*a));
}

int gs_d3d12_descriptor_allocator_create(gs_d3d12_descriptor_allocator* a, void* device, u32 type, u32 num_descriptors)
{
	gs_d3d12_descriptor_allocator_destroy(a);
	if (!gs_d3d12_device_create_descriptor_heap(device, type, num_descriptors, 1, &a->heap, &a->base_cpu,
			&a->base_gpu, &a->increment))
	{
		gs_d3d12_descriptor_allocator_init(a);
		return 0;
	}
	a->num_descriptors = num_descriptors;
	return 1;
}

void gs_d3d12_descriptor_allocator_destroy(gs_d3d12_descriptor_allocator* a)
{
	if (a->heap)
		gs_d3d12_device_release_descriptor_heap(a->heap);
	gs_d3d12_descriptor_allocator_init(a);
}

int gs_d3d12_descriptor_allocator_allocate(gs_d3d12_descriptor_allocator* a, u32 num_handles,
	gs_d3d12_descriptor_handle* out_base_handle)
{
	if (a->current_offset + num_handles > a->num_descriptors)
		return 0;
	gs_d3d12_descriptor_at(out_base_handle, a->current_offset, a->base_cpu, a->base_gpu, a->increment, 1);
	a->current_offset += num_handles;
	return 1;
}

void gs_d3d12_descriptor_allocator_reset(gs_d3d12_descriptor_allocator* a)
{
	a->current_offset = 0;
}

/* --- sampler groups --- */

typedef struct gs_d3d12_sampler_key
{
	u32 idx[GS_D3D12_SAMPLER_GROUP_MAX];
} gs_d3d12_sampler_key;

void gs_d3d12_sampler_allocator_init(gs_d3d12_sampler_allocator* s, u32 num_samplers)
{
	memset(s, 0, sizeof(*s));
	gs_d3d12_descriptor_allocator_init(&s->alloc);
	s->num_samplers = num_samplers;
	gs_flat_map_init(&s->groups, sizeof(gs_d3d12_sampler_key), sizeof(gs_d3d12_descriptor_handle));
}

int gs_d3d12_sampler_allocator_create(gs_d3d12_sampler_allocator* s, void* device, u32 num_descriptors)
{
	/* Room for a group per descriptor: no group is ever more, and none
	 * is made in play. */
	if (!gs_flat_map_reserve(&s->groups, num_descriptors))
		return 0;
	if (!gs_d3d12_descriptor_allocator_create(&s->alloc, device, GS_D3D12_DESCRIPTOR_HEAP_SAMPLER, num_descriptors))
		return 0;
	s->device = device;
	return 1;
}

void gs_d3d12_sampler_allocator_destroy(gs_d3d12_sampler_allocator* s)
{
	gs_d3d12_descriptor_allocator_destroy(&s->alloc);
	gs_flat_map_free(&s->groups);
	gs_flat_map_init(&s->groups, sizeof(gs_d3d12_sampler_key), sizeof(gs_d3d12_descriptor_handle));
	s->device = NULL;
}

/* The copy of key, num the samplers it takes, src where they are */
static int gs_d3d12_sampler_lookup(gs_d3d12_sampler_allocator* s, gs_d3d12_descriptor_handle* gpu_handle,
	const gs_d3d12_sampler_key* key, u32 num, const gs_d3d12_descriptor_handle* src)
{
	const size_t hash = gs_key_hash(key->idx, s->num_samplers * sizeof(u32));
	gs_d3d12_descriptor_handle* found =
		(gs_d3d12_descriptor_handle*)gs_flat_map_find(&s->groups, key, sizeof(*key), hash);
	int inserted;

	if (found)
	{
		*gpu_handle = *found;
		return 1;
	}
	if (!gs_d3d12_descriptor_allocator_allocate(&s->alloc, num, gpu_handle))
		return 0;

	if (num == 1)
		gs_d3d12_device_copy_sampler(s->device, gpu_handle->cpu_handle, src->cpu_handle);
	else
		gs_d3d12_device_copy_sampler_group(s->device, gpu_handle->cpu_handle, src, num);

	found = (gs_d3d12_descriptor_handle*)gs_flat_map_insert(&s->groups, key, hash, &inserted);
	if (found)
		*found = *gpu_handle;
	return 1;
}

int gs_d3d12_sampler_allocator_lookup_single(gs_d3d12_sampler_allocator* s, gs_d3d12_descriptor_handle* gpu_handle,
	const gs_d3d12_descriptor_handle* cpu_handle)
{
	gs_d3d12_sampler_key key;
	memset(&key, 0, sizeof(key));
	key.idx[0] = cpu_handle->index;
	return gs_d3d12_sampler_lookup(s, gpu_handle, &key, 1, cpu_handle);
}

int gs_d3d12_sampler_allocator_lookup_group(gs_d3d12_sampler_allocator* s, gs_d3d12_descriptor_handle* gpu_handle,
	const gs_d3d12_descriptor_handle* cpu_handles)
{
	gs_d3d12_sampler_key key;
	u32 i;
	memset(&key, 0, sizeof(key));
	for (i = 0; i < s->num_samplers; i++)
		key.idx[i] = cpu_handles[i].index;
	return gs_d3d12_sampler_lookup(s, gpu_handle, &key, s->num_samplers, cpu_handles);
}

void gs_d3d12_sampler_allocator_invalidate(gs_d3d12_sampler_allocator* s)
{
	gs_flat_map_clear(&s->groups);
}

void gs_d3d12_sampler_allocator_reset(gs_d3d12_sampler_allocator* s)
{
	gs_flat_map_clear(&s->groups);
	gs_d3d12_descriptor_allocator_reset(&s->alloc);
}

int gs_d3d12_sampler_allocator_should_reset(const gs_d3d12_sampler_allocator* s)
{
	return s->groups.count >= GS_D3D12_MAX_SHADER_VISIBLE_SAMPLERS / 2;
}
