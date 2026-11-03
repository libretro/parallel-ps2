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

#include <algorithm>
#include "common/Pcsx2Defs.h"

#include "common/Align.h"

#include "GSDeviceVK.h"
#include "GSTextureVK.h"
#include "VKBuilders.h"

static constexpr const VkComponentMapping s_identity_swizzle{VK_COMPONENT_SWIZZLE_IDENTITY,
	VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

static VkImageLayout GetVkImageLayout(GSTextureVK::Layout layout)
{
	static constexpr std::array<VkImageLayout, static_cast<u32>(GSTextureVK::Layout::Count)> s_vk_layout_mapping = {{
		VK_IMAGE_LAYOUT_UNDEFINED, // Undefined
		VK_IMAGE_LAYOUT_PREINITIALIZED, // Preinitialized
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, // ColorAttachment
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, // DepthStencilAttachment
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, // ShaderReadOnly
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, // ClearDst
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, // TransferSrc
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, // TransferDst
		VK_IMAGE_LAYOUT_GENERAL, // TransferSelf
		VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, // PresentSrc
		VK_IMAGE_LAYOUT_GENERAL, // FeedbackLoop
		VK_IMAGE_LAYOUT_GENERAL, // ReadWriteImage
		VK_IMAGE_LAYOUT_GENERAL, // ComputeReadWriteImage
		VK_IMAGE_LAYOUT_GENERAL, // General
	}};
	return s_vk_layout_mapping[static_cast<u32>(layout)];
}

/* The table GSTexture calls this one through (gs_texture_ops):
 * every entry is this class's function of that name. */
GS_TEXTURE_OPS_DEFINE_MIPMAP(GSTextureVK, vulkan);

GSTextureVK::GSTextureVK(Type type, Format format, int width, int height, int levels, VkImage image,
	const gs_vk_alloc_t& alloc, VkImageView view, VkFormat vk_format)
	: GSTexture()
	, m_image(image)
	, m_alloc(alloc)
	, m_view(view)
	, m_vk_format(vk_format)
{
	gs_array_init(&m_framebuffers, sizeof(LinkedFramebuffer));
	m_ops = &s_vulkan_texture_ops;
	m_type = type;
	m_format = format;
	m_size.x = width;
	m_size.y = height;
	m_mipmap_levels = levels;
}

GSTextureVK::~GSTextureVK()
{
	Destroy(true);
	gs_array_free(&m_framebuffers);
}

GSTextureVK* GSTextureVK::Create(Type type, Format format, int width, int height, int levels)
{
	const VkFormat vk_format = GSDeviceVK::GetInstance()->LookupNativeFormat(format);

	VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr, 0, VK_IMAGE_TYPE_2D, vk_format,
		{static_cast<u32>(width), static_cast<u32>(height), 1}, static_cast<u32>(levels), 1, VK_SAMPLE_COUNT_1_BIT,
		VK_IMAGE_TILING_OPTIMAL};

	VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, VK_NULL_HANDLE,
		VK_IMAGE_VIEW_TYPE_2D, vk_format, s_identity_swizzle, VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<u32>(levels), 0,
		1};

	switch (type)
	{
		case Type::Texture:
		{
			ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

			if (format == Format::UNorm8)
			{
				// for r8 textures, swizzle it across all 4 components. the shaders depend on it being in alpha.. why?
				static constexpr const VkComponentMapping r8_swizzle = {
					VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R};
				vci.components = r8_swizzle;
			}
		}
		break;

		case Type::RenderTarget:
			
			ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
			break;

		case Type::DepthStencil:
			ici.usage =
				VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
				VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			break;

		case Type::RWTexture:
			ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
						VK_IMAGE_USAGE_SAMPLED_BIT;
			break;

		default:
			return nullptr;
	}

	VkImage image = VK_NULL_HANDLE;
	gs_vk_alloc_t alloc = {};

	/* Out of the heap's blocks, which were taken when the device was
	 * made: this is an offset, not a call to the driver, unless every
	 * block is full and a new one is needed. */
	if (!GSDeviceVK::GetInstance()->CreateImageInHeap(&ici,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &image, &alloc))
	{
		/* The counts matter as much as the bytes. 3870 MB of images with
		 * a cache holding 350 MB says something is keeping them, and made
		 * against destroyed says whether they are live objects nobody
		 * freed or memory the heap failed to take back from freed ones. */
		log_cb(RETRO_LOG_ERROR, "GS: no room for a %ux%u texture (heap %llu MB reserved, %llu MB used: %llu MB uploads, %llu MB images; %llu images made, %llu destroyed, %llu live).\n",
			width, height,
			(unsigned long long)(GSDeviceVK::GetInstance()->GetHeap()->bytes_reserved >> 20),
			(unsigned long long)(GSDeviceVK::GetInstance()->GetHeap()->bytes_used >> 20),
			(unsigned long long)(GSDeviceVK::GetInstance()->GetHeap()->bytes_host >> 20),
			(unsigned long long)(GSDeviceVK::GetInstance()->GetHeap()->bytes_device >> 20),
			(unsigned long long)GSDeviceVK::GetInstance()->GetImagesMade(),
			(unsigned long long)GSDeviceVK::GetInstance()->GetImagesDestroyed(),
			(unsigned long long)(GSDeviceVK::GetInstance()->GetImagesMade()
				- GSDeviceVK::GetInstance()->GetImagesDestroyed()));
		return nullptr;
	}

	VkImageView view = VK_NULL_HANDLE;
	vci.image = image;
	if (vkCreateImageView(vk_init_info.device, &vci, nullptr, &view) != VK_SUCCESS)
	{
		log_cb(RETRO_LOG_ERROR, "vkCreateImageView failed: \n");
		vkDestroyImage(vk_init_info.device, image, nullptr);
		gs_vk_heap_free(GSDeviceVK::GetInstance()->GetHeap(), &alloc);
		return nullptr;
	}

	return new GSTextureVK(type, format, width, height, levels, image, alloc, view, vk_format);
}

