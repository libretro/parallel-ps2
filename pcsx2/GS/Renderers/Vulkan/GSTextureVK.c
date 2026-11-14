/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#include <stdlib.h>
#include <string.h>

#include <retro_inline.h>

#include "GSTextureVK.h"
#include "VKBuilders.h"
#include "VKStreamBuffer.h"

/* gs_vk_layout's Vulkan layouts */
static const VkImageLayout s_vk_layouts[GS_VK_LAYOUT_COUNT] = {
	VK_IMAGE_LAYOUT_UNDEFINED,                        /* Undefined */
	VK_IMAGE_LAYOUT_PREINITIALIZED,                   /* Preinitialized */
	VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,         /* ColorAttachment */
	VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, /* DepthStencilAttachment */
	VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,         /* ShaderReadOnly */
	VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,             /* ClearDst */
	VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,             /* TransferSrc */
	VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,             /* TransferDst */
	VK_IMAGE_LAYOUT_GENERAL,                          /* TransferSelf */
	VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,                  /* PresentSrc */
	VK_IMAGE_LAYOUT_GENERAL,                          /* FeedbackLoop */
	VK_IMAGE_LAYOUT_GENERAL,                          /* ReadWriteImage */
	VK_IMAGE_LAYOUT_GENERAL,                          /* ComputeReadWriteImage */
	VK_IMAGE_LAYOUT_GENERAL                           /* General */
};

VkImageLayout gs_texture_vk_image_layout(u32 layout)
{
	return s_vk_layouts[layout];
}

static void gs_texture_vk_free(gs_texture* base)
{
	gs_texture_vk* t = (gs_texture_vk*)base;
	gs_texture_vk_destroy(t, 1);
	gs_array_free(&t->framebuffers);
	free(t);
}

static int gs_texture_vk_update(gs_texture* base, const gs_texture_rect* r, const void* data, int pitch, int layer);
static int gs_texture_vk_map(gs_texture* base, gs_texture_map* m, const gs_texture_rect* r, int layer);
static void gs_texture_vk_unmap(gs_texture* base);
static void gs_texture_vk_generate_mipmap(gs_texture* base);

static const gs_texture_ops s_vk_texture_ops = {
	gs_texture_vk_free, gs_texture_vk_update, gs_texture_vk_map, gs_texture_vk_unmap,
	gs_texture_vk_generate_mipmap};

