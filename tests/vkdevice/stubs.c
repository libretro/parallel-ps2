/* The Vulkan the device sees (stubs_vk.c's stand-ins, written from
 * vk.xml): what each entry point answers, the memory behind mapped
 * allocations, the data a draw reads logged, and the loader, the frontend's
 * interface and threads (run in step as coroutines) the core's own would
 * be. */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

#include <boolean.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>

#include "stubs.h"

stub_config g_cfg;

void gs_vk_reset_function_pointers(void);

/* ==== the log ==== */

u64 g_hash  = 1469598103934665603ull;
u64 g_words = 0;
static FILE* s_dump;
static int s_dump_open;

static void dump_open(void)
{
	if (!s_dump_open)
	{
		const char* path = getenv("VKDEV_DUMP");
		s_dump_open      = 1;
		if (path)
			s_dump = fopen(path, "w");
	}
}

void log_word(u64 v)
{
	int i;
	for (i = 0; i < 8; i++)
	{
		g_hash ^= (u8)(v >> (i * 8));
		g_hash *= 1099511628211ull;
	}
	g_words++;
	dump_open();
	if (s_dump)
		fprintf(s_dump, "%llx\n", (unsigned long long)v);
}

void log_float(float f)
{
	u32 u;
	memcpy(&u, &f, sizeof(u));
	log_word(u);
}

void log_bytes(const void* p, size_t n)
{
	const u8* b = (const u8*)p;
	u64 h       = 1469598103934665603ull;
	size_t i;
	for (i = 0; i < n; i++)
	{
		h ^= b[i];
		h *= 1099511628211ull;
	}
	log_word(n);
	log_word(h);
}

void log_string(const char* s)
{
	log_bytes(s, strlen(s));
	dump_open();
	if (s_dump)
		fprintf(s_dump, "  \"%s\"\n", s);
}

void log_name(const char* s)
{
	dump_open();
	if (s_dump)
		fprintf(s_dump, "%s\n", s);
	log_bytes(s, strlen(s));
}

void log_close(void)
{
	if (s_dump)
		fclose(s_dump);
	s_dump = NULL;
}

/* ==== handles ==== */

#define MAX_HANDLES (1u << 20)
#define HANDLE_BASE 0x1000u

typedef struct handle_info
{
	const char* kind;
	u64 parent;
	int live;
	/* a buffer's or an image's */
	u64 size;
	u64 mem;
	u64 offset;
	/* a memory's */
	u8* host;
	u32 type;
	/* a render pass's attachments: 1 colour, 2 depth */
	u8 att[2];
	u32 att_count;
	/* a descriptor set's dynamic uniform buffers, by binding */
	u64 ubo_buffer[4];
	u64 ubo_offset[4];
	u64 ubo_range[4];
} handle_info;

static handle_info* s_handles;
static u32 s_next = HANDLE_BASE;
int g_dead_calls;

static handle_info* info(u64 h)
{
	if (h < HANDLE_BASE || h >= s_next)
		return NULL;
	return &s_handles[h - HANDLE_BASE];
}

u64 new_handle(const char* kind, u64 parent)
{
	handle_info* i;
	if (!s_handles)
		s_handles = (handle_info*)calloc(MAX_HANDLES, sizeof(*s_handles));
	if (s_next - HANDLE_BASE >= MAX_HANDLES)
	{
		printf("vkdevice: FAIL: too many handles\n");
		exit(1);
	}
	i         = &s_handles[s_next - HANDLE_BASE];
	i->kind   = kind;
	i->parent = parent;
	i->live   = 1;
	return s_next++;
}

void log_handle(u64 h)
{
	handle_info* i = info(h);
	log_word(h);
	if (h && i && !i->live)
	{
		if (getenv("VKDEV_TRACE"))
			fprintf(stderr, "dead handle %llx (%s)\n", (unsigned long long)h, i->kind);
		g_dead_calls++;
	}
}

void kill_handle(u64 h)
{
	handle_info* i = info(h);
	if (!i)
		return;
	if (i->host)
	{
		free(i->host);
		i->host = NULL;
	}
	i->live = 0;
}

static void kill_children(u64 parent, const char* kind)
{
	u32 n;
	for (n = 0; n < s_next - HANDLE_BASE; n++)
		if (s_handles[n].live && s_handles[n].parent == parent && !strcmp(s_handles[n].kind, kind))
			s_handles[n].live = 0;
}

