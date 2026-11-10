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

#define NOMINMAX
#include "descriptor_set.hpp"
#include "device.hpp"
#include <vector>

using namespace Util;

namespace Vulkan
{
DescriptorSetAllocator::DescriptorSetAllocator(Hash hash, Device *device_, const DescriptorSetLayout &layout,
                                               const uint32_t *stages_for_binds,
                                               const ImmutableSampler * const *immutable_samplers)
	: IntrusiveHashMapEnabled<DescriptorSetAllocator>(hash)
	, device(device_)
	, table(device_->get_device_table())
{
	bindless = layout.meta[0].array_size == DescriptorSetLayout::UNSIZED_ARRAY;

	if (!bindless)
	{
		unsigned count = device_->num_thread_indices * device_->per_frame.size();
		per_thread_and_frame.resize(count);
	}

	if (bindless && !device->get_device_features().vk12_features.descriptorIndexing)
	{
		LOGE("Cannot support descriptor indexing on this device.\n");
		return;
	}

	VkDescriptorSetLayoutCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	VkDescriptorSetLayoutBindingFlagsCreateInfoEXT flags = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT };
	VkSampler vk_immutable_samplers[VULKAN_NUM_BINDINGS] = {};
	std::vector<VkDescriptorSetLayoutBinding> bindings;
	VkDescriptorBindingFlagsEXT binding_flags = 0;

	if (bindless)
	{
		if (!device->ext.supports_descriptor_buffer)
			info.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
		info.pNext = &flags;

		flags.bindingCount = 1;
		flags.pBindingFlags = &binding_flags;

		binding_flags = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
		if (!device->ext.supports_descriptor_buffer)
		{
			// These flags are implied when using descriptor buffer.
			binding_flags |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
			                 VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
		}
	}

	if (device->ext.supports_descriptor_buffer)
		info.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;

	for (unsigned i = 0; i < VULKAN_NUM_BINDINGS; i++)
	{
		auto stages = stages_for_binds[i];
		if (stages == 0)
			continue;

		unsigned array_size = layout.meta[i].array_size;
		unsigned pool_array_size;
		if (array_size == DescriptorSetLayout::UNSIZED_ARRAY)
		{
			array_size = VULKAN_NUM_BINDINGS_BINDLESS_VARYING;
			pool_array_size = array_size;
		}
		else
			pool_array_size = array_size * VULKAN_NUM_SETS_PER_POOL;

		unsigned types = 0;
		if (layout.sampled_image_mask & (1u << i))
		{
			if ((layout.immutable_sampler_mask & (1u << i)) && immutable_samplers && immutable_samplers[i])
				vk_immutable_samplers[i] = immutable_samplers[i]->get_sampler().get_sampler();

			bindings.push_back({ i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, array_size, stages,
			                     vk_immutable_samplers[i] != VK_NULL_HANDLE ? &vk_immutable_samplers[i] : nullptr });
			pool_size.push_back({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, pool_array_size });
			types++;
		}

		if (layout.sampled_texel_buffer_mask & (1u << i))
		{
			bindings.push_back({ i, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, array_size, stages, nullptr });
			pool_size.push_back({ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, pool_array_size });
			types++;
		}

		if (layout.storage_texel_buffer_mask & (1u << i))
		{
			bindings.push_back({ i, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, array_size, stages, nullptr });
			pool_size.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, pool_array_size });
			types++;
		}

		if (layout.storage_image_mask & (1u << i))
		{
			bindings.push_back({ i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, array_size, stages, nullptr });
			pool_size.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, pool_array_size });
			types++;
		}

		if (layout.uniform_buffer_mask & (1u << i))
		{
			auto type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			bindings.push_back({ i, type, array_size, stages, nullptr });
			pool_size.push_back({ type, pool_array_size });
			types++;
		}

		if (layout.storage_buffer_mask & (1u << i))
		{
			bindings.push_back({ i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, array_size, stages, nullptr });
			pool_size.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, pool_array_size });
			types++;
		}

		if (layout.rtas_mask & (1u << i))
		{
			bindings.push_back({ i, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, array_size, stages, nullptr });
			pool_size.push_back({ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, pool_array_size });
			types++;
		}

		if (layout.input_attachment_mask & (1u << i))
		{
			bindings.push_back({ i, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, array_size, stages, nullptr });
			pool_size.push_back({ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, pool_array_size });
			types++;
		}

		if (layout.separate_image_mask & (1u << i))
		{
			bindings.push_back({ i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, array_size, stages, nullptr });
			pool_size.push_back({ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, pool_array_size });
			types++;
		}

		if (layout.sampler_mask & (1u << i))
		{
			if ((layout.immutable_sampler_mask & (1u << i)) && immutable_samplers && immutable_samplers[i])
			{
				if (!device->get_device_features().supports_descriptor_buffer)
					vk_immutable_samplers[i] = immutable_samplers[i]->get_sampler().get_sampler();
				else
					LOGE("Cannot use immutable samplers with descriptor buffer. Ignoring.\n");
			}

			bindings.push_back({ i, VK_DESCRIPTOR_TYPE_SAMPLER, array_size, stages,
			                     vk_immutable_samplers[i] != VK_NULL_HANDLE ? &vk_immutable_samplers[i] : nullptr });
			pool_size.push_back({ VK_DESCRIPTOR_TYPE_SAMPLER, pool_array_size });
			types++;
		}

		(void)types;
		VK_ASSERT(types <= 1 && "Descriptor set aliasing!");
	}

	if (!bindings.empty())
	{
		info.bindingCount = bindings.size();
		info.pBindings = bindings.data();

		if (bindless && bindings.size() != 1)
		{
			LOGE("Using bindless but have bindingCount != 1.\n");
			return;
		}
	}

	bool heap = device->get_device_features().descriptor_heap_features.descriptorHeap == VK_TRUE;

	if (!heap)
	{
#ifdef VULKAN_DEBUG
		LOGI("Creating descriptor set layout.\n");
#endif
		if (table.vkCreateDescriptorSetLayout(device->get_device(), &info, nullptr, &set_layout_pool) != VK_SUCCESS)
			LOGE("Failed to create descriptor set layout.");
	}

	if (device->ext.supports_descriptor_buffer && !heap)
	{
		// Query the memory layout.
		table.vkGetDescriptorSetLayoutSizeEXT(device->get_device(), set_layout_pool, &desc_set_size);

		if (bindless)
		{
			table.vkGetDescriptorSetLayoutBindingOffsetEXT(
					device->get_device(), set_layout_pool, 0, &desc_set_variable_offset);
		}
		else
		{
			for (auto &bind : bindings)
			{
				VkDeviceSize offset = 0;
				VkDeviceSize stride = device->managers.descriptor_buffer.get_descriptor_size_for_type(bind.descriptorType);

				table.vkGetDescriptorSetLayoutBindingOffsetEXT(
						device->get_device(), set_layout_pool, bind.binding, &offset);

				for (uint32_t i = 0; i < bind.descriptorCount; i++)
					desc_offsets[bind.binding + i] = offset + i * stride;
			}
		}
	}


	// Push descriptors is not used with descriptor buffer.
	if (!bindless && device->get_device_features().vk14_features.pushDescriptor &&
	    !heap && !device->get_device_features().descriptor_buffer_features.descriptorBuffer)
	{
		info.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
		if (table.vkCreateDescriptorSetLayout(device->get_device(), &info, nullptr, &set_layout_push) != VK_SUCCESS)
			LOGE("Failed to create descriptor set layout.");
	}
}

