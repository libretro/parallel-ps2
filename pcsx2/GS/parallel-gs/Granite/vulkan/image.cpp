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

#include "image.hpp"
#include "device.hpp"
#include "buffer.hpp"

namespace Vulkan
{
ImageView::ImageView(Device *device_, const CachedImageView &view_, const ImageViewCreateInfo &info_)
	: Cookie(device_)
	, device(device_)
	, view(view_)
	, info(info_)
{
}

const CachedImageView &ImageView::get_render_target_view(unsigned layer) const
{
	// Transient images just have one layer.
	if (info.image->get_create_info().domain == ImageDomain::Transient)
		return view;

	VK_ASSERT(layer < get_create_info().layers);

	if (render_target_views.empty())
		return view;
	else
	{
		VK_ASSERT(layer < render_target_views.size());
		return render_target_views[layer];
	}
}

const CachedImageView &ImageView::get_mip_view(unsigned level) const
{
	VK_ASSERT(level < get_create_info().levels);

	if (mip_views.empty())
		return view;
	else
	{
		VK_ASSERT(level < mip_views.size());
		return mip_views[level];
	}
}

void ImageView::free_cached_view(CachedImageView &cached)
{
	if (internal_sync)
		device->destroy_image_view_nolock(cached);
	else
		device->destroy_image_view(cached);
}

ImageView::~ImageView()
{
	free_cached_view(view);
	free_cached_view(depth_view);
	free_cached_view(stencil_view);
	free_cached_view(unorm_view);
	free_cached_view(srgb_view);
	for (auto &v : render_target_views)
		free_cached_view(v);
	for (auto &v : mip_views)
		free_cached_view(v);
}

unsigned ImageView::get_view_width() const
{
	unsigned width = info.image->get_width(info.base_level);

	if (info.aspect == VK_IMAGE_ASPECT_PLANE_1_BIT || info.aspect == VK_IMAGE_ASPECT_PLANE_2_BIT)
	{
		unsigned h = 0;
		format_ycbcr_downsample_dimensions(info.image->get_format(), info.aspect, width, h);
	}

	return width;
}

unsigned ImageView::get_view_height() const
{
	unsigned height = info.image->get_height(info.base_level);

	if (info.aspect == VK_IMAGE_ASPECT_PLANE_1_BIT || info.aspect == VK_IMAGE_ASPECT_PLANE_2_BIT)
	{
		unsigned w = 0;
		format_ycbcr_downsample_dimensions(info.image->get_format(), info.aspect, w, height);
	}

	return height;
}

unsigned ImageView::get_view_depth() const
{
	return info.image->get_depth(info.base_level);
}

Image::Image(Device *device_, VkImage image_, const CachedImageView &default_view, const DeviceAllocation &alloc_,
             const ImageCreateInfo &create_info_, VkImageViewType view_type)
    : Cookie(device_)
    , device(device_)
    , image(image_)
    , alloc(alloc_)
    , create_info(create_info_)
{
	if (view_type != VK_IMAGE_VIEW_TYPE_MAX_ENUM)
	{
		ImageViewCreateInfo info;
		info.image = this;
		info.view_type = view_type;
		info.format = create_info.format;
		info.base_level = 0;
		info.levels = create_info.levels;
		info.base_layer = 0;
		info.layers = create_info.layers;
		view = ImageViewHandle(device->handle_pool.image_views.allocate(device, default_view, info));
	}
}

DeviceAllocation Image::take_allocation_ownership()
{
	DeviceAllocation ret = {};
	std::swap(ret, alloc);
	return ret;
}

ExternalHandle Image::export_handle()
{
	return alloc.export_handle(*device);
}

void Image::disown_image()
{
	owns_image = false;
}

void Image::disown_memory_allocation()
{
	owns_memory_allocation = false;
}

Image::~Image()
{
	if (owns_image)
	{
		if (internal_sync)
			device->destroy_image_nolock(image);
		else
			device->destroy_image(image);
	}

	if (alloc.get_memory() && owns_memory_allocation)
	{
		if (internal_sync)
			device->free_memory_nolock(alloc);
		else
			device->free_memory(alloc);
	}
}

void ImageViewDeleter::operator()(ImageView *view)
{
	view->device->handle_pool.image_views.free(view);
}

void ImageDeleter::operator()(Image *image)
{
	image->device->handle_pool.images.free(image);
}
}
