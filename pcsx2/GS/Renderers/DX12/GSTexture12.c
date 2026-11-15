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

#define COBJMACROS
#include <stdlib.h>
#include <string.h>

#include "common/Align.h"
#include "GSTexture12.h"
#include "D3D12StreamBuffer.h"
#include "GS/Renderers/Common/GSTextureFormat.h"

static D3D12_CPU_DESCRIPTOR_HANDLE gs_d3d12_cpu(const gs_d3d12_descriptor_handle* h)
{
	D3D12_CPU_DESCRIPTOR_HANDLE c;
	c.ptr = h->cpu_handle.ptr;
	return c;
}

static int gs_texture_d3d12_update(gs_texture* base, const gs_texture_rect* r, const void* data, int pitch, int layer);
static int gs_texture_d3d12_map(gs_texture* base, gs_texture_map* m, const gs_texture_rect* r, int layer);
static void gs_texture_d3d12_unmap(gs_texture* base);
static void gs_texture_d3d12_generate_mipmap(gs_texture* base);

static void gs_texture_d3d12_free(gs_texture* base)
{
	gs_texture_d3d12* t = (gs_texture_d3d12*)base;
	gs_texture_d3d12_destroy(t, 1);
	free(t);
}

static const gs_texture_ops s_d3d12_texture_ops = {
	gs_texture_d3d12_free, gs_texture_d3d12_update, gs_texture_d3d12_map, gs_texture_d3d12_unmap,
	gs_texture_d3d12_generate_mipmap};

/* A descriptor back to its heap, now or with the command list */
static void gs_d3d12_release_descriptor(u32 kind, gs_d3d12_descriptor_handle* h, int defer)
{
	if (defer)
		gs_d3d12_device_defer_descriptor(kind, h);
	else
		gs_d3d12_descriptor_heap_manager_free(gs_d3d12_device_descriptor_heap(kind), h);
}

void gs_texture_d3d12_destroy(gs_texture_d3d12* t, int defer)
{
	gs_d3d12_device_unbind_texture(t);

	gs_d3d12_release_descriptor(GS_D3D12_HEAP_SRV, &t->srv_descriptor, defer);
	if (t->write_heap != GS_D3D12_HEAP_SRV)
		gs_d3d12_release_descriptor(t->write_heap, &t->write_descriptor, defer);
	if (t->uav_descriptor.index != GS_D3D12_DESCRIPTOR_INVALID)
		gs_d3d12_release_descriptor(GS_D3D12_HEAP_SRV, &t->uav_descriptor, defer);

	/* The span goes back when the command list retires, not now, either
	 * way: handed back here, the next resource could be placed in memory
	 * a command list still refers to, and the two would alias (an RTV
	 * made for this texture resolving to whatever was placed over it).
	 * At teardown the lists are dropped and the heap shut down straight
	 * after, so nothing is lost by it. */
	if (t->resource)
	{
		gs_d3d12_device_defer_resource(&t->alloc, t->resource);
		ID3D12Resource_Release(t->resource);
		t->resource = NULL;
	}
	memset(&t->alloc, 0, sizeof(t->alloc));
	t->write_heap = GS_D3D12_HEAP_SRV;
}

static int gs_texture_d3d12_srv(ID3D12Resource* resource, u32 levels, DXGI_FORMAT format, gs_d3d12_descriptor_handle* dh)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC desc;
	if (!gs_d3d12_descriptor_heap_manager_allocate(gs_d3d12_device_descriptor_heap(GS_D3D12_HEAP_SRV), dh))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate SRV descriptor\n");
		return 0;
	}
	memset(&desc, 0, sizeof(desc));
	desc.Format                  = format;
	desc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.Texture2D.MipLevels     = levels;
	ID3D12Device_CreateShaderResourceView(gs_d3d12_device_device(), resource, &desc, gs_d3d12_cpu(dh));
	return 1;
}

