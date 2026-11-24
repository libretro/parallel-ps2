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

#include "buffer.hpp"
#include "buffer_pool.hpp"
#include "vulkan_headers.hpp"
#include "image.hpp"
#include "render_pass.hpp"
#include "small_vector.hpp"
#include "sampler.hpp"
#include "shader.hpp"
#include "vulkan_common.hpp"
#include <string.h>

namespace Vulkan
{
enum CommandBufferDirtyBits
{
	COMMAND_BUFFER_DIRTY_STATIC_STATE_BIT = 1 << 0,
	COMMAND_BUFFER_DIRTY_PIPELINE_BIT = 1 << 1,

	COMMAND_BUFFER_DIRTY_VIEWPORT_BIT = 1 << 2,
	COMMAND_BUFFER_DIRTY_SCISSOR_BIT = 1 << 3,
	COMMAND_BUFFER_DIRTY_PUSH_CONSTANTS_BIT = 1 << 4,
	COMMAND_BUFFER_DIRTY_BLEND_CONSTANTS_BIT = 1 << 5,

	COMMAND_BUFFER_DYNAMIC_BITS = COMMAND_BUFFER_DIRTY_VIEWPORT_BIT | COMMAND_BUFFER_DIRTY_SCISSOR_BIT |
	                              COMMAND_BUFFER_DIRTY_BLEND_CONSTANTS_BIT
};
using CommandBufferDirtyFlags = uint32_t;

#define BLEND_FACTOR_BITS 5
#define BLEND_OP_BITS 3
/* Every pass draws a triangle strip, culling nothing, into colour alone:
 * blend, write mask and subgroup control are all a pipeline varies by. */
union PipelineState {
	struct
	{
		// Word 0, tightly packed.
		uint32_t blend_enable : 1;
		uint32_t src_color_blend : BLEND_FACTOR_BITS;
		uint32_t dst_color_blend : BLEND_FACTOR_BITS;
		uint32_t color_blend_op : BLEND_OP_BITS;
		uint32_t src_alpha_blend : BLEND_FACTOR_BITS;
		uint32_t dst_alpha_blend : BLEND_FACTOR_BITS;
		uint32_t alpha_blend_op : BLEND_OP_BITS;
		/* Blend constants are dynamic state, set by
		 * set_dynamic_blend_constants: not part of the pipeline. */
		uint32_t dynamic_blend_constants : 1;
		uint32_t subgroup_control_size : 1;
		uint32_t subgroup_full_group : 1;
		uint32_t padding0 : 2;

		// Word 1, tightly packed.
		uint32_t subgroup_minimum_size_log2 : 3;
		uint32_t subgroup_maximum_size_log2 : 3;
		uint32_t padding1 : 26;

		// Word 2
		uint32_t write_mask;
	} state;
	uint32_t words[3];
};

struct PotentialState
{
	float blend_constants[4];
	uint32_t spec_constants[VULKAN_NUM_USER_SPEC_CONSTANTS];
	uint8_t spec_constant_mask;
};

struct DynamicState
{
	float blend_constants[4] = {};
};

static_assert(VULKAN_NUM_DESCRIPTOR_SETS == 4, "Number of descriptor sets != 4.");

struct DeferredPipelineCompile
{
	Program *program;
	const PipelineLayout *layout;

	const RenderPass *compatible_render_pass;
	PipelineState static_state;
	PotentialState potential_static_state;

	Util::Hash hash;
	VkPipelineCache cache;
	uint32_t subgroup_size_tag;
};

class CommandBuffer;
struct CommandBufferDeleter
{
	void operator()(CommandBuffer *cmd);
};

class Device;
class CommandBuffer : public Util::IntrusivePtrEnabled<CommandBuffer, CommandBufferDeleter, HandleCounter>
{
public:
	friend struct CommandBufferDeleter;
	enum class Type
	{
		Generic = QUEUE_INDEX_GRAPHICS,
		AsyncCompute = QUEUE_INDEX_COMPUTE,
		AsyncTransfer = QUEUE_INDEX_TRANSFER,
		Count
	};

	~CommandBuffer();
	VkCommandBuffer get_command_buffer() const
	{
		return cmd;
	}

	void begin_region(const char *name, const float *color = nullptr);
	void insert_label(const char *name, const float *color = nullptr);
	void end_region();

	Device &get_device()
	{
		return *device;
	}

	void set_thread_index(unsigned index_)
	{
		thread_index = index_;
	}

	unsigned get_thread_index() const
	{
		return thread_index;
	}

