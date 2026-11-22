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
#include "command_buffer.hpp"
#include "device.hpp"
#include "format.hpp"
#include "thread_id.hpp"
#include <features/features_cpu.h>
#include <string.h>

using namespace Util;

namespace Vulkan
{
static inline void rect2d_clip(VkRect2D &rect)
{
	if (rect.offset.x < 0)
	{
		rect.extent.width += rect.offset.x;
		rect.offset.x = 0;
	}

	if (rect.offset.y < 0)
	{
		rect.extent.height += rect.offset.y;
		rect.offset.y = 0;
	}

	rect.extent.width = std::min<uint32_t>(rect.extent.width, 0x7fffffffu - rect.offset.x);
	rect.extent.height = std::min<uint32_t>(rect.extent.height, 0x7fffffffu - rect.offset.y);
}

static inline uint32_t get_combined_spec_constant_mask(const DeferredPipelineCompile &compile)
{
	return compile.potential_static_state.spec_constant_mask;
}

CommandBuffer::CommandBuffer(Device *device_, VkCommandBuffer cmd_, VkPipelineCache cache, Type type_, bool secondary)
    : device(device_)
    , table(device_->get_device_table())
    , cmd(cmd_)
    , type(type_)
	, is_secondary(secondary)
{
	pipeline_state.cache = cache;
	begin_compute();
	memset(&pipeline_state.static_state, 0, sizeof(pipeline_state.static_state));
	memset(&bindings, 0, sizeof(bindings));

	// Set up extra state which PSO creation depends on implicitly.
	// This needs to affect hashing to make Fossilize path behave as expected.
	auto &features = device->get_device_features();
	pipeline_state.subgroup_size_tag =
			(features.vk13_props.minSubgroupSize << 0) |
			(features.vk13_props.maxSubgroupSize << 8);

}

CommandBuffer::~CommandBuffer()
{
	VK_ASSERT(!ubo_block.is_mapped());
	VK_ASSERT(!staging_block.is_mapped());
}

void CommandBuffer::fill_buffer(const Buffer &dst, uint32_t value)
{
	fill_buffer(dst, value, 0, VK_WHOLE_SIZE);
}

void CommandBuffer::fill_buffer(const Buffer &dst, uint32_t value, VkDeviceSize offset, VkDeviceSize size)
{
	table.vkCmdFillBuffer(cmd, dst.get_buffer(), offset, size, value);
}

void CommandBuffer::copy_buffer(const Buffer &dst, VkDeviceSize dst_offset, const Buffer &src, VkDeviceSize src_offset,
                                VkDeviceSize size)
{
	const VkBufferCopy region = {
		src_offset, dst_offset, size,
	};
	table.vkCmdCopyBuffer(cmd, src.get_buffer(), dst.get_buffer(), 1, &region);
}

void CommandBuffer::copy_buffer(const Buffer &dst, const Buffer &src)
{
	VK_ASSERT(dst.get_create_info().size == src.get_create_info().size);
	copy_buffer(dst, 0, src, 0, dst.get_create_info().size);
}

void CommandBuffer::copy_image(const Vulkan::Image &dst, const Vulkan::Image &src, const VkOffset3D &dst_offset,
                               const VkOffset3D &src_offset, const VkExtent3D &extent,
                               const VkImageSubresourceLayers &dst_subresource,
                               const VkImageSubresourceLayers &src_subresource)
{
	VkImageCopy region = {};
	region.dstOffset = dst_offset;
	region.srcOffset = src_offset;
	region.extent = extent;
	region.srcSubresource = src_subresource;
	region.dstSubresource = dst_subresource;

	table.vkCmdCopyImage(cmd, src.get_image(), src.get_layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
	               dst.get_image(), dst.get_layout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
	               1, &region);
}

void CommandBuffer::copy_buffer_to_image(const Image &image, const Buffer &buffer, unsigned num_blits,
                                         const VkBufferImageCopy *blits)
{
	table.vkCmdCopyBufferToImage(cmd, buffer.get_buffer(),
	                             image.get_image(), image.get_layout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL), num_blits, blits);
}

void CommandBuffer::barrier(VkPipelineStageFlags2 src_stages, VkAccessFlags2 src_access,
                            VkPipelineStageFlags2 dst_stages, VkAccessFlags2 dst_access)
{
	VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	VkMemoryBarrier2 b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &b;
	b.srcStageMask = src_stages;
	b.dstStageMask = dst_stages;
	b.srcAccessMask = src_access;
	b.dstAccessMask = dst_access;
	barrier(dep);
}

void CommandBuffer::barrier(const VkDependencyInfo &dep)
{
	VK_ASSERT(!actual_render_pass);
	VK_ASSERT(!framebuffer);

	if (barrier_batch.active)
	{
		if (dep.memoryBarrierCount)
		{
			barrier_batch.memory_barriers.insert(
					barrier_batch.memory_barriers.end(),
					dep.pMemoryBarriers,
					dep.pMemoryBarriers + dep.memoryBarrierCount);
		}

		if (dep.bufferMemoryBarrierCount)
		{
			barrier_batch.buffer_barriers.insert(
					barrier_batch.buffer_barriers.end(),
					dep.pBufferMemoryBarriers,
					dep.pBufferMemoryBarriers + dep.bufferMemoryBarrierCount);
		}

		if (dep.imageMemoryBarrierCount)
		{
			barrier_batch.image_barriers.insert(
					barrier_batch.image_barriers.end(),
					dep.pImageMemoryBarriers,
					dep.pImageMemoryBarriers + dep.imageMemoryBarrierCount);
		}

		return;
	}


	table.vkCmdPipelineBarrier2(cmd, &dep);
}

void CommandBuffer::image_barrier(const Image &image,
                                  VkImageLayout old_layout, VkImageLayout new_layout,
                                  VkPipelineStageFlags2 src_stages, VkAccessFlags2 src_access,
                                  VkPipelineStageFlags2 dst_stages, VkAccessFlags2 dst_access)
{
	VK_ASSERT(!actual_render_pass);
	VK_ASSERT(!framebuffer);

	VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	b.srcAccessMask = src_access;
	b.dstAccessMask = dst_access;
	b.oldLayout = old_layout;
	b.newLayout = new_layout;
	b.image = image.get_image();
	b.subresourceRange.aspectMask = format_to_aspect_mask(image.get_create_info().format);
	b.subresourceRange.levelCount = image.get_create_info().levels;
	b.subresourceRange.layerCount = image.get_create_info().layers;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.srcStageMask = src_stages;
	b.dstStageMask = dst_stages;

	image_barriers(1, &b);
}

void CommandBuffer::image_barriers(uint32_t image_barriers, const VkImageMemoryBarrier2 *images)
{
	VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dep.imageMemoryBarrierCount = image_barriers;
	dep.pImageMemoryBarriers = images;
	barrier(dep);
}