unsigned live_handles(void)
{
	u32 n;
	unsigned live = 0;
	for (n = 0; n < s_next - HANDLE_BASE; n++)
	{
		if (!s_handles[n].live)
			continue;
		/* (the harness's, and what goes with the device) */
		if (!strcmp(s_handles[n].kind, "VkQueue") || !strcmp(s_handles[n].kind, "harness"))
			continue;
		if (getenv("VKDEV_TRACE"))
			fprintf(stderr, "live %x %s\n", n + HANDLE_BASE, s_handles[n].kind);
		live++;
	}
	return live;
}

#define H(x) ((u64)(uintptr_t)(x))

/* ==== the physical device ==== */

static const char* const s_exts_always[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME,
	VK_EXT_MEMORY_BUDGET_EXTENSION_NAME};

static u32 device_extensions(VkExtensionProperties* out)
{
	const char* names[16];
	u32 n = 0, i;
	for (i = 0; i < sizeof(s_exts_always) / sizeof(s_exts_always[0]); i++)
		names[n++] = s_exts_always[i];
	if (g_cfg.provoking)
		names[n++] = VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME;
	if (g_cfg.line_raster)
		names[n++] = VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME;
	if (g_cfg.raster_order)
		names[n++] = VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME;
	if (g_cfg.draw_params)
		names[n++] = VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME;
	if (g_cfg.eds)
		names[n++] = VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME;
	if (g_cfg.gpl)
	{
		names[n++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
		names[n++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
	}
	if (out)
		for (i = 0; i < n; i++)
		{
			memset(&out[i], 0, sizeof(out[i]));
			strcpy(out[i].extensionName, names[i]);
			out[i].specVersion = 1;
		}
	return n;
}

VkResult hook_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName,
	uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
{
	(void)physicalDevice;
	(void)pLayerName;
	*pPropertyCount = device_extensions(pProperties);
	return VK_SUCCESS;
}

void hook_vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures* pFeatures)
{
	(void)physicalDevice;
	memset(pFeatures, 0, sizeof(*pFeatures));
	pFeatures->dualSrcBlend             = g_cfg.dual_src;
	pFeatures->largePoints              = VK_TRUE;
	pFeatures->wideLines                = VK_TRUE;
	pFeatures->fragmentStoresAndAtomics = VK_TRUE;
	pFeatures->textureCompressionBC     = g_cfg.bc;
	pFeatures->samplerAnisotropy        = VK_TRUE;
	pFeatures->geometryShader           = VK_TRUE;
}

void hook_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format,
	VkFormatProperties* pFormatProperties)
{
	(void)physicalDevice;
	(void)format;
	memset(pFormatProperties, 0, sizeof(*pFormatProperties));
	pFormatProperties->optimalTilingFeatures = 0x7FFFFFFFu;
	pFormatProperties->linearTilingFeatures  = 0x7FFFFFFFu;
	pFormatProperties->bufferFeatures        = 0x7FFFFFFFu;
}

void hook_vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* p)
{
	(void)physicalDevice;
	memset(p, 0, sizeof(*p));
	p->apiVersion                                = g_cfg.api_version;
	p->vendorID                                  = g_cfg.vendor;
	p->deviceID                                  = 0x1234;
	p->deviceType                                = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
	strcpy(p->deviceName, "vkdevice");
	p->limits.maxImageDimension2D                = 16384;
	p->limits.minUniformBufferOffsetAlignment    = 256;
	p->limits.minTexelBufferOffsetAlignment      = 16;
	p->limits.optimalBufferCopyOffsetAlignment   = 4;
	p->limits.optimalBufferCopyRowPitchAlignment = 16;
	p->limits.bufferImageGranularity             = 1024;
	p->limits.nonCoherentAtomSize                = 64;
	p->limits.pointSizeRange[0]                  = 1.0f;
	p->limits.pointSizeRange[1]                  = 64.0f;
	p->limits.lineWidthRange[0]                  = 1.0f;
	p->limits.lineWidthRange[1]                  = 8.0f;
	p->limits.maxPushConstantsSize               = 128;
}