static int gs_texture_d3d12_rtv(ID3D12Resource* resource, DXGI_FORMAT format, gs_d3d12_descriptor_handle* dh)
{
	D3D12_RENDER_TARGET_VIEW_DESC desc;
	if (!gs_d3d12_descriptor_heap_manager_allocate(gs_d3d12_device_descriptor_heap(GS_D3D12_HEAP_RTV), dh))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate RTV descriptor\n");
		return 0;
	}
	memset(&desc, 0, sizeof(desc));
	desc.Format        = format;
	desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	ID3D12Device_CreateRenderTargetView(gs_d3d12_device_device(), resource, &desc, gs_d3d12_cpu(dh));
	return 1;
}

static int gs_texture_d3d12_dsv(ID3D12Resource* resource, DXGI_FORMAT format, gs_d3d12_descriptor_handle* dh)
{
	D3D12_DEPTH_STENCIL_VIEW_DESC desc;
	if (!gs_d3d12_descriptor_heap_manager_allocate(gs_d3d12_device_descriptor_heap(GS_D3D12_HEAP_DSV), dh))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate DSV descriptor\n");
		return 0;
	}
	memset(&desc, 0, sizeof(desc));
	desc.Format        = format;
	desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	desc.Flags         = D3D12_DSV_FLAG_NONE;
	ID3D12Device_CreateDepthStencilView(gs_d3d12_device_device(), resource, &desc, gs_d3d12_cpu(dh));
	return 1;
}

static int gs_texture_d3d12_uav(ID3D12Resource* resource, DXGI_FORMAT format, gs_d3d12_descriptor_handle* dh)
{
	D3D12_UNORDERED_ACCESS_VIEW_DESC desc;
	if (!gs_d3d12_descriptor_heap_manager_allocate(gs_d3d12_device_descriptor_heap(GS_D3D12_HEAP_SRV), dh))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate UAV descriptor\n");
		return 0;
	}
	memset(&desc, 0, sizeof(desc));
	desc.Format        = format;
	desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	ID3D12Device_CreateUnorderedAccessView(gs_d3d12_device_device(), resource, NULL, &desc, gs_d3d12_cpu(dh));
	return 1;
}

