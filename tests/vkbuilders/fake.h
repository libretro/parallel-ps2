/* A device that hashes what it is asked to create, following every
 * pointer and pNext chain: two builders that fill the same create infos
 * give the same hash wherever their arrays live. Built as C here and as
 * C++ against the C++ builders the C ones replaced, for the oracle. */
#include <string.h>

static unsigned long long vkb_hash = 1469598103934665603ull;
static unsigned long long vkb_handle = 0x1000;

static void H(const void* p, size_t n)
{
	const unsigned char* c = (const unsigned char*)p;
	size_t i;
	for (i = 0; i < n; i++)
	{
		vkb_hash ^= c[i];
		vkb_hash *= 1099511628211ull;
	}
}
static void HU(unsigned long long v) { H(&v, sizeof(v)); }
static void HF(float f) { H(&f, sizeof(f)); }
static void HP(const void* p) { HU(p != NULL); }
static void HS(const char* s) { HP(s); if (s) H(s, strlen(s) + 1); }

static void hash_chain(const void* next)
{
	const VkBaseInStructure* s = (const VkBaseInStructure*)next;
	for (; s; s = s->pNext)
	{
		HU(s->sType);
		switch (s->sType)
		{
			case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT:
			{
				const VkPipelineRasterizationLineStateCreateInfoEXT* l = (const VkPipelineRasterizationLineStateCreateInfoEXT*)s;
				HU(l->lineRasterizationMode); HU(l->stippledLineEnable); HU(l->lineStippleFactor); HU(l->lineStipplePattern);
				break;
			}
			case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT:
				HU(((const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT*)s)->provokingVertexMode);
				break;
			case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT:
				HU(((const VkGraphicsPipelineLibraryCreateInfoEXT*)s)->flags);
				break;
			case VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR:
			{
				const VkPipelineLibraryCreateInfoKHR* l = (const VkPipelineLibraryCreateInfoKHR*)s;
				uint32_t i;
				HU(l->libraryCount);
				for (i = 0; i < l->libraryCount; i++)
					HU((unsigned long long)(size_t)l->pLibraries[i]);
				break;
			}
			default:
				HU(0xbadull);
				break;
		}
	}
}

static void hash_spec(const VkSpecializationInfo* si)
{
	uint32_t i;
	HP(si);
	if (!si)
		return;
	HU(si->mapEntryCount);
	for (i = 0; i < si->mapEntryCount; i++)
	{
		HU(si->pMapEntries[i].constantID); HU(si->pMapEntries[i].offset); HU(si->pMapEntries[i].size);
	}
	HU(si->dataSize);
	H(si->pData, si->dataSize);
}

#define HN(x) HU((unsigned long long)(size_t)(x))

