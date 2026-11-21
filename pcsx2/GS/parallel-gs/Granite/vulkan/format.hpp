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

#include "vulkan_headers.hpp"
#include "texture/texture_format.hpp"

namespace Vulkan
{
static inline bool format_has_depth_aspect(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return true;

	default:
		return false;
	}
}

static inline bool format_has_stencil_aspect(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
	case VK_FORMAT_S8_UINT:
		return true;

	default:
		return false;
	}
}

static inline bool format_has_depth_or_stencil_aspect(VkFormat format)
{
	return format_has_depth_aspect(format) || format_has_stencil_aspect(format);
}

static inline VkImageAspectFlags format_to_aspect_mask(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_UNDEFINED:
		return 0;

	case VK_FORMAT_S8_UINT:
		return VK_IMAGE_ASPECT_STENCIL_BIT;

	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return VK_IMAGE_ASPECT_STENCIL_BIT | VK_IMAGE_ASPECT_DEPTH_BIT;

	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
		return VK_IMAGE_ASPECT_DEPTH_BIT;

	default:
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}
}

static inline void format_align_dim(VkFormat format, uint32_t &width, uint32_t &height)
{
	uint32_t align_width, align_height;
	TextureFormatLayout::format_block_dim(format, align_width, align_height);
	width = ((width + align_width - 1) / align_width) * align_width;
	height = ((height + align_height - 1) / align_height) * align_height;
}

static inline void format_num_blocks(VkFormat format, uint32_t &width, uint32_t &height)
{
	uint32_t align_width, align_height;
	TextureFormatLayout::format_block_dim(format, align_width, align_height);
	width = (width + align_width - 1) / align_width;
	height = (height + align_height - 1) / align_height;
}

static inline VkDeviceSize format_get_layer_size(VkFormat format, VkImageAspectFlags aspect, unsigned width, unsigned height, unsigned depth)
{
	uint32_t blocks_x = width;
	uint32_t blocks_y = height;
	format_num_blocks(format, blocks_x, blocks_y);
	format_align_dim(format, width, height);

	VkDeviceSize size = VkDeviceSize(TextureFormatLayout::format_block_size(format, aspect)) * depth * blocks_x * blocks_y;
	return size;
}

static inline unsigned format_ycbcr_num_planes(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
	case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
	case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
	case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:
	case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM:
	case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
	case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
	case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
	case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
	case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
	case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
	case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
		return 3;

	case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
	case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
	case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
	case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:
	case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
	case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
	case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
	case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
		return 2;

	default:
		return 1;
	}
}

static inline void format_ycbcr_downsample_dimensions(VkFormat format, VkImageAspectFlags aspect, uint32_t &width, uint32_t &height)
{
	if (aspect == VK_IMAGE_ASPECT_PLANE_0_BIT)
		return;

	switch (format)
	{
#define fmt(x, sub0, sub1) \
	case VK_FORMAT_##x: \
		width >>= sub0; \
		height >>= sub1; \
		break

		fmt(G8_B8_R8_3PLANE_420_UNORM, 1, 1);
		fmt(G8_B8R8_2PLANE_420_UNORM, 1, 1);
		fmt(G8_B8_R8_3PLANE_422_UNORM, 1, 0);
		fmt(G8_B8R8_2PLANE_422_UNORM, 1, 0);
		fmt(G8_B8_R8_3PLANE_444_UNORM, 0, 0);

		fmt(G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16, 1, 1);
		fmt(G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16, 1, 0);
		fmt(G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16, 0, 0);
		fmt(G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, 1, 1);
		fmt(G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16, 1, 0);

		fmt(G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16, 1, 1);
		fmt(G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16, 1, 0);
		fmt(G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16, 0, 0);
		fmt(G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16, 1, 1);
		fmt(G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16, 1, 0);

		fmt(G16_B16_R16_3PLANE_420_UNORM, 1, 1);
		fmt(G16_B16_R16_3PLANE_422_UNORM, 1, 0);
		fmt(G16_B16_R16_3PLANE_444_UNORM, 0, 0);
		fmt(G16_B16R16_2PLANE_420_UNORM, 1, 1);
		fmt(G16_B16R16_2PLANE_422_UNORM, 1, 0);

	default:
		break;
	}
#undef fmt
}

static inline bool format_supports_storage_image_read_write_without_format(VkFormat format)
{
	/* from https://www.khronos.org/registry/vulkan/specs/1.2-extensions/html/vkspec.html#formats-without-shader-storage-format */
	static const VkFormat supported_formats[] =
	{
		VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_R8G8B8A8_SNORM,
		VK_FORMAT_R8G8B8A8_UINT,
		VK_FORMAT_R8G8B8A8_SINT,
		VK_FORMAT_R32_UINT,
		VK_FORMAT_R32_SINT,
		VK_FORMAT_R32_SFLOAT,
		VK_FORMAT_R32G32_UINT,
		VK_FORMAT_R32G32_SINT,
		VK_FORMAT_R32G32_SFLOAT,
		VK_FORMAT_R32G32B32A32_UINT,
		VK_FORMAT_R32G32B32A32_SINT,
		VK_FORMAT_R32G32B32A32_SFLOAT,
		VK_FORMAT_R16G16B16A16_UINT,
		VK_FORMAT_R16G16B16A16_SINT,
		VK_FORMAT_R16G16B16A16_SFLOAT,
		VK_FORMAT_R16G16_SFLOAT,
		VK_FORMAT_B10G11R11_UFLOAT_PACK32,
		VK_FORMAT_R16_SFLOAT,
		VK_FORMAT_R16G16B16A16_UNORM,
		VK_FORMAT_A2B10G10R10_UNORM_PACK32,
		VK_FORMAT_R16G16_UNORM,
		VK_FORMAT_R8G8_UNORM,
		VK_FORMAT_R16_UNORM,
		VK_FORMAT_R8_UNORM,
		VK_FORMAT_R16G16B16A16_SNORM,
		VK_FORMAT_R16G16_SNORM,
		VK_FORMAT_R8G8_SNORM,
		VK_FORMAT_R16_SNORM,
		VK_FORMAT_R8_SNORM,
		VK_FORMAT_R16G16_SINT,
		VK_FORMAT_R8G8_SINT,
		VK_FORMAT_R16_SINT,
		VK_FORMAT_R8_SINT,
		VK_FORMAT_A2B10G10R10_UINT_PACK32,
		VK_FORMAT_R16G16_UINT,
		VK_FORMAT_R8G8_UINT,
		VK_FORMAT_R16_UINT,
		VK_FORMAT_R8_UINT,
	};

	for (auto fmt : supported_formats)
		if (fmt == format)
			return true;

	return false;
}
}
