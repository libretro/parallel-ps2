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

#ifndef GS_TEXTURE_VK_H
#define GS_TEXTURE_VK_H

#include "common/Pcsx2Defs.h"
#include "GS/Renderers/Common/GSTextureBase.h"
#include "GS/Renderers/Common/GSArray.h"
#include "GS/Renderers/Vulkan/GSVulkanHeap.h"
#include "GS/Renderers/Vulkan/VKLoader.h"

/* A texture's layout, which barriers move it between */
enum gs_vk_layout
{
	GS_VK_LAYOUT_UNDEFINED = 0,
	GS_VK_LAYOUT_PREINITIALIZED,
	GS_VK_LAYOUT_COLOR_ATTACHMENT,
	GS_VK_LAYOUT_DEPTH_STENCIL_ATTACHMENT,
	GS_VK_LAYOUT_SHADER_READ_ONLY,
	GS_VK_LAYOUT_CLEAR_DST,
	GS_VK_LAYOUT_TRANSFER_SRC,
	GS_VK_LAYOUT_TRANSFER_DST,
	GS_VK_LAYOUT_TRANSFER_SELF,
	GS_VK_LAYOUT_PRESENT_SRC,
	GS_VK_LAYOUT_FEEDBACK_LOOP,
	GS_VK_LAYOUT_READ_WRITE_IMAGE,
	GS_VK_LAYOUT_COMPUTE_READ_WRITE_IMAGE,
	GS_VK_LAYOUT_GENERAL,
	GS_VK_LAYOUT_COUNT
};

/* A framebuffer of a texture with another (a colour target's with its
 * depth, or the reverse) or with none: on both textures' lists, and
 * destroyed with whichever goes first. */
typedef struct gs_vk_linked_fb
{
	struct gs_texture_vk* other;
	VkFramebuffer fb;
	int feedback_loop;
} gs_vk_linked_fb;

typedef struct gs_texture_vk
{
	gs_texture base;
	VkImage image;
	gs_vk_alloc_t alloc; /* none: the image is not ours */
	VkImageView view;
	VkFormat vk_format;
	u32 layout;          /* gs_vk_layout */
	gs_texture_rect map_area;
	int map_level;
	gs_array_t framebuffers; /* of gs_vk_linked_fb */
	/* the fence counter of the command buffer that last used it */
	u64 use_fence_counter;
} gs_texture_vk;

typedef struct gs_download_texture_vk
{
	gs_download_texture base;
	gs_vk_alloc_t alloc;
	VkBuffer buffer;
	u64 copy_fence_counter;
	u32 buffer_size;
	int needs_cache_invalidate;
} gs_download_texture_vk;

#ifdef __cplusplus
extern "C" {
#endif

/* A new texture (type, format: gs_texture_type, gs_texture_format), or
 * NULL; freed through its ops. */
gs_texture_vk* gs_texture_vk_create(unsigned type, unsigned format, int width, int height, int levels);
gs_download_texture* gs_download_texture_vk_create(u32 width, u32 height, unsigned format);

/* The image, view and framebuffers released, now or when the GPU is
 * past them (@defer); the texture stays, empty. */
void gs_texture_vk_destroy(gs_texture_vk* t, int defer);

VkImageLayout gs_texture_vk_image_layout(u32 layout);
void gs_texture_vk_transition(gs_texture_vk* t, VkCommandBuffer cmdbuf, u32 new_layout);
void gs_texture_vk_transition_levels(gs_texture_vk* t, VkCommandBuffer cmdbuf, int start_level, int num_levels,
	u32 old_layout, u32 new_layout);
/* A pending clear recorded: in @cmdbuf, or (NULL) in the device's after
 * ending its render pass, and then only if one is pending. */
void gs_texture_vk_commit_clear(gs_texture_vk* t, VkCommandBuffer cmdbuf);
/* The framebuffer of the texture with @depth (or none), made the first
 * time it is asked for. */
VkFramebuffer gs_texture_vk_framebuffer(gs_texture_vk* t, gs_texture_vk* depth, int feedback_loop);

/* What the textures ask of the device (GSDeviceVK.c) */
VkDevice gs_vk_device_handle(void);
VkFormat gs_vk_device_native_format(unsigned format);
int gs_vk_device_create_image(const VkImageCreateInfo* ici, VkImage* image, gs_vk_alloc_t* alloc);
void gs_vk_device_image_counts(u64* made, u64* destroyed);
void gs_vk_device_count_image_destroyed(void);
void gs_vk_device_unbind_texture(gs_texture_vk* t);
void gs_vk_device_defer_framebuffer(VkFramebuffer fb);
void gs_vk_device_defer_image_view(VkImageView view);
/* the image destroyed when the GPU is past it; and if too much now
 * waits, the GPU taken to where it can be freed */
void gs_vk_device_defer_image(VkImage image, const gs_vk_alloc_t* alloc, u64 bytes);
VkCommandBuffer gs_vk_device_command_buffer(void);
VkCommandBuffer gs_vk_device_init_command_buffer(void);
void gs_vk_device_end_render_pass(void);
void gs_vk_device_execute(int wait);
VkBuffer gs_vk_device_staging_buffer(u32 size, void** mapped, gs_vk_alloc_t* alloc);
struct gs_vk_stream_buffer* gs_vk_device_upload_buffer(void);
u32 gs_vk_device_copy_offset_alignment(void);
u32 gs_vk_device_copy_pitch_alignment(void);
int gs_vk_device_stencil_buffer(void);
VkRenderPass gs_vk_device_load_store_pass(VkFormat color, VkFormat depth, int feedback_loop);
int gs_vk_device_create_readback_buffer(u32 size, VkBuffer* buffer, gs_vk_alloc_t* alloc, void** mapped);

#ifdef __cplusplus
}
#endif

#endif