gs_texture_vk* gs_texture_vk_create(unsigned type, unsigned format, int width, int height, int levels)
{
	const VkFormat vk_format = gs_vk_device_native_format(format);
	VkImageCreateInfo ici;
	VkImageViewCreateInfo vci;
	VkImage image = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	gs_vk_alloc_t alloc;
	gs_texture_vk* t;

	memset(&ici, 0, sizeof(ici));
	ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType     = VK_IMAGE_TYPE_2D;
	ici.format        = vk_format;
	ici.extent.width  = (u32)width;
	ici.extent.height = (u32)height;
	ici.extent.depth  = 1;
	ici.mipLevels     = (u32)levels;
	ici.arrayLayers   = 1;
	ici.samples       = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling        = VK_IMAGE_TILING_OPTIMAL;

	memset(&vci, 0, sizeof(vci));
	vci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	vci.format                          = vk_format;
	vci.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	vci.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	vci.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	vci.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	vci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	vci.subresourceRange.levelCount     = (u32)levels;
	vci.subresourceRange.layerCount     = 1;

	switch (type)
	{
		case GS_TEXTYPE_TEXTURE:
			ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			/* An R8 texture reads R in every channel: the shaders take it from alpha. */
			if (format == GS_TEX_UNORM8)
			{
				vci.components.r = VK_COMPONENT_SWIZZLE_R;
				vci.components.g = VK_COMPONENT_SWIZZLE_R;
				vci.components.b = VK_COMPONENT_SWIZZLE_R;
				vci.components.a = VK_COMPONENT_SWIZZLE_R;
			}
			break;
		case GS_TEXTYPE_RENDER_TARGET:
			ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
				| VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
			break;
		case GS_TEXTYPE_DEPTH_STENCIL:
			ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
				| VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			break;
		case GS_TEXTYPE_RW_TEXTURE:
			ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT
				| VK_IMAGE_USAGE_SAMPLED_BIT;
			break;
		default:
			return NULL;
	}

	/* Out of the heap's blocks, taken when the device was made: an offset,
	 * not a driver call, unless every block is full. */
	memset(&alloc, 0, sizeof(alloc));
	if (!gs_vk_device_create_image(&ici, &image, &alloc))
	{
		/* The counts matter as much as the bytes: made against destroyed
		 * says whether live images nobody freed, or memory the heap failed
		 * to take back from freed ones, fill it. */
		const gs_vk_heap_t* heap = gs_vk_device_heap();
		u64 made, destroyed;
		gs_vk_device_image_counts(&made, &destroyed);
		log_cb(RETRO_LOG_ERROR, "GS: no room for a %ux%u texture (heap %llu MB reserved, %llu MB used: %llu MB uploads, %llu MB images; %llu images made, %llu destroyed, %llu live).\n",
			(unsigned)width, (unsigned)height,
			(unsigned long long)(heap->bytes_reserved >> 20), (unsigned long long)(heap->bytes_used >> 20),
			(unsigned long long)(heap->bytes_host >> 20), (unsigned long long)(heap->bytes_device >> 20),
			(unsigned long long)made, (unsigned long long)destroyed, (unsigned long long)(made - destroyed));
		return NULL;
	}

	vci.image = image;
	if (vkCreateImageView(gs_vk_device_handle(), &vci, NULL, &view) != VK_SUCCESS)
	{
		log_cb(RETRO_LOG_ERROR, "vkCreateImageView failed\n");
		vkDestroyImage(gs_vk_device_handle(), image, NULL);
		gs_vk_heap_free(gs_vk_device_heap(), &alloc);
		return NULL;
	}

	t = (gs_texture_vk*)calloc(1, sizeof(*t));
	if (!t)
	{
		vkDestroyImageView(gs_vk_device_handle(), view, NULL);
		vkDestroyImage(gs_vk_device_handle(), image, NULL);
		gs_vk_heap_free(gs_vk_device_heap(), &alloc);
		return NULL;
	}
	gs_texture_base_init(&t->base, type, format, width, height);
	t->base.m_ops           = &s_vk_texture_ops;
	t->base.m_mipmap_levels = levels;
	t->image                = image;
	t->alloc                = alloc;
	t->view                 = view;
	t->vk_format            = vk_format;
	t->layout               = GS_VK_LAYOUT_UNDEFINED;
	t->map_level            = 0x7fffffff;
	gs_array_init(&t->framebuffers, sizeof(gs_vk_linked_fb));
	return t;
}

void gs_texture_vk_destroy(gs_texture_vk* t, int defer)
{
	const VkDevice dev = gs_vk_device_handle();
	size_t n, m;

	gs_vk_device_unbind_texture(t);

	if (t->base.m_type == GS_TEXTYPE_RENDER_TARGET || t->base.m_type == GS_TEXTYPE_DEPTH_STENCIL)
	{
		for (n = 0; n < t->framebuffers.count; n++)
		{
			const gs_vk_linked_fb* lfb = &GS_ARRAY_AT(&t->framebuffers, gs_vk_linked_fb, n);
			if (lfb->other)
			{
				gs_array_t* others = &lfb->other->framebuffers;
				for (m = 0; m < others->count; m++)
				{
					if (GS_ARRAY_AT(others, gs_vk_linked_fb, m).other == t)
					{
						gs_array_erase(others, m);
						break;
					}
				}
			}
			if (defer)
				gs_vk_device_defer_framebuffer(lfb->fb);
			else
				vkDestroyFramebuffer(dev, lfb->fb, NULL);
		}
		t->framebuffers.count = 0;
	}

	if (t->view != VK_NULL_HANDLE)
	{
		if (defer)
			gs_vk_device_defer_image_view(t->view);
		else
			vkDestroyImageView(dev, t->view, NULL);
		t->view = VK_NULL_HANDLE;
	}

	/* (without memory of ours the image is not ours to destroy) */
	if (t->alloc.memory != VK_NULL_HANDLE)
	{
		if (defer)
			gs_vk_device_defer_image(t->image, &t->alloc,
				(u64)t->base.m_size.x * (u64)t->base.m_size.y * (t->base.m_format == GS_TEX_UNORM8 ? 1u : 4u));
		else
		{
			vkDestroyImage(dev, t->image, NULL);
			gs_vk_heap_free(gs_vk_device_heap(), &t->alloc);
			gs_vk_device_count_image_destroyed();
		}
		t->image = VK_NULL_HANDLE;
		memset(&t->alloc, 0, sizeof(t->alloc));
	}
}

