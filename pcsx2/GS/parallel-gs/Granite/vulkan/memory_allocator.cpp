/* Copyright (c) 2017-2026 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "memory_allocator.hpp"
#include "device.hpp"
#include <string.h>

namespace Vulkan
{
/* 64 MB blocks, an eighth of a smaller memory heap (the heap's rule). */
static constexpr VkDeviceSize block_size = 64ull * 1024 * 1024;

/* The heap's classes: low, middle, high. */
static unsigned mode_priority(AllocationMode mode)
{
	switch (mode)
	{
	case AllocationMode::LinearDeviceHighPriority:
	case AllocationMode::OptimalRenderTarget:
		return 3;
	case AllocationMode::LinearDevice:
	case AllocationMode::OptimalResource:
		return 2;
	default:
		return 1;
	}
}

void DeviceAllocation::free_immediate(DeviceAllocator &allocator)
{
	allocator.free(*this);
}

void DeviceAllocator::init(Device *device_)
{
	device = device_;
	table = &device->get_device_table();
	const auto &features = device->get_device_features();
	const auto &mem_props = device->get_memory_properties();

	gs_vk_heap_fns_t fns = {};
	fns.allocate_memory = table->vkAllocateMemory;
	fns.free_memory = table->vkFreeMemory;
	fns.map_memory = table->vkMapMemory;
	fns.unmap_memory = table->vkUnmapMemory;
	fns.flush_ranges = table->vkFlushMappedMemoryRanges;
	fns.invalidate_ranges = table->vkInvalidateMappedMemoryRanges;
	initialized = gs_vk_heap_init(&heap, device->get_device(), &mem_props, &fns, block_size,
	                              device->get_gpu_properties().limits.nonCoherentAtomSize, 0) != 0;

	heap.use_priority = features.memory_priority_features.memoryPriority ? 1 : 0;
	if (features.vk12_features.bufferDeviceAddress)
		heap.linear_flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

	/* A PCI BAR heap - device-local and host-visible, smaller than both
	 * the device-only and the host-only heap, so not resizable BAR - is
	 * shared with the driver and the frontend and goes unstable when
	 * overused: no block past its budget, and buffers that wanted it
	 * fall back to host memory. */
	VkMemoryPropertyFlags combined[VK_MAX_MEMORY_HEAPS] = {};
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
		combined[mem_props.memoryTypes[i].heapIndex] |= mem_props.memoryTypes[i].propertyFlags;

	const VkMemoryPropertyFlags pinned = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	VkDeviceSize host_heap = 0, device_heap = 0;
	for (uint32_t i = 0; i < mem_props.memoryHeapCount; i++)
	{
		if ((combined[i] & pinned) == VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT && mem_props.memoryHeaps[i].size > host_heap)
			host_heap = mem_props.memoryHeaps[i].size;
		else if ((combined[i] & pinned) == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && mem_props.memoryHeaps[i].size > device_heap)
			device_heap = mem_props.memoryHeaps[i].size;
	}

	if (host_heap && device_heap)
	{
		HeapBudget budgets[VK_MAX_MEMORY_HEAPS];
		get_memory_budget(budgets);
		for (uint32_t i = 0; i < mem_props.memoryHeapCount; i++)
			if ((combined[i] & pinned) == pinned &&
			    mem_props.memoryHeaps[i].size < host_heap && mem_props.memoryHeaps[i].size < device_heap)
				heap.heap_limit[i] = budgets[i].budget_size;
	}
}

DeviceAllocator::~DeviceAllocator()
{
	if (initialized)
		gs_vk_heap_shutdown(&heap);
}

bool DeviceAllocator::allocate(VkDeviceSize size, VkDeviceSize alignment, AllocationMode mode, bool linear,
                               uint32_t memory_type, VkImage dedicated, DeviceAllocation *alloc)
{
	gs_vk_alloc_request_t request = {};
	request.req.size = size;
	request.req.alignment = alignment;
	request.req.memoryTypeBits = 1u << memory_type;
	request.linear = linear ? 1 : 0;
	request.priority = mode_priority(mode);
	request.dedicated_image = dedicated;

	*alloc = {};
	return initialized && gs_vk_heap_alloc_request(&heap, &request, &alloc->alloc) != 0;
}

bool DeviceAllocator::allocate_generic_memory(VkDeviceSize size, VkDeviceSize alignment, AllocationMode mode,
                                              uint32_t memory_type, DeviceAllocation *alloc)
{
	/* Buffers. */
	return allocate(size, alignment, mode, true, memory_type, VK_NULL_HANDLE, alloc);
}

bool DeviceAllocator::allocate_image_memory(VkDeviceSize size, VkDeviceSize alignment, AllocationMode mode,
                                            uint32_t memory_type, VkImage image, DeviceAllocation *alloc)
{
	VkMemoryDedicatedRequirements dedicated_req = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
	VkImageMemoryRequirementsInfo2 info = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 };
	VkMemoryRequirements2 mem_req = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
	info.image = image;
	mem_req.pNext = &dedicated_req;
	table->vkGetImageMemoryRequirements2(device->get_device(), &info, &mem_req);

	const bool dedicated = dedicated_req.prefersDedicatedAllocation || dedicated_req.requiresDedicatedAllocation;
	return allocate(size, alignment, mode, false, memory_type, dedicated ? image : VK_NULL_HANDLE, alloc);
}

void DeviceAllocator::free(DeviceAllocation &alloc)
{
	if (alloc.alloc.memory != VK_NULL_HANDLE)
		gs_vk_heap_free(&heap, &alloc.alloc);
	alloc.alloc = {};
}

