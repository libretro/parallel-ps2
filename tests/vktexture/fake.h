/* A device and Vulkan that hash what the textures ask of them: every
 * command recorded, every object made or destroyed, every buffer read,
 * with handles numbered in the order they are made. Shared by the C
 * textures' suite (main.c) and the C++ GSTextureVK they replaced (the
 * oracle), so the two see the same device. */
#ifndef VKTEXTURE_FAKE_H
#define VKTEXTURE_FAKE_H
#include <stdlib.h>
#include <string.h>

#include "GS/Renderers/Vulkan/VKStreamBuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

static unsigned long long fk_hash = 1469598103934665603ull;
static void H(const void* p, size_t n)
{
	const unsigned char* c = (const unsigned char*)p;
	size_t i;
	for (i = 0; i < n; i++)
	{
		fk_hash ^= c[i];
		fk_hash *= 1099511628211ull;
	}
}
static void HU(unsigned long long v) { H(&v, sizeof(v)); }
#define HN(x) HU((unsigned long long)(size_t)(x))

static unsigned long long fk_next = 0x100;
#define FK_NEW(T) ((T)(size_t)(fk_next++))

/* the device's state */
static unsigned long long fk_fence = 1, fk_completed = 0;
static unsigned long long fk_made = 0, fk_destroyed = 0;
static VkCommandBuffer fk_cmd, fk_init_cmd;
static int fk_stencil = 1;
static u32 fk_pitch_align = 64, fk_offset_align = 16;
static gs_vk_stream_buffer fk_upload;
static unsigned char fk_upload_mem[1u << 20];
static unsigned char fk_staging_mem[4u << 20];
static unsigned char fk_readback_mem[4u << 20];
static VkBuffer fk_staging_buffer, fk_readback_buffer;

static VkFormat fk_format(unsigned format) { return (VkFormat)(37 + format * 3); }

/* the bytes of a buffer the device reads, for the copies */
static const unsigned char* fk_buffer_bytes(VkBuffer b, size_t* size)
{
	if (b == fk_upload.buffer) { *size = sizeof(fk_upload_mem); return fk_upload_mem; }
	if (b == fk_staging_buffer) { *size = sizeof(fk_staging_mem); return fk_staging_mem; }
	*size = 0;
	return NULL;
}