void DescriptorSetAllocator::reset_bindless_pool(VkDescriptorPool pool)
{
	table.vkResetDescriptorPool(device->get_device(), pool, 0);
}

BindlessDescriptorSet DescriptorSetAllocator::allocate_bindless_set(VkDescriptorPool pool, unsigned num_descriptors)
{
	if (!pool || !bindless)
		return {};

	VkDescriptorSetAllocateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	info.descriptorPool = pool;
	info.descriptorSetCount = 1;
	info.pSetLayouts = &set_layout_pool;

	VkDescriptorSetVariableDescriptorCountAllocateInfoEXT count_info =
			{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT };

	uint32_t num_desc = num_descriptors;
	count_info.descriptorSetCount = 1;
	count_info.pDescriptorCounts = &num_desc;
	info.pNext = &count_info;

	BindlessDescriptorSet desc_set;
	if (table.vkAllocateDescriptorSets(device->get_device(), &info, &desc_set.handle.set) != VK_SUCCESS)
		return {};

	desc_set.valid = true;
	return desc_set;
}

DescriptorBufferAllocation DescriptorSetAllocator::allocate_bindless_buffer(unsigned num_sets, unsigned num_descriptors)
{
	if (!bindless)
		return {};

	VkDeviceSize size = get_variable_offset() * num_sets +
	                    device->managers.descriptor_buffer.get_descriptor_size_for_type(pool_size[0].type) *
	                    num_descriptors;

	size += (std::max<uint32_t>(num_sets, 1u) - 1u) *
			std::max<VkDeviceSize>(
				device->get_device_features().resource_heap_offset_alignment,
				device->get_device_features().resource_heap_resource_desc_size);

	return device->managers.descriptor_buffer.allocate(size);
}

VkDeviceSize DescriptorSetAllocator::get_variable_size(unsigned count) const
{
	return get_variable_offset() +
	       device->managers.descriptor_buffer.get_descriptor_size_for_type(pool_size[0].type) *
	       count;
}