void CommandBuffer::barrier_prepare_generate_mipmap(const Image &image, VkImageLayout base_level_layout,
                                                    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                                    bool need_top_level_barrier)
{
	auto &create_info = image.get_create_info();
	VkImageMemoryBarrier2 barriers[2] = {};
	VK_ASSERT(create_info.levels > 1);
	(void)create_info;

	for (unsigned i = 0; i < 2; i++)
	{
		barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barriers[i].image = image.get_image();
		barriers[i].subresourceRange.aspectMask = format_to_aspect_mask(image.get_format());
		barriers[i].subresourceRange.layerCount = image.get_create_info().layers;
		barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[i].srcStageMask = src_stage;
		barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;

		if (i == 0)
		{
			barriers[i].oldLayout = base_level_layout;
			barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barriers[i].srcAccessMask = src_access;
			barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barriers[i].subresourceRange.baseMipLevel = 0;
			barriers[i].subresourceRange.levelCount = 1;
		}
		else
		{
			barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barriers[i].srcAccessMask = 0;
			barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barriers[i].subresourceRange.baseMipLevel = 1;
			barriers[i].subresourceRange.levelCount = image.get_create_info().levels - 1;
		}
	}

	image_barriers(need_top_level_barrier ? 2 : 1, need_top_level_barrier ? barriers : barriers + 1);
}

void CommandBuffer::generate_mipmap(const Image &image)
{
	auto &create_info = image.get_create_info();
	VkOffset3D size = { int(create_info.width), int(create_info.height), int(create_info.depth) };
	const VkOffset3D origin = { 0, 0, 0 };

	VK_ASSERT(image.get_layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	b.image = image.get_image();
	b.subresourceRange.levelCount = 1;
	b.subresourceRange.layerCount = image.get_create_info().layers;
	b.subresourceRange.aspectMask = format_to_aspect_mask(image.get_format());
	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
	b.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;

	for (unsigned i = 1; i < create_info.levels; i++)
	{
		VkOffset3D src_size = size;
		size.x = std::max(size.x >> 1, 1);
		size.y = std::max(size.y >> 1, 1);
		size.z = std::max(size.z >> 1, 1);

		blit_image(image, image,
		           origin, size, origin, src_size, i, i - 1, 0, 0, create_info.layers, VK_FILTER_LINEAR);

		b.subresourceRange.baseMipLevel = i;
		image_barriers(1, &b);
	}
}

void CommandBuffer::blit_image(const Image &dst, const Image &src,
                               const VkOffset3D &dst_offset,
                               const VkOffset3D &dst_extent, const VkOffset3D &src_offset, const VkOffset3D &src_extent,
                               unsigned dst_level, unsigned src_level, unsigned dst_base_layer, unsigned src_base_layer,
                               unsigned num_layers, VkFilter filter)
{
	const auto add_offset = [](const VkOffset3D &a, const VkOffset3D &b) -> VkOffset3D {
		return { a.x + b.x, a.y + b.y, a.z + b.z };
	};

	const VkImageBlit blit = {
		{ format_to_aspect_mask(src.get_create_info().format), src_level, src_base_layer, num_layers },
		{ src_offset, add_offset(src_offset, src_extent) },
		{ format_to_aspect_mask(dst.get_create_info().format), dst_level, dst_base_layer, num_layers },
		{ dst_offset, add_offset(dst_offset, dst_extent) },
	};

	table.vkCmdBlitImage(cmd,
	                     src.get_image(), src.get_layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
	                     dst.get_image(), dst.get_layout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
	                     1, &blit, filter);
}

void CommandBuffer::begin_context()
{
	dirty = ~0u;
	dirty_sets_realloc = ~0u;
	current_pipeline = {};
	current_pipeline_layout = VK_NULL_HANDLE;
	pipeline_state.layout = nullptr;
	pipeline_state.program = nullptr;
	pipeline_state.potential_static_state.spec_constant_mask = 0;
	memset(bindings.cookies, 0, sizeof(bindings.cookies));
	memset(bindings.secondary_cookies, 0, sizeof(bindings.secondary_cookies));

	VK_ASSERT(!barrier_batch.active);
}

void CommandBuffer::begin_compute()
{
	is_compute = true;
	begin_context();
}

void CommandBuffer::begin_graphics()
{
	is_compute = false;
	begin_context();
}

void CommandBuffer::init_viewport_scissor(const Framebuffer *fb)
{
	VkRect2D rect = { { 0, 0 }, { fb->get_width(), fb->get_height() } };

	viewport = {
		float(rect.offset.x), float(rect.offset.y),
		float(rect.extent.width), float(rect.extent.height),
		0.0f, 1.0f
	};
	scissor = rect;
}

void CommandBuffer::begin_render_pass(const RenderPassInfo &info, VkSubpassContents contents)
{
	VK_ASSERT(!framebuffer);
	VK_ASSERT(!pipeline_state.compatible_render_pass);
	VK_ASSERT(!actual_render_pass);

	framebuffer = &device->request_framebuffer(info);
	pipeline_state.compatible_render_pass = &framebuffer->get_compatible_render_pass();
	actual_render_pass = &device->request_render_pass(info, false);

	init_viewport_scissor(framebuffer);

	VkClearValue clear_values[VULKAN_NUM_ATTACHMENTS];
	unsigned num_clear_values = 0;

	for (unsigned i = 0; i < info.num_color_attachments; i++)
	{
		VK_ASSERT(info.color_attachments[i]);
		if (info.clear_attachments & (1u << i))
		{
			clear_values[i].color = info.clear_color[i];
			num_clear_values = i + 1;
		}
	}

	VkRenderPassBeginInfo begin_info = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	begin_info.renderPass = actual_render_pass->get_render_pass();
	begin_info.framebuffer = framebuffer->get_framebuffer();
	begin_info.renderArea = scissor;
	begin_info.clearValueCount = num_clear_values;
	begin_info.pClearValues = clear_values;

	table.vkCmdBeginRenderPass(cmd, &begin_info, contents);

	current_contents = contents;
	begin_graphics();
}

void CommandBuffer::end_render_pass()
{
	VK_ASSERT(framebuffer);
	VK_ASSERT(actual_render_pass);
	VK_ASSERT(pipeline_state.compatible_render_pass);

	table.vkCmdEndRenderPass(cmd);

	framebuffer = nullptr;
	actual_render_pass = nullptr;
	pipeline_state.compatible_render_pass = nullptr;
	begin_compute();
}

static void log_compile_time(const char *tag, Hash hash,
                             int64_t time_ns, VkResult result,
                             CommandBuffer::CompileMode mode)
{
	bool stall = time_ns >= 5 * 1000 * 1000 && mode != CommandBuffer::CompileMode::AsyncThread;
	// If a compile takes more than 5 ms and it's not happening on an async thread,
	// we consider it a stall.
	if (stall)
	{
		double time_us = 1e-3 * double(time_ns);
		const char *mode_str;

		switch (mode)
		{
		case CommandBuffer::CompileMode::Sync:
			mode_str = "sync";
			break;

		case CommandBuffer::CompileMode::FailOnCompileRequired:
			mode_str = "fail-on-compile-required";
			break;

		default:
			mode_str = "async-thread";
			break;
		}

		{
			LOGW("Stalled compile (%s, %016llx): thread %u - %.3f us (mode: %s, success: %s).\n",
			     tag, static_cast<unsigned long long>(hash),
			     get_current_thread_index(),
			     time_us, mode_str, result == VK_SUCCESS ? "yes" : "no");
		}
	}
}

Pipeline CommandBuffer::build_compute_pipeline(Device *device, const DeferredPipelineCompile &compile,
                                               CompileMode mode)
{
	// If we don't have pipeline creation cache control feature,
	// we must assume compilation can be synchronous.
	if (mode == CompileMode::FailOnCompileRequired &&
	    (device->get_workarounds().broken_pipeline_cache_control ||
	     !device->get_device_features().vk13_features.pipelineCreationCacheControl))
	{
		return {};
	}

	auto &shader = *compile.program->get_shader(ShaderStage::Compute);
	VkComputePipelineCreateInfo info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	info.layout = compile.layout->get_layout();
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.module = shader.get_module();
	info.stage.pName = "main";
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;

	VkSpecializationInfo spec_info = {};
	VkSpecializationMapEntry spec_entries[VULKAN_NUM_USER_SPEC_CONSTANTS];
	auto mask = compile.layout->get_resource_layout().combined_spec_constant_mask &
	            get_combined_spec_constant_mask(compile);

	uint32_t spec_constants[VULKAN_NUM_USER_SPEC_CONSTANTS];

	if (mask)
	{
		info.stage.pSpecializationInfo = &spec_info;
		spec_info.pData = spec_constants;
		spec_info.pMapEntries = spec_entries;

		for_each_bit(mask, [&](uint32_t bit) {
			auto &entry = spec_entries[spec_info.mapEntryCount];
			entry.offset = sizeof(uint32_t) * spec_info.mapEntryCount;
			entry.size = sizeof(uint32_t);
			entry.constantID = bit;

			spec_constants[spec_info.mapEntryCount] = compile.potential_static_state.spec_constants[bit];
			spec_info.mapEntryCount++;
		});
		spec_info.dataSize = spec_info.mapEntryCount * sizeof(uint32_t);
	}

	VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_info;

	if (compile.static_state.state.subgroup_control_size)
	{
		if (!setup_subgroup_size_control(*device, info.stage, subgroup_size_info,
		                                 VK_SHADER_STAGE_COMPUTE_BIT,
		                                 compile.static_state.state.subgroup_full_group,
		                                 compile.static_state.state.subgroup_minimum_size_log2,
		                                 compile.static_state.state.subgroup_maximum_size_log2))
		{
			LOGE("Subgroup size configuration not supported.\n");
			return {};
		}
	}

	VkPipeline compute_pipeline = VK_NULL_HANDLE;

	auto &table = device->get_device_table();

	if (mode == CompileMode::FailOnCompileRequired)
		info.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;

	auto start_ts = (int64_t(cpu_features_get_time_usec()) * 1000);
	VkResult vr = table.vkCreateComputePipelines(device->get_device(), compile.cache, 1, &info, nullptr,
	                                             &compute_pipeline);
	auto end_ts = (int64_t(cpu_features_get_time_usec()) * 1000);
	log_compile_time("compute", compile.hash, end_ts - start_ts, vr, mode);

	if (vr != VK_SUCCESS || compute_pipeline == VK_NULL_HANDLE)
	{
		if (vr < 0)
			LOGE("Failed to create compute pipeline!\n");
		return {};
	}

	/* Built ahead: the caller's to hand over, nothing holds it yet. */
	if (mode == CompileMode::AsyncThread)
		return { compute_pipeline, 0 };
	auto returned_pipeline = compile.program->add_pipeline(compile.hash, { compute_pipeline, 0 });
	if (returned_pipeline.pipeline != compute_pipeline)
		table.vkDestroyPipeline(device->get_device(), compute_pipeline, nullptr);
	return returned_pipeline;
}

void CommandBuffer::extract_pipeline_state(DeferredPipelineCompile &compile) const
{
	compile = pipeline_state;

	if (!compile.program)
	{
		LOGE("Attempting to extract pipeline state when no program is bound.\n");
		return;
	}

	if (is_compute)
		update_hash_compute_pipeline(compile);
	else
		update_hash_graphics_pipeline(compile);
}

bool CommandBuffer::setup_subgroup_size_control(
		Vulkan::Device &device, VkPipelineShaderStageCreateInfo &stage_info,
		VkPipelineShaderStageRequiredSubgroupSizeCreateInfo &required_info,
		VkShaderStageFlagBits stage, bool full_group,
		unsigned min_size_log2, unsigned max_size_log2)
{
	if (!device.supports_subgroup_size_log2(full_group, min_size_log2, max_size_log2, stage))
		return false;

	auto &features = device.get_device_features();

	if (full_group)
		stage_info.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;

	// If subgroup size control is only enabled, we want varying subgroup size, whatever it is.
	uint32_t min_subgroups = 1u << min_size_log2;
	uint32_t max_subgroups = 1u << max_size_log2;
	if ((min_size_log2 == 0 && max_size_log2 == 0) ||
	    ((min_subgroups <= features.vk13_props.minSubgroupSize &&
	      max_subgroups >= features.vk13_props.maxSubgroupSize)))
	{
		stage_info.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT;
	}
	else
	{
		required_info = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO };

		// Pick a fixed subgroup size. Prefer smallest subgroup size.
		if (min_subgroups < features.vk13_props.minSubgroupSize)
			required_info.requiredSubgroupSize = features.vk13_props.minSubgroupSize;
		else
			required_info.requiredSubgroupSize = min_subgroups;

		required_info.pNext = const_cast<void *>(stage_info.pNext);
		stage_info.pNext = &required_info;
	}

	return true;
}

