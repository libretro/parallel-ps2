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

#pragma once

#include "vulkan_headers.hpp"
#include "vulkan_common.hpp"
#include "../../../Renderers/Vulkan/GSVulkanHeap.h"
#include <stddef.h>
#include <stdint.h>

/* Device memory is the core's heap (GS/Renderers/Vulkan/GSVulkanHeap.c),
 * the one the Vulkan renderer uses: blocks taken from the driver and
 * kept, allocations an offset inside one, host-visible blocks mapped
 * once. On top of what that renderer asks of it, this asks for memory
 * priorities, device addresses for buffers, dedicated memory where the
 * driver prefers it, and a limit on a small shared PCI BAR heap. */

namespace Vulkan
{
class Device;


/* What the memory is for: whether it is a buffer (linear) or an image,
 * and its priority under pressure. */
enum class AllocationMode : uint8_t
{
	LinearHostMappable = 0,
	LinearDevice,
	LinearDeviceHighPriority,
	OptimalResource,
	OptimalRenderTarget,
	Count
};

enum MemoryAccessFlag : uint32_t
{
	MEMORY_ACCESS_WRITE_BIT = 1,
	MEMORY_ACCESS_READ_BIT = 2,
	MEMORY_ACCESS_READ_WRITE_BIT = MEMORY_ACCESS_WRITE_BIT | MEMORY_ACCESS_READ_BIT
};
using MemoryAccessFlags = uint32_t;

class DeviceAllocator;

struct DeviceAllocation
{
	friend class DeviceAllocator;

public:
	inline VkDeviceMemory get_memory() const
	{
		return alloc.memory;
	}

	inline VkDeviceSize get_offset() const
	{
		return alloc.offset;
	}

	inline VkDeviceSize get_size() const
	{
		return alloc.size;
	}

	void free_immediate(DeviceAllocator &allocator);

private:
	gs_vk_alloc_t alloc = {};
};

struct HeapBudget
{
	VkDeviceSize max_size;
	VkDeviceSize budget_size;
	VkDeviceSize tracked_usage;
	VkDeviceSize device_usage;
};

class DeviceAllocator
{
public:
	void init(Device *device);
	~DeviceAllocator();

	bool allocate_generic_memory(VkDeviceSize size, VkDeviceSize alignment, AllocationMode mode,
	                             uint32_t memory_type, DeviceAllocation *alloc);
	bool allocate_image_memory(VkDeviceSize size, VkDeviceSize alignment, AllocationMode mode,
	                           uint32_t memory_type, VkImage image, DeviceAllocation *alloc);
	void free(DeviceAllocation &alloc);

	/* Empty blocks back to the driver. */
	void garbage_collect();
	void *map_memory(const DeviceAllocation &alloc, MemoryAccessFlags flags, VkDeviceSize offset, VkDeviceSize length);
	void unmap_memory(const DeviceAllocation &alloc, MemoryAccessFlags flags, VkDeviceSize offset, VkDeviceSize length);

	void get_memory_budget(HeapBudget *heaps);

private:
	bool allocate(VkDeviceSize size, VkDeviceSize alignment, AllocationMode mode, bool linear,
	              uint32_t memory_type, VkImage dedicated, DeviceAllocation *alloc);
	void range(const DeviceAllocation &alloc, VkDeviceSize offset, VkDeviceSize length,
	           VkMappedMemoryRange *out) const;

	Device *device = nullptr;
	const VulkanDeviceTable *table = nullptr;
	gs_vk_heap_t heap = {};
	bool initialized = false;
};

// Avoid cross-dependency in header.
class Buffer;

struct CachedImageView
{
	VkImageView view;
};

struct CachedBufferView
{
	VkBufferView view;
};

struct BufferViewCreateInfo;

/* Makes and frees the device's image views, buffer views and samplers. */
class ViewFactory
{
public:
	bool init(Device *device);
	void teardown() {}

	bool create_image_view(const VkImageViewCreateInfo &info, VkImageUsageFlags usage, CachedImageView &view);
	void free_image_view(const CachedImageView &view);
	bool create_buffer_view(const BufferViewCreateInfo &info, CachedBufferView &view);
	void free_buffer_view(const CachedBufferView &view);

	VkSampler create_sampler(const VkSamplerCreateInfo *info);
	void destroy_sampler(VkSampler sampler);

private:
	Device *device = nullptr;
};
}