void hook_vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t* pCount,
	VkQueueFamilyProperties* pProps)
{
	(void)physicalDevice;
	if (pProps)
	{
		u32 i;
		for (i = 0; i < *pCount && i < 2; i++)
		{
			memset(&pProps[i], 0, sizeof(pProps[i]));
			pProps[i].queueFlags = i == 0 ? VK_QUEUE_TRANSFER_BIT : (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
			pProps[i].queueCount = 1;
		}
	}
	*pCount = 2;
}

void hook_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* p)
{
	(void)physicalDevice;
	memset(p, 0, sizeof(*p));
	p->memoryTypeCount              = 3;
	p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	p->memoryTypes[0].heapIndex     = 0;
	p->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	p->memoryTypes[1].heapIndex     = 1;
	p->memoryTypes[2].propertyFlags =
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	p->memoryTypes[2].heapIndex = 1;
	p->memoryHeapCount          = 2;
	p->memoryHeaps[0].size      = 8ull << 30;
	p->memoryHeaps[0].flags     = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
	p->memoryHeaps[1].size      = 16ull << 30;
}

void hook_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures)
{
	VkBaseOutStructure* s;
	hook_vkGetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);
	for (s = (VkBaseOutStructure*)pFeatures->pNext; s; s = s->pNext)
	{
		switch ((int)s->sType)
		{
			case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:
				((VkPhysicalDeviceExtendedDynamicStateFeaturesEXT*)s)->extendedDynamicState = g_cfg.eds;
				break;
			case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT:
				((VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT*)s)->graphicsPipelineLibrary = g_cfg.gpl;
				break;
			case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT:
				((VkPhysicalDeviceProvokingVertexFeaturesEXT*)s)->provokingVertexLast = g_cfg.provoking;
				break;
			case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT:
				((VkPhysicalDeviceLineRasterizationFeaturesEXT*)s)->bresenhamLines = g_cfg.line_raster;
				break;
			case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT:
				((VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT*)s)
					->rasterizationOrderColorAttachmentAccess = g_cfg.raster_order;
				break;
			default:
				break;
		}
	}
}

void hook_vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties)
{
	VkBaseOutStructure* s;
	hook_vkGetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);
	for (s = (VkBaseOutStructure*)pProperties->pNext; s; s = s->pNext)
	{
		switch ((int)s->sType)
		{
			case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT:
				((VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT*)s)->graphicsPipelineLibraryFastLinking =
					g_cfg.gpl;
				break;
			case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR:
				((VkPhysicalDevicePushDescriptorPropertiesKHR*)s)->maxPushDescriptors = 32;
				break;
			case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES:
				((VkPhysicalDeviceDriverProperties*)s)->driverID = VK_DRIVER_ID_MESA_LLVMPIPE;
				break;
			default:
				break;
		}
	}
}

/* ==== memory, buffers, images ==== */

VkResult hook_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo,
	const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory)
{
	u64 h = new_handle("VkDeviceMemory", H(device));
	handle_info* i = info(h);
	(void)pAllocator;
	i->size = pAllocateInfo->allocationSize;
	i->type = pAllocateInfo->memoryTypeIndex;
	/* (host memory behind the mappable types; untouched pages are free) */
	if (i->type != 0)
	{
		i->host = (u8*)calloc(1, (size_t)i->size);
		if (!i->host)
		{
			printf("vkdevice: FAIL: no host memory for %llu bytes\n", (unsigned long long)i->size);
			exit(1);
		}
	}
	*pMemory = (VkDeviceMemory)h;
	return VK_SUCCESS;
}

void hook_vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator)
{
	(void)device;
	(void)pAllocator;
	kill_handle(H(memory));
}

VkResult hook_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
	VkMemoryMapFlags flags, void** ppData)
{
	handle_info* i = info(H(memory));
	(void)device;
	(void)size;
	(void)flags;
	if (!i || !i->host)
	{
		printf("vkdevice: FAIL: device-local memory mapped\n");
		exit(1);
	}
	*ppData = i->host + offset;
	return VK_SUCCESS;
}

VkResult hook_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo,
	const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer)
{
	u64 h = new_handle("VkBuffer", H(device));
	(void)pAllocator;
	info(h)->size = pCreateInfo->size;
	*pBuffer      = (VkBuffer)h;
	return VK_SUCCESS;
}

VkResult hook_vkCreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo,
	const VkAllocationCallbacks* pAllocator, VkImage* pImage)
{
	u64 h = new_handle("VkImage", H(device));
	(void)pAllocator;
	info(h)->size = (u64)pCreateInfo->extent.width * pCreateInfo->extent.height * 8u * pCreateInfo->mipLevels;
	*pImage       = (VkImage)h;
	return VK_SUCCESS;
}

