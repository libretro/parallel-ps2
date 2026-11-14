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

#include <string.h>

#include "VKBuilders.h"

void gs_vk_add_to_chain(void* head, const void* ptr)
{
	VkBaseOutStructure* last = (VkBaseOutStructure*)head;
	while (last->pNext)
	{
		if ((const void*)last->pNext == ptr)
			return;
		last = last->pNext;
	}
	last->pNext = (VkBaseOutStructure*)ptr;
}

void gs_vk_dsl_init(struct gs_vk_dsl_builder* b)
{
	memset(&b->ci, 0, sizeof(b->ci));
	b->ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
}

void gs_vk_dsl_add_binding(struct gs_vk_dsl_builder* b, u32 binding, VkDescriptorType type, u32 count,
	VkShaderStageFlags stages)
{
	VkDescriptorSetLayoutBinding* d = &b->bindings[b->ci.bindingCount++];
	d->binding            = binding;
	d->descriptorType     = type;
	d->descriptorCount    = count;
	d->stageFlags         = stages;
	d->pImmutableSamplers = NULL;
	b->ci.pBindings       = b->bindings;
}

VkDescriptorSetLayout gs_vk_dsl_create(struct gs_vk_dsl_builder* b, VkDevice device)
{
	VkDescriptorSetLayout layout;
	if (vkCreateDescriptorSetLayout(device, &b->ci, NULL, &layout) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	gs_vk_dsl_init(b);
	return layout;
}

void gs_vk_pl_init(struct gs_vk_pl_builder* b)
{
	memset(&b->ci, 0, sizeof(b->ci));
	b->ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
}

void gs_vk_pl_add_set(struct gs_vk_pl_builder* b, VkDescriptorSetLayout layout)
{
	b->sets[b->ci.setLayoutCount++] = layout;
	b->ci.pSetLayouts               = b->sets;
}

void gs_vk_pl_add_push_constants(struct gs_vk_pl_builder* b, VkShaderStageFlags stages, u32 offset, u32 size)
{
	VkPushConstantRange* r = &b->push_constants[b->ci.pushConstantRangeCount++];
	r->stageFlags            = stages;
	r->offset                = offset;
	r->size                  = size;
	b->ci.pPushConstantRanges = b->push_constants;
}

VkPipelineLayout gs_vk_pl_create(struct gs_vk_pl_builder* b, VkDevice device)
{
	VkPipelineLayout layout;
	if (vkCreatePipelineLayout(device, &b->ci, NULL, &layout) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	gs_vk_pl_init(b);
	return layout;
}

#define GS_VK_RGBA (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)

void gs_vk_gp_init(struct gs_vk_gp_builder* b)
{
	/* Every array and state zeroed in one go: the counts are what say
	 * how much of each is used. */
	memset(b, 0, sizeof(*b));
	b->ci.sType                  = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	b->vertex_input.sType        = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	b->input_assembly.sType      = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	b->rasterization.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	b->depth.sType               = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	b->blend.sType               = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	b->viewport_state.sType      = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	b->dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	b->multisample.sType         = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	b->provoking_vertex.sType    = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT;
	b->line_rasterization.sType  = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT;
	b->library.sType             = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT;
	b->linked.sType              = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;

	b->ci.pVertexInputState      = &b->vertex_input;

	b->rasterization.lineWidth   = 1.0f;
	gs_vk_gp_set_rasterization(b, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	gs_vk_gp_set_depth(b, 0, 0, VK_COMPARE_OP_ALWAYS);
	gs_vk_gp_set_blend_attachment(b, 0, 0, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
		VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, GS_VK_RGBA);
	gs_vk_gp_set_topology(b, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

	/* (needed even when dynamic) */
	b->viewport.width            = 1.0f;
	b->viewport.height           = 1.0f;
	b->viewport.maxDepth         = 1.0f;
	b->viewport_state.viewportCount = 1;
	b->viewport_state.pViewports = &b->viewport;
	b->scissor.extent.width      = 1;
	b->scissor.extent.height     = 1;
	b->viewport_state.scissorCount = 1;
	b->viewport_state.pScissors  = &b->scissor;
	b->ci.pViewportState         = &b->viewport_state;

	b->multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	b->ci.pMultisampleState      = &b->multisample;
}

void gs_vk_gp_set_layout(struct gs_vk_gp_builder* b, VkPipelineLayout layout)
{
	b->ci.layout = layout;
}

void gs_vk_gp_set_render_pass(struct gs_vk_gp_builder* b, VkRenderPass render_pass, u32 subpass)
{
	b->ci.renderPass = render_pass;
	b->ci.subpass    = subpass;
}

void gs_vk_gp_set_shader(struct gs_vk_gp_builder* b, VkShaderStageFlagBits stage, VkShaderModule module,
	const VkSpecializationInfo* spec)
{
	VkPipelineShaderStageCreateInfo* s;
	u32 i;
	for (i = 0; i < b->ci.stageCount; i++)
		if (b->shader_stages[i].stage == stage)
			break;
	if (i == b->ci.stageCount)
	{
		b->ci.stageCount++;
		b->ci.pStages = b->shader_stages;
	}
	s = &b->shader_stages[i];
	s->sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	s->stage               = stage;
	s->module              = module;
	s->pName               = "main";
	s->pSpecializationInfo = spec;
}

void gs_vk_gp_add_vertex_buffer(struct gs_vk_gp_builder* b, u32 binding, u32 stride)
{
	VkVertexInputBindingDescription* d = &b->vertex_buffers[b->vertex_input.vertexBindingDescriptionCount++];
	d->binding    = binding;
	d->stride     = stride;
	d->inputRate  = VK_VERTEX_INPUT_RATE_VERTEX;
	b->vertex_input.pVertexBindingDescriptions = b->vertex_buffers;
}

void gs_vk_gp_add_vertex_attribute(struct gs_vk_gp_builder* b, u32 location, u32 binding, VkFormat format, u32 offset)
{
	VkVertexInputAttributeDescription* a = &b->vertex_attributes[b->vertex_input.vertexAttributeDescriptionCount++];
	a->location = location;
	a->binding  = binding;
	a->format   = format;
	a->offset   = offset;
	b->vertex_input.pVertexAttributeDescriptions = b->vertex_attributes;
}

void gs_vk_gp_set_topology(struct gs_vk_gp_builder* b, VkPrimitiveTopology topology)
{
	b->input_assembly.topology               = topology;
	b->input_assembly.primitiveRestartEnable = VK_FALSE;
	b->ci.pInputAssemblyState                = &b->input_assembly;
}

void gs_vk_gp_set_rasterization(struct gs_vk_gp_builder* b, VkPolygonMode polygon_mode, VkCullModeFlags cull_mode,
	VkFrontFace front_face)
{
	b->rasterization.polygonMode = polygon_mode;
	b->rasterization.cullMode    = cull_mode;
	b->rasterization.frontFace   = front_face;
	b->ci.pRasterizationState    = &b->rasterization;
}

void gs_vk_gp_set_line_rasterization(struct gs_vk_gp_builder* b, VkLineRasterizationModeEXT mode)
{
	gs_vk_add_to_chain(&b->rasterization, &b->line_rasterization);
	b->line_rasterization.lineRasterizationMode = mode;
}

void gs_vk_gp_set_provoking_vertex(struct gs_vk_gp_builder* b, VkProvokingVertexModeEXT mode)
{
	gs_vk_add_to_chain(&b->rasterization, &b->provoking_vertex);
	b->provoking_vertex.provokingVertexMode = mode;
}

void gs_vk_gp_set_depth(struct gs_vk_gp_builder* b, int test, int write, VkCompareOp op)
{
	b->depth.depthTestEnable  = test ? VK_TRUE : VK_FALSE;
	b->depth.depthWriteEnable = write ? VK_TRUE : VK_FALSE;
	b->depth.depthCompareOp   = op;
	b->ci.pDepthStencilState  = &b->depth;
}

void gs_vk_gp_set_stencil(struct gs_vk_gp_builder* b, int test, const VkStencilOpState* front,
	const VkStencilOpState* back)
{
	b->depth.stencilTestEnable = test ? VK_TRUE : VK_FALSE;
	b->depth.front             = *front;
	b->depth.back              = *back;
}

void gs_vk_gp_set_blend_attachment(struct gs_vk_gp_builder* b, u32 attachment, int enable, VkBlendFactor src,
	VkBlendFactor dst, VkBlendOp op, VkBlendFactor alpha_src, VkBlendFactor alpha_dst, VkBlendOp alpha_op,
	VkColorComponentFlags write_mask)
{
	VkPipelineColorBlendAttachmentState* s = &b->blend_attachments[attachment];
	s->blendEnable         = enable ? VK_TRUE : VK_FALSE;
	s->srcColorBlendFactor = src;
	s->dstColorBlendFactor = dst;
	s->colorBlendOp        = op;
	s->srcAlphaBlendFactor = alpha_src;
	s->dstAlphaBlendFactor = alpha_dst;
	s->alphaBlendOp        = alpha_op;
	s->colorWriteMask      = write_mask;
	if (attachment >= b->blend.attachmentCount)
	{
		b->blend.attachmentCount = attachment + 1u;
		b->blend.pAttachments    = b->blend_attachments;
		b->ci.pColorBlendState   = &b->blend;
	}
}

void gs_vk_gp_set_write_mask(struct gs_vk_gp_builder* b, u32 attachment, VkColorComponentFlags write_mask)
{
	b->blend_attachments[attachment].colorWriteMask = write_mask;
}

void gs_vk_gp_add_blend_flags(struct gs_vk_gp_builder* b, VkPipelineColorBlendStateCreateFlags flags)
{
	b->blend.flags |= flags;
}

void gs_vk_gp_add_dynamic_state(struct gs_vk_gp_builder* b, VkDynamicState state)
{
	b->dynamic_values[b->dynamic.dynamicStateCount++] = state;
	b->dynamic.pDynamicStates = b->dynamic_values;
	b->ci.pDynamicState       = &b->dynamic;
}

void gs_vk_gp_set_library(struct gs_vk_gp_builder* b, VkGraphicsPipelineLibraryFlagsEXT parts)
{
	u32 i, n = 0;
	b->library.flags = parts;
	b->ci.flags     |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
	gs_vk_add_to_chain(&b->ci, &b->library);
	for (i = 0; i < b->ci.stageCount; i++)
	{
		const VkGraphicsPipelineLibraryFlagsEXT part = (b->shader_stages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) ?
			VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT :
			VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
		if (parts & part)
			b->shader_stages[n++] = b->shader_stages[i];
	}
	b->ci.stageCount = n;
	b->ci.pStages    = n ? b->shader_stages : NULL;
}

void gs_vk_gp_set_linked_libraries(struct gs_vk_gp_builder* b, const VkPipeline* libraries, u32 count)
{
	u32 i;
	for (i = 0; i < count && i < GS_VK_GP_MAX_LIBRARIES; i++)
		b->linked_libraries[i] = libraries[i];
	b->linked.libraryCount = i;
	b->linked.pLibraries   = b->linked_libraries;
	gs_vk_add_to_chain(&b->ci, &b->linked);
	b->ci.stageCount = 0;
	b->ci.pStages    = NULL;
}

VkPipeline gs_vk_gp_create(struct gs_vk_gp_builder* b, VkDevice device, VkPipelineCache cache)
{
	VkPipeline pipeline;
	if (vkCreateGraphicsPipelines(device, cache, 1, &b->ci, NULL, &pipeline) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	gs_vk_gp_init(b);
	return pipeline;
}

void gs_vk_dsu_init(struct gs_vk_ds_update* u)
{
	/* (each write is filled whole when added: nothing to zero) */
	u->num_writes       = 0;
	u->num_image_infos  = 0;
	u->num_buffer_infos = 0;
}

static VkWriteDescriptorSet* gs_vk_dsu_write(struct gs_vk_ds_update* u, VkDescriptorSet set, u32 binding,
	VkDescriptorType type)
{
	VkWriteDescriptorSet* w = &u->writes[u->num_writes++];
	w->sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	w->pNext            = NULL;
	w->dstSet           = set;
	w->dstBinding       = binding;
	w->dstArrayElement  = 0;
	w->descriptorCount  = 1;
	w->descriptorType   = type;
	w->pImageInfo       = NULL;
	w->pBufferInfo      = NULL;
	w->pTexelBufferView = NULL;
	return w;
}

void gs_vk_dsu_add_image(struct gs_vk_ds_update* u, VkDescriptorSet set, u32 binding, VkDescriptorType type,
	VkImageView view, VkSampler sampler, VkImageLayout layout)
{
	VkDescriptorImageInfo* ii = &u->image_infos[u->num_image_infos++];
	ii->sampler     = sampler;
	ii->imageView   = view;
	ii->imageLayout = layout;
	gs_vk_dsu_write(u, set, binding, type)->pImageInfo = ii;
}

void gs_vk_dsu_add_buffer(struct gs_vk_ds_update* u, VkDescriptorSet set, u32 binding, VkDescriptorType type,
	VkBuffer buffer, u32 offset, u32 size)
{
	VkDescriptorBufferInfo* bi = &u->buffer_infos[u->num_buffer_infos++];
	bi->buffer = buffer;
	bi->offset = offset;
	bi->range  = size;
	gs_vk_dsu_write(u, set, binding, type)->pBufferInfo = bi;
}

void gs_vk_dsu_update(struct gs_vk_ds_update* u, VkDevice device)
{
	vkUpdateDescriptorSets(device, u->num_writes, u->num_writes ? u->writes : NULL, 0, NULL);
	gs_vk_dsu_init(u);
}

void gs_vk_fb_init(struct gs_vk_fb_builder* b, VkRenderPass render_pass, u32 width, u32 height)
{
	memset(&b->ci, 0, sizeof(b->ci));
	b->ci.sType      = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	b->ci.renderPass = render_pass;
	b->ci.width      = width;
	b->ci.height     = height;
	b->ci.layers     = 1;
}

void gs_vk_fb_add_attachment(struct gs_vk_fb_builder* b, VkImageView view)
{
	b->attachments[b->ci.attachmentCount++] = view;
	b->ci.pAttachments = b->attachments;
}

VkFramebuffer gs_vk_fb_create(struct gs_vk_fb_builder* b, VkDevice device)
{
	VkFramebuffer fb;
	if (vkCreateFramebuffer(device, &b->ci, NULL, &fb) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return fb;
}