gs_texture_d3d12* gs_texture_d3d12_create(unsigned type, unsigned format, int width, int height, int levels,
	DXGI_FORMAT dxgi_format, DXGI_FORMAT srv_format, DXGI_FORMAT rtv_format, DXGI_FORMAT dsv_format,
	DXGI_FORMAT uav_format)
{
	D3D12_RESOURCE_DESC desc;
	D3D12_CLEAR_VALUE clear;
	D3D12_RESOURCE_STATES state;
	ID3D12Resource* resource = NULL;
	gs_d3d12_alloc_t alloc;
	gs_d3d12_descriptor_handle srv, write, uav;
	u32 write_heap = GS_D3D12_HEAP_SRV;
	gs_texture_d3d12* t;
	int ok = 1;

	memset(&desc, 0, sizeof(desc));
	memset(&clear, 0, sizeof(clear));
	desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width            = (UINT64)width;
	desc.Height           = (UINT)height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels        = (UINT16)levels;
	desc.Format           = dxgi_format;
	desc.SampleDesc.Count = 1;
	desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	switch (type)
	{
		case GS_TEXTYPE_TEXTURE:
			/* Mipmaps are drawn, so a texture that has them is a render
			 * target too; a compressed one never generates any. */
			desc.Flags = (levels > 1 && !gs_texture_is_compressed(format)) ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
				: D3D12_RESOURCE_FLAG_NONE;
			state = D3D12_RESOURCE_STATE_COPY_DEST;
			break;
		case GS_TEXTYPE_RENDER_TARGET:
			/* Placed in a heap the allocator already has: an offset, not a
			 * driver heap per target. */
			desc.Flags   = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			clear.Format = rtv_format;
			state        = D3D12_RESOURCE_STATE_RENDER_TARGET;
			break;
		case GS_TEXTYPE_DEPTH_STENCIL:
			desc.Flags   = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
			clear.Format = dsv_format;
			state        = D3D12_RESOURCE_STATE_DEPTH_WRITE;
			break;
		case GS_TEXTYPE_RW_TEXTURE:
			state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			break;
		default:
			return NULL;
	}

	if (uav_format != DXGI_FORMAT_UNKNOWN)
		desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	memset(&alloc, 0, sizeof(alloc));
	if (!gs_d3d12_device_create_placed_resource(&desc, D3D12_HEAP_TYPE_DEFAULT, state,
			(type == GS_TEXTYPE_RENDER_TARGET || type == GS_TEXTYPE_DEPTH_STENCIL) ? &clear : NULL, &resource, &alloc))
		return NULL;

	gs_d3d12_descriptor_handle_clear(&srv);
	gs_d3d12_descriptor_handle_clear(&write);
	gs_d3d12_descriptor_handle_clear(&uav);

	if (srv_format != DXGI_FORMAT_UNKNOWN)
		ok = gs_texture_d3d12_srv(resource, (u32)levels, srv_format, &srv);
	if (ok && type == GS_TEXTYPE_RENDER_TARGET)
	{
		write_heap = GS_D3D12_HEAP_RTV;
		ok         = gs_texture_d3d12_rtv(resource, rtv_format, &write);
	}
	else if (ok && type == GS_TEXTYPE_DEPTH_STENCIL)
	{
		write_heap = GS_D3D12_HEAP_DSV;
		ok         = gs_texture_d3d12_dsv(resource, dsv_format, &write);
	}
	if (ok && uav_format != DXGI_FORMAT_UNKNOWN)
		ok = gs_texture_d3d12_uav(resource, uav_format, &uav);

	t = ok ? (gs_texture_d3d12*)calloc(1, sizeof(*t)) : NULL;
	if (!t)
	{
		/* Each descriptor back to the heap it came from. The resource is
		 * named in an aliasing barrier on this frame's init list already,
		 * so it and its span go back when that list has run. */
		gs_d3d12_release_descriptor(GS_D3D12_HEAP_SRV, &srv, 0);
		gs_d3d12_release_descriptor(GS_D3D12_HEAP_SRV, &uav, 0);
		if (write_heap != GS_D3D12_HEAP_SRV)
			gs_d3d12_release_descriptor(write_heap, &write, 0);
		gs_d3d12_device_defer_resource(&alloc, resource);
		ID3D12Resource_Release(resource);
		return NULL;
	}

	gs_texture_base_init(&t->base, type, format, width, height);
	t->base.m_ops           = &s_d3d12_texture_ops;
	t->base.m_mipmap_levels = levels;
	t->resource             = resource;
	t->alloc                = alloc;
	t->srv_descriptor       = srv;
	t->write_descriptor     = write;
	t->uav_descriptor       = uav;
	t->write_heap           = write_heap;
	t->dxgi_format          = dxgi_format;
	t->resource_state       = state;
	t->map_level            = 0x7FFFFFFF;
	return t;
}

void gs_texture_d3d12_transition_level(const gs_texture_d3d12* t, ID3D12GraphicsCommandList* cmdlist, int level,
	D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource   = t->resource;
	barrier.Transition.Subresource = (UINT)level;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter  = after;
	ID3D12GraphicsCommandList_ResourceBarrier(cmdlist, 1, &barrier);
}

void gs_texture_d3d12_transition(gs_texture_d3d12* t, ID3D12GraphicsCommandList* cmdlist, D3D12_RESOURCE_STATES state)
{
	if (t->resource_state == state)
		return;
	gs_texture_d3d12_transition_level(t, cmdlist, (int)D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, t->resource_state, state);
	t->resource_state = state;
}