void hook_vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements* pMemoryRequirements)
{
	(void)device;
	pMemoryRequirements->size           = (info(H(buffer))->size + 255u) & ~255ull;
	pMemoryRequirements->alignment      = 256;
	pMemoryRequirements->memoryTypeBits = 7;
}

void hook_vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements* pMemoryRequirements)
{
	(void)device;
	pMemoryRequirements->size           = (info(H(image))->size + 4095u) & ~4095ull;
	pMemoryRequirements->alignment      = 4096;
	pMemoryRequirements->memoryTypeBits = 7;
}

VkResult hook_vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset)
{
	handle_info* i = info(H(buffer));
	(void)device;
	i->mem    = H(memory);
	i->offset = memoryOffset;
	return VK_SUCCESS;
}

/* The bytes of a buffer from offset, or NULL where no host memory backs
 * them */
static const u8* buffer_bytes(u64 buffer, u64 offset, u64 size)
{
	handle_info* b = info(buffer);
	handle_info* m;
	if (!b || !b->mem)
		return NULL;
	m = info(b->mem);
	if (!m || !m->host || b->offset + offset + size > m->size)
		return NULL;
	return m->host + b->offset + offset;
}

/* ==== render passes, descriptors, command buffers ==== */

VkResult hook_vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo,
	const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass)
{
	u64 h = new_handle("VkRenderPass", H(device));
	handle_info* i = info(h);
	u32 a;
	(void)pAllocator;
	i->att_count = pCreateInfo->attachmentCount;
	for (a = 0; a < pCreateInfo->attachmentCount && a < 2; a++)
	{
		const VkFormat f = pCreateInfo->pAttachments[a].format;
		i->att[a] = (f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT) ? 2 : 1;
	}
	*pRenderPass = (VkRenderPass)h;
	return VK_SUCCESS;
}

/* A clear value as what it clears is */
static void log_clear_value(const VkClearValue* cv, int depth)
{
	if (depth)
	{
		log_float(cv->depthStencil.depth);
		log_word(cv->depthStencil.stencil);
	}
	else
		log_bytes(cv->color.float32, sizeof(cv->color.float32));
}

void hook_vkCmdBeginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
	VkSubpassContents contents)
{
	handle_info* rp = info(H(pRenderPassBegin->renderPass));
	u32 a;
	(void)commandBuffer;
	(void)contents;
	for (a = 0; rp && a < pRenderPassBegin->clearValueCount && a < rp->att_count; a++)
		log_clear_value(&pRenderPassBegin->pClearValues[a], rp->att[a] == 2);
}

void hook_vkCmdClearAttachments(VkCommandBuffer commandBuffer, uint32_t attachmentCount,
	const VkClearAttachment* pAttachments, uint32_t rectCount, const VkClearRect* pRects)
{
	u32 a;
	(void)commandBuffer;
	(void)rectCount;
	(void)pRects;
	for (a = 0; a < attachmentCount; a++)
		log_clear_value(&pAttachments[a].clearValue, (pAttachments[a].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) == 0);
}

/* (by the type: the other pointers are not read, and need not be set) */
void ser_override_VkWriteDescriptorSet(const VkWriteDescriptorSet* p)
{
	u32 i;
	log_word(p->sType);
	log_handle(H(p->dstSet));
	log_word(p->dstBinding);
	log_word(p->dstArrayElement);
	log_word(p->descriptorCount);
	log_word(p->descriptorType);
	for (i = 0; i < p->descriptorCount; i++)
	{
		switch (p->descriptorType)
		{
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
				log_handle(H(p->pBufferInfo[i].buffer));
				log_word(p->pBufferInfo[i].offset);
				log_word(p->pBufferInfo[i].range);
				break;
			default:
				log_handle(H(p->pImageInfo[i].sampler));
				log_handle(H(p->pImageInfo[i].imageView));
				log_word(p->pImageInfo[i].imageLayout);
				break;
		}
	}
}