GSTextureVK* GSTextureVK::Adopt(
	VkImage image, Type type, Format format, int width, int height, int levels, VkFormat vk_format)
{
	// Only need to create the image view, this is mainly for swap chains.
	const VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, image,
		VK_IMAGE_VIEW_TYPE_2D, vk_format, s_identity_swizzle,
		{(type == Type::DepthStencil) ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT) :
										static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
			0u, static_cast<u32>(levels), 0u, 1u}};

	// Memory is managed by the owner of the image.
	VkImageView view = VK_NULL_HANDLE;
	VkResult res = vkCreateImageView(vk_init_info.device, &view_info, nullptr, &view);
	if (res != VK_SUCCESS)
	{
		log_cb(RETRO_LOG_ERROR, "vkCreateImageView failed: \n");
		return nullptr;
	}

	return new GSTextureVK(type, format, width, height, levels, image, gs_vk_alloc_t{}, view, vk_format);
}

void GSTextureVK::Destroy(bool defer)
{
	GSDeviceVK::GetInstance()->UnbindTexture(this);

	if (m_type == Type::RenderTarget || m_type == Type::DepthStencil)
	{
		for (size_t n = 0; n < m_framebuffers.count; n++)
		{
			const LinkedFramebuffer& lfb = GS_ARRAY_AT(&m_framebuffers, LinkedFramebuffer, n);
			GSTextureVK* other_tex = lfb.other;
			if (other_tex)
			{
				gs_array_t* others = &other_tex->m_framebuffers;
				for (size_t m = 0; m < others->count; m++)
				{
					if (GS_ARRAY_AT(others, LinkedFramebuffer, m).other == this)
					{
						gs_array_erase(others, m);
						break;
					}
				}
			}

			if (defer)
				GSDeviceVK::GetInstance()->DeferFramebufferDestruction(lfb.fb);
			else
				vkDestroyFramebuffer(vk_init_info.device, lfb.fb, nullptr);
		}
		m_framebuffers.count = 0;
	}

		if (m_view != VK_NULL_HANDLE)
	{
		if (defer)
			GSDeviceVK::GetInstance()->DeferImageViewDestruction(m_view);
		else
			vkDestroyImageView(vk_init_info.device, m_view, nullptr);
		m_view = VK_NULL_HANDLE;
	}

	// If we don't have device memory allocated, the image is not owned by us (e.g. swapchain)
	if (m_alloc.memory != VK_NULL_HANDLE)
	{
		if (defer)
		{
			GSDeviceVK::GetInstance()->DeferImageDestruction(m_image, m_alloc);

			/* And if too much is now waiting on fences, get the GPU to
			 * a point where it can be freed rather than letting the
			 * list grow to the size of the card.
			 *
			 * The braces matter and were missing: without them the else
			 * below belonged to this if, so every deferred image whose
			 * budget check came back false was destroyed here as well -
			 * a second destroy of an image already on a cleanup list,
			 * and its memory handed back while the GPU was still
			 * reading it. Mine, from the commit that added the budget. */
			GSDeviceVK::GetInstance()->RelieveMemPressure(
				GSDeviceVK::GetInstance()->DeferredDestructionPressure(GetMemUsage()));
		}
		else
		{
			vkDestroyImage(vk_init_info.device, m_image, nullptr);
			gs_vk_heap_free(GSDeviceVK::GetInstance()->GetHeap(), &m_alloc);
			GSDeviceVK::GetInstance()->CountImageDestroyed();
		}
		m_image = VK_NULL_HANDLE;
		m_alloc = gs_vk_alloc_t{};
	}
}