VkDescriptorPool DescriptorSetAllocator::allocate_bindless_pool(unsigned num_sets, unsigned num_descriptors)
{
	if (!bindless)
		return VK_NULL_HANDLE;

	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorPoolCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT;
	info.maxSets = num_sets;
	info.poolSizeCount = 1;

	VkDescriptorPoolSize size = pool_size[0];
	size.descriptorCount = num_descriptors;
	info.pPoolSizes = &size;

	if (table.vkCreateDescriptorPool(device->get_device(), &info, nullptr, &pool) != VK_SUCCESS)
	{
		LOGE("Failed to create descriptor pool.\n");
		return VK_NULL_HANDLE;
	}

	return pool;
}

void DescriptorSetAllocator::begin_frame()
{
	if (!bindless)
	{
		// This can only be called in a situation where no command buffers are alive,
		// so we don't need to consider any locks here.
		if (device->per_frame.size() * device->num_thread_indices != per_thread_and_frame.size())
			per_thread_and_frame.resize(device->per_frame.size() * device->num_thread_indices);

		// It would be safe to set all offsets to 0 here, but that's a little wasteful.
		for (uint32_t i = 0; i < device->num_thread_indices; i++)
			per_thread_and_frame[i * device->per_frame.size() + device->frame_context_index].offset = 0;
	}
}

VkDescriptorSet DescriptorSetAllocator::request_descriptor_set(unsigned thread_index, unsigned frame_index)
{
	VK_ASSERT(!bindless);

	size_t flattened_index = thread_index * device->per_frame.size() + frame_index;

	auto &state = per_thread_and_frame[flattened_index];

	unsigned pool_index = state.offset / VULKAN_NUM_SETS_PER_POOL;
	unsigned pool_offset = state.offset % VULKAN_NUM_SETS_PER_POOL;

	if (pool_index >= state.pools.size())
	{
		Pool *pool = state.object_pool.allocate();

		VkDescriptorPoolCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		info.maxSets = VULKAN_NUM_SETS_PER_POOL;
		if (!pool_size.empty())
		{
			info.poolSizeCount = pool_size.size();
			info.pPoolSizes = pool_size.data();
		}

		bool overallocation =
		    device->get_device_features().descriptor_pool_overallocation_features.descriptorPoolOverallocation ==
		    VK_TRUE;

		if (overallocation)
		{
			// No point in allocating new pools if we can keep using the existing one.
			info.flags |= VK_DESCRIPTOR_POOL_CREATE_ALLOW_OVERALLOCATION_POOLS_BIT_NV |
			              VK_DESCRIPTOR_POOL_CREATE_ALLOW_OVERALLOCATION_SETS_BIT_NV;
		}

		bool need_alloc = !overallocation || state.pools.empty();

		pool->pool = VK_NULL_HANDLE;
		if (need_alloc && table.vkCreateDescriptorPool(device->get_device(), &info, nullptr, &pool->pool) != VK_SUCCESS)
		{
			LOGE("Failed to create descriptor pool.\n");
			state.object_pool.free(pool);
			return VK_NULL_HANDLE;
		}

		VkDescriptorSetLayout layouts[VULKAN_NUM_SETS_PER_POOL];
		std::fill(std::begin(layouts), std::end(layouts), set_layout_pool);

		VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		alloc.descriptorPool = pool->pool != VK_NULL_HANDLE ? pool->pool : state.pools.front()->pool;
		alloc.descriptorSetCount = VULKAN_NUM_SETS_PER_POOL;
		alloc.pSetLayouts = layouts;

		if (table.vkAllocateDescriptorSets(device->get_device(), &alloc, pool->sets) != VK_SUCCESS)
			LOGE("Failed to allocate descriptor sets.\n");
		state.pools.push_back(pool);
	}

	VkDescriptorSet vk_set = state.pools[pool_index]->sets[pool_offset];
	state.offset++;
	return vk_set;
}

void DescriptorSetAllocator::clear()
{
	for (auto &state : per_thread_and_frame)
	{
		for (auto *obj : state.pools)
		{
			table.vkDestroyDescriptorPool(device->get_device(), obj->pool, nullptr);
			state.object_pool.free(obj);
		}
		state.pools.clear();
		state.offset = 0;
		state.object_pool = {};
	}
}

DescriptorSetAllocator::~DescriptorSetAllocator()
{
	table.vkDestroyDescriptorSetLayout(device->get_device(), set_layout_pool, nullptr);
	table.vkDestroyDescriptorSetLayout(device->get_device(), set_layout_push, nullptr);
	clear();
}

BindlessDescriptorPool::BindlessDescriptorPool(Device *device_, DescriptorSetAllocator *allocator_,
                                               VkDescriptorPool pool, uint32_t num_sets, uint32_t num_desc)
	: device(device_), allocator(allocator_), desc_pool(pool),
	  total_descriptors(pool ? std::min<uint32_t>(num_desc, VULKAN_NUM_BINDINGS_BINDLESS_VARYING) : num_desc)
{
	if (!desc_pool)
		bindless_buffer = allocator->allocate_bindless_buffer(num_sets, num_desc);
}