void hook_vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount,
	const VkWriteDescriptorSet* pDescriptorWrites, uint32_t descriptorCopyCount,
	const VkCopyDescriptorSet* pDescriptorCopies)
{
	u32 w;
	(void)device;
	(void)descriptorCopyCount;
	(void)pDescriptorCopies;
	for (w = 0; w < descriptorWriteCount; w++)
	{
		const VkWriteDescriptorSet* d = &pDescriptorWrites[w];
		handle_info* s                = info(H(d->dstSet));
		if (s && d->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && d->dstBinding < 4)
		{
			s->ubo_buffer[d->dstBinding] = H(d->pBufferInfo[0].buffer);
			s->ubo_offset[d->dstBinding] = d->pBufferInfo[0].offset;
			s->ubo_range[d->dstBinding]  = d->pBufferInfo[0].range;
		}
	}
}

/* The constant buffers a draw reads: at the dynamic offsets bound */
void hook_vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
	VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets,
	uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets)
{
	u32 s, b, used = 0;
	(void)commandBuffer;
	(void)pipelineBindPoint;
	(void)layout;
	(void)firstSet;
	for (s = 0; s < descriptorSetCount; s++)
	{
		handle_info* set = info(H(pDescriptorSets[s]));
		for (b = 0; set && b < 4; b++)
		{
			const u8* bytes;
			if (!set->ubo_buffer[b] || used >= dynamicOffsetCount)
				continue;
			bytes = buffer_bytes(set->ubo_buffer[b], set->ubo_offset[b] + pDynamicOffsets[used], set->ubo_range[b]);
			if (bytes)
				log_bytes(bytes, (size_t)set->ubo_range[b]);
			used++;
		}
	}
}

VkResult hook_vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* pAllocateInfo,
	VkDescriptorSet* pDescriptorSets)
{
	u32 i;
	(void)device;
	for (i = 0; i < pAllocateInfo->descriptorSetCount; i++)
		pDescriptorSets[i] = (VkDescriptorSet)new_handle("VkDescriptorSet", H(pAllocateInfo->descriptorPool));
	return VK_SUCCESS;
}

VkResult hook_vkFreeDescriptorSets(VkDevice device, VkDescriptorPool descriptorPool, uint32_t descriptorSetCount,
	const VkDescriptorSet* pDescriptorSets)
{
	u32 i;
	(void)device;
	(void)descriptorPool;
	for (i = 0; i < descriptorSetCount; i++)
		kill_handle(H(pDescriptorSets[i]));
	return VK_SUCCESS;
}

VkResult hook_vkResetDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool, VkDescriptorPoolResetFlags flags)
{
	(void)device;
	(void)flags;
	kill_children(H(descriptorPool), "VkDescriptorSet");
	return VK_SUCCESS;
}

void hook_vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool,
	const VkAllocationCallbacks* pAllocator)
{
	(void)device;
	(void)pAllocator;
	kill_children(H(descriptorPool), "VkDescriptorSet");
}

VkResult hook_vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo,
	VkCommandBuffer* pCommandBuffers)
{
	u32 i;
	(void)device;
	for (i = 0; i < pAllocateInfo->commandBufferCount; i++)
		pCommandBuffers[i] = (VkCommandBuffer)(uintptr_t)new_handle("VkCommandBuffer", H(pAllocateInfo->commandPool));
	return VK_SUCCESS;
}

void hook_vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount,
	const VkCommandBuffer* pCommandBuffers)
{
	u32 i;
	(void)device;
	(void)commandPool;
	for (i = 0; i < commandBufferCount; i++)
		kill_handle(H(pCommandBuffers[i]));
}

void hook_vkDestroyCommandPool(VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks* pAllocator)
{
	(void)device;
	(void)pAllocator;
	kill_children(H(commandPool), "VkCommandBuffer");
}

/* ==== the data a draw reads ==== */

static u64 s_vb, s_vb_offset, s_ib, s_ib_offset;

void hook_vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount,
	const VkBuffer* pBuffers, const VkDeviceSize* pOffsets)
{
	(void)commandBuffer;
	if (firstBinding == 0 && bindingCount)
	{
		s_vb        = H(pBuffers[0]);
		s_vb_offset = pOffsets[0];
	}
}

void hook_vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
	VkIndexType indexType)
{
	(void)commandBuffer;
	(void)indexType;
	s_ib        = H(buffer);
	s_ib_offset = offset;
}

#define VERTEX_SIZE 32u

void hook_vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
	uint32_t firstInstance)
{
	const u8* v = buffer_bytes(s_vb, s_vb_offset + (u64)firstVertex * VERTEX_SIZE, (u64)vertexCount * VERTEX_SIZE);
	(void)commandBuffer;
	(void)instanceCount;
	(void)firstInstance;
	if (v)
		log_bytes(v, (size_t)vertexCount * VERTEX_SIZE);
}

