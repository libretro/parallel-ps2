/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#pragma once
#include "common/Pcsx2Defs.h"
#include "VKLoader.h"
#include <string.h>

namespace Vulkan
{
	// Adds a structure to a chain.
	void AddPointerToChain(void* head, const void* ptr);

	class DescriptorSetLayoutBuilder
	{
	public:
		enum : u32
		{
			MAX_BINDINGS = 16,
		};

		DescriptorSetLayoutBuilder();

		void Clear();
		void SetPushFlag();

		VkDescriptorSetLayout Create(VkDevice device);

		void AddBinding(u32 binding, VkDescriptorType dtype, u32 dcount, VkShaderStageFlags stages);

	private:
		VkDescriptorSetLayoutCreateInfo m_ci{};
		VkDescriptorSetLayoutBinding m_bindings[MAX_BINDINGS] = {};
	};

	class PipelineLayoutBuilder
	{
	public:
		enum : u32
		{
			MAX_SETS = 8,
			MAX_PUSH_CONSTANTS = 1
		};

		PipelineLayoutBuilder();

		void Clear();

		VkPipelineLayout Create(VkDevice device);

		void AddDescriptorSet(VkDescriptorSetLayout layout);

		void AddPushConstants(VkShaderStageFlags stages, u32 offset, u32 size);

	private:
		VkPipelineLayoutCreateInfo m_ci{};
		VkDescriptorSetLayout m_sets[MAX_SETS] = {};
		VkPushConstantRange m_push_constants[MAX_PUSH_CONSTANTS] = {};
	};

	class GraphicsPipelineBuilder
	{
	public:
		enum : u32
		{
			MAX_SHADER_STAGES = 3,
			MAX_VERTEX_ATTRIBUTES = 16,
			MAX_VERTEX_BUFFERS = 8,
			MAX_ATTACHMENTS = 2,
			MAX_DYNAMIC_STATE = 16,
			MAX_LIBRARIES = 2
		};

		GraphicsPipelineBuilder();

		void Clear();

		VkPipeline Create(VkDevice device, VkPipelineCache pipeline_cache = VK_NULL_HANDLE, bool clear = true);

		/* spec: the stage's specialization constants, or none; held, not
		 * copied, until Create() */
		void SetShaderStage(VkShaderStageFlagBits stage, VkShaderModule module, const char* entry_point,
			const VkSpecializationInfo* spec = nullptr);
		void SetVertexShader(VkShaderModule module, const VkSpecializationInfo* spec = nullptr)
		{
			SetShaderStage(VK_SHADER_STAGE_VERTEX_BIT, module, "main", spec);
		}
		void SetFragmentShader(VkShaderModule module, const VkSpecializationInfo* spec = nullptr)
		{
			SetShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, module, "main", spec);
		}

		void AddVertexBuffer(u32 binding, u32 stride, VkVertexInputRate input_rate = VK_VERTEX_INPUT_RATE_VERTEX);
		void AddVertexAttribute(u32 location, u32 binding, VkFormat format, u32 offset);

		void SetPrimitiveTopology(VkPrimitiveTopology topology, bool enable_primitive_restart = false);

		void SetRasterizationState(VkPolygonMode polygon_mode, VkCullModeFlags cull_mode, VkFrontFace front_face);
		void SetLineWidth(float width);
		void SetLineRasterizationMode(VkLineRasterizationModeEXT mode);
		void SetMultisamples(u32 multisamples, bool per_sample_shading);
		void SetNoCullRasterizationState();

		void SetDepthState(bool depth_test, bool depth_write, VkCompareOp compare_op);
		void SetStencilState(bool stencil_test, const VkStencilOpState& front, const VkStencilOpState& back);
		void SetNoDepthTestState();
		void SetNoStencilState();