BindlessDescriptorPool::~BindlessDescriptorPool()
{
	if (desc_pool)
	{
		if (internal_sync)
			device->destroy_descriptor_pool_nolock(desc_pool);
		else
			device->destroy_descriptor_pool(desc_pool);
	}

	if (bindless_buffer.get_size() != 0)
	{
		if (internal_sync)
			device->free_descriptor_buffer_allocation_nolock(bindless_buffer);
		else
			device->free_descriptor_buffer_allocation(bindless_buffer);
	}
}

BindlessDescriptorSet BindlessDescriptorPool::get_descriptor_set() const
{
	return desc_set;
}

uint32_t BindlessDescriptorPool::get_descriptor_base() const
{
	return desc_pool ? descriptor_base : 0;
}

void BindlessDescriptorPool::reset()
{
	if (desc_pool != VK_NULL_HANDLE)
		allocator->reset_bindless_pool(desc_pool);
	desc_set = {};
	descriptor_base = 0;
	allocated_descriptor_count = 0;
	bindless_buffer_offset = 0;
}

bool BindlessDescriptorPool::allocate_descriptors(unsigned count)
{
	if (device->get_device_features().supports_descriptor_buffer_or_heap)
	{
		auto alignment = std::max<VkDeviceSize>(
				device->get_device_features().resource_heap_offset_alignment,
				device->get_device_features().resource_heap_resource_desc_size);

		bindless_buffer_offset = (bindless_buffer_offset + alignment - 1) & ~(alignment - 1);
		VkDeviceSize size = allocator->get_variable_size(count);

		desc_set = {};
		if (bindless_buffer_offset + size <= bindless_buffer.get_size())
		{
			desc_set.handle.offset = bindless_buffer_offset + bindless_buffer.get_offset();
			desc_set.valid = true;
			bindless_buffer_offset += size;

			allocated_descriptor_count += count;
		}

		info_ptrs.reserve(count);
	}
	else
	{
		// One set a pool, of every descriptor the pool has, and each
		// allocation a range of it from where the last ended: a set is
		// allocated once, written at its range while bound (update after
		// bind), and a driver that backs a set with its layout's whole
		// capacity, whatever its count, is charged for one set rather than
		// one per draw.
		if (allocated_descriptor_count + count > total_descriptors)
			return false;
		if (!desc_set)
		{
			desc_set = allocator->allocate_bindless_set(desc_pool, total_descriptors);
			if (!desc_set)
				return false;
		}

		descriptor_base = allocated_descriptor_count;
		allocated_descriptor_count += count;
		infos.reserve(count);
	}

	write_count = 0;
	return bool(desc_set);
}

void BindlessDescriptorPool::push_texture(const ImageView &view)
{
	// TODO: Deal with integer view for depth-stencil images?
	if (!desc_pool)
		push_texture(view.get_float_view().sampled.ptr);
	else
		push_texture(view.get_float_view().view, view.get_image().get_layout(VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL));
}

void BindlessDescriptorPool::push_texture(VkImageView view, VkImageLayout layout)
{
	VK_ASSERT(write_count < infos.get_capacity());
	auto &image_info = infos[write_count];
	image_info = { VK_NULL_HANDLE, view, layout };
	write_count++;
}

void BindlessDescriptorPool::push_texture(const uint8_t *ptr)
{
	VK_ASSERT(write_count < info_ptrs.get_capacity());
	info_ptrs[write_count++] = ptr;
}

void BindlessDescriptorPool::update()
{
	if (device->get_device_features().supports_descriptor_buffer_or_heap)
	{
		device->managers.descriptor_buffer.copy_sampled_image_n(
				device->managers.descriptor_buffer.get_resource_heap().mapped +
				desc_set.handle.offset + allocator->get_variable_offset(),
				info_ptrs.data(), write_count);
	}
	else
	{
		VkWriteDescriptorSet desc = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		desc.descriptorCount = write_count;
		desc.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		desc.dstSet = desc_set.handle.set;
		desc.dstArrayElement = descriptor_base;

		desc.pImageInfo = infos.data();
		desc.pBufferInfo = nullptr;
		desc.pTexelBufferView = nullptr;

		if (write_count)
		{
			auto &table = device->get_device_table();
			table.vkUpdateDescriptorSets(device->get_device(), 1, &desc, 0, nullptr);
		}
	}
}

void BindlessDescriptorPoolDeleter::operator()(BindlessDescriptorPool *pool)
{
	pool->device->handle_pool.bindless_descriptor_pool.free(pool);
}

}