void hook_vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount,
	uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
	const u8* ib = buffer_bytes(s_ib, s_ib_offset + (u64)firstIndex * 2u, (u64)indexCount * 2u);
	(void)commandBuffer;
	(void)instanceCount;
	(void)firstInstance;
	if (ib)
	{
		u16 lo = 0xFFFF, hi = 0;
		u32 i;
		const u8* v;
		for (i = 0; i < indexCount; i++)
		{
			u16 x;
			memcpy(&x, ib + i * 2u, 2);
			lo = x < lo ? x : lo;
			hi = x > hi ? x : hi;
		}
		log_bytes(ib, (size_t)indexCount * 2u);
		if (indexCount)
		{
			v = buffer_bytes(s_vb, s_vb_offset + ((u64)vertexOffset + lo) * VERTEX_SIZE, (u64)(hi - lo + 1) * VERTEX_SIZE);
			if (v)
				log_bytes(v, (size_t)(hi - lo + 1) * VERTEX_SIZE);
		}
	}
}

/* What an upload copies: the rows of each region, as four bytes a texel */
void hook_vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage,
	VkImageLayout dstImageLayout, uint32_t regionCount, const VkBufferImageCopy* pRegions)
{
	u32 r;
	(void)commandBuffer;
	(void)dstImage;
	(void)dstImageLayout;
	for (r = 0; r < regionCount; r++)
	{
		const VkBufferImageCopy* c = &pRegions[r];
		const u64 row = (u64)(c->bufferRowLength ? c->bufferRowLength : c->imageExtent.width) * 4u;
		const u64 n   = row * c->imageExtent.height;
		const u8* b   = buffer_bytes(H(srcBuffer), c->bufferOffset, n);
		if (b)
			log_bytes(b, (size_t)n);
	}
}

VkResult hook_vkGetPipelineCacheData(VkDevice device, VkPipelineCache pipelineCache, size_t* pDataSize, void* pData)
{
	(void)device;
	(void)pipelineCache;
	(void)pData;
	*pDataSize = 0;
	return VK_SUCCESS;
}

/* ==== the loader ==== */

#define VULKAN_MODULE_ENTRY_POINT(name, required) PFN_##name pcsx2_##name;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) PFN_##name pcsx2_##name;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) PFN_##name pcsx2_##name;
#include "GS/Renderers/Vulkan/VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT

static PFN_vkVoidFunction find(const char* name)
{
	const stub_entry* e;
	char ext[96];
	for (e = g_stub_entries; e->name; e++)
		if (!strcmp(e->name, name))
			return e->fn;
	/* (Vulkan 1.3's names of the EXT commands) */
	snprintf(ext, sizeof(ext), "%sEXT", name);
	for (e = g_stub_entries; e->name; e++)
		if (!strcmp(e->name, ext))
			return e->fn;
	return NULL;
}

static PFN_vkVoidFunction VKAPI_CALL stub_get_device_proc_addr(VkDevice device, const char* name)
{
	(void)device;
	if (!strcmp(name, "vkGetDeviceProcAddr"))
		return (PFN_vkVoidFunction)stub_get_device_proc_addr;
	/* (Vulkan 1.3's: only with the instance at 1.3) */
	if (g_cfg.api_version < VK_API_VERSION_1_3 && !strstr(name, "EXT") && strstr(name, "vkCmdSet")
		&& (strstr(name, "Depth") || strstr(name, "Stencil")) && !strcmp(name + strlen(name) - 6, "Enable"))
		return NULL;
	return find(name);
}

PFN_vkVoidFunction VKAPI_CALL stub_get_instance_proc_addr(VkInstance instance, const char* name)
{
	(void)instance;
	if (!strcmp(name, "vkGetInstanceProcAddr"))
		return (PFN_vkVoidFunction)stub_get_instance_proc_addr;
	if (!strcmp(name, "vkGetDeviceProcAddr"))
		return (PFN_vkVoidFunction)stub_get_device_proc_addr;
	return find(name);
}

static int s_library_refs;

int vk_load_library(void)
{
	pcsx2_vkGetInstanceProcAddr = stub_get_instance_proc_addr;
	s_library_refs++;
	return 1;
}

void vk_unload_library(void)
{
	if (s_library_refs && --s_library_refs == 0)
		gs_vk_reset_function_pointers();
}

