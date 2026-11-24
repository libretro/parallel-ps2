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

#define NOMINMAX
#include "thread_prims.hpp"
#include "render_pass.hpp"
#include "device.hpp"
#include "quirks.hpp"
#include "owned.hpp"
#include <utility>
#include <cstring>

using namespace Util;

#define LOCK() OWNED()

namespace Vulkan
{
RenderPass::RenderPass(Hash hash, Device *device_, const RenderPassInfo &info)
	: IntrusiveHashMapEnabled<RenderPass>(hash)
	, device(device_)
{
	VkAttachmentDescription2 attachments[VULKAN_NUM_ATTACHMENTS];
	VkAttachmentReference2 colors[VULKAN_NUM_ATTACHMENTS];
	VkAttachmentReference2 depth = { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2 };
	VkSubpassDescription2 subpass = { VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2 };
	VkRenderPassCreateInfo2 rp_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2 };

	VK_ASSERT(info.num_color_attachments);
	VK_ASSERT(!(info.clear_attachments & info.load_attachments));

	memset(&signature, 0, sizeof(signature));
	signature.num_color = info.num_color_attachments;

	/* Each attachment stays in the layout its image keeps, start to end,
	 * so the pass needs no external dependency. */
	for (unsigned i = 0; i < info.num_color_attachments; i++)
	{
		VK_ASSERT(info.color_attachments[i]);
		auto &image = info.color_attachments[i]->get_image();
		const VkImageLayout layout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
		auto &att = attachments[i];

		signature.formats[i] = info.color_attachments[i]->get_format();
		signature.samples[i] = image.get_create_info().samples;

		att = { VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2 };
		att.format = info.color_attachments[i]->get_format();
		att.samples = image.get_create_info().samples;
		if (info.clear_attachments & (1u << i))
			att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		else if (info.load_attachments & (1u << i))
			att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		else
			att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.storeOp = (info.store_attachments & (1u << i)) ? VK_ATTACHMENT_STORE_OP_STORE :
		              VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att.initialLayout = layout;
		att.finalLayout = layout;

		colors[i] = { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2 };
		colors[i].attachment = i;
		colors[i].layout = layout;
	}
	samples = signature.samples[0];

	depth.attachment = VK_ATTACHMENT_UNUSED;
	depth.layout = VK_IMAGE_LAYOUT_UNDEFINED;

	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = info.num_color_attachments;
	subpass.pColorAttachments = colors;
	subpass.pDepthStencilAttachment = &depth;

	rp_info.attachmentCount = info.num_color_attachments;
	rp_info.pAttachments = attachments;
	rp_info.subpassCount = 1;
	rp_info.pSubpasses = &subpass;

	auto &table = device->get_device_table();
	if (table.vkCreateRenderPass2(device->get_device(), &rp_info, nullptr, &render_pass) != VK_SUCCESS)
		LOGE("Failed to create render pass.");
}

RenderPass::~RenderPass()
{
	auto &table = device->get_device_table();
	if (render_pass != VK_NULL_HANDLE)
		table.vkDestroyRenderPass(device->get_device(), render_pass, nullptr);
}

unsigned Framebuffer::setup_raw_views(VkImageView *views, const RenderPassInfo &info)
{
	for (unsigned i = 0; i < info.num_color_attachments; i++)
	{
		VK_ASSERT(info.color_attachments[i]);
		views[i] = info.color_attachments[i]->get_view().view;
	}
	return info.num_color_attachments;
}

void Framebuffer::compute_dimensions(const RenderPassInfo &info, uint32_t &width, uint32_t &height)
{
	width = UINT32_MAX;
	height = UINT32_MAX;
	VK_ASSERT(info.num_color_attachments);

	for (unsigned i = 0; i < info.num_color_attachments; i++)
	{
		VK_ASSERT(info.color_attachments[i]);
		width = std::min(width, info.color_attachments[i]->get_view_width());
		height = std::min(height, info.color_attachments[i]->get_view_height());
	}
}

Framebuffer::Framebuffer(Device *device_, const RenderPass &rp, const RenderPassInfo &info_)
    : Cookie(device_)
    , device(device_)
    , render_pass(rp)
    , info(info_)
{
	compute_dimensions(info_, width, height);
	VkImageView views[VULKAN_NUM_ATTACHMENTS];
	unsigned num_views = 0;

	num_views = setup_raw_views(views, info_);

	VkFramebufferCreateInfo fb_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	fb_info.renderPass = rp.get_render_pass();
	fb_info.attachmentCount = num_views;
	fb_info.pAttachments = views;
	fb_info.width = width;
	fb_info.height = height;
	fb_info.layers = 1;

	auto &table = device->get_device_table();
	if (table.vkCreateFramebuffer(device->get_device(), &fb_info, nullptr, &framebuffer) != VK_SUCCESS)
		LOGE("Failed to create framebuffer.");
}

Framebuffer::~Framebuffer()
{
	if (framebuffer != VK_NULL_HANDLE)
	{
		if (internal_sync)
			device->destroy_framebuffer_nolock(framebuffer);
		else
			device->destroy_framebuffer(framebuffer);
	}
}

FramebufferAllocator::FramebufferAllocator(Device *device_)
    : device(device_)
{
}

void FramebufferAllocator::clear()
{
	framebuffers.clear();
}

void FramebufferAllocator::begin_frame()
{
	framebuffers.begin_frame();
}

Framebuffer &FramebufferAllocator::request_framebuffer(const RenderPassInfo &info)
{
	auto &rp = device->request_render_pass(info, true);
	Hasher h;
	h.u64(rp.get_hash());

	for (unsigned i = 0; i < info.num_color_attachments; i++)
	{
		VK_ASSERT(info.color_attachments[i]);
		h.u64(info.color_attachments[i]->get_cookie());
	}

	auto hash = h.get();

	LOCK();
	auto *node = framebuffers.request(hash);
	if (node)
		return *node;

	return *framebuffers.emplace(hash, device, rp, info);
}

}