/* In the init command buffer, before the frame's, when nothing this frame
 * has used the texture; otherwise in the frame's, out of the render pass. */
static VkCommandBuffer gs_texture_vk_update_cmdbuf(gs_texture_vk* t)
{
	if (t->base.m_type != GS_TEXTYPE_TEXTURE || t->use_fence_counter == gs_vk_device_current_fence())
	{
		gs_vk_device_end_render_pass();
		return gs_vk_device_command_buffer();
	}
	return gs_vk_device_init_command_buffer();
}

static void gs_texture_vk_copy_for_upload(const gs_texture_vk* t, void* dst, const void* src, u32 pitch,
	u32 upload_pitch, u32 height)
{
	const u32 block = gs_texture_block_size(t->base.m_format);
	gs_stride_memcpy(dst, upload_pitch, src, pitch, upload_pitch < pitch ? upload_pitch : pitch,
		(height + block - 1) / block);
}

static u32 gs_vk_align_up(u32 v, u32 align)
{
	return (v + align - 1) & ~(align - 1);
}

static void gs_texture_vk_from_buffer(gs_texture_vk* t, VkCommandBuffer cmdbuf, int level, u32 x, u32 y,
	u32 width, u32 height, u32 buffer_height, u32 row_length, VkBuffer buffer, u32 buffer_offset)
{
	const u32 old_layout = t->layout;
	VkBufferImageCopy bic;

	if (old_layout == GS_VK_LAYOUT_UNDEFINED)
		gs_texture_vk_transition(t, cmdbuf, GS_VK_LAYOUT_TRANSFER_DST);
	else if (old_layout != GS_VK_LAYOUT_TRANSFER_DST)
		gs_texture_vk_transition_levels(t, cmdbuf, level, 1, old_layout, GS_VK_LAYOUT_TRANSFER_DST);

	bic.bufferOffset                    = buffer_offset;
	bic.bufferRowLength                 = row_length;
	bic.bufferImageHeight               = buffer_height;
	bic.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	bic.imageSubresource.mipLevel       = (u32)level;
	bic.imageSubresource.baseArrayLayer = 0;
	bic.imageSubresource.layerCount     = 1;
	bic.imageOffset.x                   = (s32)x;
	bic.imageOffset.y                   = (s32)y;
	bic.imageOffset.z                   = 0;
	bic.imageExtent.width               = width;
	bic.imageExtent.height              = height;
	bic.imageExtent.depth               = 1;
	vkCmdCopyBufferToImage(cmdbuf, buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);

	if (old_layout != GS_VK_LAYOUT_TRANSFER_DST && old_layout != GS_VK_LAYOUT_UNDEFINED)
		gs_texture_vk_transition_levels(t, cmdbuf, level, 1, GS_VK_LAYOUT_TRANSFER_DST, old_layout);
}

/* The upload recorded: the area of @level from @buffer, after any
 * pending clear of a target the upload does not cover whole. */
static void gs_texture_vk_upload(gs_texture_vk* t, const gs_texture_rect* r, int level, u32 upload_pitch,
	VkBuffer buffer, u32 buffer_offset)
{
	const VkCommandBuffer cmdbuf = gs_texture_vk_update_cmdbuf(t);
	const u32 width              = (u32)(r->right - r->left);
	const u32 height             = (u32)(r->bottom - r->top);

	/* (never left undefined once used) */
	if (t->layout == GS_VK_LAYOUT_UNDEFINED)
		gs_texture_vk_transition(t, cmdbuf, GS_VK_LAYOUT_TRANSFER_DST);

	if (t->base.m_type == GS_TEXTYPE_RENDER_TARGET)
	{
		if (r->left != 0 || r->top != 0 || r->right != t->base.m_size.x || r->bottom != t->base.m_size.y)
			gs_texture_vk_commit_clear(t, cmdbuf);
		else
			t->base.m_state = GS_TEXSTATE_DIRTY;
	}

	gs_texture_vk_from_buffer(t, cmdbuf, level, (u32)r->left, (u32)r->top, width, height,
		gs_vk_align_up(height, gs_texture_block_size(t->base.m_format)),
		gs_texture_row_length(t->base.m_format, upload_pitch), buffer, buffer_offset);
	gs_texture_vk_transition(t, cmdbuf, GS_VK_LAYOUT_SHADER_READ_ONLY);

	if (t->base.m_type == GS_TEXTYPE_TEXTURE)
		t->base.m_needs_mipmaps_generated |= (level == 0);
}