#define LOAD(name) pcsx2_##name = (PFN_##name)find(#name);

int gs_vk_load_instance_functions(VkInstance instance)
{
	(void)instance;
#define VULKAN_MODULE_ENTRY_POINT(name, required)
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) LOAD(name)
#define VULKAN_DEVICE_ENTRY_POINT(name, required)
#include "GS/Renderers/Vulkan/VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
	pcsx2_vkGetDeviceProcAddr = stub_get_device_proc_addr;
	return 1;
}

int gs_vk_load_device_functions(VkDevice device)
{
	(void)device;
#define VULKAN_MODULE_ENTRY_POINT(name, required)
#define VULKAN_INSTANCE_ENTRY_POINT(name, required)
#define VULKAN_DEVICE_ENTRY_POINT(name, required) LOAD(name)
#include "GS/Renderers/Vulkan/VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
	/* (the extended dynamic state's: only with the extension) */
	if (!g_cfg.eds)
	{
		pcsx2_vkCmdSetDepthTestEnableEXT   = NULL;
		pcsx2_vkCmdSetDepthWriteEnableEXT  = NULL;
		pcsx2_vkCmdSetDepthCompareOpEXT    = NULL;
		pcsx2_vkCmdSetStencilTestEnableEXT = NULL;
		pcsx2_vkCmdSetStencilOpEXT         = NULL;
	}
	return 1;
}

void gs_vk_reset_function_pointers(void)
{
#define VULKAN_MODULE_ENTRY_POINT(name, required) pcsx2_##name = NULL;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) pcsx2_##name = NULL;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) pcsx2_##name = NULL;
#include "GS/Renderers/Vulkan/VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
}

/* ==== threads, as coroutines in step ====
 *
 * A thread runs when the drawing thread waits for it or tells it there is
 * work, until it waits itself: then the drawing thread goes on where it
 * was. What it makes is ready at the same point every run. */

struct sthread
{
	ucontext_t ctx;
	void* stack;
	void (*fn)(void*);
	void* data;
	int finished;
};

static ucontext_t s_main_ctx;
static struct sthread* s_current; /* the thread running, or NULL: the drawing thread */
static struct sthread* s_thread;  /* (one at a time) */

static void thread_entry(void)
{
	struct sthread* t = s_current;
	t->fn(t->data);
	t->finished = 1;
	s_current   = NULL;
	swapcontext(&t->ctx, &s_main_ctx);
}

static void run_thread(struct sthread* t)
{
	if (!t || t->finished || s_current)
		return;
	s_current = t;
	swapcontext(&s_main_ctx, &t->ctx);
}

sthread_t* sthread_create(void (*thread_func)(void*), void* userdata)
{
	struct sthread* t = (struct sthread*)calloc(1, sizeof(*t));
	t->fn    = thread_func;
	t->data  = userdata;
	t->stack = malloc(1u << 20);
	getcontext(&t->ctx);
	t->ctx.uc_stack.ss_sp   = t->stack;
	t->ctx.uc_stack.ss_size = 1u << 20;
	t->ctx.uc_link          = NULL;
	makecontext(&t->ctx, thread_entry, 0);
	s_thread = t;
	return (sthread_t*)t;
}

void sthread_join(sthread_t* thread)
{
	struct sthread* t = (struct sthread*)thread;
	while (!t->finished)
		run_thread(t);
	free(t->stack);
	if (s_thread == t)
		s_thread = NULL;
	free(t);
}

bool retro_eventcount_init(retro_eventcount_t* ec)
{
	memset(ec, 0, sizeof(*ec));
	return true;
}

void retro_eventcount_free(retro_eventcount_t* ec)
{
	(void)ec;
}

void retro_eventcount_notify(retro_eventcount_t* ec)
{
	(void)ec;
	if (!s_current)
		run_thread(s_thread);
}

int retro_eventcount_prepare_wait(retro_eventcount_t* ec)
{
	(void)ec;
	return 0;
}

void retro_eventcount_cancel_wait(retro_eventcount_t* ec)
{
	(void)ec;
}

void retro_eventcount_commit_wait(retro_eventcount_t* ec, int key)
{
	(void)ec;
	(void)key;
	if (s_current)
	{
		/* the thread waits: back to the drawing thread */
		struct sthread* t = s_current;
		s_current         = NULL;
		swapcontext(&t->ctx, &s_main_ctx);
	}
	else
		run_thread(s_thread);
}