VkImageLayout GSTextureVK::GetVkLayout() const
{
	return GetVkImageLayout(m_layout);
}

VkCommandBuffer GSTextureVK::GetCommandBufferForUpdate()
{
	if (m_type != Type::Texture || m_use_fence_counter == GSDeviceVK::GetInstance()->GetCurrentFenceCounter())
	{
		// log_cb(RETRO_LOG_INFO, "Texture update within frame, can't use do beforehand\n");
		GSDeviceVK::GetInstance()->EndRenderPass();
		return GSDeviceVK::GetInstance()->GetCurrentCommandBuffer();
	}

	return GSDeviceVK::GetInstance()->GetCurrentInitCommandBuffer();
}

void GSTextureVK::CopyTextureDataForUpload(void* dst, const void* src, u32 pitch, u32 upload_pitch, u32 height) const
{
	const u32 block_size = GetCompressedBlockSize();
	const u32 count = (height + (block_size - 1)) / block_size;
	GSStrideMemCpy(dst, upload_pitch, src, pitch, pcsx2_min_i(upload_pitch, pitch), count);
}

VkBuffer GSTextureVK::AllocateUploadStagingBuffer(const void* data, u32 pitch, u32 upload_pitch, u32 height) const
{
	const u32 size = upload_pitch * height;
	void* mapped = nullptr;
	gs_vk_alloc_t alloc = {};
	VkBuffer buffer;

	/* Out of the device's inventory of upload buffers rather than made
	 * here and thrown away: this ran once per texture upload that did not
	 * fit the stream buffer, which under any texture churn is hundreds of
	 * driver allocations a frame, none of them counted anywhere. The
	 * device hands one back to its free list when the command buffer that
	 * reads it completes. */
	buffer = GSDeviceVK::GetInstance()->AcquireStagingBuffer(size, &mapped, &alloc);
	if (buffer == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	// Don't worry about setting the coherent bit for this upload, the main reason we had
	// that set in StreamBuffer was for MoltenVK, which would upload the whole buffer on
	// smaller uploads, but we're writing to the whole thing anyway.
	CopyTextureDataForUpload(mapped, data, pitch, upload_pitch, height);
	gs_vk_heap_flush(GSDeviceVK::GetInstance()->GetHeap(), &alloc);
	return buffer;
}

void GSTextureVK::UpdateFromBuffer(VkCommandBuffer cmdbuf, int level, int layer, u32 x, u32 y, u32 width, u32 height,
		u32 buffer_height, u32 row_length, VkBuffer buffer, u32 buffer_offset)
{
	const Layout old_layout = m_layout;
	if (old_layout == Layout::Undefined)
		TransitionToLayout(cmdbuf, Layout::TransferDst);
	else if (old_layout != Layout::TransferDst)
		TransitionSubresourcesToLayout(
				cmdbuf, level, 1, old_layout, Layout::TransferDst);

	const VkBufferImageCopy bic = {static_cast<VkDeviceSize>(buffer_offset), row_length, buffer_height,
		{VK_IMAGE_ASPECT_COLOR_BIT, static_cast<u32>(level), 0u, 1u}, {static_cast<s32>(x), static_cast<s32>(y), 0},
		{width, height, 1u}};

	vkCmdCopyBufferToImage(cmdbuf, buffer, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);

	if (old_layout != Layout::TransferDst && old_layout != Layout::Undefined)
		TransitionSubresourcesToLayout(
				cmdbuf, level, 1, Layout::TransferDst, old_layout);
}


bool GSTextureVK::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
	if (layer >= m_mipmap_levels)
		return false;

	const u32 width = r.width();
	const u32 height = r.height();
	const u32 upload_pitch = pcsx2_align_up_pow2_u32(pitch, GSDeviceVK::GetInstance()->GetBufferCopyRowPitchAlignment());
	const u32 required_size = CalcUploadSize(height, upload_pitch);

	// If the texture is larger than half our streaming buffer size, use a separate buffer.
	// Otherwise allocation will either fail, or require lots of cmdbuffer submissions.
	VkBuffer buffer;
	u32 buffer_offset;
	if (required_size > (GSDeviceVK::GetInstance()->GetTextureUploadBuffer().GetCurrentSize() / 2))
	{
		buffer_offset = 0;
		buffer = AllocateUploadStagingBuffer(data, pitch, upload_pitch, height);
		if (buffer == VK_NULL_HANDLE)
			return false;
	}
	else
	{
		VKStreamBuffer& sbuffer = GSDeviceVK::GetInstance()->GetTextureUploadBuffer();
		if (!sbuffer.ReserveMemory(required_size, GSDeviceVK::GetInstance()->GetBufferCopyOffsetAlignment()))
		{
			/* While waiting for x bytes in texture upload buffer */
			GSDeviceVK::GetInstance()->ExecuteCommandBuffer(false);
			if (!sbuffer.ReserveMemory(required_size, GSDeviceVK::GetInstance()->GetBufferCopyOffsetAlignment()))
			{
				log_cb(RETRO_LOG_ERROR, "Failed to reserve texture upload memory (%u bytes).\n", required_size);
				return false;
			}
		}

		buffer = sbuffer.GetBuffer();
		buffer_offset = sbuffer.GetCurrentOffset();
		CopyTextureDataForUpload(sbuffer.GetCurrentHostPointer(), data, pitch, upload_pitch, height);
		sbuffer.CommitMemory(required_size);
	}

	const VkCommandBuffer cmdbuf = GetCommandBufferForUpdate();

	// first time the texture is used? don't leave it undefined
	if (m_layout == Layout::Undefined)
		TransitionToLayout(cmdbuf, Layout::TransferDst);

	// if we're an rt and have been cleared, and the full rect isn't being uploaded, do the clear
	if (m_type == Type::RenderTarget)
	{
		if (!r.eq(GSVector4i(0, 0, m_size.x, m_size.y)))
			CommitClear(cmdbuf);
		else
			m_state = State::Dirty;
	}

	UpdateFromBuffer(cmdbuf, layer, 0, r.x, r.y, width, height,
		pcsx2_align_up_pow2_u32(height, GetCompressedBlockSize()),
		CalcUploadRowLengthFromPitch(upload_pitch), buffer, buffer_offset);
	TransitionToLayout(cmdbuf, Layout::ShaderReadOnly);

	if (m_type == Type::Texture)
		m_needs_mipmaps_generated |= (layer == 0);

	return true;
}