	void fill_buffer(const Buffer &dst, uint32_t value);
	void fill_buffer(const Buffer &dst, uint32_t value, VkDeviceSize offset, VkDeviceSize size);
	void copy_buffer(const Buffer &dst, VkDeviceSize dst_offset, const Buffer &src, VkDeviceSize src_offset,
	                 VkDeviceSize size);
	void copy_buffer(const Buffer &dst, const Buffer &src);
	void copy_image(const Image &dst, const Image &src,
	                const VkOffset3D &dst_offset, const VkOffset3D &src_offset,
	                const VkExtent3D &extent,
	                const VkImageSubresourceLayers &dst_subresource,
	                const VkImageSubresourceLayers &src_subresource);

	void copy_buffer_to_image(const Image &image, const Buffer &buffer, unsigned num_blits, const VkBufferImageCopy *blits);

	// Simplified global memory barrier.
	void barrier(VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
	             VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);

	// Full expressive barrier.
	void barrier(const VkDependencyInfo &dep);

	void image_barrier(const Image &image,
	                   VkImageLayout old_layout, VkImageLayout new_layout,
	                   VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
	                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);

	void image_barriers(uint32_t image_barriers, const VkImageMemoryBarrier2 *images);

	void begin_render_pass(const RenderPassInfo &info, VkSubpassContents contents = VK_SUBPASS_CONTENTS_INLINE);
	void end_render_pass();

	void set_program(Program *program);

	void set_buffer_view(unsigned set, unsigned binding, const BufferView &view);
	void set_texture(unsigned set, unsigned binding, const ImageView &view);
	void set_texture(unsigned set, unsigned binding, const ImageView &view, const Sampler &sampler);
	void set_texture(unsigned set, unsigned binding, const ImageView &view, StockSampler sampler);
	void set_storage_texture_level(unsigned set, unsigned binding, const ImageView &view, unsigned level);
	void set_sampler(unsigned set, unsigned binding, const Sampler &sampler);
	void set_sampler(unsigned set, unsigned binding, StockSampler sampler);
	void set_uniform_buffer(unsigned set, unsigned binding, const Buffer &buffer, VkDeviceSize offset,
	                        VkDeviceSize range);
	void set_storage_buffer(unsigned set, unsigned binding, const Buffer &buffer);
	void set_storage_buffer(unsigned set, unsigned binding, const Buffer &buffer, VkDeviceSize offset,
	                        VkDeviceSize range);

	void set_bindless(unsigned set, const BindlessDescriptorSet &handle);

	void push_constants(const void *data, VkDeviceSize offset, VkDeviceSize range);

	void *allocate_constant_data(unsigned set, unsigned binding, VkDeviceSize size);

	template <typename T>
	T *allocate_typed_constant_data(unsigned set, unsigned binding, unsigned count)
	{
		return static_cast<T *>(allocate_constant_data(set, binding, count * sizeof(T)));
	}

	void *update_buffer(const Buffer &buffer, VkDeviceSize offset, VkDeviceSize size);
	BufferBlockAllocation request_scratch_buffer_memory(VkDeviceSize size);

	void set_viewport(const VkViewport &viewport);
	void set_scissor(const VkRect2D &rect);

	void draw(uint32_t vertex_count, uint32_t instance_count = 1, uint32_t first_vertex = 0,
	          uint32_t first_instance = 0);

	void dispatch(uint32_t groups_x, uint32_t groups_y, uint32_t groups_z);

	void dispatch_indirect(const Buffer &buffer, VkDeviceSize offset);
	void set_opaque_sprite_state();

#define SET_STATIC_STATE(value)                               \
	do                                                        \
	{                                                         \
		if (pipeline_state.static_state.state.value != value) \
		{                                                     \
			pipeline_state.static_state.state.value = value;  \
			set_dirty(COMMAND_BUFFER_DIRTY_STATIC_STATE_BIT); \
		}                                                     \
	} while (0)

#define SET_POTENTIALLY_STATIC_STATE(value)                       \
	do                                                            \
	{                                                             \
		if (pipeline_state.potential_static_state.value != value) \
		{                                                         \
			pipeline_state.potential_static_state.value = value;  \
			set_dirty(COMMAND_BUFFER_DIRTY_STATIC_STATE_BIT);     \
		}                                                         \
	} while (0)

	inline void set_blend_enable(bool blend_enable)
	{
		SET_STATIC_STATE(blend_enable);
	}

	inline void set_blend_factors(VkBlendFactor src_color_blend, VkBlendFactor src_alpha_blend,
	                              VkBlendFactor dst_color_blend, VkBlendFactor dst_alpha_blend)
	{
		SET_STATIC_STATE(src_color_blend);
		SET_STATIC_STATE(dst_color_blend);
		SET_STATIC_STATE(src_alpha_blend);
		SET_STATIC_STATE(dst_alpha_blend);
	}

	inline void set_blend_factors(VkBlendFactor src_blend, VkBlendFactor dst_blend)
	{
		set_blend_factors(src_blend, src_blend, dst_blend, dst_blend);
	}