void gs_texture_d3d12_commit_clear(gs_texture_d3d12* t, ID3D12GraphicsCommandList* cmdlist)
{
	if (!cmdlist)
	{
		if (t->base.m_state != GS_TEXSTATE_CLEARED)
			return;
		gs_d3d12_device_end_render_pass();
		cmdlist = gs_d3d12_device_command_list();
	}

	if (t->base.m_type == GS_TEXTYPE_DEPTH_STENCIL)
	{
		gs_texture_d3d12_transition(t, cmdlist, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		ID3D12GraphicsCommandList_ClearDepthStencilView(cmdlist, gs_d3d12_cpu(&t->write_descriptor),
			D3D12_CLEAR_FLAG_DEPTH, t->base.m_clear_value.depth, 0, 0, NULL);
	}
	else
	{
		const u32 c = t->base.m_clear_value.color;
		float rgba[4];
		rgba[0] = (float)(c & 0xFFu) * (1.0f / 255.0f);
		rgba[1] = (float)((c >> 8) & 0xFFu) * (1.0f / 255.0f);
		rgba[2] = (float)((c >> 16) & 0xFFu) * (1.0f / 255.0f);
		rgba[3] = (float)(c >> 24) * (1.0f / 255.0f);
		gs_texture_d3d12_transition(t, cmdlist, D3D12_RESOURCE_STATE_RENDER_TARGET);
		ID3D12GraphicsCommandList_ClearRenderTargetView(cmdlist, gs_d3d12_cpu(&t->write_descriptor), rgba, 0, NULL);
	}
	t->base.m_state = GS_TEXSTATE_DIRTY;
}

/* The command list an upload goes in: the init list, run before this
 * frame's, unless the texture is a target or this frame used it */
static ID3D12GraphicsCommandList* gs_texture_d3d12_update_list(gs_texture_d3d12* t)
{
	if (t->base.m_type != GS_TEXTYPE_TEXTURE || t->use_fence_counter == gs_d3d12_device_current_fence())
	{
		gs_d3d12_device_end_render_pass();
		return gs_d3d12_device_command_list();
	}
	return gs_d3d12_device_init_command_list();
}

static void gs_texture_d3d12_copy_rows(const gs_texture_d3d12* t, void* dst, const void* src, u32 pitch, u32 upload_pitch,
	u32 row_bytes, u32 height)
{
	const u32 block = gs_texture_block_size(t->base.m_format);
	gs_stride_memcpy(dst, upload_pitch, src, pitch, row_bytes, (height + (block - 1)) / block);
}

/* A staging buffer of its own for an upload too big for the stream
 * buffer, filled, and already queued to be freed after the list */
static ID3D12Resource* gs_texture_d3d12_staging(const gs_texture_d3d12* t, const void* data, u32 pitch, u32 upload_pitch,
	u32 row_bytes, u32 height)
{
	const u32 size = gs_texture_upload_size(t->base.m_format, height, upload_pitch);
	D3D12_RESOURCE_DESC desc;
	D3D12_RANGE written;
	ID3D12Resource* resource = NULL;
	gs_d3d12_alloc_t alloc;
	void* mapped;
	HRESULT hr;

	memset(&desc, 0, sizeof(desc));
	desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width            = size;
	desc.Height           = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels        = 1;
	desc.Format           = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags            = D3D12_RESOURCE_FLAG_NONE;

	memset(&alloc, 0, sizeof(alloc));
	if (!gs_d3d12_device_create_placed_resource(&desc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
			&resource, &alloc))
	{
		log_cb(RETRO_LOG_INFO, "(AllocateUploadStagingBuffer) no room for a %u byte staging buffer\n", size);
		return NULL;
	}

	hr = ID3D12Resource_Map(resource, 0, NULL, &mapped);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_INFO, "(AllocateUploadStagingBuffer) Map() failed with %08X\n", (unsigned)hr);
		gs_d3d12_device_defer_resource(&alloc, resource);
		ID3D12Resource_Release(resource);
		return NULL;
	}

	gs_texture_d3d12_copy_rows(t, mapped, data, pitch, upload_pitch, row_bytes, height);
	written.Begin = 0;
	written.End   = size;
	ID3D12Resource_Unmap(resource, 0, &written);

	/* Freed after the list: the deferral holds the reference the copy
	 * needs. */
	gs_d3d12_device_defer_resource(&alloc, resource);
	ID3D12Resource_Release(resource);
	return resource;
}