/* Vulkan */
static VkResult VKAPI_CALL fk_create_view(VkDevice d, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a, VkImageView* v)
{
	(void)d; (void)a;
	HU(0x71e3); HN(ci->image); HU(ci->viewType); HU(ci->format);
	HU(ci->components.r); HU(ci->components.g); HU(ci->components.b); HU(ci->components.a);
	HU(ci->subresourceRange.aspectMask); HU(ci->subresourceRange.baseMipLevel); HU(ci->subresourceRange.levelCount);
	HU(ci->subresourceRange.baseArrayLayer); HU(ci->subresourceRange.layerCount);
	*v = FK_NEW(VkImageView);
	return VK_SUCCESS;
}
static void VKAPI_CALL fk_destroy_view(VkDevice d, VkImageView v, const VkAllocationCallbacks* a) { (void)d; (void)a; HU(0xd71e); HN(v); }
static void VKAPI_CALL fk_destroy_image(VkDevice d, VkImage v, const VkAllocationCallbacks* a) { (void)d; (void)a; HU(0xd1a6); HN(v); }
static void VKAPI_CALL fk_destroy_fb(VkDevice d, VkFramebuffer v, const VkAllocationCallbacks* a) { (void)d; (void)a; HU(0xdfb0); HN(v); }
static void VKAPI_CALL fk_destroy_buffer(VkDevice d, VkBuffer v, const VkAllocationCallbacks* a) { (void)d; (void)a; HU(0xdb0f); HN(v); }
static VkResult VKAPI_CALL fk_create_fb(VkDevice d, const VkFramebufferCreateInfo* ci, const VkAllocationCallbacks* a, VkFramebuffer* fb)
{
	uint32_t i;
	(void)d; (void)a;
	HU(0xcfb0); HN(ci->renderPass); HU(ci->attachmentCount);
	for (i = 0; i < ci->attachmentCount; i++)
		HN(ci->pAttachments[i]);
	HU(ci->width); HU(ci->height); HU(ci->layers);
	*fb = FK_NEW(VkFramebuffer);
	return VK_SUCCESS;
}
static void VKAPI_CALL fk_barrier(VkCommandBuffer c, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags f,
	uint32_t nm, const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b, uint32_t ni, const VkImageMemoryBarrier* im)
{
	uint32_t i;
	HU(0xba5); HN(c); HU(src); HU(dst); HU(f); HU(nm); (void)m; HU(nb); HU(ni);
	for (i = 0; i < nb; i++)
	{
		HU(b[i].srcAccessMask); HU(b[i].dstAccessMask); HN(b[i].buffer); HU(b[i].offset); HU(b[i].size);
		HU(b[i].srcQueueFamilyIndex); HU(b[i].dstQueueFamilyIndex);
	}
	for (i = 0; i < ni; i++)
	{
		HU(im[i].srcAccessMask); HU(im[i].dstAccessMask); HU(im[i].oldLayout); HU(im[i].newLayout);
		HU(im[i].srcQueueFamilyIndex); HU(im[i].dstQueueFamilyIndex); HN(im[i].image);
		HU(im[i].subresourceRange.aspectMask); HU(im[i].subresourceRange.baseMipLevel);
		HU(im[i].subresourceRange.levelCount); HU(im[i].subresourceRange.baseArrayLayer);
		HU(im[i].subresourceRange.layerCount); HN(im[i].pNext);
	}
}
static void fk_hash_bic(const VkBufferImageCopy* r)
{
	HU(r->bufferOffset); HU(r->bufferRowLength); HU(r->bufferImageHeight);
	HU(r->imageSubresource.aspectMask); HU(r->imageSubresource.mipLevel);
	HU(r->imageSubresource.baseArrayLayer); HU(r->imageSubresource.layerCount);
	HU((u32)r->imageOffset.x); HU((u32)r->imageOffset.y); HU((u32)r->imageOffset.z);
	HU(r->imageExtent.width); HU(r->imageExtent.height); HU(r->imageExtent.depth);
}
static void VKAPI_CALL fk_copy_b2i(VkCommandBuffer c, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy* r)
{
	size_t size;
	const unsigned char* bytes = fk_buffer_bytes(b, &size);
	HU(0xb21); HN(c); HN(b); HN(i); HU(l); HU(n);
	fk_hash_bic(r);
	/* what the copy reads: rows of the image's width, as packed or as pitched */
	if (bytes && r->bufferOffset < size)
	{
		size_t len = size - (size_t)r->bufferOffset;
		if (len > 8192)
			len = 8192;
		H(bytes + r->bufferOffset, len);
	}
}
static void VKAPI_CALL fk_copy_i2b(VkCommandBuffer c, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy* r)
{
	HU(0x12b); HN(c); HN(i); HU(l); HN(b); HU(n);
	fk_hash_bic(r);
	/* (the "GPU" writes a pattern the reads can check) */
	if (b == fk_readback_buffer && r->bufferOffset < sizeof(fk_readback_mem))
	{
		size_t k;
		for (k = (size_t)r->bufferOffset; k < sizeof(fk_readback_mem) && k < (size_t)r->bufferOffset + 65536; k++)
			fk_readback_mem[k] = (unsigned char)(k * 7 + fk_next);
	}
}
static void VKAPI_CALL fk_blit(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
	const VkImageBlit* r, VkFilter f)
{
	int k;
	HU(0xb17); HN(c); HN(s); HU(sl); HN(d); HU(dl); HU(n); HU(f);
	HU(r->srcSubresource.aspectMask); HU(r->srcSubresource.mipLevel); HU(r->srcSubresource.layerCount);
	HU(r->dstSubresource.aspectMask); HU(r->dstSubresource.mipLevel); HU(r->dstSubresource.layerCount);
	for (k = 0; k < 2; k++)
	{
		HU((u32)r->srcOffsets[k].x); HU((u32)r->srcOffsets[k].y); HU((u32)r->srcOffsets[k].z);
		HU((u32)r->dstOffsets[k].x); HU((u32)r->dstOffsets[k].y); HU((u32)r->dstOffsets[k].z);
	}
}
static void VKAPI_CALL fk_clear_color(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearColorValue* v, uint32_t n,
	const VkImageSubresourceRange* r)
{
	HU(0xcc0); HN(c); HN(i); HU(l); H(v->float32, 16); HU(n);
	HU(r->aspectMask); HU(r->baseMipLevel); HU(r->levelCount); HU(r->baseArrayLayer); HU(r->layerCount);
}
static void VKAPI_CALL fk_clear_depth(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearDepthStencilValue* v,
	uint32_t n, const VkImageSubresourceRange* r)
{
	HU(0xcd0); HN(c); HN(i); HU(l); H(&v->depth, 4); HU(v->stencil); HU(n);
	HU(r->aspectMask); HU(r->baseMipLevel); HU(r->levelCount); HU(r->baseArrayLayer); HU(r->layerCount);
}

static void fk_install(void)
{
	vkCreateImageView = fk_create_view;
	vkDestroyImageView = fk_destroy_view;
	vkDestroyImage = fk_destroy_image;
	vkDestroyFramebuffer = fk_destroy_fb;
	vkDestroyBuffer = fk_destroy_buffer;
	vkCreateFramebuffer = fk_create_fb;
	vkCmdPipelineBarrier = fk_barrier;
	vkCmdCopyBufferToImage = fk_copy_b2i;
	vkCmdCopyImageToBuffer = fk_copy_i2b;
	vkCmdBlitImage = fk_blit;
	vkCmdClearColorImage = fk_clear_color;
	vkCmdClearDepthStencilImage = fk_clear_depth;
	fk_cmd = FK_NEW(VkCommandBuffer);
	fk_init_cmd = FK_NEW(VkCommandBuffer);
}