bool GSTextureVK::Map(GSMap& m, const GSVector4i* r, int layer)
{
	if (layer >= m_mipmap_levels || IsCompressedFormat())
		return false;

	// map for writing
	m_map_area  = r ? *r : GetRect();
	m_map_level = layer;

	m.pitch     = pcsx2_align_up_pow2_u32(CalcUploadPitch(m_map_area.width()),
			GSDeviceVK::GetInstance()->GetBufferCopyRowPitchAlignment());

	// see note in Update() for the reason why.
	const u32 required_size = CalcUploadSize(m_map_area.height(), m.pitch);
	VKStreamBuffer& buffer  = GSDeviceVK::GetInstance()->GetTextureUploadBuffer();
	if (required_size >= (buffer.GetCurrentSize() / 2))
		return false;

	if (!buffer.ReserveMemory(required_size, GSDeviceVK::GetInstance()->GetBufferCopyOffsetAlignment()))
	{
		/* While waiting for x bytes in texture upload buffer */
		GSDeviceVK::GetInstance()->ExecuteCommandBuffer(false);
		if (!buffer.ReserveMemory(required_size, GSDeviceVK::GetInstance()->GetBufferCopyOffsetAlignment()))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to reserve texture upload memory\n");
			return false;
		}
	}

	m.bits = static_cast<u8*>(buffer.GetCurrentHostPointer());
	return true;
}

