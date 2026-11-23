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
#include "shader.hpp"
#include "device.hpp"

using namespace Util;

namespace Vulkan
{
void PipelineLayout::init_legacy()
{
	VkDescriptorSetLayout layouts[VULKAN_NUM_DESCRIPTOR_SETS] = {};
	unsigned num_sets = 0;
	for (unsigned i = 0; i < VULKAN_NUM_DESCRIPTOR_SETS; i++)
	{
		set_allocators[i] = device->request_descriptor_set_allocator(layout.sets[i], layout.stages_for_bindings[i]);
		layouts[i] = set_allocators[i]->get_layout_for_pool();
		if (layout.descriptor_set_mask & (1u << i))
		{
			num_sets = i + 1;

			// Assume the last set index in layout is the highest frequency update one, make that push descriptor if possible.
			// Only one descriptor set can be push descriptor.
			bool has_push_layout = set_allocators[i]->get_layout_for_push() != VK_NULL_HANDLE;
			if (has_push_layout)
				push_set_index = i;
		}
	}

	if (push_set_index != UINT32_MAX)
		layouts[push_set_index] = set_allocators[push_set_index]->get_layout_for_push();

	if (num_sets > VULKAN_NUM_DESCRIPTOR_SETS)
		LOGE("Number of sets %u exceeds limit of %u.\n", num_sets, VULKAN_NUM_DESCRIPTOR_SETS);

	VkPipelineLayoutCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	if (num_sets)
	{
		info.setLayoutCount = num_sets;
		info.pSetLayouts = layouts;
	}

	if (layout.push_constant_range.stageFlags != 0)
	{
		info.pushConstantRangeCount = 1;
		info.pPushConstantRanges = &layout.push_constant_range;
	}

	auto &table = device->get_device_table();
	if (table.vkCreatePipelineLayout(device->get_device(), &info, nullptr, &pipe_layout) != VK_SUCCESS)
		LOGE("Failed to create pipeline layout.\n");

	create_update_templates();
}

PipelineLayout::PipelineLayout(Hash hash, Device *device_, const CombinedResourceLayout &layout_)
	: IntrusiveHashMapEnabled<PipelineLayout>(hash)
	, device(device_)
	, layout(layout_)
{
	init_legacy();
}

void PipelineLayout::create_update_templates()
{
	auto &table = device->get_device_table();
	for (unsigned desc_set = 0; desc_set < VULKAN_NUM_DESCRIPTOR_SETS; desc_set++)
	{
		if ((layout.descriptor_set_mask & (1u << desc_set)) == 0)
			continue;
		if ((layout.bindless_descriptor_set_mask & (1u << desc_set)) != 0)
			continue;

		VkDescriptorUpdateTemplateEntry update_entries[VULKAN_NUM_BINDINGS];
		uint32_t update_count = 0;

		auto &set_layout = layout.sets[desc_set];

		for_each_bit(set_layout.uniform_buffer_mask, [&](uint32_t binding) {
			unsigned array_size = set_layout.meta[binding].array_size;
			VK_ASSERT(update_count < VULKAN_NUM_BINDINGS);
			// Work around a RenderDoc capture bug where descriptorCount > 1 is not handled correctly.
			for (unsigned i = 0; i < array_size; i++)
			{
				auto &entry = update_entries[update_count++];
				entry.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				entry.dstBinding = binding;
				entry.dstArrayElement = i;
				entry.descriptorCount = 1;
				entry.offset = offsetof(ResourceBinding, buffer) + sizeof(ResourceBinding) * (binding + i);
				entry.stride = sizeof(ResourceBinding);
			}
		});

		for_each_bit(set_layout.storage_buffer_mask, [&](uint32_t binding) {
			unsigned array_size = set_layout.meta[binding].array_size;
			VK_ASSERT(update_count < VULKAN_NUM_BINDINGS);
			for (unsigned i = 0; i < array_size; i++)
			{
				auto &entry = update_entries[update_count++];
				entry.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				entry.dstBinding = binding;
				entry.dstArrayElement = i;
				entry.descriptorCount = 1;
				entry.offset = offsetof(ResourceBinding, buffer) + sizeof(ResourceBinding) * (binding + i);
				entry.stride = sizeof(ResourceBinding);
			}
		});

		for_each_bit(set_layout.sampled_texel_buffer_mask, [&](uint32_t binding) {
			unsigned array_size = set_layout.meta[binding].array_size;
			VK_ASSERT(update_count < VULKAN_NUM_BINDINGS);
			for (unsigned i = 0; i < array_size; i++)
			{
				auto &entry = update_entries[update_count++];
				entry.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
				entry.dstBinding = binding;
				entry.dstArrayElement = i;
				entry.descriptorCount = 1;
				entry.offset = offsetof(ResourceBinding, buffer_view.handle) + sizeof(ResourceBinding) * (binding + i);
				entry.stride = sizeof(ResourceBinding);
			}
		});

		for_each_bit(set_layout.storage_texel_buffer_mask, [&](uint32_t binding) {
			unsigned array_size = set_layout.meta[binding].array_size;
			VK_ASSERT(update_count < VULKAN_NUM_BINDINGS);
			for (unsigned i = 0; i < array_size; i++)
			{
				auto &entry = update_entries[update_count++];
				entry.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
				entry.dstBinding = binding;
				entry.dstArrayElement = i;
				entry.descriptorCount = 1;
				entry.offset = offsetof(ResourceBinding, buffer_view.handle) + sizeof(ResourceBinding) * (binding + i);
				entry.stride = sizeof(ResourceBinding);
			}
		});

		for_each_bit(set_layout.sampled_image_mask, [&](uint32_t binding) {
			unsigned array_size = set_layout.meta[binding].array_size;
			VK_ASSERT(update_count < VULKAN_NUM_BINDINGS);
			for (unsigned i = 0; i < array_size; i++)
			{
				auto &entry = update_entries[update_count++];
				entry.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				entry.dstBinding = binding;
				entry.dstArrayElement = i;
				entry.descriptorCount = 1;
				entry.offset = offsetof(ResourceBinding, image) + sizeof(ResourceBinding) * (binding + i);
				entry.stride = sizeof(ResourceBinding);
			}
		});

		for_each_bit(set_layout.separate_image_mask, [&](uint32_t binding) {
			unsigned array_size = set_layout.meta[binding].array_size;
			VK_ASSERT(update_count < VULKAN_NUM_BINDINGS);
			for (unsigned i = 0; i < array_size; i++)
			{
				auto &entry = update_entries[update_count++];
				entry.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
				entry.dstBinding = binding;
				entry.dstArrayElement = i;
				entry.descriptorCount = 1;
				entry.offset = offsetof(ResourceBinding, image) + sizeof(ResourceBinding) * (binding + i);
				entry.stride = sizeof(ResourceBinding);
			}
		});

		for_each_bit(set_layout.sampler_mask & ~set_layout.immutable_sampler_mask, [&](uint32_t binding) {
			unsigned array_size = set_layout.meta[binding].array_size;
			VK_ASSERT(update_count < VULKAN_NUM_BINDINGS);
			for (unsigned i = 0; i < array_size; i++)
			{
				auto &entry = update_entries[update_count++];
				entry.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
				entry.dstBinding = binding;
				entry.dstArrayElement = i;
				entry.descriptorCount = 1;
				entry.offset = offsetof(ResourceBinding, image) + sizeof(ResourceBinding) * (binding + i);
				entry.stride = sizeof(ResourceBinding);
			}
		});

		for_each_bit(set_layout.storage_image_mask, [&](uint32_t binding) {
			unsigned array_size = set_layout.meta[binding].array_size;
			VK_ASSERT(update_count < VULKAN_NUM_BINDINGS);
			for (unsigned i = 0; i < array_size; i++)
			{
				auto &entry = update_entries[update_count++];
				entry.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				entry.dstBinding = binding;
				entry.dstArrayElement = i;
				entry.descriptorCount = 1;
				entry.offset = offsetof(ResourceBinding, image) + sizeof(ResourceBinding) * (binding + i);
				entry.stride = sizeof(ResourceBinding);
			}
		});

		for_each_bit(set_layout.input_attachment_mask, [&](uint32_t binding) {
			unsigned array_size = set_layout.meta[binding].array_size;
			VK_ASSERT(update_count < VULKAN_NUM_BINDINGS);
			for (unsigned i = 0; i < array_size; i++)
			{
				auto &entry = update_entries[update_count++];
				entry.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
				entry.dstBinding = binding;
				entry.dstArrayElement = i;
				entry.descriptorCount = 1;
				entry.offset = offsetof(ResourceBinding, image) + sizeof(ResourceBinding) * (binding + i);
				entry.stride = sizeof(ResourceBinding);
			}
		});

		VkDescriptorUpdateTemplateCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO };
		info.pipelineLayout = pipe_layout;

		if (desc_set == push_set_index)
		{
			info.descriptorSetLayout = set_allocators[desc_set]->get_layout_for_push();
			info.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS;
		}
		else
		{
			info.descriptorSetLayout = set_allocators[desc_set]->get_layout_for_pool();
			info.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
		}

		info.set = desc_set;
		info.descriptorUpdateEntryCount = update_count;
		info.pDescriptorUpdateEntries = update_entries;
		info.pipelineBindPoint = (layout.stages_for_sets[desc_set] & VK_SHADER_STAGE_COMPUTE_BIT) ?
				VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;

		if (table.vkCreateDescriptorUpdateTemplate(device->get_device(), &info, nullptr,
		                                           &update_template[desc_set]) != VK_SUCCESS)
		{
			LOGE("Failed to create descriptor update template.\n");
		}
	}
}