/* Room for @size in the texture stream buffer, the list executed once to
 * make it if it has to be */
static int gs_texture_d3d12_reserve(struct gs_d3d12_stream_buffer* sb, u32 size)
{
	if (gs_d3d12_stream_buffer_reserve(sb, size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT))
		return 1;
	gs_d3d12_device_execute(0);
	if (gs_d3d12_stream_buffer_reserve(sb, size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT))
		return 1;
	gs_d3d12_device_report_recurring("Reserving texture upload memory", E_OUTOFMEMORY);
	return 0;
}

/* The copy of an upload recorded: @src at @x,@y of @layer, the texture
 * moved to COPY_DEST and back, a pending clear committed unless the
 * upload covers the whole target. */
static void gs_texture_d3d12_record_upload(gs_texture_d3d12* t, int layer, const gs_texture_rect* covered, u32 x, u32 y,
	const D3D12_TEXTURE_COPY_LOCATION* src, u32 width, u32 height)
{
	ID3D12GraphicsCommandList* cmdlist = gs_texture_d3d12_update_list(t);
	D3D12_TEXTURE_COPY_LOCATION dst;
	D3D12_BOX box;

	/* first use? not left undefined */
	if (t->resource_state == D3D12_RESOURCE_STATE_COMMON)
		gs_texture_d3d12_transition(t, cmdlist, D3D12_RESOURCE_STATE_COPY_DEST);
	else if (t->resource_state != D3D12_RESOURCE_STATE_COPY_DEST)
		gs_texture_d3d12_transition_level(t, cmdlist, layer, t->resource_state, D3D12_RESOURCE_STATE_COPY_DEST);

	if (t->base.m_type == GS_TEXTYPE_RENDER_TARGET)
	{
		if (covered->left != 0 || covered->top != 0 || covered->right != t->base.m_size.x
				|| covered->bottom != t->base.m_size.y)
			gs_texture_d3d12_commit_clear(t, cmdlist);
		else
			t->base.m_state = GS_TEXSTATE_DIRTY;
	}

	memset(&dst, 0, sizeof(dst));
	dst.pResource        = t->resource;
	dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = (UINT)layer;

	box.left   = 0;
	box.top    = 0;
	box.front  = 0;
	box.right  = width;
	box.bottom = height;
	box.back   = 1;
	ID3D12GraphicsCommandList_CopyTextureRegion(cmdlist, &dst, x, y, 0, src, &box);

	if (t->resource_state != D3D12_RESOURCE_STATE_COPY_DEST)
		gs_texture_d3d12_transition_level(t, cmdlist, layer, D3D12_RESOURCE_STATE_COPY_DEST, t->resource_state);

	if (t->base.m_type == GS_TEXTYPE_TEXTURE)
		t->base.m_needs_mipmaps_generated |= (layer == 0);
}

