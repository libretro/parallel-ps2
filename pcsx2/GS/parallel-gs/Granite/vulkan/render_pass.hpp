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

#pragma once

#include "thread_prims.hpp"
#include "cookie.hpp"
#include "hash.hpp"
#include "image.hpp"
#include "intrusive.hpp"
#include "limits.hpp"
#include "object_pool.hpp"
#include "temporary_hashmap.hpp"
#include "vulkan_headers.hpp"

namespace Vulkan
{
class ImageView;
/* One subpass drawing to colour attachments: all paraLLEl-GS asks of a
 * render pass. */
struct RenderPassInfo
{
	const ImageView *color_attachments[VULKAN_NUM_ATTACHMENTS];
	unsigned num_color_attachments = 0;
	uint32_t clear_attachments = 0;
	uint32_t load_attachments = 0;
	uint32_t store_attachments = 0;
	VkClearColorValue clear_color[VULKAN_NUM_ATTACHMENTS] = {};
};

/* What a compatible render pass is made from, without its images: the
 * attachment formats and sample counts. Compatibility goes by these
 * alone, so 1x1 stand-ins for the attachments make the same pass again. */
struct RenderPassSignature
{
	uint32_t formats[VULKAN_NUM_ATTACHMENTS];
	uint32_t samples[VULKAN_NUM_ATTACHMENTS];
	uint32_t num_color;
};

class RenderPass : public HashedObject<RenderPass>, public NoCopyNoMove
{
public:
	RenderPass(Util::Hash hash, Device *device, const RenderPassInfo &info);
	~RenderPass();

	VkRenderPass get_render_pass() const
	{
		return render_pass;
	}

	const RenderPassSignature &get_signature() const
	{
		return signature;
	}

	uint32_t get_sample_count() const
	{
		return samples;
	}

	unsigned get_num_color_attachments() const
	{
		return signature.num_color;
	}

private:
	Device *device;
	VkRenderPass render_pass = VK_NULL_HANDLE;
	RenderPassSignature signature;
	uint32_t samples = 1;
};

class Framebuffer : public Cookie, public NoCopyNoMove, public InternalSyncEnabled
{
public:
	Framebuffer(Device *device, const RenderPass &rp, const RenderPassInfo &info);
	~Framebuffer();

	VkFramebuffer get_framebuffer() const
	{
		return framebuffer;
	}

	static unsigned setup_raw_views(VkImageView *views, const RenderPassInfo &info);
	static void compute_dimensions(const RenderPassInfo &info, uint32_t &width, uint32_t &height);

	uint32_t get_width() const
	{
		return width;
	}

	uint32_t get_height() const
	{
		return height;
	}

	const RenderPass &get_compatible_render_pass() const
	{
		return render_pass;
	}

private:
	Device *device;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	const RenderPass &render_pass;
	RenderPassInfo info;
	uint32_t width = 0;
	uint32_t height = 0;
};

static const unsigned VULKAN_FRAMEBUFFER_RING_SIZE = 16;
class FramebufferAllocator
{
public:
	explicit FramebufferAllocator(Device *device);
	Framebuffer &request_framebuffer(const RenderPassInfo &info);

	void begin_frame();
	void clear();

private:
	struct FramebufferNode : Util::TemporaryHashmapEnabled<FramebufferNode>,
	                         Util::IntrusiveListEnabled<FramebufferNode>,
	                         Framebuffer
	{
		FramebufferNode(Device *device_, const RenderPass &rp, const RenderPassInfo &info_)
		    : Framebuffer(device_, rp, info_)
		{
			set_internal_sync_object();
		}
	};

	Device *device;
	Util::TemporaryHashmap<FramebufferNode, VULKAN_FRAMEBUFFER_RING_SIZE, false> framebuffers;
};
}