/* the stream buffer's device: one host buffer, the fences above */
static gs_vk_heap_t fk_heap;
gs_vk_heap_t* gs_vk_device_heap(void) { return &fk_heap; }
int gs_vk_device_create_host_buffer(VkBufferUsageFlags usage, u32 size, VkBuffer* buffer, gs_vk_alloc_t* alloc, void** mapped)
{
	(void)usage;
	if (size > sizeof(fk_upload_mem))
		return 0;
	memset(alloc, 0, sizeof(*alloc));
	alloc->memory = FK_NEW(VkDeviceMemory);
	*buffer = FK_NEW(VkBuffer);
	*mapped = fk_upload_mem;
	return 1;
}
void gs_vk_device_release_buffer(VkBuffer buffer, const gs_vk_alloc_t* alloc, int defer)
{
	HU(0x4e1); HN(buffer); HN(alloc->memory); HU(defer);
}
u64 gs_vk_device_current_fence(void) { return fk_fence; }
u64 gs_vk_device_completed_fence(void) { return fk_completed; }
void gs_vk_device_wait_fence(u64 counter) { HU(0xa17); HU(counter); fk_completed = counter; }

void gs_vk_heap_free(gs_vk_heap_t* heap, const gs_vk_alloc_t* alloc) { (void)heap; HU(0xf4ee); HN(alloc->memory); }
void gs_vk_heap_flush(gs_vk_heap_t* heap, const gs_vk_alloc_t* alloc) { (void)heap; HU(0xf105); HN(alloc->memory); }
void gs_vk_heap_invalidate(gs_vk_heap_t* heap, const gs_vk_alloc_t* alloc) { (void)heap; HU(0x1a7); HN(alloc->memory); }

/* the rest of the device, as the textures used it */
static int fk_create_image(const VkImageCreateInfo* ici, VkImage* image, gs_vk_alloc_t* alloc)
{
	HU(0x1a6e); HU(ici->imageType); HU(ici->format); HU(ici->extent.width); HU(ici->extent.height);
	HU(ici->extent.depth); HU(ici->mipLevels); HU(ici->arrayLayers); HU(ici->samples); HU(ici->tiling);
	HU(ici->usage); HU(ici->flags);
	if (ici->extent.width > 3000)
		return 0;
	memset(alloc, 0, sizeof(*alloc));
	alloc->memory = FK_NEW(VkDeviceMemory);
	*image = FK_NEW(VkImage);
	fk_made++;
	return 1;
}
static void fk_unbind(void) { HU(0x0b1d); }
static void fk_defer_fb(VkFramebuffer fb) { HU(0xdefb); HN(fb); }
static void fk_defer_view(VkImageView v) { HU(0xdef1); HN(v); }
static void fk_defer_image(VkImage i, const gs_vk_alloc_t* a, unsigned long long bytes)
{
	HU(0xdef6); HN(i); HN(a->memory); HU(bytes);
}
static void fk_end_render_pass(void) { HU(0xe2d); }
static void fk_execute(int wait)
{
	HU(0xe8e); HU(wait);
	fk_fence++;
	if (wait)
		fk_completed = fk_fence - 1;
	fk_cmd = FK_NEW(VkCommandBuffer);
	fk_init_cmd = FK_NEW(VkCommandBuffer);
}
static VkCommandBuffer fk_init_cmdbuf(void) { HU(0x1c3d); return fk_init_cmd; }
static VkBuffer fk_staging(u32 size, void** mapped, gs_vk_alloc_t* alloc)
{
	HU(0x57a6); HU(size);
	if (size > sizeof(fk_staging_mem))
		return VK_NULL_HANDLE;
	memset(alloc, 0, sizeof(*alloc));
	alloc->memory = FK_NEW(VkDeviceMemory);
	fk_staging_buffer = FK_NEW(VkBuffer);
	memset(fk_staging_mem, 0xee, sizeof(fk_staging_mem));
	*mapped = fk_staging_mem;
	return fk_staging_buffer;
}
static VkRenderPass fk_pass(VkFormat color, VkFormat depth, int feedback)
{
	HU(0x9a55); HU(color); HU(depth); HU(feedback != 0);
	if (color == fk_format(5) || depth == fk_format(5))
		return VK_NULL_HANDLE; /* (a format with no pass: the failure path) */
	return (VkRenderPass)(size_t)(0x5000 + color * 16 + depth * 4 + (feedback != 0));
}
static int fk_readback(u32 size, VkBuffer* buffer, gs_vk_alloc_t* alloc, void** mapped)
{
	HU(0x2eadb); HU(size);
	if (size > sizeof(fk_readback_mem))
		return 0;
	memset(alloc, 0, sizeof(*alloc));
	alloc->memory = FK_NEW(VkDeviceMemory);
	fk_readback_buffer = FK_NEW(VkBuffer);
	*buffer = fk_readback_buffer;
	*mapped = fk_readback_mem;
	return 1;
}

#ifdef __cplusplus
}
#endif

/* Picks one statement at a time: argument order is the compiler's. */
static unsigned fk_rng = 0x9e3779b9u;
static unsigned R(unsigned n)
{
	fk_rng ^= fk_rng << 13;
	fk_rng ^= fk_rng >> 17;
	fk_rng ^= fk_rng << 5;
	return fk_rng % n;
}
#endif