	inline void set_blend_op(VkBlendOp color_blend_op, VkBlendOp alpha_blend_op)
	{
		SET_STATIC_STATE(color_blend_op);
		SET_STATIC_STATE(alpha_blend_op);
	}

	inline void set_blend_op(VkBlendOp blend_op)
	{
		set_blend_op(blend_op, blend_op);
	}

	inline void set_specialization_constant_mask(uint32_t spec_constant_mask)
	{
		VK_ASSERT((spec_constant_mask & ~((1u << VULKAN_NUM_USER_SPEC_CONSTANTS) - 1u)) == 0u);
		SET_POTENTIALLY_STATIC_STATE(spec_constant_mask);
	}

	template <typename T>
	inline void set_specialization_constant(unsigned index, const T &value)
	{
		VK_ASSERT(index < VULKAN_NUM_USER_SPEC_CONSTANTS);
		static_assert(sizeof(value) == sizeof(uint32_t), "Spec constant data must be 32-bit.");
		if (memcmp(&pipeline_state.potential_static_state.spec_constants[index], &value, sizeof(value)))
		{
			memcpy(&pipeline_state.potential_static_state.spec_constants[index], &value, sizeof(value));
			if (pipeline_state.potential_static_state.spec_constant_mask & (1u << index))
				set_dirty(COMMAND_BUFFER_DIRTY_STATIC_STATE_BIT);
		}
	}

	inline void set_specialization_constant(unsigned index, bool value)
	{
		set_specialization_constant(index, uint32_t(value));
	}

	inline uint32_t get_specialization_constant(unsigned index) const
	{
		uint32_t value;
		VK_ASSERT(index < VULKAN_NUM_USER_SPEC_CONSTANTS);
		memcpy(&value, &pipeline_state.potential_static_state.spec_constants[index], sizeof(value));
		return value;
	}

	inline Program *get_program() const
	{
		return pipeline_state.program;
	}

	inline uint32_t get_specialization_constant_mask() const
	{
		return pipeline_state.potential_static_state.spec_constant_mask;
	}

	inline void enable_subgroup_size_control(bool subgroup_control_size)
	{
		SET_STATIC_STATE(subgroup_control_size);
	}

	inline void set_subgroup_size_log2(bool subgroup_full_group,
	                                   uint8_t subgroup_minimum_size_log2,
	                                   uint8_t subgroup_maximum_size_log2)
	{
		VK_ASSERT(subgroup_minimum_size_log2 < 8);
		VK_ASSERT(subgroup_maximum_size_log2 < 8);

		SET_STATIC_STATE(subgroup_full_group);
		SET_STATIC_STATE(subgroup_minimum_size_log2);
		SET_STATIC_STATE(subgroup_maximum_size_log2);
	}

#define SET_DYNAMIC_STATE(state, flags)   \
	do                                    \
	{                                     \
		if (dynamic_state.state != state) \
		{                                 \
			dynamic_state.state = state;  \
			set_dirty(flags);             \
		}                                 \
	} while (0)

	/* Blend constants the pipeline takes as dynamic state: one pipeline
	 * for every value, the value set with vkCmdSetBlendConstants when it
	 * changes. Static state again after the next clear of the state. */
	inline void set_dynamic_blend_constants(const float blend_constants[4])
	{
		if (!pipeline_state.static_state.state.dynamic_blend_constants)
		{
			pipeline_state.static_state.state.dynamic_blend_constants = 1;
			set_dirty(COMMAND_BUFFER_DIRTY_STATIC_STATE_BIT);
		}
		SET_DYNAMIC_STATE(blend_constants[0], COMMAND_BUFFER_DIRTY_BLEND_CONSTANTS_BIT);
		SET_DYNAMIC_STATE(blend_constants[1], COMMAND_BUFFER_DIRTY_BLEND_CONSTANTS_BIT);
		SET_DYNAMIC_STATE(blend_constants[2], COMMAND_BUFFER_DIRTY_BLEND_CONSTANTS_BIT);
		SET_DYNAMIC_STATE(blend_constants[3], COMMAND_BUFFER_DIRTY_BLEND_CONSTANTS_BIT);
	}

	inline Type get_command_buffer_type() const
	{
		return type;
	}

	// Used when recording command buffers in a thread, and submitting them in a different thread.
	// Need to make sure that no further commands on the VkCommandBuffer happen.
	void end_threaded_recording();
	// End is called automatically by Device in submission. Should not be called by application.
	void end();

	void extract_pipeline_state(DeferredPipelineCompile &compile) const;