Pipeline CommandBuffer::build_graphics_pipeline(Device *device, const DeferredPipelineCompile &compile,
                                                CompileMode mode)
{
	// If we don't have pipeline creation cache control feature,
	// we must assume compilation can be synchronous.
	if (mode == CompileMode::FailOnCompileRequired &&
	    (device->get_workarounds().broken_pipeline_cache_control ||
	     !device->get_device_features().vk13_features.pipelineCreationCacheControl))
	{
		return {};
	}

	// Viewport state
	VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	// Dynamic state
	VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dyn.dynamicStateCount = 2;
	VkDynamicState states[3] = {
		VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_VIEWPORT,
	};
	dyn.pDynamicStates = states;

	uint32_t dynamic_mask = COMMAND_BUFFER_DIRTY_VIEWPORT_BIT | COMMAND_BUFFER_DIRTY_SCISSOR_BIT;

	if (compile.static_state.state.dynamic_blend_constants)
	{
		states[dyn.dynamicStateCount++] = VK_DYNAMIC_STATE_BLEND_CONSTANTS;
		dynamic_mask |= COMMAND_BUFFER_DIRTY_BLEND_CONSTANTS_BIT;
	}

	// Blend state
	VkPipelineColorBlendAttachmentState blend_attachments[VULKAN_NUM_ATTACHMENTS];
	VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	blend.attachmentCount = compile.compatible_render_pass->get_num_color_attachments();
	blend.pAttachments = blend_attachments;
	for (unsigned i = 0; i < blend.attachmentCount; i++)
	{
		auto &att = blend_attachments[i];
		att = {};

		if (compile.layout->get_resource_layout().render_target_mask & (1u << i))
		{
			att.colorWriteMask = (compile.static_state.state.write_mask >> (4 * i)) & 0xf;
			att.blendEnable = compile.static_state.state.blend_enable;
			if (att.blendEnable)
			{
				att.alphaBlendOp = static_cast<VkBlendOp>(compile.static_state.state.alpha_blend_op);
				att.colorBlendOp = static_cast<VkBlendOp>(compile.static_state.state.color_blend_op);
				att.dstAlphaBlendFactor = static_cast<VkBlendFactor>(compile.static_state.state.dst_alpha_blend);
				att.srcAlphaBlendFactor = static_cast<VkBlendFactor>(compile.static_state.state.src_alpha_blend);
				att.dstColorBlendFactor = static_cast<VkBlendFactor>(compile.static_state.state.dst_color_blend);
				att.srcColorBlendFactor = static_cast<VkBlendFactor>(compile.static_state.state.src_color_blend);
			}
		}
	}
	if (!compile.static_state.state.dynamic_blend_constants)
		memcpy(blend.blendConstants, compile.potential_static_state.blend_constants, sizeof(blend.blendConstants));

	/* No pass has a depth attachment: no depth or stencil test. */
	VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };

	/* No vertex input: each pass makes its vertices from their index. */
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(compile.compatible_render_pass->get_sample_count());

	VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	raster.polygonMode = VK_POLYGON_MODE_FILL;

	// Stages
	VkPipelineShaderStageCreateInfo stages[Util::ecast(ShaderStage::Count)];
	unsigned num_stages = 0;

	VkSpecializationInfo spec_info[ecast(ShaderStage::Count)] = {};
	VkSpecializationMapEntry spec_entries[ecast(ShaderStage::Count)][VULKAN_NUM_USER_SPEC_CONSTANTS];
	uint32_t spec_constants[Util::ecast(ShaderStage::Count)][VULKAN_NUM_USER_SPEC_CONSTANTS];

	for (int i = 0; i < Util::ecast(ShaderStage::Count); i++)
	{
		auto mask = compile.layout->get_resource_layout().spec_constant_mask[i] &
		            get_combined_spec_constant_mask(compile);

		if (mask)
		{
			spec_info[i].pData = spec_constants[i];
			spec_info[i].pMapEntries = spec_entries[i];

			for_each_bit(mask, [&](uint32_t bit)
			{
				auto &entry = spec_entries[i][spec_info[i].mapEntryCount];
				entry.offset = sizeof(uint32_t) * spec_info[i].mapEntryCount;
				entry.size = sizeof(uint32_t);
				entry.constantID = bit;
				spec_constants[i][spec_info[i].mapEntryCount] = compile.potential_static_state.spec_constants[bit];
				spec_info[i].mapEntryCount++;
			});
			spec_info[i].dataSize = spec_info[i].mapEntryCount * sizeof(uint32_t);
		}
	}

	for (int i = 0; i < Util::ecast(ShaderStage::Count); i++)
	{
		auto stage = static_cast<ShaderStage>(i);
		if (compile.program->get_shader(stage))
		{
			auto &s = stages[num_stages++];
			s = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
			s.module = compile.program->get_shader(stage)->get_module();
			s.pName = "main";
			s.stage = static_cast<VkShaderStageFlagBits>(1u << i);
			if (spec_info[i].mapEntryCount)
				s.pSpecializationInfo = &spec_info[i];

		}
	}

	VkGraphicsPipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipe.layout = compile.layout->get_layout();
	pipe.renderPass = compile.compatible_render_pass->get_render_pass();
	pipe.subpass = 0;

	pipe.pViewportState = &vp;
	pipe.pDynamicState = &dyn;
	pipe.pColorBlendState = &blend;
	pipe.pDepthStencilState = &ds;
	if (compile.program->get_shader(ShaderStage::Vertex))
	{
		pipe.pVertexInputState = &vi;
		pipe.pInputAssemblyState = &ia;
	}
	pipe.pMultisampleState = &ms;
	pipe.pRasterizationState = &raster;
	pipe.pStages = stages;
	pipe.stageCount = num_stages;

	VkPipeline pipeline = VK_NULL_HANDLE;

	auto &table = device->get_device_table();

	if (mode == CompileMode::FailOnCompileRequired)
		pipe.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;

	auto start_ts = (int64_t(cpu_features_get_time_usec()) * 1000);
	VkResult res = table.vkCreateGraphicsPipelines(device->get_device(), compile.cache, 1, &pipe, nullptr, &pipeline);
	auto end_ts = (int64_t(cpu_features_get_time_usec()) * 1000);
	log_compile_time("graphics", compile.hash, end_ts - start_ts, res, mode);

	if (res != VK_SUCCESS || pipeline == VK_NULL_HANDLE)
	{
		if (res < 0)
			LOGE("Failed to create graphics pipeline!\n");
		return {};
	}

	/* Built ahead: the caller's to hand over, nothing holds it yet. */
	if (mode == CompileMode::AsyncThread)
		return { pipeline, dynamic_mask };
	auto returned_pipeline = compile.program->add_pipeline(compile.hash, { pipeline, dynamic_mask });
	if (returned_pipeline.pipeline != pipeline)
		table.vkDestroyPipeline(device->get_device(), pipeline, nullptr);
	return returned_pipeline;
}