/* @size bytes of the upload buffer at its current offset, the command
 * buffer submitted once to make room if it has to be */
static int gs_vk_reserve_upload(struct gs_vk_stream_buffer* sb, u32 size)
{
	if (gs_vk_stream_buffer_reserve(sb, size, gs_vk_device_copy_offset_alignment()))
		return 1;
	gs_vk_device_execute(0);
	if (gs_vk_stream_buffer_reserve(sb, size, gs_vk_device_copy_offset_alignment()))
		return 1;
	log_cb(RETRO_LOG_ERROR, "Failed to reserve texture upload memory (%u bytes).\n", size);
	return 0;
}

static int gs_texture_vk_update(gs_texture* base, const gs_texture_rect* r, const void* data, int pitch, int layer)
{
	gs_texture_vk* t                 = (gs_texture_vk*)base;
	struct gs_vk_stream_buffer* sb   = gs_vk_device_upload_buffer();
	const u32 height                 = (u32)(r->bottom - r->top);
	u32 upload_pitch, required_size, buffer_offset;
	VkBuffer buffer;

	if (layer >= base->m_mipmap_levels)
		return 0;

	upload_pitch  = gs_vk_align_up((u32)pitch, gs_vk_device_copy_pitch_alignment());
	required_size = gs_texture_upload_size(base->m_format, height, upload_pitch);

	/* More than half the stream buffer: a buffer of its own, rather than
	 * a failed reservation or a string of submissions. */
	if (required_size > sb->size / 2)
	{
		void* mapped = NULL;
		gs_vk_alloc_t alloc;
		memset(&alloc, 0, sizeof(alloc));
		/* (from the device's free list; back on it when the command buffer
		 * reading it completes) */
		buffer = gs_vk_device_staging_buffer(required_size, &mapped, &alloc);
		if (buffer == VK_NULL_HANDLE)
			return 0;
		gs_texture_vk_copy_for_upload(t, mapped, data, (u32)pitch, upload_pitch, height);
		gs_vk_heap_flush(gs_vk_device_heap(), &alloc);
		buffer_offset = 0;
	}
	else
	{
		if (!gs_vk_reserve_upload(sb, required_size))
			return 0;
		buffer        = sb->buffer;
		buffer_offset = sb->current_offset;
		gs_texture_vk_copy_for_upload(t, sb->host_pointer + sb->current_offset, data, (u32)pitch, upload_pitch, height);
		gs_vk_stream_buffer_commit(sb, required_size);
	}

	gs_texture_vk_upload(t, r, layer, upload_pitch, buffer, buffer_offset);
	return 1;
}

static int gs_texture_vk_map(gs_texture* base, gs_texture_map* m, const gs_texture_rect* r, int layer)
{
	gs_texture_vk* t               = (gs_texture_vk*)base;
	struct gs_vk_stream_buffer* sb = gs_vk_device_upload_buffer();
	u32 required_size;

	if (layer >= base->m_mipmap_levels || gs_texture_is_compressed(base->m_format))
		return 0;

	if (r)
		t->map_area = *r;
	else
	{
		t->map_area.left   = 0;
		t->map_area.top    = 0;
		t->map_area.right  = base->m_size.x;
		t->map_area.bottom = base->m_size.y;
	}
	t->map_level = layer;

	m->pitch = (int)gs_vk_align_up(gs_texture_upload_pitch(base->m_format, (u32)(t->map_area.right - t->map_area.left)),
		gs_vk_device_copy_pitch_alignment());

	/* (as Update: a map bigger than half the buffer is not had) */
	required_size = gs_texture_upload_size(base->m_format, (u32)(t->map_area.bottom - t->map_area.top), (u32)m->pitch);
	if (required_size >= sb->size / 2)
		return 0;
	if (!gs_vk_reserve_upload(sb, required_size))
		return 0;

	m->bits = sb->host_pointer + sb->current_offset;
	return 1;
}

static void gs_texture_vk_unmap(gs_texture* base)
{
	gs_texture_vk* t               = (gs_texture_vk*)base;
	struct gs_vk_stream_buffer* sb = gs_vk_device_upload_buffer();
	const u32 pitch = gs_vk_align_up(gs_texture_upload_pitch(base->m_format, (u32)(t->map_area.right - t->map_area.left)),
		gs_vk_device_copy_pitch_alignment());
	const u32 required_size = gs_texture_upload_size(base->m_format, (u32)(t->map_area.bottom - t->map_area.top), pitch);
	const u32 buffer_offset = sb->current_offset;

	gs_vk_stream_buffer_commit(sb, required_size);
	gs_texture_vk_upload(t, &t->map_area, t->map_level, pitch, sb->buffer, buffer_offset);
}