PipelineLayout::~PipelineLayout()
{
	auto &table = device->get_device_table();
	if (pipe_layout != VK_NULL_HANDLE)
		table.vkDestroyPipelineLayout(device->get_device(), pipe_layout, nullptr);

	for (auto &update : update_template)
		if (update != VK_NULL_HANDLE)
			table.vkDestroyDescriptorUpdateTemplate(device->get_device(), update, nullptr);
}

// Implicitly also checks for endian issues.
static const uint16_t reflection_magic[] = { 'G', 'R', 'A', ResourceLayout::Version };

bool ResourceLayout::unserialize(const uint8_t *data, size_t size)
{
	if (size != sizeof(*this) + sizeof(reflection_magic))
	{
		LOGE("Reflection size mismatch.\n");
		return false;
	}

	if (memcmp(data, reflection_magic, sizeof(reflection_magic)) != 0)
	{
		LOGE("Magic mismatch.\n");
		return false;
	}

	memcpy(this, data + sizeof(reflection_magic), sizeof(*this));
	return true;
}

Util::Hash Shader::hash(const uint32_t *data, size_t size)
{
	Util::Hasher hasher;
	hasher.data(data, size);
	return hasher.get();
}

Shader::Shader(Hash hash, Device *device_, const uint32_t *data, size_t size,
               const ResourceLayout *resource_layout)
	: IntrusiveHashMapEnabled<Shader>(hash)
	, device(device_)
{
	VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	info.codeSize = size;
	info.pCode = data;

	auto &table = device->get_device_table();
	if (table.vkCreateShaderModule(device->get_device(), &info, nullptr, &module) != VK_SUCCESS)
		LOGE("Failed to create shader module.\n");

	if (resource_layout)
		layout = *resource_layout;

	if (layout.bindless_set_mask != 0 && !device->get_device_features().vk12_features.descriptorIndexing)
		LOGE("Sufficient features for descriptor indexing is not supported on this device.\n");
}