static int gs_texture_d3d12_update(gs_texture* base, const gs_texture_rect* r, const void* data, int pitch, int layer)
{
	gs_texture_d3d12* t = (gs_texture_d3d12*)base;
	struct gs_d3d12_stream_buffer* sb = gs_d3d12_device_texture_stream_buffer();
	D3D12_TEXTURE_COPY_LOCATION src;
	u32 block, width, height, rect_bytes, row_bytes, upload_pitch, required_size;

	if (layer >= base->m_mipmap_levels)
		return 0;

	/* footprint and box block aligned for compressed textures */
	block         = gs_texture_block_size(base->m_format);
	width         = pcsx2_align_up_pow2_u32((u32)(r->right - r->left), block);
	height        = pcsx2_align_up_pow2_u32((u32)(r->bottom - r->top), block);
	/* Only the rect's texels reach the texture: rows wider than it (a
	 * region of a larger decode) are not staged whole. */
	rect_bytes    = gs_texture_upload_pitch(base->m_format, width);
	row_bytes     = ((u32)pitch < rect_bytes) ? (u32)pitch : rect_bytes;
	upload_pitch  = pcsx2_align_up_pow2_u32(row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	required_size = gs_texture_upload_size(base->m_format, (u32)(r->bottom - r->top), upload_pitch);

	memset(&src, 0, sizeof(src));
	src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Footprint.Width    = width;
	src.PlacedFootprint.Footprint.Height   = height;
	src.PlacedFootprint.Footprint.Depth    = 1;
	src.PlacedFootprint.Footprint.Format   = t->dxgi_format;
	src.PlacedFootprint.Footprint.RowPitch = upload_pitch;

	/* More than half the stream buffer: a buffer of its own, rather than
	 * a failed reservation or a string of submissions. */
	if (required_size > sb->size / 2)
	{
		src.pResource = gs_texture_d3d12_staging(t, data, (u32)pitch, upload_pitch, row_bytes, height);
		if (!src.pResource)
			return 0;
		src.PlacedFootprint.Offset = 0;
	}
	else
	{
		if (!gs_texture_d3d12_reserve(sb, required_size))
			return 0;
		src.pResource              = (ID3D12Resource*)sb->buffer;
		src.PlacedFootprint.Offset = sb->current_offset;
		gs_texture_d3d12_copy_rows(t, sb->host_pointer + sb->current_offset, data, (u32)pitch, upload_pitch, row_bytes,
			height);
		gs_d3d12_stream_buffer_commit(sb, required_size);
	}

	gs_texture_d3d12_record_upload(t, layer, r, pcsx2_align_down_pow2_u32((u32)r->left, block),
		pcsx2_align_down_pow2_u32((u32)r->top, block), &src, width, height);
	return 1;
}

static int gs_texture_d3d12_map(gs_texture* base, gs_texture_map* m, const gs_texture_rect* r, int layer)
{
	gs_texture_d3d12* t = (gs_texture_d3d12*)base;
	struct gs_d3d12_stream_buffer* sb = gs_d3d12_device_texture_stream_buffer();
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

	m->pitch = (int)pcsx2_align_up_pow2_u32(
		gs_texture_upload_pitch(base->m_format, (u32)(t->map_area.right - t->map_area.left)),
		D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

	/* (as Update: a map bigger than half the buffer is not had) */
	required_size = gs_texture_upload_size(base->m_format, (u32)(t->map_area.bottom - t->map_area.top), (u32)m->pitch);
	if (required_size >= sb->size / 2)
		return 0;
	/* (nothing reserved at the host pointer: the caller falls back to
	 * Update rather than writing there) */
	if (!gs_texture_d3d12_reserve(sb, required_size))
		return 0;

	m->bits = sb->host_pointer + sb->current_offset;
	return 1;
}

static void gs_texture_d3d12_unmap(gs_texture* base)
{
	gs_texture_d3d12* t = (gs_texture_d3d12*)base;
	struct gs_d3d12_stream_buffer* sb = gs_d3d12_device_texture_stream_buffer();
	const u32 width  = (u32)(t->map_area.right - t->map_area.left);
	const u32 height = (u32)(t->map_area.bottom - t->map_area.top);
	const u32 pitch  = pcsx2_align_up_pow2_u32(gs_texture_upload_pitch(base->m_format, width),
		D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	const u32 required_size = gs_texture_upload_size(base->m_format, height, pitch);
	const u32 buffer_offset = sb->current_offset;
	D3D12_TEXTURE_COPY_LOCATION src;

	gs_d3d12_stream_buffer_commit(sb, required_size);

	memset(&src, 0, sizeof(src));
	src.pResource                          = (ID3D12Resource*)sb->buffer;
	src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Offset             = buffer_offset;
	src.PlacedFootprint.Footprint.Width    = width;
	src.PlacedFootprint.Footprint.Height   = height;
	src.PlacedFootprint.Footprint.Depth    = 1;
	src.PlacedFootprint.Footprint.Format   = t->dxgi_format;
	src.PlacedFootprint.Footprint.RowPitch = pitch;

	gs_texture_d3d12_record_upload(t, t->map_level, &t->map_area, (u32)t->map_area.left, (u32)t->map_area.top, &src,
		width, height);
}

static void gs_texture_d3d12_generate_mipmap(gs_texture* base)
{
	gs_texture_d3d12* t = (gs_texture_d3d12*)base;
	int dst_level;

	for (dst_level = 1; dst_level < base->m_mipmap_levels; dst_level++)
	{
		const int src_level  = dst_level - 1;
		const int src_width  = pcsx2_max_i(base->m_size.x >> src_level, 1);
		const int src_height = pcsx2_max_i(base->m_size.y >> src_level, 1);
		const int dst_width  = pcsx2_max_i(base->m_size.x >> dst_level, 1);
		const int dst_height = pcsx2_max_i(base->m_size.y >> dst_level, 1);

		gs_d3d12_device_render_mipmap(t, (u32)dst_level, (u32)dst_width, (u32)dst_height, (u32)src_level,
			(u32)src_width, (u32)src_height);
	}
	t->use_fence_counter = gs_d3d12_device_current_fence();
}

/* --- read back --- */

static void gs_download_texture_d3d12_unmap(gs_download_texture* base)
{
	gs_download_texture_d3d12* t = (gs_download_texture_d3d12*)base;
	D3D12_RANGE written;

	if (!base->m_map_pointer)
		return;
	written.Begin = 0;
	written.End   = 0;
	ID3D12Resource_Unmap(t->buffer, 0, &written);
	base->m_map_pointer = NULL;
}

static void gs_download_texture_d3d12_free(gs_download_texture* base)
{
	gs_download_texture_d3d12* t = (gs_download_texture_d3d12*)base;
	gs_download_texture_d3d12_unmap(base);
	if (t->buffer)
	{
		gs_d3d12_device_defer_resource(&t->alloc, t->buffer);
		ID3D12Resource_Release(t->buffer);
	}
	free(t);
}

static void gs_download_texture_d3d12_copy(gs_download_texture* base, const gs_texture_rect* drc, gs_texture* stex,
	const gs_texture_rect* src_rc, u32 src_level, int use_transfer_pitch)
{
	gs_download_texture_d3d12* t = (gs_download_texture_d3d12*)base;
	gs_texture_d3d12* st          = (gs_texture_d3d12*)stex;
	D3D12_TEXTURE_COPY_LOCATION src, dst;
	D3D12_RESOURCE_STATES old_state;
	ID3D12GraphicsCommandList* cmdlist;
	gs_texture_transfer tr;
	D3D12_BOX box;

	base->m_current_pitch = gs_texture_transfer_pitch(base->m_format,
		use_transfer_pitch ? (u32)(drc->right - drc->left) : base->m_width, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	tr = gs_texture_transfer_of(base->m_format, base->m_current_pitch, (u32)drc->left, (u32)drc->top,
		(u32)(drc->right - drc->left), (u32)(drc->bottom - drc->top));

	gs_d3d12_device_end_render_pass();
	gs_texture_d3d12_commit_clear(st, NULL);

	gs_download_texture_d3d12_unmap(base);

	cmdlist = gs_d3d12_device_command_list();

	memset(&src, 0, sizeof(src));
	src.pResource        = st->resource;
	src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = src_level;

	memset(&dst, 0, sizeof(dst));
	dst.pResource                          = t->buffer;
	dst.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Offset             = tr.offset;
	dst.PlacedFootprint.Footprint.Format   = st->dxgi_format;
	dst.PlacedFootprint.Footprint.Width    = (UINT)(drc->right - drc->left);
	dst.PlacedFootprint.Footprint.Height   = (UINT)(drc->bottom - drc->top);
	dst.PlacedFootprint.Footprint.Depth    = 1;
	dst.PlacedFootprint.Footprint.RowPitch = base->m_current_pitch;

	old_state = st->resource_state;
	if (old_state != D3D12_RESOURCE_STATE_COPY_SOURCE)
		gs_texture_d3d12_transition_level(st, cmdlist, (int)src_level, old_state, D3D12_RESOURCE_STATE_COPY_SOURCE);

	box.left   = (UINT)src_rc->left;
	box.top    = (UINT)src_rc->top;
	box.front  = 0;
	box.right  = (UINT)src_rc->right;
	box.bottom = (UINT)src_rc->bottom;
	box.back   = 1;
	ID3D12GraphicsCommandList_CopyTextureRegion(cmdlist, &dst, 0, 0, 0, &src, &box);

	if (old_state != D3D12_RESOURCE_STATE_COPY_SOURCE)
		gs_texture_d3d12_transition_level(st, cmdlist, (int)src_level, D3D12_RESOURCE_STATE_COPY_SOURCE, old_state);

	t->copy_fence_value = gs_d3d12_device_current_fence();
	base->m_needs_flush = 1;
}

static int gs_download_texture_d3d12_map(gs_download_texture* base, const gs_texture_rect* read_rc)
{
	gs_download_texture_d3d12* t = (gs_download_texture_d3d12*)base;
	gs_texture_transfer tr;
	D3D12_RANGE read;
	void* mapped = NULL;
	HRESULT hr;

	if (base->m_map_pointer)
		return 1;
	/* never written */
	if (!base->m_current_pitch)
		return 0;

	tr = gs_texture_transfer_of(base->m_format, base->m_current_pitch, (u32)read_rc->left, (u32)read_rc->top,
		(u32)(read_rc->right - read_rc->left), (u32)(read_rc->bottom - read_rc->top));
	read.Begin = tr.offset;
	read.End   = tr.offset + tr.size;
	hr         = ID3D12Resource_Map(t->buffer, 0, &read, &mapped);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "(GSDownloadTexture12::Map) Map() failed with HRESULT %08X\n", (unsigned)hr);
		return 0;
	}
	base->m_map_pointer = (const u8*)mapped;
	return 1;
}

static void gs_download_texture_d3d12_flush(gs_download_texture* base)
{
	gs_download_texture_d3d12* t = (gs_download_texture_d3d12*)base;

	if (!base->m_needs_flush)
		return;
	base->m_needs_flush = 0;

	if (gs_d3d12_device_completed_fence() >= t->copy_fence_value)
		return;
	if (gs_d3d12_device_current_fence() == t->copy_fence_value)
		gs_d3d12_device_execute_for_readback();
	else
		gs_d3d12_device_wait_fence(t->copy_fence_value);
}

static const gs_download_texture_ops s_d3d12_download_texture_ops = {
	gs_download_texture_d3d12_free, gs_download_texture_d3d12_copy, gs_download_texture_d3d12_map,
	gs_download_texture_d3d12_unmap, gs_download_texture_d3d12_flush};

gs_download_texture* gs_download_texture_d3d12_create(u32 width, u32 height, unsigned format)
{
	const u32 size = gs_texture_buffer_size(format, width, height, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	D3D12_RESOURCE_DESC desc;
	ID3D12Resource* buffer = NULL;
	gs_d3d12_alloc_t alloc;
	gs_download_texture_d3d12* t;

	memset(&desc, 0, sizeof(desc));
	desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width            = size;
	desc.Height           = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels        = 1;
	desc.Format           = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags            = D3D12_RESOURCE_FLAG_NONE;

	memset(&alloc, 0, sizeof(alloc));
	if (!gs_d3d12_device_create_placed_resource(&desc, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
			&buffer, &alloc))
	{
		log_cb(RETRO_LOG_ERROR, "(GSDownloadTexture12::Create) no room for a %u byte readback buffer\n", size);
		return NULL;
	}

	t = (gs_download_texture_d3d12*)calloc(1, sizeof(*t));
	if (!t)
	{
		gs_d3d12_device_defer_resource(&alloc, buffer);
		ID3D12Resource_Release(buffer);
		return NULL;
	}
	gs_download_texture_base_init(&t->base, width, height, format);
	t->base.m_ops   = &s_d3d12_download_texture_ops;
	t->alloc        = alloc;
	t->buffer       = buffer;
	t->buffer_size  = size;
	return &t->base;
}