void GSTextureVK::Unmap()
{
	const u32 width         = m_map_area.width();
	const u32 height        = m_map_area.height();
	const u32 pitch         = pcsx2_align_up_pow2_u32(CalcUploadPitch(width),
			GSDeviceVK::GetInstance()->GetBufferCopyRowPitchAlignment());
	const u32 required_size = CalcUploadSize(height, pitch);
	VKStreamBuffer& buffer  = GSDeviceVK::GetInstance()->GetTextureUploadBuffer();
	const u32 buffer_offset = buffer.GetCurrentOffset();
	buffer.CommitMemory(required_size);

	const VkCommandBuffer cmdbuf = GetCommandBufferForUpdate();

	// first time the texture is used? don't leave it undefined
	if (m_layout == Layout::Undefined)
		TransitionToLayout(cmdbuf, Layout::TransferDst);

	// if we're an rt and have been cleared, and the full rect isn't being uploaded, do the clear
	if (m_type == Type::RenderTarget)
	{
		if (!m_map_area.eq(GSVector4i(0, 0, m_size.x, m_size.y)))
			CommitClear(cmdbuf);
		else
			m_state = State::Dirty;
	}

	UpdateFromBuffer(cmdbuf, m_map_level, 0, m_map_area.x, m_map_area.y, width, height,
		pcsx2_align_up_pow2_u32(height, GetCompressedBlockSize()),
		CalcUploadRowLengthFromPitch(pitch), buffer.GetBuffer(), buffer_offset);
	TransitionToLayout(cmdbuf, Layout::ShaderReadOnly);

	if (m_type == Type::Texture)
		m_needs_mipmaps_generated |= (m_map_level == 0);
}

void GSTextureVK::GenerateMipmap()
{
	const VkCommandBuffer cmdbuf = GetCommandBufferForUpdate();

	if (m_layout == Layout::Undefined)
		TransitionToLayout(cmdbuf, Layout::TransferSrc);

	for (int dst_level = 1; dst_level < m_mipmap_levels; dst_level++)
	{
		const int src_level = dst_level - 1;
		const int src_width = pcsx2_max_i(m_size.x >> src_level, 1);
		const int src_height = pcsx2_max_i(m_size.y >> src_level, 1);
		const int dst_width = pcsx2_max_i(m_size.x >> dst_level, 1);
		const int dst_height = pcsx2_max_i(m_size.y >> dst_level, 1);

		TransitionSubresourcesToLayout(
			cmdbuf, src_level, 1, m_layout, Layout::TransferSrc);
		TransitionSubresourcesToLayout(
			cmdbuf, dst_level, 1, m_layout, Layout::TransferDst);

		const VkImageBlit blit = {
			{VK_IMAGE_ASPECT_COLOR_BIT, static_cast<u32>(src_level), 0u, 1u}, // srcSubresource
			{{0, 0, 0}, {src_width, src_height, 1}}, // srcOffsets
			{VK_IMAGE_ASPECT_COLOR_BIT, static_cast<u32>(dst_level), 0u, 1u}, // dstSubresource
			{{0, 0, 0}, {dst_width, dst_height, 1}} // dstOffsets
		};

		vkCmdBlitImage(cmdbuf, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

		TransitionSubresourcesToLayout(
			cmdbuf, src_level, 1, Layout::TransferSrc, m_layout);
		TransitionSubresourcesToLayout(
			cmdbuf, dst_level, 1, Layout::TransferDst, m_layout);
	}
}

void GSTextureVK::CommitClear()
{
	if (m_state != GSTexture::State::Cleared)
		return;

	GSDeviceVK::GetInstance()->EndRenderPass();

	CommitClear(GSDeviceVK::GetInstance()->GetCurrentCommandBuffer());
}

void GSTextureVK::CommitClear(VkCommandBuffer cmdbuf)
{
	TransitionToLayout(cmdbuf, Layout::ClearDst);

	if (IsDepthStencil())
	{
		const VkClearDepthStencilValue cv = { m_clear_value.depth };
		const VkImageSubresourceRange srr = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u };
		vkCmdClearDepthStencilImage(cmdbuf, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &srr);
	}
	else
	{
		alignas(16) VkClearColorValue cv;
		GSVector4::store<true>(cv.float32, GetUNormClearColor());
		const VkImageSubresourceRange srr = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
		vkCmdClearColorImage(cmdbuf, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &srr);
	}

	SetState(GSTexture::State::Dirty);
}

void GSTextureVK::OverrideImageLayout(Layout new_layout) { m_layout = new_layout; }