bool CommandBuffer::flush_compute_pipeline(bool synchronous)
{
	update_hash_compute_pipeline(pipeline_state);
	current_pipeline = pipeline_state.program->get_pipeline(pipeline_state.hash);
	if (current_pipeline.pipeline == VK_NULL_HANDLE)
		return flush_compute_pipeline_miss(synchronous);
	return true;
}

/* Not in the table: one built ahead is taken from the staged ones -
 * after collecting what the threads building ahead have handed over
 * since the last miss - else it is built here. Either way the device
 * records it, so the next device builds it ahead first. */
static Pipeline take_built_pipeline(Device *device, Program *program, Util::Hash hash)
{
	Pipeline staged = program->take_staged_pipeline(hash);
	if (staged.pipeline == VK_NULL_HANDLE)
	{
		device->collect_built_pipelines();
		staged = program->take_staged_pipeline(hash);
	}
	return staged;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
bool CommandBuffer::flush_compute_pipeline_miss(bool synchronous)
{
	Pipeline staged = take_built_pipeline(device, pipeline_state.program, pipeline_state.hash);

	if (staged.pipeline != VK_NULL_HANDLE)
		current_pipeline = pipeline_state.program->add_pipeline(pipeline_state.hash, staged);
	else
		current_pipeline = build_compute_pipeline(
			device, pipeline_state,
			synchronous ? CompileMode::Sync : CompileMode::FailOnCompileRequired);
	if (current_pipeline.pipeline == VK_NULL_HANDLE)
		return false;
	device->record_compute_pipeline(pipeline_state);
	return true;
}

void CommandBuffer::update_hash_compute_pipeline(DeferredPipelineCompile &compile)
{
	Hasher h;
	h.u64(compile.program->get_hash());
	h.u64(compile.layout->get_hash());

	// Spec constants.
	auto &layout = compile.layout->get_resource_layout();
	uint32_t combined_spec_constant = layout.combined_spec_constant_mask;
	combined_spec_constant &= get_combined_spec_constant_mask(compile);
	h.u32(combined_spec_constant);
	for_each_bit(combined_spec_constant, [&](uint32_t bit) {
		h.u32(compile.potential_static_state.spec_constants[bit]);
	});

	if (compile.static_state.state.subgroup_control_size)
	{
		h.s32(1);
		h.u32(compile.static_state.state.subgroup_minimum_size_log2);
		h.u32(compile.static_state.state.subgroup_maximum_size_log2);
		h.u32(compile.static_state.state.subgroup_full_group);
		// Required for Fossilize since we don't know exactly how to lower these requirements to a PSO
		// without knowing some device state.
		h.u32(compile.subgroup_size_tag);
	}
	else
		h.s32(0);

	compile.hash = h.get();
}

void CommandBuffer::update_hash_graphics_pipeline(DeferredPipelineCompile &compile)
{
	Hasher h;
	auto &layout = compile.layout->get_resource_layout();

	h.u64(compile.compatible_render_pass->get_hash());
	h.u64(compile.program->get_hash());
	h.u64(compile.layout->get_hash());
	h.data(compile.static_state.words, sizeof(compile.static_state.words));

	if (compile.static_state.state.blend_enable && !compile.static_state.state.dynamic_blend_constants)
	{
		const auto needs_blend_constant = [](VkBlendFactor factor) {
			return factor == VK_BLEND_FACTOR_CONSTANT_COLOR || factor == VK_BLEND_FACTOR_CONSTANT_ALPHA;
		};
		bool b0 = needs_blend_constant(static_cast<VkBlendFactor>(compile.static_state.state.src_color_blend));
		bool b1 = needs_blend_constant(static_cast<VkBlendFactor>(compile.static_state.state.src_alpha_blend));
		bool b2 = needs_blend_constant(static_cast<VkBlendFactor>(compile.static_state.state.dst_color_blend));
		bool b3 = needs_blend_constant(static_cast<VkBlendFactor>(compile.static_state.state.dst_alpha_blend));
		if (b0 || b1 || b2 || b3)
			h.data(reinterpret_cast<const uint32_t *>(compile.potential_static_state.blend_constants),
			       sizeof(compile.potential_static_state.blend_constants));
	}

	// Spec constants.
	uint32_t combined_spec_constant = layout.combined_spec_constant_mask;
	combined_spec_constant &= get_combined_spec_constant_mask(compile);
	h.u32(combined_spec_constant);
	for_each_bit(combined_spec_constant, [&](uint32_t bit) {
		h.u32(compile.potential_static_state.spec_constants[bit]);
	});

	compile.hash = h.get();
}

/* Blend constants: off the draw path. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
void CommandBuffer::flush_dynamic_render_state()
{
	if (get_and_clear(current_pipeline.dynamic_mask & COMMAND_BUFFER_DIRTY_BLEND_CONSTANTS_BIT))
		table.vkCmdSetBlendConstants(cmd, dynamic_state.blend_constants);
}

bool CommandBuffer::flush_graphics_pipeline(bool synchronous)
{
	update_hash_graphics_pipeline(pipeline_state);
	current_pipeline = pipeline_state.program->get_pipeline(pipeline_state.hash);
	if (current_pipeline.pipeline == VK_NULL_HANDLE)
		return flush_graphics_pipeline_miss(synchronous);
	return true;
}

/* As flush_compute_pipeline_miss. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
bool CommandBuffer::flush_graphics_pipeline_miss(bool synchronous)
{
	Pipeline staged = take_built_pipeline(device, pipeline_state.program, pipeline_state.hash);

	if (staged.pipeline != VK_NULL_HANDLE)
		current_pipeline = pipeline_state.program->add_pipeline(pipeline_state.hash, staged);
	else
		current_pipeline = build_graphics_pipeline(
			device, pipeline_state,
			synchronous ? CompileMode::Sync : CompileMode::FailOnCompileRequired);
	if (current_pipeline.pipeline == VK_NULL_HANDLE)
		return false;
	device->record_graphics_pipeline(pipeline_state);
	return true;
}

void CommandBuffer::extract_graphics_pipeline_state(DeferredPipelineCompile &compile,
                                                    const RenderPass &pass) const
{
	compile = pipeline_state;
	compile.compatible_render_pass = &pass;
	if (compile.program)
		update_hash_graphics_pipeline(compile);
}

void CommandBuffer::bind_pipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline, uint32_t active_dynamic_state)
{
	table.vkCmdBindPipeline(cmd, bind_point, pipeline);

	// If some dynamic state is static in the pipeline it clobbers the dynamic state.
	// As a performance optimization don't clobber everything.
	uint32_t static_state_clobber = ~active_dynamic_state & COMMAND_BUFFER_DYNAMIC_BITS;
	set_dirty(static_state_clobber);
}

VkPipeline CommandBuffer::flush_compute_state(bool synchronous)
{
	VK_ASSERT(!barrier_batch.active);
	if (!pipeline_state.program)
		return VK_NULL_HANDLE;
	VK_ASSERT(pipeline_state.layout);

	if (current_pipeline.pipeline == VK_NULL_HANDLE)
		set_dirty(COMMAND_BUFFER_DIRTY_PIPELINE_BIT);

	if (get_and_clear(COMMAND_BUFFER_DIRTY_STATIC_STATE_BIT | COMMAND_BUFFER_DIRTY_PIPELINE_BIT))
	{
		VkPipeline old_pipe = current_pipeline.pipeline;
		if (!flush_compute_pipeline(synchronous))
			return VK_NULL_HANDLE;

		if (old_pipe != current_pipeline.pipeline)
			bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, current_pipeline.pipeline, current_pipeline.dynamic_mask);
	}

	if (current_pipeline.pipeline == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	flush_descriptor_sets();

	if (get_and_clear(COMMAND_BUFFER_DIRTY_PUSH_CONSTANTS_BIT))
	{
		auto &range = pipeline_state.layout->get_resource_layout().push_constant_range;

		if (range.stageFlags != 0)
		{
			VK_ASSERT(range.offset == 0);
			table.vkCmdPushConstants(cmd, current_pipeline_layout, range.stageFlags,
									 0, range.size,
									 bindings.push_constant_data);
		}
	}

	return current_pipeline.pipeline;
}

VkPipeline CommandBuffer::flush_render_state(bool synchronous)
{
	VK_ASSERT(!barrier_batch.active);
	if (!pipeline_state.program)
		return VK_NULL_HANDLE;
	VK_ASSERT(pipeline_state.layout);

	if (current_pipeline.pipeline == VK_NULL_HANDLE)
		set_dirty(COMMAND_BUFFER_DIRTY_PIPELINE_BIT);

	// We've invalidated pipeline state, update the VkPipeline.
	if (get_and_clear(COMMAND_BUFFER_DIRTY_STATIC_STATE_BIT | COMMAND_BUFFER_DIRTY_PIPELINE_BIT))
	{
		VkPipeline old_pipe = current_pipeline.pipeline;
		if (!flush_graphics_pipeline(synchronous))
			return VK_NULL_HANDLE;

		if (old_pipe != current_pipeline.pipeline)
			bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, current_pipeline.pipeline, current_pipeline.dynamic_mask);
	}

	if (current_pipeline.pipeline == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	flush_descriptor_sets();

	if (get_and_clear(COMMAND_BUFFER_DIRTY_PUSH_CONSTANTS_BIT))
	{
		auto &range = pipeline_state.layout->get_resource_layout().push_constant_range;

		if (range.stageFlags != 0)
		{
			VK_ASSERT(range.offset == 0);
			table.vkCmdPushConstants(cmd, current_pipeline_layout, range.stageFlags,
									 0, range.size,
									 bindings.push_constant_data);
		}
	}

	if (get_and_clear(COMMAND_BUFFER_DIRTY_VIEWPORT_BIT))
	{
		table.vkCmdSetViewport(cmd, 0, 1, &viewport);
	}

	if (get_and_clear(COMMAND_BUFFER_DIRTY_SCISSOR_BIT))
	{
		auto tmp_scissor = scissor;
		rect2d_clip(tmp_scissor);
		table.vkCmdSetScissor(cmd, 0, 1, &tmp_scissor);
	}

	/* The bound pipeline's dynamic mask has the bit only when its state
	 * takes the constants. */
	if (dirty & current_pipeline.dynamic_mask & COMMAND_BUFFER_DIRTY_BLEND_CONSTANTS_BIT)
		flush_dynamic_render_state();

	return current_pipeline.pipeline;
}

