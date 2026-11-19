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

#ifndef GS_TEXTURE_12_H
#define GS_TEXTURE_12_H

#include "common/Pcsx2Defs.h"
#include "GS/Renderers/Common/GSTextureBase.h"
#include "GSD3D12Heap.h"
#include "D3D12DescriptorHeapManager.h"

#include <windows.h>
#include <d3d12.h>

/* Which descriptor heap a descriptor is in */
enum gs_d3d12_heap_kind
{
	GS_D3D12_HEAP_SRV = 0, /* the shader-visible one: SRVs and UAVs */
	GS_D3D12_HEAP_RTV,
	GS_D3D12_HEAP_DSV
};

typedef struct gs_texture_d3d12
{
	gs_texture base;
	ID3D12Resource* resource;
	gs_d3d12_alloc_t alloc;
	gs_d3d12_descriptor_handle srv_descriptor;
	gs_d3d12_descriptor_handle write_descriptor;
	gs_d3d12_descriptor_handle uav_descriptor;
	u32 write_heap; /* GS_D3D12_HEAP_RTV or _DSV, or none (_SRV) */
	DXGI_FORMAT dxgi_format;
	D3D12_RESOURCE_STATES resource_state;
	/* the fence value of the command list that last used it */
	u64 use_fence_counter;
	int map_level;
	gs_texture_rect map_area;
} gs_texture_d3d12;

typedef struct gs_download_texture_d3d12
{
	gs_download_texture base;
	gs_d3d12_alloc_t alloc;
	ID3D12Resource* buffer;
	u64 copy_fence_value;
	u32 buffer_size;
} gs_download_texture_d3d12;

#ifdef __cplusplus
extern "C" {
#endif

/* A new texture (type, format: gs_texture_type, gs_texture_format) with
 * the views asked for (DXGI_FORMAT_UNKNOWN: none), or NULL; freed
 * through its ops. */
gs_texture_d3d12* gs_texture_d3d12_create(unsigned type, unsigned format, int width, int height, int levels,
	DXGI_FORMAT dxgi_format, DXGI_FORMAT srv_format, DXGI_FORMAT rtv_format, DXGI_FORMAT dsv_format,
	DXGI_FORMAT uav_format);
gs_download_texture* gs_download_texture_d3d12_create(u32 width, u32 height, unsigned format);

/* The resource and descriptors released, the descriptors now or when the
 * command list is past them (@defer), the resource's memory always then;
 * the texture stays, empty. */
void gs_texture_d3d12_destroy(gs_texture_d3d12* t, int defer);

void gs_texture_d3d12_transition(gs_texture_d3d12* t, ID3D12GraphicsCommandList* cmdlist, D3D12_RESOURCE_STATES state);
void gs_texture_d3d12_transition_level(const gs_texture_d3d12* t, ID3D12GraphicsCommandList* cmdlist, int level,
	D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
/* A pending clear recorded: in @cmdlist, or (NULL) in the device's after
 * ending its render pass, and then only if one is pending. */
void gs_texture_d3d12_commit_clear(gs_texture_d3d12* t, ID3D12GraphicsCommandList* cmdlist);

/* What the textures ask of the device (GSDevice12.cpp) */
ID3D12Device* gs_d3d12_device_device(void);
int gs_d3d12_device_create_placed_resource(const D3D12_RESOURCE_DESC* desc, D3D12_HEAP_TYPE heap_type,
	D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear_value, ID3D12Resource** resource,
	gs_d3d12_alloc_t* alloc);
/* (takes a reference: the caller's own stays the caller's) */
void gs_d3d12_device_defer_resource(const gs_d3d12_alloc_t* alloc, ID3D12Resource* resource);
gs_d3d12_descriptor_heap_manager* gs_d3d12_device_descriptor_heap(u32 kind);
void gs_d3d12_device_defer_descriptor(u32 kind, gs_d3d12_descriptor_handle* handle);
void gs_d3d12_device_unbind_texture(gs_texture_d3d12* t);
ID3D12GraphicsCommandList* gs_d3d12_device_command_list(void);
ID3D12GraphicsCommandList* gs_d3d12_device_init_command_list(void);
void gs_d3d12_device_end_render_pass(void);
void gs_d3d12_device_execute(int wait);
void gs_d3d12_device_execute_for_readback(void);
struct gs_d3d12_stream_buffer* gs_d3d12_device_texture_stream_buffer(void);
void gs_d3d12_device_report_recurring(const char* what, HRESULT hr);
void gs_d3d12_device_render_mipmap(gs_texture_d3d12* t, u32 dst_level, u32 dst_width, u32 dst_height, u32 src_level,
	u32 src_width, u32 src_height);

#ifdef __cplusplus
}
#endif

#endif