		void AddBlendAttachment(bool blend_enable, VkBlendFactor src_factor, VkBlendFactor dst_factor, VkBlendOp op,
			VkBlendFactor alpha_src_factor, VkBlendFactor alpha_dst_factor, VkBlendOp alpha_op,
			VkColorComponentFlags write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
											   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
		void SetBlendAttachment(u32 attachment, bool blend_enable, VkBlendFactor src_factor, VkBlendFactor dst_factor,
			VkBlendOp op, VkBlendFactor alpha_src_factor, VkBlendFactor alpha_dst_factor, VkBlendOp alpha_op,
			VkColorComponentFlags write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
											   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
		void SetColorWriteMask(
			u32 attachment, VkColorComponentFlags write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
		void AddBlendFlags(u32 flags);
		void ClearBlendAttachments();

		void SetBlendConstants(float r, float g, float b, float a);
		void SetNoBlendingState();

		void AddDynamicState(VkDynamicState state);

		void SetDynamicViewportAndScissorState();
		void SetViewport(float x, float y, float width, float height, float min_depth, float max_depth);
		void SetScissorRect(s32 x, s32 y, u32 width, u32 height);

		void SetMultisamples(VkSampleCountFlagBits samples);

		void SetPipelineLayout(VkPipelineLayout layout);
		void SetRenderPass(VkRenderPass render_pass, u32 subpass);

		void SetProvokingVertex(VkProvokingVertexModeEXT mode);

		/* A graphics pipeline library of these parts (VK_EXT_graphics_
		 * pipeline_library) instead of a pipeline: the shader stages of
		 * other parts are left out, the rest of their state is ignored. */
		void SetLibrary(VkGraphicsPipelineLibraryFlagsEXT parts);
		/* A pipeline linked from libraries that hold all its parts, without
		 * link-time optimization; its own stages and state are unused. */
		void SetLinkedLibraries(const VkPipeline* libraries, u32 count);

	private:
		VkGraphicsPipelineCreateInfo m_ci;
		VkPipelineShaderStageCreateInfo m_shader_stages[MAX_SHADER_STAGES];

		VkPipelineVertexInputStateCreateInfo m_vertex_input_state;
		VkVertexInputBindingDescription m_vertex_buffers[MAX_VERTEX_BUFFERS];
		VkVertexInputAttributeDescription m_vertex_attributes[MAX_VERTEX_ATTRIBUTES];

		VkPipelineInputAssemblyStateCreateInfo m_input_assembly;

		VkPipelineRasterizationStateCreateInfo m_rasterization_state;
		VkPipelineDepthStencilStateCreateInfo m_depth_state;

		VkPipelineColorBlendStateCreateInfo m_blend_state;
		VkPipelineColorBlendAttachmentState m_blend_attachments[MAX_ATTACHMENTS];

		VkPipelineViewportStateCreateInfo m_viewport_state;
		VkViewport m_viewport;
		VkRect2D m_scissor;

		VkPipelineDynamicStateCreateInfo m_dynamic_state;
		VkDynamicState m_dynamic_state_values[MAX_DYNAMIC_STATE];

		VkPipelineMultisampleStateCreateInfo m_multisample_state;

		VkPipelineRasterizationProvokingVertexStateCreateInfoEXT m_provoking_vertex;
		VkPipelineRasterizationLineStateCreateInfoEXT m_line_rasterization_state;

		VkGraphicsPipelineLibraryCreateInfoEXT m_library;
		VkPipelineLibraryCreateInfoKHR m_linked;
		VkPipeline m_linked_libraries[MAX_LIBRARIES];
	};

	class ComputePipelineBuilder
	{
	public:
		enum : u32
		{
			SPECIALIZATION_CONSTANT_SIZE = 4,
			MAX_SPECIALIZATION_CONSTANTS = 4,
		};

		ComputePipelineBuilder();

		void Clear();

		VkPipeline Create(VkDevice device, VkPipelineCache pipeline_cache = VK_NULL_HANDLE, bool clear = true);

		void SetShader(VkShaderModule module, const char* entry_point);

		void SetPipelineLayout(VkPipelineLayout layout);

		void SetSpecializationBool(u32 index, bool value);

	private:
		void SetSpecializationValue(u32 index, u32 value);

		VkComputePipelineCreateInfo m_ci;

		VkSpecializationInfo m_si;
		VkSpecializationMapEntry m_smap_entries[MAX_SPECIALIZATION_CONSTANTS];
		u8 m_smap_constants[SPECIALIZATION_CONSTANT_SIZE* MAX_SPECIALIZATION_CONSTANTS];
	};

	class SamplerBuilder
	{
	public:
		SamplerBuilder();

		void Clear();

		VkSampler Create(VkDevice device, bool clear = true);

		void SetFilter(VkFilter mag_filter, VkFilter min_filter, VkSamplerMipmapMode mip_filter);
		void SetAddressMode(VkSamplerAddressMode u, VkSamplerAddressMode v, VkSamplerAddressMode w);

		void SetPointSampler(VkSamplerAddressMode address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
		void SetLinearSampler(
			bool mipmaps, VkSamplerAddressMode address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);

	private:
		VkSamplerCreateInfo m_ci;
	};

	class DescriptorSetUpdateBuilder
	{
		enum : u32
		{
			MAX_WRITES = 16,
			MAX_IMAGE_INFOS = 8,
			MAX_BUFFER_INFOS = 4,
			MAX_VIEWS = 4,
		};

	public:
		DescriptorSetUpdateBuilder();

		void Clear();

		void Update(VkDevice device, bool clear = true);
		void PushUpdate(VkCommandBuffer cmdbuf, VkPipelineBindPoint bind_point, VkPipelineLayout layout, u32 set,
			bool clear = true);

		void AddImageDescriptorWrite(VkDescriptorSet set, u32 binding, VkImageView view,
			VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		void AddSamplerDescriptorWrite(VkDescriptorSet set, u32 binding, VkSampler sampler);
		void AddSamplerDescriptorWrites(VkDescriptorSet set, u32 binding, const VkSampler* samplers, u32 num_samplers);
		void AddCombinedImageSamplerDescriptorWrite(VkDescriptorSet set, u32 binding, VkImageView view,
			VkSampler sampler, VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		void AddCombinedImageSamplerDescriptorWrites(VkDescriptorSet set, u32 binding, const VkImageView* views,
			const VkSampler* samplers, u32 num_views, VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		void AddBufferDescriptorWrite(
			VkDescriptorSet set, u32 binding, VkDescriptorType dtype, VkBuffer buffer, u32 offset, u32 size);
		void AddBufferViewDescriptorWrite(VkDescriptorSet set, u32 binding, VkDescriptorType dtype, VkBufferView view);
		void AddInputAttachmentDescriptorWrite(
			VkDescriptorSet set, u32 binding, VkImageView view, VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL);
		void AddStorageImageDescriptorWrite(
			VkDescriptorSet set, u32 binding, VkImageView view, VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL);

	private:
		VkWriteDescriptorSet m_writes[MAX_WRITES];
		u32 m_num_writes = 0;

		VkDescriptorBufferInfo m_buffer_infos[MAX_BUFFER_INFOS];
		VkDescriptorImageInfo m_image_infos[MAX_IMAGE_INFOS];
		VkBufferView m_views[MAX_VIEWS];
		u32 m_num_buffer_infos = 0;
		u32 m_num_image_infos = 0;
		u32 m_num_views = 0;
	};

	class FramebufferBuilder
	{
		enum : u32
		{
			MAX_ATTACHMENTS = 2,
		};

	public:
		FramebufferBuilder();

		void Clear();

		VkFramebuffer Create(VkDevice device, bool clear = true);

		void AddAttachment(VkImageView image);

		void SetSize(u32 width, u32 height, u32 layers);

		void SetRenderPass(VkRenderPass render_pass);

	private:
		VkFramebufferCreateInfo m_ci;
		VkImageView m_images[MAX_ATTACHMENTS];
	};

	class RenderPassBuilder
	{
		enum : u32
		{
			MAX_ATTACHMENTS = 2,
			MAX_ATTACHMENT_REFERENCES = 2,
			MAX_SUBPASSES = 1,
		};

	public:
		RenderPassBuilder();

		void Clear();

		VkRenderPass Create(VkDevice device, bool clear = true);

		u32 AddAttachment(VkFormat format, VkSampleCountFlagBits samples, VkAttachmentLoadOp load_op,
			VkAttachmentStoreOp store_op, VkImageLayout initial_layout, VkImageLayout final_layout);

		u32 AddSubpass();
		void AddSubpassColorAttachment(u32 subpass, u32 attachment, VkImageLayout layout);
		void AddSubpassDepthAttachment(u32 subpass, u32 attachment, VkImageLayout layout);

	private:
		VkRenderPassCreateInfo m_ci;
		VkAttachmentDescription m_attachments[MAX_ATTACHMENTS];
		VkAttachmentReference m_attachment_references[MAX_ATTACHMENT_REFERENCES];
		u32 m_num_attachment_references = 0;
		VkSubpassDescription m_subpasses[MAX_SUBPASSES];
	};

	class BufferViewBuilder
	{
	public:
		BufferViewBuilder();

		void Clear();

		VkBufferView Create(VkDevice device, bool clear = true);

		void Set(VkBuffer buffer, VkFormat format, u32 offset, u32 size);

	private:
		VkBufferViewCreateInfo m_ci;
	};

} // namespace Vulkan