void CommandBuffer::set_viewport(const VkViewport &viewport_)
{
	VK_ASSERT(framebuffer);
	viewport = viewport_;
	set_dirty(COMMAND_BUFFER_DIRTY_VIEWPORT_BIT);
}

void CommandBuffer::set_scissor(const VkRect2D &rect)
{
	VK_ASSERT(framebuffer);
	VK_ASSERT(rect.offset.x >= 0);
	VK_ASSERT(rect.offset.y >= 0);
	scissor = rect;
	set_dirty(COMMAND_BUFFER_DIRTY_SCISSOR_BIT);
}

void CommandBuffer::push_constants(const void *data, VkDeviceSize offset, VkDeviceSize range)
{
	VK_ASSERT(offset + range <= VULKAN_PUSH_CONSTANT_SIZE);
	memcpy(bindings.push_constant_data + offset, data, range);
	set_dirty(COMMAND_BUFFER_DIRTY_PUSH_CONSTANTS_BIT);
}

void CommandBuffer::set_program(Program *program)
{
	if (pipeline_state.program == program)
		return;

	pipeline_state.program = program;
	current_pipeline = {};

	set_dirty(COMMAND_BUFFER_DIRTY_PIPELINE_BIT);
	if (!program)
		return;

	set_program_layout(program->get_pipeline_layout());
}

