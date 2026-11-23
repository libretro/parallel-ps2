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

#include "sampler.hpp"
#include "device.hpp"

namespace Vulkan
{
Sampler::Sampler(Device *device_, VkSampler sampler_, const SamplerCreateInfo &info, bool immutable_)
    : Cookie(device_)
    , device(device_)
    , sampler(sampler_)
    , create_info(info)
    , immutable(immutable_)
{
}

Sampler::~Sampler()
{
	if (sampler)
	{
		if (immutable)
			device->managers.views.destroy_sampler(sampler);
		else if (internal_sync)
			device->destroy_sampler_nolock(sampler);
		else
			device->destroy_sampler(sampler);
	}
}

void SamplerDeleter::operator()(Sampler *sampler)
{
	sampler->device->handle_pool.samplers.free(sampler);
}

VkSamplerCreateInfo Sampler::fill_vk_sampler_info(const SamplerCreateInfo &sampler_info)
{
	VkSamplerCreateInfo info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };

	info.magFilter = sampler_info.mag_filter;
	info.minFilter = sampler_info.min_filter;
	info.mipmapMode = sampler_info.mipmap_mode;
	info.addressModeU = sampler_info.address_mode_u;
	info.addressModeV = sampler_info.address_mode_v;
	info.addressModeW = sampler_info.address_mode_w;
	info.maxAnisotropy = 1.0f;
	info.minLod = sampler_info.min_lod;
	info.maxLod = sampler_info.max_lod;
	return info;
}

ImmutableSampler::ImmutableSampler(Util::Hash hash, Device *device_, const SamplerCreateInfo &sampler_info)
	: HashedObject<ImmutableSampler>(hash), device(device_)
{
	auto info = Sampler::fill_vk_sampler_info(sampler_info);
	VkSampler vk_sampler = device->managers.views.create_sampler(&info);

	sampler = SamplerHandle(device->handle_pool.samplers.allocate(device, vk_sampler, sampler_info, true));
}
}