void GSTextureVK::TransitionToLayout(Layout layout)
{
	TransitionToLayout(GSDeviceVK::GetInstance()->GetCurrentCommandBuffer(), layout);
}

void GSTextureVK::TransitionToLayout(VkCommandBuffer command_buffer, Layout new_layout)
{
	if (m_layout == new_layout)
		return;

	TransitionSubresourcesToLayout(command_buffer, 0, m_mipmap_levels, m_layout, new_layout);

	m_layout = new_layout;
}

void GSTextureVK::TransitionSubresourcesToLayout(VkCommandBuffer command_buffer, int start_level, int num_levels,
		Layout old_layout, Layout new_layout)
{
	VkImageAspectFlags aspect;
	if (m_type == Type::DepthStencil)
		aspect = g_gs_device->Features().stencil_buffer ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
	else
		aspect = VK_IMAGE_ASPECT_COLOR_BIT;

	VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, 0, GetVkImageLayout(old_layout),
		GetVkImageLayout(new_layout), VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, m_image,
		{aspect, static_cast<u32>(start_level), static_cast<u32>(num_levels), 0u, 1u} // VkImageSubresourceRange    subresourceRange
	};

	// srcStageMask -> Stages that must complete before the barrier
	// dstStageMask -> Stages that must wait for after the barrier before beginning
	VkPipelineStageFlags srcStageMask, dstStageMask;
	switch (old_layout)
	{
		case Layout::Undefined:
			// Layout undefined therefore contents undefined, and we don't care what happens to it.
			barrier.srcAccessMask = 0;
			srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			break;

		case Layout::Preinitialized:
			// Image has been pre-initialized by the host, so ensure all writes have completed.
			barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
			srcStageMask = VK_PIPELINE_STAGE_HOST_BIT;
			break;

		case Layout::ColorAttachment:
			// Image was being used as a color attachment, so ensure all writes have completed.
			barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			break;

		case Layout::DepthStencilAttachment:
			// Image was being used as a depthstencil attachment, so ensure all writes have completed.
			barrier.srcAccessMask =
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			break;

		case Layout::ShaderReadOnly:
			// Image was being used as a shader resource, make sure all reads have finished.
			barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			break;

		case Layout::ClearDst:
			// Image was being used as a clear destination, ensure all writes have finished.
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;

		case Layout::TransferSrc:
			// Image was being used as a copy source, ensure all reads have finished.
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;

		case Layout::TransferDst:
			// Image was being used as a copy destination, ensure all writes have finished.
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		case Layout::TransferSelf:
			// Image was being used as a copy source and destination, ensure all reads and writes have finished.
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;

		case Layout::FeedbackLoop:
			barrier.srcAccessMask = (aspect == VK_IMAGE_ASPECT_COLOR_BIT) ?
										(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
										 VK_ACCESS_INPUT_ATTACHMENT_READ_BIT) :
										(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
											VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
			srcStageMask = (aspect == VK_IMAGE_ASPECT_COLOR_BIT) ?
							   (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) :
							   (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
								   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
			break;

		case Layout::ReadWriteImage:
			barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			break;

		case Layout::ComputeReadWriteImage:
			barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			break;

		case Layout::General:
		default:
			srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			break;
	}

		switch (new_layout)
	{
		case Layout::Undefined:
			barrier.dstAccessMask = 0;
			dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			break;

		case Layout::ColorAttachment:
			barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			break;

		case Layout::DepthStencilAttachment:
			barrier.dstAccessMask =
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			break;

		case Layout::ShaderReadOnly:
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			break;

		case Layout::ClearDst:
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;

		case Layout::TransferSrc:
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;

		case Layout::TransferDst:
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;

		case Layout::TransferSelf:
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;

		case Layout::PresentSrc:
			srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			break;

		case Layout::FeedbackLoop:
			barrier.dstAccessMask = (aspect == VK_IMAGE_ASPECT_COLOR_BIT) ?
										(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
										VK_ACCESS_INPUT_ATTACHMENT_READ_BIT) :
										(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
											VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
			dstStageMask = (aspect == VK_IMAGE_ASPECT_COLOR_BIT) ?
							   (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) :
							   (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
								   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
			break;

		case Layout::ReadWriteImage:
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			break;

		case Layout::ComputeReadWriteImage:
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			break;

		case Layout::General:
		default:
			dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			break;
	}
	vkCmdPipelineBarrier(command_buffer, srcStageMask, dstStageMask, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

VkFramebuffer GSTextureVK::GetFramebuffer(bool feedback_loop) { return GetLinkedFramebuffer(nullptr, feedback_loop); }

VkFramebuffer GSTextureVK::GetLinkedFramebuffer(GSTextureVK* depth_texture, bool feedback_loop)
{
	for (size_t n = 0; n < m_framebuffers.count; n++)
	{
		const LinkedFramebuffer& lfb = GS_ARRAY_AT(&m_framebuffers, LinkedFramebuffer, n);
		if (lfb.other == depth_texture && lfb.feedback_loop == feedback_loop)
			return lfb.fb;
	}

	VkRenderPass rp = GSDeviceVK::GetInstance()->GetRenderPass(
		(m_type != GSTexture::Type::DepthStencil) ? m_vk_format : VK_FORMAT_UNDEFINED,
		(m_type != GSTexture::Type::DepthStencil) ? (depth_texture ? depth_texture->m_vk_format : VK_FORMAT_UNDEFINED)
		 : m_vk_format, 
		VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD,
		VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, feedback_loop);
	if (!rp)
		return VK_NULL_HANDLE;

	Vulkan::FramebufferBuilder fbb;
	fbb.AddAttachment(m_view);
	if (depth_texture)
		fbb.AddAttachment(depth_texture->m_view);
	fbb.SetSize(m_size.x, m_size.y, 1);
	fbb.SetRenderPass(rp);

	VkFramebuffer fb = fbb.Create(vk_init_info.device);
	if (!fb)
		return VK_NULL_HANDLE;

	LinkedFramebuffer* mine = static_cast<LinkedFramebuffer*>(gs_array_push(&m_framebuffers));
	LinkedFramebuffer* theirs = depth_texture ? static_cast<LinkedFramebuffer*>(gs_array_push(&depth_texture->m_framebuffers)) : nullptr;
	if (!mine || (depth_texture && !theirs))
	{
		/* (whichever was pushed comes back off) */
		if (mine)
			m_framebuffers.count--;
		if (theirs)
			depth_texture->m_framebuffers.count--;
		vkDestroyFramebuffer(vk_init_info.device, fb, nullptr);
		return VK_NULL_HANDLE;
	}
	mine->other         = depth_texture;
	mine->fb            = fb;
	mine->feedback_loop = feedback_loop;
	if (theirs)
	{
		theirs->other         = this;
		theirs->fb            = fb;
		theirs->feedback_loop = feedback_loop;
	}
	return fb;
}

/* The table GSDownloadTexture calls this one through. */
GS_DOWNLOAD_TEXTURE_OPS_DEFINE(GSDownloadTextureVK, vulkan_dl);

GSDownloadTextureVK::GSDownloadTextureVK(u32 width, u32 height, GSTexture::Format format)
	: GSDownloadTexture(width, height, format)
{
	m_ops = &s_vulkan_dl_download_texture_ops;
}

GSDownloadTextureVK::~GSDownloadTextureVK()
{
	// Buffer was created mapped, no need to manually unmap.
	if (m_buffer != VK_NULL_HANDLE)
		GSDeviceVK::GetInstance()->DeferBufferDestruction(m_buffer, m_alloc);
}

GSDownloadTextureVK* GSDownloadTextureVK::Create(u32 width, u32 height, GSTexture::Format format)
{
	const u32 buffer_size = GetBufferSize(width, height, format, GSDeviceVK::GetInstance()->GetBufferCopyRowPitchAlignment());

	const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0u, buffer_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_SHARING_MODE_EXCLUSIVE, 0u, nullptr};

	gs_vk_alloc_t alloc = {};
	VkBuffer buffer = VK_NULL_HANDLE;
	void* mapped = nullptr;

	/* Host visible so it can be read back, cached where the driver has
	 * such a type - that is what GPU_TO_CPU meant. */
	if (!GSDeviceVK::GetInstance()->CreateBufferInHeap(&bci,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
			&buffer, &alloc, &mapped))
		return nullptr;

	GSDownloadTextureVK* tex = new GSDownloadTextureVK(width, height, format);
	tex->m_alloc = alloc;
	tex->m_buffer = buffer;
	tex->m_buffer_size = buffer_size;
	tex->m_map_pointer = static_cast<const u8*>(mapped);
	return tex;
}

void GSDownloadTextureVK::CopyFromTexture(
	const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
{
	GSTextureVK* const vkTex = static_cast<GSTextureVK*>(stex);
	u32 copy_offset, copy_size, copy_rows;
	m_current_pitch =
		GetTransferPitch(use_transfer_pitch ? static_cast<u32>(drc.width()) : m_width, GSDeviceVK::GetInstance()->GetBufferCopyRowPitchAlignment());
	GetTransferSize(drc, &copy_offset, &copy_size, &copy_rows);

	GSDeviceVK::GetInstance()->EndRenderPass();
	vkTex->CommitClear();

	const VkCommandBuffer cmdbuf = GSDeviceVK::GetInstance()->GetCurrentCommandBuffer();

	GSTextureVK::Layout old_layout = vkTex->GetLayout();
	if (old_layout == GSTextureVK::Layout::Undefined)
		vkTex->TransitionToLayout(cmdbuf, GSTextureVK::Layout::TransferSrc);
	else if (old_layout != GSTextureVK::Layout::TransferSrc)
		vkTex->TransitionSubresourcesToLayout(cmdbuf, src_level, 1, old_layout, GSTextureVK::Layout::TransferSrc);

	VkBufferImageCopy image_copy    = {};
	const VkImageAspectFlags aspect = vkTex->IsDepthStencil() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	image_copy.bufferOffset         = copy_offset;
	image_copy.bufferRowLength      = GSTexture::CalcUploadRowLengthFromPitch(m_format, m_current_pitch);
	image_copy.bufferImageHeight    = 0;
	image_copy.imageSubresource     = {aspect, src_level, 0u, 1u};
	image_copy.imageOffset          = {src.left, src.top, 0};
	image_copy.imageExtent          = {static_cast<u32>(src.width()), static_cast<u32>(src.height()), 1u};

	// do the copy
	vkCmdCopyImageToBuffer(cmdbuf, vkTex->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_buffer, 1, &image_copy);

	// flush gpu cache, over every row the copy wrote and on to the end
	const VkBufferMemoryBarrier buffer_info = {
		VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, // VkStructureType    sType
		nullptr, // const void*        pNext
		VK_ACCESS_TRANSFER_WRITE_BIT, // VkAccessFlags      srcAccessMask
		VK_ACCESS_HOST_READ_BIT, // VkAccessFlags      dstAccessMask
		VK_QUEUE_FAMILY_IGNORED, // uint32_t           srcQueueFamilyIndex
		VK_QUEUE_FAMILY_IGNORED, // uint32_t           dstQueueFamilyIndex
		m_buffer, // VkBuffer           buffer
		copy_offset, // VkDeviceSize       offset
		VK_WHOLE_SIZE // VkDeviceSize       size
	};
	vkCmdPipelineBarrier(
			cmdbuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &buffer_info, 0, nullptr);

	if (old_layout != GSTextureVK::Layout::TransferSrc && old_layout != GSTextureVK::Layout::Undefined)
		vkTex->TransitionSubresourcesToLayout(cmdbuf, src_level, 1, GSTextureVK::Layout::TransferSrc, old_layout);

	m_copy_fence_counter     = GSDeviceVK::GetInstance()->GetCurrentFenceCounter();
	m_needs_cache_invalidate = true;
	m_needs_flush = true;
}

bool GSDownloadTextureVK::Map(const GSVector4i& read_rc)
{
	// Always mapped, but we might need to invalidate the cache.
	if (m_needs_cache_invalidate)
	{
		/* The heap invalidates the whole allocation, rounded to the
		 * atom size, and does nothing when the memory is coherent. */
		gs_vk_heap_invalidate(GSDeviceVK::GetInstance()->GetHeap(), &m_alloc);
		m_needs_cache_invalidate = false;
	}

	return true;
}

void GSDownloadTextureVK::Unmap()
{
	// Always mapped.
}

void GSDownloadTextureVK::Flush()
{
	if (!m_needs_flush)
		return;

	m_needs_flush = false;

	if (GSDeviceVK::GetInstance()->GetCompletedFenceCounter() >= m_copy_fence_counter)
		return;

	// Need to execute command buffer.
	if (GSDeviceVK::GetInstance()->GetCurrentFenceCounter() == m_copy_fence_counter)
		GSDeviceVK::GetInstance()->ExecuteCommandBuffer(true);
	else
		GSDeviceVK::GetInstance()->WaitForFenceCounter(m_copy_fence_counter);
}