void CommandBuffer::set_program_layout(const PipelineLayout *layout)
{
	VK_ASSERT(layout);
	if (!pipeline_state.layout)
	{
		dirty_sets_realloc = ~0u;
		set_dirty(COMMAND_BUFFER_DIRTY_PUSH_CONSTANTS_BIT);
	}
	else if (layout->get_hash() != pipeline_state.layout->get_hash())
	{
		auto &new_layout = layout->get_resource_layout();
		auto &old_layout = pipeline_state.layout->get_resource_layout();

		uint32_t first_invalidated_set_index = VULKAN_NUM_DESCRIPTOR_SETS;
		uint32_t new_push_set = layout->get_push_set_index();
		uint32_t old_push_set = pipeline_state.layout->get_push_set_index();
		if (new_push_set == old_push_set)
		{
			new_push_set = UINT32_MAX;
			old_push_set = UINT32_MAX;
		}

		// If the push constant layout changes, all descriptor sets
		// are invalidated.
		if (new_layout.push_constant_layout_hash != old_layout.push_constant_layout_hash)
		{
			first_invalidated_set_index = 0;
			set_dirty(COMMAND_BUFFER_DIRTY_PUSH_CONSTANTS_BIT);
		}
		else
		{
			// Find the first set whose descriptor set layout differs.
			for (unsigned set = 0; set < VULKAN_NUM_DESCRIPTOR_SETS; set++)
			{
				if (layout->get_allocator(set) != pipeline_state.layout->get_allocator(set) ||
				    set == new_push_set || set == old_push_set)
				{
					first_invalidated_set_index = set;
					break;
				}
			}
		}

		if (first_invalidated_set_index < VULKAN_NUM_DESCRIPTOR_SETS)
		{
			dirty_sets_rebind |= ~((1u << first_invalidated_set_index) - 1u);

			for (unsigned set = first_invalidated_set_index; set < VULKAN_NUM_DESCRIPTOR_SETS; set++)
			{
				if (layout->get_allocator(set) != pipeline_state.layout->get_allocator(set) ||
					set == new_push_set || set == old_push_set)
				{
					dirty_sets_realloc |= 1u << set;
				}
			}
		}
	}

	pipeline_state.layout = layout;
	current_pipeline_layout = pipeline_state.layout->get_layout();
}

void *CommandBuffer::allocate_constant_data(unsigned set, unsigned binding, VkDeviceSize size)
{
	VK_ASSERT(size <= VULKAN_MAX_UBO_SIZE);
	auto data = ubo_block.allocate(size);
	if (!data.host)
	{
		device->request_uniform_block(ubo_block, size);
		data = ubo_block.allocate(size);
	}
	set_uniform_buffer(set, binding, *data.buffer, data.offset, data.padded_size);
	return data.host;
}

BufferBlockAllocation CommandBuffer::request_scratch_buffer_memory(VkDeviceSize size)
{
	if (size == 0)
		return {};

	auto data = staging_block.allocate(size);
	if (!data.host)
	{
		device->request_staging_block(staging_block, size);
		data = staging_block.allocate(size);
	}

	return data;
}

void *CommandBuffer::update_buffer(const Buffer &buffer, VkDeviceSize offset, VkDeviceSize size)
{
	auto data = request_scratch_buffer_memory(size);
	if (data.host)
		copy_buffer(buffer, offset, *data.buffer, data.offset, size);
	return data.host;
}