Shader::~Shader()
{
	auto &table = device->get_device_table();
	if (module)
		table.vkDestroyShaderModule(device->get_device(), module, nullptr);
}

void Program::set_shader(ShaderStage stage, Shader *handle)
{
	shaders[Util::ecast(stage)] = handle;
}

Program::Program(Device *device_, Shader *vertex, Shader *fragment)
    : device(device_)
{
	set_shader(ShaderStage::Vertex, vertex);
	set_shader(ShaderStage::Fragment, fragment);
	device->bake_program(*this);
	/* A pipeline built ahead is staged here during play. */
	staged.reserve();
}

Program::Program(Device *device_, Shader *compute_shader)
    : device(device_)
{
	set_shader(ShaderStage::Compute, compute_shader);
	device->bake_program(*this);
	/* A pipeline built ahead is staged here during play. */
	staged.reserve();
}

Pipeline Program::get_pipeline(Hash hash) const
{
	auto *ret = pipelines.find(hash);
	return ret ? ret->get() : Pipeline{};
}

Pipeline Program::add_pipeline(Hash hash, const Pipeline &pipeline)
{
	return pipelines.emplace_yield(hash, pipeline)->get();
}

Pipeline Program::take_staged_pipeline(Hash hash)
{
	auto *ret = staged.find(hash);
	Pipeline pipeline = {};
	if (ret)
	{
		pipeline = ret->get();
		staged.erase(ret);
	}
	return pipeline;
}

void Program::stage_pipeline(Hash hash, const Pipeline &pipeline)
{
	if (pipelines.find(hash) || staged.find(hash))
		destroy_pipeline(pipeline);
	else
		staged.emplace_yield(hash, pipeline);
}

void Program::reserve_pipelines(size_t n)
{
	pipelines.reserve(n);
	staged.reserve(n);
}

bool Program::has_pipeline(Hash hash) const
{
	return pipelines.find(hash) || staged.find(hash);
}

void Program::destroy_pipeline(const Pipeline &pipeline)
{
	device->get_device_table().vkDestroyPipeline(device->get_device(), pipeline.pipeline, nullptr);
}

Program::~Program()
{
	/* (one taken left the set: what is here was never used) */
	for (auto &pipe : staged)
		destroy_pipeline(pipe.get());
	pipelines.for_each([this](Util::IntrusivePODWrapper<Pipeline> &pipe) {
		destroy_pipeline(pipe.get());
	});
}
}