	enum class CompileMode
	{
		Sync,
		FailOnCompileRequired,
		/* Built ahead, off the recording thread: returned to the caller
		 * and held by nothing, for Device::hand_over_pipeline. */
		AsyncThread
	};
	static Pipeline build_graphics_pipeline(Device *device, const DeferredPipelineCompile &compile, CompileMode mode);
	static Pipeline build_compute_pipeline(Device *device, const DeferredPipelineCompile &compile, CompileMode mode);
	static void update_hash_compute_pipeline(DeferredPipelineCompile &compile);
	static void update_hash_graphics_pipeline(DeferredPipelineCompile &compile);
	/* The graphics state set so far as it would be drawn with in @pass,
	 * whether or not a render pass is begun. */
	void extract_graphics_pipeline_state(DeferredPipelineCompile &compile,
	                                     const RenderPass &pass) const;

private:
	friend class Util::ObjectPool<CommandBuffer>;
	friend class Util::ThreadSafeObjectPool<CommandBuffer>;
	CommandBuffer(Device *device, VkCommandBuffer cmd, VkPipelineCache cache, Type type, bool secondary);

	Device *device;
	const VulkanDeviceTable &table;
	VkCommandBuffer cmd;
	Type type;

	const Framebuffer *framebuffer = nullptr;
	const RenderPass *actual_render_pass = nullptr;

	ResourceBindings bindings;
	VkDescriptorSet bindless_sets[VULKAN_NUM_DESCRIPTOR_SETS] = {};
	VkDescriptorSet allocated_sets[VULKAN_NUM_DESCRIPTOR_SETS] = {};

	Pipeline current_pipeline = {};
	VkPipelineLayout current_pipeline_layout = VK_NULL_HANDLE;
	VkSubpassContents current_contents = VK_SUBPASS_CONTENTS_INLINE;
	unsigned thread_index = 0;

	VkViewport viewport = {};
	VkRect2D scissor = {};

	CommandBufferDirtyFlags dirty = ~0u;
	uint32_t dirty_sets_realloc = 0;
	uint32_t dirty_sets_rebind = 0;
	bool is_compute = true;
	bool is_secondary = false;
	bool is_ended = false;

	void set_dirty(CommandBufferDirtyFlags flags)
	{
		dirty |= flags;
	}

	CommandBufferDirtyFlags get_and_clear(CommandBufferDirtyFlags flags)
	{
		auto mask = dirty & flags;
		dirty &= ~flags;
		return mask;
	}

	DeferredPipelineCompile pipeline_state = {};
	DynamicState dynamic_state = {};
#ifndef _MSC_VER
	static_assert(sizeof(pipeline_state.static_state.words) >= sizeof(pipeline_state.static_state.state),
	              "Hashable pipeline state is not large enough!");
#endif

	VkPipeline flush_render_state(bool synchronous);
	VkPipeline flush_compute_state(bool synchronous);
	void clear_render_state();

	bool flush_graphics_pipeline(bool synchronous);
	bool flush_compute_pipeline(bool synchronous);
	bool flush_compute_pipeline_miss(bool synchronous);
	bool flush_graphics_pipeline_miss(bool synchronous);
	void flush_dynamic_render_state();
	void flush_descriptor_sets();
	void begin_graphics();
	void flush_descriptor_set(
		uint32_t set, VkDescriptorSet *sets,
		uint32_t &first_set, uint32_t &set_count);
	void push_descriptor_set(uint32_t set);
	void rebind_descriptor_set(
		uint32_t set, VkDescriptorSet *sets,
		uint32_t &first_set, uint32_t &set_count);
	void flush_descriptor_binds(const VkDescriptorSet *sets,
		uint32_t &first_set, uint32_t &set_count);

	void begin_compute();
	void begin_context();

	BufferBlock ubo_block;
	BufferBlock staging_block;

	void set_texture(unsigned set, unsigned binding,
	                 VkImageView view, VkImageLayout layout, uint64_t cookie);
	void set_buffer_view_common(unsigned set, unsigned binding, const BufferView &view, VkDescriptorType type);

	void init_viewport_scissor(const Framebuffer *framebuffer);

	void bind_pipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline, uint32_t active_dynamic_state);

	void set_program_layout(const PipelineLayout *layout);

	static bool setup_subgroup_size_control(Device &device, VkPipelineShaderStageCreateInfo &stage_info,
	                                        VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT &required_info,
	                                        VkShaderStageFlagBits stage,
	                                        bool full_group, unsigned min_size_log2, unsigned max_size_log2);

	struct
	{
		Util::SmallVector<VkMemoryBarrier2> memory_barriers;
		Util::SmallVector<VkBufferMemoryBarrier2> buffer_barriers;
		Util::SmallVector<VkImageMemoryBarrier2> image_barriers;
		bool active = false;
	} barrier_batch;
};

using CommandBufferHandle = Util::IntrusivePtr<CommandBuffer>;
}