void CommandBuffer::set_uniform_buffer(unsigned set, unsigned binding, const Buffer &buffer, VkDeviceSize offset,
                                       VkDeviceSize range)
{
	VK_ASSERT(set < VULKAN_NUM_DESCRIPTOR_SETS);
	VK_ASSERT(binding < VULKAN_NUM_BINDINGS);
	VK_ASSERT(buffer.get_create_info().usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
	auto &b = bindings.bindings[set][binding];

	if (buffer.get_cookie() != bindings.cookies[set][binding] ||
	         b.buffer.range != range || b.buffer.offset != offset)
	{
		b.buffer = { buffer.get_buffer(), offset, range };
	}

	bindings.cookies[set][binding] = buffer.get_cookie();
	bindings.secondary_cookies[set][binding] = 0;
	dirty_sets_realloc |= 1u << set;
}

void CommandBuffer::set_storage_buffer(unsigned set, unsigned binding, const Buffer &buffer, VkDeviceSize offset,
                                       VkDeviceSize range)
{
	VK_ASSERT(set < VULKAN_NUM_DESCRIPTOR_SETS);
	VK_ASSERT(binding < VULKAN_NUM_BINDINGS);
	VK_ASSERT(buffer.get_create_info().usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	auto &b = bindings.bindings[set][binding];

	if (buffer.get_cookie() != bindings.cookies[set][binding] ||
	         b.buffer.offset != offset || b.buffer.range != range)
	{
		b.buffer = { buffer.get_buffer(), offset, range };
	}

	bindings.cookies[set][binding] = buffer.get_cookie();
	bindings.secondary_cookies[set][binding] = 0;
	dirty_sets_realloc |= 1u << set;
}

void CommandBuffer::set_storage_buffer(unsigned set, unsigned binding, const Buffer &buffer)
{
	set_storage_buffer(set, binding, buffer, 0, buffer.get_create_info().size);
}

void CommandBuffer::set_sampler(unsigned set, unsigned binding, const Sampler &sampler)
{
	VK_ASSERT(set < VULKAN_NUM_DESCRIPTOR_SETS);
	VK_ASSERT(binding < VULKAN_NUM_BINDINGS);
	if (sampler.get_cookie() == bindings.secondary_cookies[set][binding])
		return;

	auto &b = bindings.bindings[set][binding];
	b.image.sampler = sampler.get_sampler();
	dirty_sets_realloc |= 1u << set;
	bindings.secondary_cookies[set][binding] = sampler.get_cookie();
}

void CommandBuffer::set_buffer_view_common(unsigned set, unsigned binding, const BufferView &view,
                                           VkDescriptorType desc_type)
{
	VK_ASSERT(set < VULKAN_NUM_DESCRIPTOR_SETS);
	VK_ASSERT(binding < VULKAN_NUM_BINDINGS);

	auto cookie = view.get_cookie() + (desc_type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
	if (cookie == bindings.cookies[set][binding])
		return;
	auto &b = bindings.bindings[set][binding];

	b.buffer_view.handle = view.get_view();

	bindings.cookies[set][binding] = cookie;
	bindings.secondary_cookies[set][binding] = 0;
	dirty_sets_realloc |= 1u << set;
}

void CommandBuffer::set_buffer_view(unsigned set, unsigned binding, const BufferView &view)
{
	VK_ASSERT(view.get_buffer().get_create_info().usage & VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
	set_buffer_view_common(set, binding, view, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
}

void CommandBuffer::set_texture(unsigned set, unsigned binding,
                                VkImageView view, VkImageLayout layout, uint64_t cookie)
{
	VK_ASSERT(set < VULKAN_NUM_DESCRIPTOR_SETS);
	VK_ASSERT(binding < VULKAN_NUM_BINDINGS);

	if (cookie == bindings.cookies[set][binding] && bindings.bindings[set][binding].image.imageLayout == layout)
		return;

	auto &b = bindings.bindings[set][binding];
	b.image.imageLayout = layout;
	b.image.imageView = view;
	bindings.cookies[set][binding] = cookie;
	dirty_sets_realloc |= 1u << set;
}

void CommandBuffer::set_bindless(unsigned set, const BindlessDescriptorSet &handle)
{
	VK_ASSERT(set < VULKAN_NUM_DESCRIPTOR_SETS);
	VK_ASSERT(handle.valid);
	bindless_sets[set] = handle.handle.set;
	dirty_sets_realloc |= 1u << set;
}

void CommandBuffer::set_texture(unsigned set, unsigned binding, const ImageView &view)
{
	VK_ASSERT(view.get_image().get_create_info().usage & VK_IMAGE_USAGE_SAMPLED_BIT);
	set_texture(set, binding, view.get_view().view, view.get_image().get_layout(VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL),
	            view.get_cookie());
}

enum CookieBits
{
	COOKIE_BIT_UNORM = 1 << 0,
	COOKIE_BIT_SRGB = 1 << 1,
	COOKIE_BIT_PER_MIP = 1 << 4
};

void CommandBuffer::set_texture(unsigned set, unsigned binding, const ImageView &view, const Sampler &sampler)
{
	set_sampler(set, binding, sampler);
	set_texture(set, binding, view);
}

void CommandBuffer::set_texture(unsigned set, unsigned binding, const ImageView &view, StockSampler stock)
{
	VK_ASSERT(set < VULKAN_NUM_DESCRIPTOR_SETS);
	VK_ASSERT(binding < VULKAN_NUM_BINDINGS);
	VK_ASSERT(view.get_image().get_create_info().usage & VK_IMAGE_USAGE_SAMPLED_BIT);
	const auto &sampler = device->get_stock_sampler(stock);
	set_texture(set, binding, view, sampler);
}

void CommandBuffer::set_sampler(unsigned set, unsigned binding, StockSampler stock)
{
	const auto &sampler = device->get_stock_sampler(stock);
	set_sampler(set, binding, sampler);
}

void CommandBuffer::set_storage_texture_level(unsigned set, unsigned binding,
                                              const Vulkan::ImageView &view, unsigned level)
{
	VK_ASSERT(view.get_image().get_create_info().usage & VK_IMAGE_USAGE_STORAGE_BIT);
	auto &mip_view = view.get_mip_view(level);
	set_texture(set, binding, mip_view.view,
	            view.get_image().get_layout(VK_IMAGE_LAYOUT_GENERAL),
	            view.get_cookie() | COOKIE_BIT_PER_MIP | level);
}

void CommandBuffer::flush_descriptor_binds(const VkDescriptorSet *sets,
                                           uint32_t &first_set, uint32_t &set_count)
{
	if (!set_count)
		return;

	table.vkCmdBindDescriptorSets(
			cmd, actual_render_pass ? VK_PIPELINE_BIND_POINT_GRAPHICS : VK_PIPELINE_BIND_POINT_COMPUTE,
			current_pipeline_layout, first_set, set_count, sets, 0, nullptr);

	set_count = 0;
}

void CommandBuffer::rebind_descriptor_set(uint32_t set, VkDescriptorSet *sets, uint32_t &first_set, uint32_t &set_count)
{
	if (set_count == 0)
	{
		first_set = set;
	}
	else if (first_set + set_count != set)
	{
		flush_descriptor_binds(sets, first_set, set_count);
		first_set = set;
	}

	auto &layout = pipeline_state.layout->get_resource_layout();
	if (layout.bindless_descriptor_set_mask & (1u << set))
	{
		VK_ASSERT(bindless_sets[set]);
		sets[set_count++] = bindless_sets[set];
	}
	else
		sets[set_count++] = allocated_sets[set];
}

void CommandBuffer::push_descriptor_set(uint32_t set)
{

	VkDescriptorUpdateTemplate update_template = pipeline_state.layout->get_update_template(set);
	VK_ASSERT(update_template);
	table.vkCmdPushDescriptorSetWithTemplate(
		cmd, update_template,
		pipeline_state.layout->get_layout(), set, bindings.bindings[set]);
}

void CommandBuffer::flush_descriptor_set(uint32_t set, VkDescriptorSet *sets,
                                         uint32_t &first_set, uint32_t &set_count)
{
	if (set_count == 0)
	{
		first_set = set;
	}
	else if (first_set + set_count != set)
	{
		flush_descriptor_binds(sets, first_set, set_count);
		first_set = set;
	}

	auto &layout = pipeline_state.layout->get_resource_layout();
	if (layout.bindless_descriptor_set_mask & (1u << set))
	{
		VK_ASSERT(bindless_sets[set]);
		sets[set_count++] = bindless_sets[set];
		return;
	}


	auto vk_set = pipeline_state.layout->get_allocator(set)->request_descriptor_set(thread_index, device->frame_context_index);

	VkDescriptorUpdateTemplate update_template = pipeline_state.layout->get_update_template(set);
	VK_ASSERT(update_template);
	table.vkUpdateDescriptorSetWithTemplate(device->get_device(), vk_set, update_template, bindings.bindings[set]);
	sets[set_count++] = vk_set;
	allocated_sets[set] = vk_set;
}

void CommandBuffer::flush_descriptor_sets()
{
	auto &layout = pipeline_state.layout->get_resource_layout();

	uint32_t first_set = 0;
	uint32_t set_count = 0;

	dirty_sets_rebind |= dirty_sets_realloc;
	uint32_t set_update_mask = layout.descriptor_set_mask & dirty_sets_rebind;

	VkDescriptorSet sets[VULKAN_NUM_DESCRIPTOR_SETS];

	uint32_t push_set_index = pipeline_state.layout->get_push_set_index();
	if (push_set_index != UINT32_MAX && (dirty_sets_rebind & (1u << push_set_index)) != 0)
	{
		push_descriptor_set(push_set_index);
		set_update_mask &= ~(1u << push_set_index);
	}

	for_each_bit(set_update_mask, [&](uint32_t set)
	{
		if ((dirty_sets_realloc & (1u << set)) != 0)
			flush_descriptor_set(set, sets, first_set, set_count);
		else
			rebind_descriptor_set(set, sets, first_set, set_count);
	});

	flush_descriptor_binds(sets, first_set, set_count);

	dirty_sets_realloc = 0;
	dirty_sets_rebind = 0;
}

void CommandBuffer::draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance)
{
	VK_ASSERT(!is_compute);
	if (flush_render_state(true) != VK_NULL_HANDLE)
	{
		VK_ASSERT(pipeline_state.program->get_shader(ShaderStage::Vertex) != nullptr);
		table.vkCmdDraw(cmd, vertex_count, instance_count, first_vertex, first_instance);
	}
	else
		LOGE("Failed to flush render state, draw call will be dropped.\n");
}

void CommandBuffer::dispatch_indirect(const Buffer &buffer, VkDeviceSize offset)
{
	VK_ASSERT(is_compute);
	if (flush_compute_state(true) != VK_NULL_HANDLE)
	{
		table.vkCmdDispatchIndirect(cmd, buffer.get_buffer(), offset);
	}
	else
		LOGE("Failed to flush render state, dispatch will be dropped.\n");
}

void CommandBuffer::dispatch(uint32_t groups_x, uint32_t groups_y, uint32_t groups_z)
{
	VK_ASSERT(is_compute);
	if (flush_compute_state(true) != VK_NULL_HANDLE)
	{
		table.vkCmdDispatch(cmd, groups_x, groups_y, groups_z);
	}
	else
		LOGE("Failed to flush render state, dispatch will be dropped.\n");
}

void CommandBuffer::clear_render_state()
{
	// Preserve spec constant mask.
	auto &state = pipeline_state.static_state.state;
	memset(&state, 0, sizeof(state));
}

void CommandBuffer::set_opaque_sprite_state()
{
	clear_render_state();
	pipeline_state.static_state.state.write_mask = ~0u;
	set_dirty(COMMAND_BUFFER_DIRTY_STATIC_STATE_BIT);
}

void CommandBuffer::end_threaded_recording()
{
	if (is_ended)
		return;

	is_ended = true;

	// We must end a command buffer on the same thread index we started it on.
	VK_ASSERT(get_current_thread_index() == thread_index);

	if (table.vkEndCommandBuffer(cmd) != VK_SUCCESS)
		LOGE("Failed to end command buffer.\n");
}

void CommandBuffer::end()
{
	VK_ASSERT(!barrier_batch.active);

	// When called, we're holding a device submission lock.
	end_threaded_recording();

	if (ubo_block.is_mapped())
		device->request_uniform_block_nolock(ubo_block, 0);
	if (staging_block.is_mapped())
		device->request_staging_block_nolock(staging_block, 0);
}

/* Labels and regions are recorded only for a tool that reads them
 * (Device::consumes_debug_markers): with none attached each is a driver
 * call on the draw path that nothing ever sees. */
void CommandBuffer::insert_label(const char *name, const float *color)
{
	if (!device->consumes_debug_markers() || !vkCmdInsertDebugUtilsLabelEXT)
		return;

	VkDebugUtilsLabelEXT info = { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
	if (color)
	{
		for (unsigned i = 0; i < 4; i++)
			info.color[i] = color[i];
	}
	else
	{
		for (unsigned i = 0; i < 4; i++)
			info.color[i] = 1.0f;
	}

	info.pLabelName = name;
	vkCmdInsertDebugUtilsLabelEXT(cmd, &info);
}

void CommandBuffer::begin_region(const char *name, const float *color)
{
	if (!device->consumes_debug_markers() || !vkCmdBeginDebugUtilsLabelEXT)
		return;

	VkDebugUtilsLabelEXT info = { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
	if (color)
	{
		for (unsigned i = 0; i < 4; i++)
			info.color[i] = color[i];
	}
	else
	{
		for (unsigned i = 0; i < 4; i++)
			info.color[i] = 1.0f;
	}

	info.pLabelName = name;
	vkCmdBeginDebugUtilsLabelEXT(cmd, &info);
}

void CommandBuffer::end_region()
{
	if (device->consumes_debug_markers() && vkCmdEndDebugUtilsLabelEXT)
		vkCmdEndDebugUtilsLabelEXT(cmd);
}

void CommandBufferDeleter::operator()(Vulkan::CommandBuffer *cmd)
{
	cmd->device->handle_pool.command_buffers.free(cmd);
}
}