void DeviceAllocator::garbage_collect()
{
	if (initialized)
		gs_vk_heap_trim(&heap);
}

/* A range of an allocation, on nonCoherentAtomSize. */
void DeviceAllocator::range(const DeviceAllocation &alloc, VkDeviceSize offset, VkDeviceSize length,
                            VkMappedMemoryRange *out) const
{
	const VkDeviceSize atom = heap.atom_size;
	const VkDeviceSize block = heap.blocks[alloc.alloc.block].size;
	VkDeviceSize begin = alloc.alloc.offset + offset;
	VkDeviceSize end = begin + length;

	begin &= ~(atom - 1);
	end = (end + atom - 1) & ~(atom - 1);
	if (end > block)
		end = block;

	*out = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
	out->memory = alloc.alloc.memory;
	out->offset = begin;
	out->size = end - begin;
}

void *DeviceAllocator::map_memory(const DeviceAllocation &alloc, MemoryAccessFlags flags,
                                  VkDeviceSize offset, VkDeviceSize length)
{
	/* Device-local memory, which cannot be mapped. */
	if (!alloc.alloc.mapped)
		return nullptr;

	if ((flags & MEMORY_ACCESS_READ_BIT) &&
	    !(heap.props.memoryTypes[alloc.alloc.type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
	{
		VkMappedMemoryRange r;
		range(alloc, offset, length, &r);
		table->vkInvalidateMappedMemoryRanges(device->get_device(), 1, &r);
	}

	return static_cast<uint8_t *>(alloc.alloc.mapped) + offset;
}

void DeviceAllocator::unmap_memory(const DeviceAllocation &alloc, MemoryAccessFlags flags,
                                   VkDeviceSize offset, VkDeviceSize length)
{
	if (!alloc.alloc.mapped)
		return;

	if ((flags & MEMORY_ACCESS_WRITE_BIT) &&
	    !(heap.props.memoryTypes[alloc.alloc.type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
	{
		VkMappedMemoryRange r;
		range(alloc, offset, length, &r);
		table->vkFlushMappedMemoryRanges(device->get_device(), 1, &r);
	}
}

void DeviceAllocator::get_memory_budget(HeapBudget *heap_budgets)
{
	const auto &mem_props = device->get_memory_properties();
	const uint32_t num_heaps = mem_props.memoryHeapCount;

	if (device->get_device_features().supports_memory_budget)
	{
		VkPhysicalDeviceMemoryProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
		VkPhysicalDeviceMemoryBudgetPropertiesEXT budget_props =
			{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
		props.pNext = &budget_props;
		vkGetPhysicalDeviceMemoryProperties2(device->get_physical_device(), &props);

		for (uint32_t i = 0; i < num_heaps; i++)
		{
			auto &b = heap_budgets[i];
			b.max_size = mem_props.memoryHeaps[i].size;
			b.budget_size = budget_props.heapBudget[i];
			b.device_usage = budget_props.heapUsage[i];
			b.tracked_usage = heap.heap_reserved[i];
		}
	}
	else
	{
		for (uint32_t i = 0; i < num_heaps; i++)
		{
			auto &b = heap_budgets[i];
			b.max_size = mem_props.memoryHeaps[i].size;
			/* Three quarters. */
			b.budget_size = b.max_size - (b.max_size / 4);
			b.tracked_usage = heap.heap_reserved[i];
			b.device_usage = heap.heap_reserved[i];
		}
	}
}

bool ViewFactory::init(Vulkan::Device *device_)
{
	device = device_;
	return true;
}

VkSampler ViewFactory::create_sampler(const VkSamplerCreateInfo *info)
{
	VkSampler samp = VK_NULL_HANDLE;
	if (device->get_device_table().vkCreateSampler(device->get_device(), info, nullptr, &samp) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return samp;
}

void ViewFactory::destroy_sampler(VkSampler sampler)
{
	device->get_device_table().vkDestroySampler(device->get_device(), sampler, nullptr);
}

bool ViewFactory::create_image_view(const VkImageViewCreateInfo &info, VkImageUsageFlags usage,
                                    CachedImageView &view)
{
	VkImageViewUsageCreateInfo view_usage_create_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
	auto tmpinfo = info;
	view = {};
	view_usage_create_info.usage = usage;
	view_usage_create_info.pNext = tmpinfo.pNext;
	tmpinfo.pNext = &view_usage_create_info;
	return device->get_device_table().vkCreateImageView(device->get_device(), &tmpinfo, nullptr, &view.view) == VK_SUCCESS;
}

void ViewFactory::free_image_view(const CachedImageView &view)
{
	if (view.view)
		device->get_device_table().vkDestroyImageView(device->get_device(), view.view, nullptr);
}

bool ViewFactory::create_buffer_view(const BufferViewCreateInfo &info, CachedBufferView &view)
{
	VkBufferViewCreateInfo vk_info = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO };
	view = {};
	vk_info.buffer = info.buffer->get_buffer();
	vk_info.format = info.format;
	vk_info.offset = info.offset;
	vk_info.range = info.range;
	return device->get_device_table().vkCreateBufferView(device->get_device(), &vk_info, nullptr, &view.view) == VK_SUCCESS;
}

void ViewFactory::free_buffer_view(const CachedBufferView &view)
{
	if (view.view)
		device->get_device_table().vkDestroyBufferView(device->get_device(), view.view, nullptr);
}
}
