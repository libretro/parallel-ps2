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

#ifdef __cplusplus
extern "C" {
#endif

/* Vulkan create infos filled in place: each builder holds the arrays its
 * create info points into, so it must not be copied once something is
 * added. */

/* @ptr appended to @head's pNext chain, unless it is in it already. */
void gs_vk_add_to_chain(void* head, const void* ptr);

#define GS_VK_DSL_MAX_BINDINGS 16
struct gs_vk_dsl_builder
{
	VkDescriptorSetLayoutCreateInfo ci;
	VkDescriptorSetLayoutBinding bindings[GS_VK_DSL_MAX_BINDINGS];
};
void gs_vk_dsl_init(struct gs_vk_dsl_builder* b);
void gs_vk_dsl_add_binding(struct gs_vk_dsl_builder* b, u32 binding, VkDescriptorType type, u32 count,
	VkShaderStageFlags stages);
/* VK_NULL_HANDLE on failure; emptied for the next on success. */
VkDescriptorSetLayout gs_vk_dsl_create(struct gs_vk_dsl_builder* b, VkDevice device);

#define GS_VK_PL_MAX_SETS 8
#define GS_VK_PL_MAX_PUSH_CONSTANTS 1
struct gs_vk_pl_builder
{
	VkPipelineLayoutCreateInfo ci;
	VkDescriptorSetLayout sets[GS_VK_PL_MAX_SETS];
	VkPushConstantRange push_constants[GS_VK_PL_MAX_PUSH_CONSTANTS];
};
void gs_vk_pl_init(struct gs_vk_pl_builder* b);
void gs_vk_pl_add_set(struct gs_vk_pl_builder* b, VkDescriptorSetLayout layout);
void gs_vk_pl_add_push_constants(struct gs_vk_pl_builder* b, VkShaderStageFlags stages, u32 offset, u32 size);
/* VK_NULL_HANDLE on failure; emptied for the next on success. */
VkPipelineLayout gs_vk_pl_create(struct gs_vk_pl_builder* b, VkDevice device);

#define GS_VK_GP_MAX_SHADER_STAGES 3
#define GS_VK_GP_MAX_VERTEX_ATTRIBUTES 16
#define GS_VK_GP_MAX_VERTEX_BUFFERS 8
#define GS_VK_GP_MAX_ATTACHMENTS 2
#define GS_VK_GP_MAX_DYNAMIC_STATE 16
#define GS_VK_GP_MAX_LIBRARIES 3
struct gs_vk_gp_builder
{
	VkGraphicsPipelineCreateInfo ci;
	VkPipelineShaderStageCreateInfo shader_stages[GS_VK_GP_MAX_SHADER_STAGES];
	VkPipelineVertexInputStateCreateInfo vertex_input;
	VkVertexInputBindingDescription vertex_buffers[GS_VK_GP_MAX_VERTEX_BUFFERS];
	VkVertexInputAttributeDescription vertex_attributes[GS_VK_GP_MAX_VERTEX_ATTRIBUTES];
	VkPipelineInputAssemblyStateCreateInfo input_assembly;
	VkPipelineRasterizationStateCreateInfo rasterization;
	VkPipelineDepthStencilStateCreateInfo depth;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineColorBlendAttachmentState blend_attachments[GS_VK_GP_MAX_ATTACHMENTS];
	VkPipelineViewportStateCreateInfo viewport_state;
	VkViewport viewport;
	VkRect2D scissor;
	VkPipelineDynamicStateCreateInfo dynamic;
	VkDynamicState dynamic_values[GS_VK_GP_MAX_DYNAMIC_STATE];
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_vertex;
	VkPipelineRasterizationLineStateCreateInfoEXT line_rasterization;
	VkGraphicsPipelineLibraryCreateInfoEXT library;
	VkPipelineLibraryCreateInfoKHR linked;
	VkPipeline linked_libraries[GS_VK_GP_MAX_LIBRARIES];
};
/* Triangle lists, no culling, depth, stencil or blending, writing all of
 * RGBA, one sample, a 1x1 viewport and scissor. */
void gs_vk_gp_init(struct gs_vk_gp_builder* b);
void gs_vk_gp_set_layout(struct gs_vk_gp_builder* b, VkPipelineLayout layout);
void gs_vk_gp_set_render_pass(struct gs_vk_gp_builder* b, VkRenderPass render_pass, u32 subpass);
/* The stage's entry point "main"; @spec, or NULL, is held, not copied,
 * until gs_vk_gp_create(). */
void gs_vk_gp_set_shader(struct gs_vk_gp_builder* b, VkShaderStageFlagBits stage, VkShaderModule module,
	const VkSpecializationInfo* spec);
void gs_vk_gp_add_vertex_buffer(struct gs_vk_gp_builder* b, u32 binding, u32 stride);
void gs_vk_gp_add_vertex_attribute(struct gs_vk_gp_builder* b, u32 location, u32 binding, VkFormat format, u32 offset);
void gs_vk_gp_set_topology(struct gs_vk_gp_builder* b, VkPrimitiveTopology topology);
void gs_vk_gp_set_rasterization(struct gs_vk_gp_builder* b, VkPolygonMode polygon_mode, VkCullModeFlags cull_mode,
	VkFrontFace front_face);
void gs_vk_gp_set_line_rasterization(struct gs_vk_gp_builder* b, VkLineRasterizationModeEXT mode);
void gs_vk_gp_set_provoking_vertex(struct gs_vk_gp_builder* b, VkProvokingVertexModeEXT mode);
void gs_vk_gp_set_depth(struct gs_vk_gp_builder* b, int test, int write, VkCompareOp op);
void gs_vk_gp_set_stencil(struct gs_vk_gp_builder* b, int test, const VkStencilOpState* front,
	const VkStencilOpState* back);
void gs_vk_gp_set_blend_attachment(struct gs_vk_gp_builder* b, u32 attachment, int enable, VkBlendFactor src,
	VkBlendFactor dst, VkBlendOp op, VkBlendFactor alpha_src, VkBlendFactor alpha_dst, VkBlendOp alpha_op,
	VkColorComponentFlags write_mask);
void gs_vk_gp_set_write_mask(struct gs_vk_gp_builder* b, u32 attachment, VkColorComponentFlags write_mask);
void gs_vk_gp_add_blend_flags(struct gs_vk_gp_builder* b, VkPipelineColorBlendStateCreateFlags flags);
void gs_vk_gp_add_dynamic_state(struct gs_vk_gp_builder* b, VkDynamicState state);
/* A graphics pipeline library of these parts (VK_EXT_graphics_pipeline_
 * library) instead of a pipeline: the shader stages of other parts are
 * left out, the rest of their state is ignored. */
void gs_vk_gp_set_library(struct gs_vk_gp_builder* b, VkGraphicsPipelineLibraryFlagsEXT parts);
/* A pipeline linked from libraries that hold all its parts, without
 * link-time optimization; its own stages and state are unused. */
void gs_vk_gp_set_linked_libraries(struct gs_vk_gp_builder* b, const VkPipeline* libraries, u32 count);
/* VK_NULL_HANDLE on failure; back to its initial state on success. */
VkPipeline gs_vk_gp_create(struct gs_vk_gp_builder* b, VkDevice device, VkPipelineCache cache);

/* Descriptor writes for one vkUpdateDescriptorSets. */
#define GS_VK_DSU_MAX_WRITES 16
#define GS_VK_DSU_MAX_IMAGE_INFOS 8
#define GS_VK_DSU_MAX_BUFFER_INFOS 4
struct gs_vk_ds_update
{
	VkWriteDescriptorSet writes[GS_VK_DSU_MAX_WRITES];
	VkDescriptorImageInfo image_infos[GS_VK_DSU_MAX_IMAGE_INFOS];
	VkDescriptorBufferInfo buffer_infos[GS_VK_DSU_MAX_BUFFER_INFOS];
	u32 num_writes;
	u32 num_image_infos;
	u32 num_buffer_infos;
};
void gs_vk_dsu_init(struct gs_vk_ds_update* u);
void gs_vk_dsu_add_image(struct gs_vk_ds_update* u, VkDescriptorSet set, u32 binding, VkDescriptorType type,
	VkImageView view, VkSampler sampler, VkImageLayout layout);
void gs_vk_dsu_add_buffer(struct gs_vk_ds_update* u, VkDescriptorSet set, u32 binding, VkDescriptorType type,
	VkBuffer buffer, u32 offset, u32 size);
/* The writes made, and the update emptied for the next. */
void gs_vk_dsu_update(struct gs_vk_ds_update* u, VkDevice device);

#define GS_VK_FB_MAX_ATTACHMENTS 2
struct gs_vk_fb_builder
{
	VkFramebufferCreateInfo ci;
	VkImageView attachments[GS_VK_FB_MAX_ATTACHMENTS];
};
void gs_vk_fb_init(struct gs_vk_fb_builder* b, VkRenderPass render_pass, u32 width, u32 height);
void gs_vk_fb_add_attachment(struct gs_vk_fb_builder* b, VkImageView view);
VkFramebuffer gs_vk_fb_create(struct gs_vk_fb_builder* b, VkDevice device);

#ifdef __cplusplus
}
#endif