static VkResult VKAPI_CALL fake_create_graphics_pipelines(VkDevice d, VkPipelineCache cache, uint32_t n,
	const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks* a, VkPipeline* out)
{
	uint32_t i;
	(void)a;
	HU(0x6770); HN(d); HN(cache); HU(n);
	HU(ci->sType); hash_chain(ci->pNext); HU(ci->flags); HU(ci->stageCount);
	HP(ci->pStages);
	for (i = 0; ci->pStages && i < ci->stageCount; i++)
	{
		const VkPipelineShaderStageCreateInfo* s = &ci->pStages[i];
		HU(s->sType); HP(s->pNext); HU(s->flags); HU(s->stage); HN(s->module); HS(s->pName);
		hash_spec(s->pSpecializationInfo);
	}
	HP(ci->pVertexInputState);
	if (ci->pVertexInputState)
	{
		const VkPipelineVertexInputStateCreateInfo* v = ci->pVertexInputState;
		HU(v->sType); HP(v->pNext); HU(v->vertexBindingDescriptionCount); HU(v->vertexAttributeDescriptionCount);
		for (i = 0; i < v->vertexBindingDescriptionCount; i++)
		{
			HU(v->pVertexBindingDescriptions[i].binding); HU(v->pVertexBindingDescriptions[i].stride);
			HU(v->pVertexBindingDescriptions[i].inputRate);
		}
		for (i = 0; i < v->vertexAttributeDescriptionCount; i++)
		{
			HU(v->pVertexAttributeDescriptions[i].location); HU(v->pVertexAttributeDescriptions[i].binding);
			HU(v->pVertexAttributeDescriptions[i].format); HU(v->pVertexAttributeDescriptions[i].offset);
		}
	}
	HP(ci->pInputAssemblyState);
	if (ci->pInputAssemblyState)
	{
		HU(ci->pInputAssemblyState->sType); HU(ci->pInputAssemblyState->topology);
		HU(ci->pInputAssemblyState->primitiveRestartEnable);
	}
	HP(ci->pTessellationState);
	HP(ci->pViewportState);
	if (ci->pViewportState)
	{
		const VkPipelineViewportStateCreateInfo* v = ci->pViewportState;
		HU(v->sType); HU(v->viewportCount); HU(v->scissorCount);
		for (i = 0; i < v->viewportCount; i++)
		{
			HF(v->pViewports[i].x); HF(v->pViewports[i].y); HF(v->pViewports[i].width);
			HF(v->pViewports[i].height); HF(v->pViewports[i].minDepth); HF(v->pViewports[i].maxDepth);
		}
		for (i = 0; i < v->scissorCount; i++)
		{
			HU(v->pScissors[i].offset.x); HU(v->pScissors[i].offset.y);
			HU(v->pScissors[i].extent.width); HU(v->pScissors[i].extent.height);
		}
	}
	HP(ci->pRasterizationState);
	if (ci->pRasterizationState)
	{
		const VkPipelineRasterizationStateCreateInfo* r = ci->pRasterizationState;
		HU(r->sType); hash_chain(r->pNext); HU(r->depthClampEnable); HU(r->rasterizerDiscardEnable);
		HU(r->polygonMode); HU(r->cullMode); HU(r->frontFace); HU(r->depthBiasEnable);
		HF(r->depthBiasConstantFactor); HF(r->depthBiasClamp); HF(r->depthBiasSlopeFactor); HF(r->lineWidth);
	}
	HP(ci->pMultisampleState);
	if (ci->pMultisampleState)
	{
		const VkPipelineMultisampleStateCreateInfo* m = ci->pMultisampleState;
		HU(m->sType); HU(m->rasterizationSamples); HU(m->sampleShadingEnable); HF(m->minSampleShading);
		HP(m->pSampleMask); HU(m->alphaToCoverageEnable); HU(m->alphaToOneEnable);
	}
	HP(ci->pDepthStencilState);
	if (ci->pDepthStencilState)
	{
		const VkPipelineDepthStencilStateCreateInfo* z = ci->pDepthStencilState;
		HU(z->sType); HU(z->flags); HU(z->depthTestEnable); HU(z->depthWriteEnable); HU(z->depthCompareOp);
		HU(z->depthBoundsTestEnable); HU(z->stencilTestEnable);
		H(&z->front, sizeof(z->front)); H(&z->back, sizeof(z->back));
		HF(z->minDepthBounds); HF(z->maxDepthBounds);
	}
	HP(ci->pColorBlendState);
	if (ci->pColorBlendState)
	{
		const VkPipelineColorBlendStateCreateInfo* b = ci->pColorBlendState;
		HU(b->sType); HU(b->flags); HU(b->logicOpEnable); HU(b->logicOp); HU(b->attachmentCount);
		for (i = 0; i < b->attachmentCount; i++)
			H(&b->pAttachments[i], sizeof(b->pAttachments[i]));
		for (i = 0; i < 4; i++)
			HF(b->blendConstants[i]);
	}
	HP(ci->pDynamicState);
	if (ci->pDynamicState)
	{
		HU(ci->pDynamicState->dynamicStateCount);
		for (i = 0; i < ci->pDynamicState->dynamicStateCount; i++)
			HU(ci->pDynamicState->pDynamicStates[i]);
	}
	HN(ci->layout); HN(ci->renderPass); HU(ci->subpass); HN(ci->basePipelineHandle); HU(ci->basePipelineIndex);
	*out = (VkPipeline)(size_t)(vkb_handle++);
	return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_create_dsl(VkDevice d, const VkDescriptorSetLayoutCreateInfo* ci,
	const VkAllocationCallbacks* a, VkDescriptorSetLayout* out)
{
	uint32_t i;
	(void)a;
	HU(0xd51); HN(d); HU(ci->sType); HP(ci->pNext); HU(ci->flags); HU(ci->bindingCount);
	for (i = 0; i < ci->bindingCount; i++)
	{
		HU(ci->pBindings[i].binding); HU(ci->pBindings[i].descriptorType); HU(ci->pBindings[i].descriptorCount);
		HU(ci->pBindings[i].stageFlags); HP(ci->pBindings[i].pImmutableSamplers);
	}
	*out = (VkDescriptorSetLayout)(size_t)(vkb_handle++);
	return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_create_pl(VkDevice d, const VkPipelineLayoutCreateInfo* ci,
	const VkAllocationCallbacks* a, VkPipelineLayout* out)
{
	uint32_t i;
	(void)a;
	HU(0x91); HN(d); HU(ci->sType); HP(ci->pNext); HU(ci->flags); HU(ci->setLayoutCount);
	for (i = 0; i < ci->setLayoutCount; i++)
		HN(ci->pSetLayouts[i]);
	HU(ci->pushConstantRangeCount);
	for (i = 0; i < ci->pushConstantRangeCount; i++)
	{
		HU(ci->pPushConstantRanges[i].stageFlags); HU(ci->pPushConstantRanges[i].offset);
		HU(ci->pPushConstantRanges[i].size);
	}
	*out = (VkPipelineLayout)(size_t)(vkb_handle++);
	return VK_SUCCESS;
}

static VkResult VKAPI_CALL fake_create_fb(VkDevice d, const VkFramebufferCreateInfo* ci,
	const VkAllocationCallbacks* a, VkFramebuffer* out)
{
	uint32_t i;
	(void)a;
	HU(0xfb); HN(d); HU(ci->sType); HP(ci->pNext); HU(ci->flags); HN(ci->renderPass); HU(ci->attachmentCount);
	for (i = 0; i < ci->attachmentCount; i++)
		HN(ci->pAttachments[i]);
	HU(ci->width); HU(ci->height); HU(ci->layers);
	*out = (VkFramebuffer)(size_t)(vkb_handle++);
	return VK_SUCCESS;
}

static void VKAPI_CALL fake_update_ds(VkDevice d, uint32_t n, const VkWriteDescriptorSet* w, uint32_t nc,
	const VkCopyDescriptorSet* c)
{
	uint32_t i, j;
	HU(0x5e7); HN(d); HU(n); HP(w); HU(nc); HP(c);
	for (i = 0; i < n; i++)
	{
		HU(w[i].sType); HP(w[i].pNext); HN(w[i].dstSet); HU(w[i].dstBinding); HU(w[i].dstArrayElement);
		HU(w[i].descriptorCount); HU(w[i].descriptorType);
		HP(w[i].pImageInfo); HP(w[i].pBufferInfo); HP(w[i].pTexelBufferView);
		for (j = 0; w[i].pImageInfo && j < w[i].descriptorCount; j++)
		{
			HN(w[i].pImageInfo[j].sampler); HN(w[i].pImageInfo[j].imageView); HU(w[i].pImageInfo[j].imageLayout);
		}
		for (j = 0; w[i].pBufferInfo && j < w[i].descriptorCount; j++)
		{
			HN(w[i].pBufferInfo[j].buffer); HU(w[i].pBufferInfo[j].offset); HU(w[i].pBufferInfo[j].range);
		}
	}
}

static void fake_install(void)
{
	vkCreateGraphicsPipelines   = fake_create_graphics_pipelines;
	vkCreateDescriptorSetLayout = fake_create_dsl;
	vkCreatePipelineLayout      = fake_create_pl;
	vkCreateFramebuffer         = fake_create_fb;
	vkUpdateDescriptorSets      = fake_update_ds;
}

/* Picks made one statement at a time: argument order is the compiler's. */
static unsigned vkb_rng = 12345u;
static unsigned R(unsigned n)
{
	vkb_rng = vkb_rng * 1103515245u + 12345u;
	return (vkb_rng >> 8) % n;
}
#define HDL(T, v) ((T)(size_t)(0x100000u + (v)))