static void gs_texture_vk_generate_mipmap(gs_texture* base)
{
	gs_texture_vk* t             = (gs_texture_vk*)base;
	const VkCommandBuffer cmdbuf = gs_texture_vk_update_cmdbuf(t);
	int dst_level;

	if (t->layout == GS_VK_LAYOUT_UNDEFINED)
		gs_texture_vk_transition(t, cmdbuf, GS_VK_LAYOUT_TRANSFER_SRC);

	for (dst_level = 1; dst_level < base->m_mipmap_levels; dst_level++)
	{
		const int src_level = dst_level - 1;
		VkImageBlit blit;

		gs_texture_vk_transition_levels(t, cmdbuf, src_level, 1, t->layout, GS_VK_LAYOUT_TRANSFER_SRC);
		gs_texture_vk_transition_levels(t, cmdbuf, dst_level, 1, t->layout, GS_VK_LAYOUT_TRANSFER_DST);

		memset(&blit, 0, sizeof(blit));
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.mipLevel   = (u32)src_level;
		blit.srcSubresource.layerCount = 1;
		blit.srcOffsets[1].x           = base->m_size.x >> src_level > 1 ? base->m_size.x >> src_level : 1;
		blit.srcOffsets[1].y           = base->m_size.y >> src_level > 1 ? base->m_size.y >> src_level : 1;
		blit.srcOffsets[1].z           = 1;
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.mipLevel   = (u32)dst_level;
		blit.dstSubresource.layerCount = 1;
		blit.dstOffsets[1].x           = base->m_size.x >> dst_level > 1 ? base->m_size.x >> dst_level : 1;
		blit.dstOffsets[1].y           = base->m_size.y >> dst_level > 1 ? base->m_size.y >> dst_level : 1;
		blit.dstOffsets[1].z           = 1;
		vkCmdBlitImage(cmdbuf, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

		gs_texture_vk_transition_levels(t, cmdbuf, src_level, 1, GS_VK_LAYOUT_TRANSFER_SRC, t->layout);
		gs_texture_vk_transition_levels(t, cmdbuf, dst_level, 1, GS_VK_LAYOUT_TRANSFER_DST, t->layout);
	}
}

void gs_texture_vk_commit_clear(gs_texture_vk* t, VkCommandBuffer cmdbuf)
{
	VkImageSubresourceRange srr;

	if (cmdbuf == VK_NULL_HANDLE)
	{
		if (t->base.m_state != GS_TEXSTATE_CLEARED)
			return;
		gs_vk_device_end_render_pass();
		cmdbuf = gs_vk_device_command_buffer();
	}

	gs_texture_vk_transition(t, cmdbuf, GS_VK_LAYOUT_CLEAR_DST);

	srr.baseMipLevel   = 0;
	srr.levelCount     = 1;
	srr.baseArrayLayer = 0;
	srr.layerCount     = 1;
	if (t->base.m_type == GS_TEXTYPE_DEPTH_STENCIL)
	{
		VkClearDepthStencilValue cv;
		cv.depth          = t->base.m_clear_value.depth;
		cv.stencil        = 0;
		srr.aspectMask    = VK_IMAGE_ASPECT_DEPTH_BIT;
		vkCmdClearDepthStencilImage(cmdbuf, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &srr);
	}
	else
	{
		/* the RGBA8 clear colour as UNorm (GSVector4::unorm8) */
		const u32 c = t->base.m_clear_value.color;
		VkClearColorValue cv;
		cv.float32[0]  = (float)(c & 0xff) * (1.0f / 255.0f);
		cv.float32[1]  = (float)((c >> 8) & 0xff) * (1.0f / 255.0f);
		cv.float32[2]  = (float)((c >> 16) & 0xff) * (1.0f / 255.0f);
		cv.float32[3]  = (float)(c >> 24) * (1.0f / 255.0f);
		srr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vkCmdClearColorImage(cmdbuf, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &srr);
	}

	t->base.m_state = GS_TEXSTATE_DIRTY;
}

void gs_texture_vk_transition(gs_texture_vk* t, VkCommandBuffer cmdbuf, u32 new_layout)
{
	if (t->layout == new_layout)
		return;
	gs_texture_vk_transition_levels(t, cmdbuf, 0, t->base.m_mipmap_levels, t->layout, new_layout);
	t->layout = new_layout;
}

/* What a layout's last use wrote or read, or (@dst) its next will, and
 * in which stages: the barrier's half for that side. */
static void gs_vk_layout_access(u32 layout, int color, int dst, VkAccessFlags* access, VkPipelineStageFlags* stages)
{
	switch (layout)
	{
		case GS_VK_LAYOUT_UNDEFINED:
			*access = 0;
			*stages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			break;
		case GS_VK_LAYOUT_PREINITIALIZED:
			/* (only ever a source: the host's writes) */
			*access = dst ? 0 : VK_ACCESS_HOST_WRITE_BIT;
			*stages = dst ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_HOST_BIT;
			break;
		case GS_VK_LAYOUT_COLOR_ATTACHMENT:
			*access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			*stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			break;
		case GS_VK_LAYOUT_DEPTH_STENCIL_ATTACHMENT:
			*access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			*stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			break;
		case GS_VK_LAYOUT_SHADER_READ_ONLY:
			*access = VK_ACCESS_SHADER_READ_BIT;
			*stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			break;
		case GS_VK_LAYOUT_CLEAR_DST:
		case GS_VK_LAYOUT_TRANSFER_DST:
			*access = VK_ACCESS_TRANSFER_WRITE_BIT;
			*stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		case GS_VK_LAYOUT_TRANSFER_SRC:
			*access = VK_ACCESS_TRANSFER_READ_BIT;
			*stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		case GS_VK_LAYOUT_TRANSFER_SELF:
			*access = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			*stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		case GS_VK_LAYOUT_PRESENT_SRC:
			/* (only ever a destination) */
			*access = 0;
			*stages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			break;
		case GS_VK_LAYOUT_FEEDBACK_LOOP:
			/* (a colour target in the loop is also sampled as a texture) */
			*access = color ? (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				| VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT)
				: (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
				| VK_ACCESS_SHADER_READ_BIT);
			*stages = color ? (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
				: (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
				| VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
			break;
		case GS_VK_LAYOUT_READ_WRITE_IMAGE:
			*access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			*stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			break;
		case GS_VK_LAYOUT_COMPUTE_READ_WRITE_IMAGE:
			*access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			*stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			break;
		case GS_VK_LAYOUT_GENERAL:
		default:
			/* Anything may use it: whatever came before is made visible to
			 * whatever comes after (a cleared null texture, sampled). */
			*access = dst ? (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT) : VK_ACCESS_MEMORY_WRITE_BIT;
			*stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			break;
	}
}

void gs_texture_vk_transition_levels(gs_texture_vk* t, VkCommandBuffer cmdbuf, int start_level, int num_levels,
	u32 old_layout, u32 new_layout)
{
	VkImageMemoryBarrier barrier;
	VkPipelineStageFlags src_stages, dst_stages;
	VkImageAspectFlags aspect;

	if (t->base.m_type == GS_TEXTYPE_DEPTH_STENCIL)
		aspect = gs_vk_device_stencil_buffer() ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
			: VK_IMAGE_ASPECT_DEPTH_BIT;
	else
		aspect = VK_IMAGE_ASPECT_COLOR_BIT;

	barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.pNext                           = NULL;
	barrier.oldLayout                       = s_vk_layouts[old_layout];
	barrier.newLayout                       = s_vk_layouts[new_layout];
	barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                           = t->image;
	barrier.subresourceRange.aspectMask     = aspect;
	barrier.subresourceRange.baseMipLevel   = (u32)start_level;
	barrier.subresourceRange.levelCount     = (u32)num_levels;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount     = 1;

	gs_vk_layout_access(old_layout, aspect == VK_IMAGE_ASPECT_COLOR_BIT, 0, &barrier.srcAccessMask, &src_stages);
	gs_vk_layout_access(new_layout, aspect == VK_IMAGE_ASPECT_COLOR_BIT, 1, &barrier.dstAccessMask, &dst_stages);
	/* To present: after everything. */
	if (new_layout == GS_VK_LAYOUT_PRESENT_SRC)
		src_stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

	vkCmdPipelineBarrier(cmdbuf, src_stages, dst_stages, 0, 0, NULL, 0, NULL, 1, &barrier);
}

VkFramebuffer gs_texture_vk_framebuffer(gs_texture_vk* t, gs_texture_vk* depth, int feedback_loop)
{
	struct gs_vk_fb_builder fbb;
	gs_vk_linked_fb* mine;
	gs_vk_linked_fb* theirs = NULL;
	VkRenderPass rp;
	VkFramebuffer fb;
	size_t n;
	const int is_depth = t->base.m_type == GS_TEXTYPE_DEPTH_STENCIL;

	feedback_loop = feedback_loop ? 1 : 0;
	for (n = 0; n < t->framebuffers.count; n++)
	{
		const gs_vk_linked_fb* lfb = &GS_ARRAY_AT(&t->framebuffers, gs_vk_linked_fb, n);
		if (lfb->other == depth && lfb->feedback_loop == feedback_loop)
			return lfb->fb;
	}

	rp = gs_vk_device_load_store_pass(is_depth ? VK_FORMAT_UNDEFINED : t->vk_format,
		is_depth ? t->vk_format : (depth ? depth->vk_format : VK_FORMAT_UNDEFINED), feedback_loop);
	if (!rp)
		return VK_NULL_HANDLE;

	gs_vk_fb_init(&fbb, rp, (u32)t->base.m_size.x, (u32)t->base.m_size.y);
	gs_vk_fb_add_attachment(&fbb, t->view);
	if (depth)
		gs_vk_fb_add_attachment(&fbb, depth->view);
	fb = gs_vk_fb_create(&fbb, gs_vk_device_handle());
	if (!fb)
		return VK_NULL_HANDLE;

	mine = (gs_vk_linked_fb*)gs_array_push(&t->framebuffers);
	if (depth)
		theirs = (gs_vk_linked_fb*)gs_array_push(&depth->framebuffers);
	if (!mine || (depth && !theirs))
	{
		/* (whichever was pushed comes back off) */
		if (mine)
			t->framebuffers.count--;
		if (theirs)
			depth->framebuffers.count--;
		vkDestroyFramebuffer(gs_vk_device_handle(), fb, NULL);
		return VK_NULL_HANDLE;
	}
	mine->other         = depth;
	mine->fb            = fb;
	mine->feedback_loop = feedback_loop;
	if (theirs)
	{
		theirs->other         = t;
		theirs->fb            = fb;
		theirs->feedback_loop = feedback_loop;
	}
	return fb;
}

/* The download texture: a host-visible buffer the copies land in, kept
 * mapped. */

static void gs_download_texture_vk_free(gs_download_texture* base)
{
	gs_download_texture_vk* t = (gs_download_texture_vk*)base;
	if (t->buffer != VK_NULL_HANDLE)
		gs_vk_device_release_buffer(t->buffer, &t->alloc, 1);
	free(t);
}

static void gs_download_texture_vk_copy(gs_download_texture* base, const gs_texture_rect* drc, gs_texture* stex,
	const gs_texture_rect* src, u32 src_level, int use_transfer_pitch)
{
	gs_download_texture_vk* t = (gs_download_texture_vk*)base;
	gs_texture_vk* vt         = (gs_texture_vk*)stex;
	const u32 pitch_align     = gs_vk_device_copy_pitch_alignment();
	gs_texture_transfer tr;
	VkCommandBuffer cmdbuf;
	VkBufferImageCopy bic;
	VkBufferMemoryBarrier bmb;
	u32 old_layout;

	base->m_current_pitch = gs_texture_transfer_pitch(base->m_format,
		use_transfer_pitch ? (u32)(drc->right - drc->left) : base->m_width, pitch_align);
	tr = gs_texture_transfer_of(base->m_format, base->m_current_pitch, (u32)drc->left, (u32)drc->top,
		(u32)(drc->right - drc->left), (u32)(drc->bottom - drc->top));

	gs_vk_device_end_render_pass();
	gs_texture_vk_commit_clear(vt, VK_NULL_HANDLE);

	cmdbuf     = gs_vk_device_command_buffer();
	old_layout = vt->layout;
	if (old_layout == GS_VK_LAYOUT_UNDEFINED)
		gs_texture_vk_transition(vt, cmdbuf, GS_VK_LAYOUT_TRANSFER_SRC);
	else if (old_layout != GS_VK_LAYOUT_TRANSFER_SRC)
		gs_texture_vk_transition_levels(vt, cmdbuf, (int)src_level, 1, old_layout, GS_VK_LAYOUT_TRANSFER_SRC);

	bic.bufferOffset                    = tr.offset;
	bic.bufferRowLength                 = gs_texture_row_length(base->m_format, base->m_current_pitch);
	bic.bufferImageHeight               = 0;
	bic.imageSubresource.aspectMask     = vt->base.m_type == GS_TEXTYPE_DEPTH_STENCIL ? VK_IMAGE_ASPECT_DEPTH_BIT
		: VK_IMAGE_ASPECT_COLOR_BIT;
	bic.imageSubresource.mipLevel       = src_level;
	bic.imageSubresource.baseArrayLayer = 0;
	bic.imageSubresource.layerCount     = 1;
	bic.imageOffset.x                   = src->left;
	bic.imageOffset.y                   = src->top;
	bic.imageOffset.z                   = 0;
	bic.imageExtent.width               = (u32)(src->right - src->left);
	bic.imageExtent.height              = (u32)(src->bottom - src->top);
	bic.imageExtent.depth               = 1;
	vkCmdCopyImageToBuffer(cmdbuf, vt->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->buffer, 1, &bic);

	/* The GPU's writes made visible to the host, over every row the copy
	 * wrote and on to the end. */
	bmb.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	bmb.pNext               = NULL;
	bmb.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	bmb.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
	bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bmb.buffer              = t->buffer;
	bmb.offset              = tr.offset;
	bmb.size                = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bmb, 0,
		NULL);

	if (old_layout != GS_VK_LAYOUT_TRANSFER_SRC && old_layout != GS_VK_LAYOUT_UNDEFINED)
		gs_texture_vk_transition_levels(vt, cmdbuf, (int)src_level, 1, GS_VK_LAYOUT_TRANSFER_SRC, old_layout);

	t->copy_fence_counter     = gs_vk_device_current_fence();
	t->needs_cache_invalidate = 1;
	base->m_needs_flush       = 1;
}

static int gs_download_texture_vk_map(gs_download_texture* base, const gs_texture_rect* read_rc)
{
	gs_download_texture_vk* t = (gs_download_texture_vk*)base;
	(void)read_rc;
	/* Always mapped; the heap invalidates the whole allocation, and
	 * nothing for coherent memory. */
	if (t->needs_cache_invalidate)
	{
		gs_vk_heap_invalidate(gs_vk_device_heap(), &t->alloc);
		t->needs_cache_invalidate = 0;
	}
	return 1;
}

static void gs_download_texture_vk_unmap(gs_download_texture* base)
{
	(void)base;
}

static void gs_download_texture_vk_flush(gs_download_texture* base)
{
	gs_download_texture_vk* t = (gs_download_texture_vk*)base;

	if (!base->m_needs_flush)
		return;
	base->m_needs_flush = 0;

	if (gs_vk_device_completed_fence() >= t->copy_fence_counter)
		return;
	if (gs_vk_device_current_fence() == t->copy_fence_counter)
		gs_vk_device_execute(1);
	else
		gs_vk_device_wait_fence(t->copy_fence_counter);
}

static const gs_download_texture_ops s_vk_download_texture_ops = {
	gs_download_texture_vk_free, gs_download_texture_vk_copy, gs_download_texture_vk_map,
	gs_download_texture_vk_unmap, gs_download_texture_vk_flush};

gs_download_texture* gs_download_texture_vk_create(u32 width, u32 height, unsigned format)
{
	const u32 size = gs_texture_buffer_size(format, width, height, gs_vk_device_copy_pitch_alignment());
	gs_download_texture_vk* t;
	gs_vk_alloc_t alloc;
	VkBuffer buffer = VK_NULL_HANDLE;
	void* mapped    = NULL;

	memset(&alloc, 0, sizeof(alloc));
	if (!gs_vk_device_create_readback_buffer(size, &buffer, &alloc, &mapped))
		return NULL;

	t = (gs_download_texture_vk*)calloc(1, sizeof(*t));
	if (!t)
	{
		gs_vk_device_release_buffer(buffer, &alloc, 0);
		return NULL;
	}
	gs_download_texture_base_init(&t->base, width, height, format);
	t->base.m_ops         = &s_vk_download_texture_ops;
	t->base.m_map_pointer = (const u8*)mapped;
	t->alloc              = alloc;
	t->buffer             = buffer;
	t->buffer_size        = size;
	return &t->base;
}
