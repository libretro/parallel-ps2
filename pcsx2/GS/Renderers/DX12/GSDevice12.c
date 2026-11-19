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
#define CINTERFACE
/* (the methods that return a struct, as functions that do it right) */
#define WIDL_C_INLINE_WRAPPERS

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <libretro_d3d.h>
#include <file/file_path.h>
#include <memalign.h>

#include "common/Pcsx2Defs.h"
#include "Config.h"
#include "ShaderCacheVersion.h"
#include "GS.h"
#include "GS/GSBackend.h"
#include "GS/GSLibretroHooks.h"
#include "GS/GSRegs.h"
#include "GS/Renderers/Common/GSKeyHash.h"
#include "D3D12Builders.h"
#include "GSDevice12.h"

extern retro_environment_t environ_cb;

/* The interfaces asked for (here rather than from dxguid, which is not
 * linked) */
static const IID s_iid_descriptor_heap = {0x8efb471d, 0x616c, 0x4f49, {0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51}};
static const IID s_iid_command_allocator = {0x6102dee4, 0xaf59, 0x4b09, {0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24}};
static const IID s_iid_fence = {0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}};
static const IID s_iid_info_queue = {0x0742a90b, 0xc387, 0x483f, {0xb9, 0x46, 0x30, 0xa7, 0xe4, 0xe6, 0x14, 0x58}};
static const IID s_iid_resource = {0x696442be, 0xa72e, 0x4059, {0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad}};
static const IID s_iid_pipeline_state = {0x765a30f3, 0xf624, 0x4c6f, {0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45}};
static const IID s_iid_heap = {0x6b3b2502, 0x6e51, 0x45b3, {0x90, 0xee, 0x98, 0x84, 0x26, 0x5e, 0x8d, 0xf3}};
static const IID s_iid_root_signature = {0xc54a6b66, 0x72df, 0x4ee8, {0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14}};
static const IID s_iid_dxgi_adapter = {0x2411e7e1, 0x12ac, 0x4ccf, {0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0}};
static const IID s_iid_command_list4 = {0x8754318e, 0xd3a9, 0x4541, {0x98, 0xcf, 0x64, 0x5b, 0x50, 0xdc, 0x48, 0x74}};

#define D12_DEV(d) ((gs_device_d3d12*)(d))
#define D12_TEX(t) ((gs_texture_d3d12*)(t))
#define D12_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define D12_RELEASE(p)                        \
	do                                        \
	{                                         \
		if (p)                                \
			IUnknown_Release((IUnknown*)(p)); \
		(p) = NULL;                           \
	} while (0)

/* Pixel shaders and pipelines the maps hold before they grow, and the
 * resources and descriptors a command list holds */
#define D12_PS_RESERVE 2048
#define D12_PIPELINES_RESERVE 4096
#define D12_PENDING_RESERVE 1024

/* Queued pipelines the warm-up thread has not handed back, at most */
#define D3D12_TFX_QUEUED 64u

/* Bumped with the shaders, and whenever what a selector stands for
 * changes; the record size is checked as well. */
#define TFX_LIST_VERSION ((SHADER_CACHE_VERSION << 8) | 2u)

/* The bits of the dirty flags */
#define DIRTY_FLAG_VS_CONSTANT_BUFFER (1u << 0)
#define DIRTY_FLAG_PS_CONSTANT_BUFFER (1u << 1)
#define DIRTY_FLAG_TFX_TEXTURES (1u << 2)
#define DIRTY_FLAG_TFX_SAMPLERS (1u << 3)
#define DIRTY_FLAG_TFX_RT_TEXTURES (1u << 4)
#define DIRTY_FLAG_VS_CONSTANT_BUFFER_BINDING (1u << 5)
#define DIRTY_FLAG_PS_CONSTANT_BUFFER_BINDING (1u << 6)
#define DIRTY_FLAG_VS_VERTEX_BUFFER_BINDING (1u << 7)
#define DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE (1u << 8)
#define DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE (1u << 9)
#define DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE_2 (1u << 10)
#define DIRTY_FLAG_VERTEX_BUFFER (1u << 11)
#define DIRTY_FLAG_INDEX_BUFFER (1u << 12)
#define DIRTY_FLAG_PRIMITIVE_TOPOLOGY (1u << 13)
#define DIRTY_FLAG_VIEWPORT (1u << 14)
#define DIRTY_FLAG_SCISSOR (1u << 15)
#define DIRTY_FLAG_RENDER_TARGET (1u << 16)
#define DIRTY_FLAG_PIPELINE (1u << 17)
#define DIRTY_FLAG_BLEND_CONSTANTS (1u << 18)
#define DIRTY_FLAG_STENCIL_REF (1u << 19)

#define DIRTY_BASE_STATE                                                                                             \
	(DIRTY_FLAG_VS_CONSTANT_BUFFER_BINDING | DIRTY_FLAG_PS_CONSTANT_BUFFER_BINDING |                                 \
		DIRTY_FLAG_VS_VERTEX_BUFFER_BINDING | DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE |                                 \
		DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE | DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE_2 | DIRTY_FLAG_VERTEX_BUFFER |   \
		DIRTY_FLAG_INDEX_BUFFER | DIRTY_FLAG_PRIMITIVE_TOPOLOGY | DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR |         \
		DIRTY_FLAG_RENDER_TARGET | DIRTY_FLAG_PIPELINE | DIRTY_FLAG_BLEND_CONSTANTS | DIRTY_FLAG_STENCIL_REF)
#define DIRTY_TFX_STATE \
	(DIRTY_BASE_STATE | DIRTY_FLAG_TFX_TEXTURES | DIRTY_FLAG_TFX_SAMPLERS | DIRTY_FLAG_TFX_RT_TEXTURES)
#define DIRTY_UTILITY_STATE DIRTY_BASE_STATE
#define DIRTY_CONSTANT_BUFFER_STATE (DIRTY_FLAG_VS_CONSTANT_BUFFER | DIRTY_FLAG_PS_CONSTANT_BUFFER)

/* (the C names of D3D12's handles: the same layout) */
static D3D12_CPU_DESCRIPTOR_HANDLE d12_cpu(const gs_d3d12_descriptor_handle* h)
{
	D3D12_CPU_DESCRIPTOR_HANDLE r;
	r.ptr = h->cpu_handle.ptr;
	return r;
}

static D3D12_GPU_DESCRIPTOR_HANDLE d12_gpu(const gs_d3d12_descriptor_handle* h)
{
	D3D12_GPU_DESCRIPTOR_HANDLE r;
	r.ptr = h->gpu_handle.ptr;
	return r;
}

#define D12_HANDLE_VALID(h) ((h).index != GS_D3D12_DESCRIPTOR_INVALID)
#define D12_HANDLE_EQ(a, b) ((a).index == (b).index)

/* The vertex of the utility passes (C++: GSVertexPT1) */
typedef struct PCSX2_ALIGN(32) d12_vertex_pt1
{
	float p[4];
	float t[2];
	char pad[4];
	u32 c;
} d12_vertex_pt1;

/* (MergeConstantBuffer) */
typedef struct PCSX2_ALIGN(16) d12_merge_cb
{
	float bg_color[4];
	u32 emoda;
	u32 emodc;
	u32 doffset;
	float scale_factor;
} d12_merge_cb;

/* The interface the frontend handed over */
static struct retro_hw_render_interface_d3d12* d3d12;

/* --- libretro_d3d12.h version 2 ------------------------------------------
 *
 * Version 1 hands the frontend the one texture the GS merges into, which
 * it draws into again next frame; the frontend has to copy it before
 * video_refresh returns, and one that presents from another thread has to
 * record and submit that copy on this thread. With version 2 the core
 * keeps a present texture per sync index, says when each is complete (the
 * fence its own submit signals), and is told when it may have it back.
 * The present texture is the device's own target - the merge, a
 * deinterlacer's output, the VR eyes - and the target and the index's
 * texture change places, so nothing is copied anywhere, nothing extra is
 * submitted and the frontend records nothing here. */

#define GS_D3D12_MAX_SYNC_INDICES 32

static struct retro_hw_render_context_negotiation_interface_d3d12 s_d3d12_negotiation;
static gs_texture_d3d12* s_d3d12_present_textures[GS_D3D12_MAX_SYNC_INDICES];

/* a utility pipeline failed at first use: the next device builds them at its creation */
static bool s_utility_build_failed;

void gs_d3d12_negotiate_hw_interface(retro_environment_t cb)
{
	struct retro_hw_render_context_negotiation_interface probe;

	if (!cb)
		return;

	/* A frontend that does not know the type answers version 0, or does
	 * not answer; then nothing has been asked for and version 1 is what
	 * GET_HW_RENDER_INTERFACE returns, as it always has. */
	probe.interface_type    = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D12;
	probe.interface_version = 0;
	if (!cb(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT, &probe)
		|| probe.interface_version < RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D12_VERSION)
		return;

	s_d3d12_negotiation.interface_type               = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D12;
	s_d3d12_negotiation.interface_version            = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D12_VERSION;
	s_d3d12_negotiation.max_render_interface_version = RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2;
	cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, &s_d3d12_negotiation);
}

static void gs_d3d12_free_present_textures(void)
{
	unsigned i;
	for (i = 0; i < GS_D3D12_MAX_SYNC_INDICES; i++)
	{
		if (s_d3d12_present_textures[i])
			s_d3d12_present_textures[i]->base.m_ops->free(&s_d3d12_present_textures[i]->base);
		s_d3d12_present_textures[i] = NULL;
	}
}

/* ==== d3d12.dll ==== */

static HMODULE s_d3d12_library;
static PFN_D3D12_CREATE_DEVICE s_d3d12_create_device;
static PFN_D3D12_GET_DEBUG_INTERFACE s_d3d12_get_debug_interface;
static PFN_D3D12_SERIALIZE_ROOT_SIGNATURE s_d3d12_serialize_root_signature;

static bool d12_load_library(void)
{
	if ((s_d3d12_library = LoadLibraryW(L"d3d12.dll")) == NULL
		|| (s_d3d12_create_device = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(s_d3d12_library, "D3D12CreateDevice"))
			   == NULL
		|| (s_d3d12_get_debug_interface =
				   (PFN_D3D12_GET_DEBUG_INTERFACE)GetProcAddress(s_d3d12_library, "D3D12GetDebugInterface"))
			   == NULL
		|| (s_d3d12_serialize_root_signature =
				   (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)GetProcAddress(s_d3d12_library, "D3D12SerializeRootSignature"))
			   == NULL)
	{
		log_cb(RETRO_LOG_ERROR, "d3d12.dll could not be loaded.\n");
		s_d3d12_create_device            = NULL;
		s_d3d12_get_debug_interface      = NULL;
		s_d3d12_serialize_root_signature = NULL;
		if (s_d3d12_library)
			FreeLibrary(s_d3d12_library);
		s_d3d12_library = NULL;
		return false;
	}

	return true;
}

static void d12_unload_library(void)
{
	s_d3d12_serialize_root_signature = NULL;
	s_d3d12_get_debug_interface      = NULL;
	s_d3d12_create_device            = NULL;
	if (s_d3d12_library)
	{
		FreeLibrary(s_d3d12_library);
		s_d3d12_library = NULL;
	}
}

/* ==== forward ==== */

static void d12_end_render_pass(gs_device_d3d12* dev);
static void d12_execute_command_list(gs_device_d3d12* dev, bool wait_for_completion);
static void d12_execute_command_list_and_restart_render_pass(gs_device_d3d12* dev, bool wait_for_completion);
static void d12_wait_for_fence(gs_device_d3d12* dev, u64 fence);
static void d12_defer_resource(gs_device_d3d12* dev, const gs_d3d12_alloc_t* alloc, ID3D12DeviceChild* resource);
static void d12_defer_descriptor(gs_device_d3d12* dev, gs_d3d12_descriptor_heap_manager* manager,
	gs_d3d12_descriptor_handle* handle);
static void d12_unbind_texture(gs_device_d3d12* dev, gs_texture_d3d12* tex);
static void d12_render_texture_mipmap(gs_device_d3d12* dev, gs_texture_d3d12* texture, u32 dst_level, u32 dst_width,
	u32 dst_height, u32 src_level, u32 src_width, u32 src_height);
static bool d12_create_placed_resource(gs_device_d3d12* dev, const D3D12_RESOURCE_DESC* desc,
	D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear_value,
	ID3D12Resource** out_resource, gs_d3d12_alloc_t* out_alloc);
static ID3D12GraphicsCommandList4* d12_init_command_list(gs_device_d3d12* dev);
static void d12_report_recurring(const char* what, HRESULT hr);

static ID3D12GraphicsCommandList4* d12_cmdlist(gs_device_d3d12* dev)
{
	return dev->command_lists[dev->current_command_list].command_lists[1];
}

/* ==== what the C stream buffers, descriptor heaps and textures ask of
 * the device ==== */

int gs_d3d12_device_create_upload_buffer(u32 size, void** resource, gs_d3d12_alloc_t* alloc, u8** host_pointer,
	u64* gpu_pointer)
{
	gs_device_d3d12* const dev = D12_DEV(g_gs_device_c);
	D3D12_RESOURCE_DESC resource_desc;
	D3D12_RANGE read_range;
	ID3D12Resource* buffer = NULL;
	void* mapped           = NULL;

	memset(&resource_desc, 0, sizeof(resource_desc));
	resource_desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
	resource_desc.Width              = size;
	resource_desc.Height             = 1;
	resource_desc.DepthOrArraySize   = 1;
	resource_desc.MipLevels          = 1;
	resource_desc.Format             = DXGI_FORMAT_UNKNOWN;
	resource_desc.SampleDesc.Count   = 1;
	resource_desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resource_desc.Flags              = D3D12_RESOURCE_FLAG_NONE;
	read_range.Begin                 = 0;
	read_range.End                   = 0;

	if (!d12_create_placed_resource(dev, &resource_desc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
			NULL, &buffer, alloc))
		return 0;
	if (FAILED(ID3D12Resource_Map(buffer, 0, &read_range, &mapped)))
	{
		/* Named in an aliasing barrier on this frame's init list: it and
		 * its span go back once that list has run. */
		d12_defer_resource(dev, alloc, (ID3D12DeviceChild*)buffer);
		ID3D12Resource_Release(buffer);
		return 0;
	}
	*resource     = buffer;
	*host_pointer = (u8*)mapped;
	*gpu_pointer  = ID3D12Resource_GetGPUVirtualAddress(buffer);
	return 1;
}

void gs_d3d12_device_release_upload_buffer(void* resource, const gs_d3d12_alloc_t* alloc, u32 size, int defer)
{
	gs_device_d3d12* const dev   = D12_DEV(g_gs_device_c);
	ID3D12Resource* const buffer = (ID3D12Resource*)resource;
	D3D12_RANGE written_range;
	written_range.Begin = 0;
	written_range.End   = size;

	ID3D12Resource_Unmap(buffer, 0, &written_range);
	if (defer)
	{
		/* Deferred: the span goes back with the command list. */
		d12_defer_resource(dev, alloc, (ID3D12DeviceChild*)buffer);
		ID3D12Resource_Release(buffer);
	}
	else
	{
		ID3D12Resource_Release(buffer);
		gs_d3d12_heap_free(&dev->heap, alloc);
	}
}

u64 gs_d3d12_device_current_fence(void) { return D12_DEV(g_gs_device_c)->current_fence_value; }
u64 gs_d3d12_device_completed_fence(void) { return D12_DEV(g_gs_device_c)->completed_fence_value; }
void gs_d3d12_device_wait_fence(u64 counter) { d12_wait_for_fence(D12_DEV(g_gs_device_c), counter); }

int gs_d3d12_device_create_descriptor_heap(void* device, u32 type, u32 num_descriptors, int shader_visible,
	void** heap, gs_d3d12_cpu_handle* base_cpu, gs_d3d12_gpu_handle* base_gpu, u32* increment)
{
	ID3D12Device* const d = (ID3D12Device*)device;
	D3D12_DESCRIPTOR_HEAP_DESC desc;
	ID3D12DescriptorHeap* h = NULL;

	desc.Type           = (D3D12_DESCRIPTOR_HEAP_TYPE)type;
	desc.NumDescriptors = (UINT)num_descriptors;
	desc.Flags          = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	desc.NodeMask       = 0;

	if (FAILED(ID3D12Device_CreateDescriptorHeap(d, &desc, &s_iid_descriptor_heap, (void**)&h)))
		return 0;
	*heap = h;
	{
		D3D12_CPU_DESCRIPTOR_HANDLE c;
		h->lpVtbl->GetCPUDescriptorHandleForHeapStart(h, &c);
		base_cpu->ptr = c.ptr;
	}
	if (shader_visible)
	{
		D3D12_GPU_DESCRIPTOR_HANDLE g;
		h->lpVtbl->GetGPUDescriptorHandleForHeapStart(h, &g);
		base_gpu->ptr = g.ptr;
	}
	*increment = ID3D12Device_GetDescriptorHandleIncrementSize(d, desc.Type);
	return 1;
}

void gs_d3d12_device_release_descriptor_heap(void* heap)
{
	ID3D12DescriptorHeap_Release((ID3D12DescriptorHeap*)heap);
}

void gs_d3d12_device_copy_sampler(void* device, gs_d3d12_cpu_handle dst, gs_d3d12_cpu_handle src)
{
	D3D12_CPU_DESCRIPTOR_HANDLE d, s;
	d.ptr = dst.ptr;
	s.ptr = src.ptr;
	ID3D12Device_CopyDescriptorsSimple((ID3D12Device*)device, 1, d, s, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
}

void gs_d3d12_device_copy_sampler_group(void* device, gs_d3d12_cpu_handle dst, const gs_d3d12_descriptor_handle* src,
	u32 count)
{
	D3D12_CPU_DESCRIPTOR_HANDLE src_handles[GS_D3D12_SAMPLER_GROUP_MAX];
	UINT src_sizes[GS_D3D12_SAMPLER_GROUP_MAX];
	D3D12_CPU_DESCRIPTOR_HANDLE d;
	UINT dst_size = count;
	u32 i;
	for (i = 0; i < count; i++)
	{
		src_handles[i].ptr = src[i].cpu_handle.ptr;
		src_sizes[i]       = 1;
	}
	d.ptr = dst.ptr;
	ID3D12Device_CopyDescriptors((ID3D12Device*)device, 1, &d, &dst_size, count, src_handles, src_sizes,
		D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
}

ID3D12Device* gs_d3d12_device_device(void) { return D12_DEV(g_gs_device_c)->device; }

int gs_d3d12_device_create_placed_resource(const D3D12_RESOURCE_DESC* desc, D3D12_HEAP_TYPE heap_type,
	D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear_value, ID3D12Resource** resource,
	gs_d3d12_alloc_t* alloc)
{
	return d12_create_placed_resource(D12_DEV(g_gs_device_c), desc, heap_type, state, clear_value, resource, alloc);
}

void gs_d3d12_device_defer_resource(const gs_d3d12_alloc_t* alloc, ID3D12Resource* resource)
{
	d12_defer_resource(D12_DEV(g_gs_device_c), alloc, (ID3D12DeviceChild*)resource);
}

static gs_d3d12_descriptor_heap_manager* d12_heap_of(gs_device_d3d12* dev, u32 kind)
{
	return (kind == GS_D3D12_HEAP_RTV) ? &dev->rtv_heap_manager
		: (kind == GS_D3D12_HEAP_DSV)  ? &dev->dsv_heap_manager
									   : &dev->descriptor_heap_manager;
}

gs_d3d12_descriptor_heap_manager* gs_d3d12_device_descriptor_heap(u32 kind)
{
	return d12_heap_of(D12_DEV(g_gs_device_c), kind);
}

void gs_d3d12_device_defer_descriptor(u32 kind, gs_d3d12_descriptor_handle* handle)
{
	gs_device_d3d12* const dev = D12_DEV(g_gs_device_c);
	d12_defer_descriptor(dev, d12_heap_of(dev, kind), handle);
}

void gs_d3d12_device_unbind_texture(gs_texture_d3d12* t) { d12_unbind_texture(D12_DEV(g_gs_device_c), t); }

ID3D12GraphicsCommandList* gs_d3d12_device_command_list(void)
{
	return (ID3D12GraphicsCommandList*)d12_cmdlist(D12_DEV(g_gs_device_c));
}

ID3D12GraphicsCommandList* gs_d3d12_device_init_command_list(void)
{
	return (ID3D12GraphicsCommandList*)d12_init_command_list(D12_DEV(g_gs_device_c));
}

void gs_d3d12_device_end_render_pass(void) { d12_end_render_pass(D12_DEV(g_gs_device_c)); }
void gs_d3d12_device_execute(int wait) { d12_execute_command_list(D12_DEV(g_gs_device_c), wait != 0); }
void gs_d3d12_device_execute_for_readback(void) { d12_execute_command_list(D12_DEV(g_gs_device_c), true); }

gs_d3d12_stream_buffer* gs_d3d12_device_texture_stream_buffer(void)
{
	return &D12_DEV(g_gs_device_c)->texture_stream_buffer;
}

void gs_d3d12_device_report_recurring(const char* what, HRESULT hr) { d12_report_recurring(what, hr); }

void gs_d3d12_device_render_mipmap(gs_texture_d3d12* t, u32 dst_level, u32 dst_width, u32 dst_height, u32 src_level,
	u32 src_width, u32 src_height)
{
	d12_render_texture_mipmap(D12_DEV(g_gs_device_c), t, dst_level, dst_width, dst_height, src_level, src_width,
		src_height);
}

/* ==== the objects made from descriptions ==== */

static ID3DBlob* d12_serialize_root_signature(const D3D12_ROOT_SIGNATURE_DESC* desc)
{
	ID3DBlob* blob       = NULL;
	ID3DBlob* error_blob = NULL;
	const HRESULT hr = s_d3d12_serialize_root_signature(desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error_blob);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "D3D12SerializeRootSignature() failed: %08X\n", (unsigned)hr);
		if (error_blob)
			log_cb(RETRO_LOG_ERROR, "%s\n", (const char*)ID3D10Blob_GetBufferPointer(error_blob));
		D12_RELEASE(error_blob);
		D12_RELEASE(blob);
		return NULL;
	}
	D12_RELEASE(error_blob);
	return blob;
}

static ID3D12RootSignature* d12_create_root_signature(gs_device_d3d12* dev, const D3D12_ROOT_SIGNATURE_DESC* desc)
{
	ID3DBlob* blob          = d12_serialize_root_signature(desc);
	ID3D12RootSignature* rs = NULL;
	HRESULT hr;
	if (!blob)
		return NULL;

	hr = ID3D12Device_CreateRootSignature(dev->device, 0, ID3D10Blob_GetBufferPointer(blob),
		ID3D10Blob_GetBufferSize(blob), &s_iid_root_signature, (void**)&rs);
	ID3D10Blob_Release(blob);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "CreateRootSignature() failed: %08X\n", (unsigned)hr);
		return NULL;
	}
	return rs;
}

/* (RootSignatureBuilder::Create: the builder cleared after) */
static ID3D12RootSignature* d12_rs_create(gs_device_d3d12* dev, gs_d3d12_rs_builder* b)
{
	ID3D12RootSignature* rs = d12_create_root_signature(dev, &b->desc);
	if (!rs)
		return NULL;
	gs_d3d12_rs_clear(b);
	return rs;
}

/* (GraphicsPipelineBuilder::Create, without the cache) */
static ID3D12PipelineState* d12_gp_create(ID3D12Device* device, gs_d3d12_gp_builder* b, bool clear)
{
	ID3D12PipelineState* ps = NULL;
	const HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(device, &b->desc, &s_iid_pipeline_state, (void**)&ps);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "CreateGraphicsPipelineState() failed: %08X\n", (unsigned)hr);
		return NULL;
	}
	if (clear)
		gs_d3d12_gp_clear(b);
	return ps;
}

/* (GraphicsPipelineBuilder::Create, through the cache) */
static ID3D12PipelineState* d12_gp_create_cached(gs_device_d3d12* dev, gs_d3d12_gp_builder* b, bool clear)
{
	ID3D12PipelineState* pso = gs_d3d12_shader_cache_graphics_pipeline(&dev->shader_cache, dev->device, &b->desc);
	if (!pso)
		return NULL;
	if (clear)
		gs_d3d12_gp_clear(b);
	return pso;
}

static void d12_gp_set_vs(gs_d3d12_gp_builder* b, const ID3DBlob* blob)
{
	ID3DBlob* const bl = (ID3DBlob*)blob;
	gs_d3d12_gp_set_vertex_shader(b, ID3D10Blob_GetBufferPointer(bl), (u32)ID3D10Blob_GetBufferSize(bl));
}

static void d12_gp_set_ps(gs_d3d12_gp_builder* b, const ID3DBlob* blob)
{
	ID3DBlob* const bl = (ID3DBlob*)blob;
	gs_d3d12_gp_set_pixel_shader(b, ID3D10Blob_GetBufferPointer(bl), (u32)ID3D10Blob_GetBufferSize(bl));
}

#define d12_gp_no_cull(b) gs_d3d12_gp_set_rasterization((b), D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, 0)
#define d12_gp_no_depth(b) gs_d3d12_gp_set_depth((b), 0, 0, D3D12_COMPARISON_FUNC_ALWAYS)

/* ==== the device's basics ==== */

static bool d12_supports_texture_format(gs_device_d3d12* dev, DXGI_FORMAT format)
{
	const u32 required = (u32)D3D12_FORMAT_SUPPORT1_TEXTURE2D | (u32)D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support;
	memset(&support, 0, sizeof(support));
	support.Format = format;
	return SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev->device, D3D12_FEATURE_FORMAT_SUPPORT, &support,
			   sizeof(support)))
		&& ((u32)support.Support1 & required) == required;
}

static u32 d12_adapter_vendor_id(gs_device_d3d12* dev)
{
	DXGI_ADAPTER_DESC desc;
	if (!dev->adapter)
		return 0;
	if (FAILED(IDXGIAdapter_GetDesc(dev->adapter, &desc)))
		return 0;
	return desc.VendorId;
}

/* The device calls the heap needs, so that GSD3D12Heap.c needs no
 * d3d12.h and can be built and tested without a GPU. */
static int gs_d3d12_create_heap_cb(void* device, uint64_t size, uint32_t heap_type, uint32_t heap_flags,
	void** out_heap)
{
	ID3D12Device* dev = (ID3D12Device*)device;
	ID3D12Heap* heap  = NULL;
	D3D12_HEAP_DESC hd;

	memset(&hd, 0, sizeof(hd));
	hd.SizeInBytes     = size;
	hd.Properties.Type = (D3D12_HEAP_TYPE)heap_type;
	hd.Alignment       = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	hd.Flags           = (D3D12_HEAP_FLAGS)heap_flags;

	if (FAILED(ID3D12Device_CreateHeap(dev, &hd, &s_iid_heap, (void**)&heap)) || !heap)
		return 0;

	*out_heap = heap;
	return 1;
}

static void gs_d3d12_release_heap_cb(void* device, void* heap)
{
	(void)device;
	if (heap)
		ID3D12Heap_Release((ID3D12Heap*)heap);
}

static bool d12_create_heap(gs_device_d3d12* dev)
{
	D3D12_FEATURE_DATA_D3D12_OPTIONS options;
	gs_d3d12_heap_fns_t fns;
	const float scale = GSConfig.UpscaleMultiplier > 0.0f ? GSConfig.UpscaleMultiplier : 1.0f;
	u64 block;
	u64 ceiling;

	/* Tier 1 keeps buffers, render targets and other textures in heaps of
	 * their own; tier 2 takes them all in one, which means fewer heaps and
	 * less of each one wasted. */
	memset(&options, 0, sizeof(options));
	dev->heaps_hold_anything = false;
	if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev->device, D3D12_FEATURE_D3D12_OPTIONS, &options,
			sizeof(options))))
		dev->heaps_hold_anything = (options.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2);

	/* Heaps big enough that a frame's targets come out of one of them, so
	 * that making a target is a placement inside a heap that exists rather
	 * than a heap of its own. Vulkan sizes its blocks the same way. */
	block = (u64)((float)VM_SIZE * scale * scale) * 2u;
	if (block < 64ull * 1024ull * 1024ull)
		block = 64ull * 1024ull * 1024ull;
	/* 64 MB, not 512: a block is only given back when it is entirely
	 * empty, which at 512 MB never happens once allocations are scattered
	 * through it, so trim returns nothing and the tails of the blocks are
	 * memory nothing can reach. It is worse here than on Vulkan, because a
	 * resource-heap-tier-1 device needs separate blocks for buffers, for
	 * render targets and depth stencils, and for other textures. */
	if (block > 64ull * 1024ull * 1024ull)
		block = 64ull * 1024ull * 1024ull;

	/* And the same ceiling: the budgets above the heap already come to
	 * more than half of it, and a ceiling under that sum refuses
	 * allocations the renderer is entitled to make. Never fewer than four
	 * blocks, or at native it would be smaller than one block. */
	ceiling = (u64)((float)VM_SIZE * scale * scale) * 16u;
	if (ceiling < block * 4u)
		ceiling = block * 4u;

	fns.create_heap  = gs_d3d12_create_heap_cb;
	fns.release_heap = gs_d3d12_release_heap_cb;

	log_cb(RETRO_LOG_INFO, "GS: D3D12 heap, %llu MB blocks, %llu MB ceiling, heaps hold %s.\n",
		(unsigned long long)(block >> 20), (unsigned long long)(ceiling >> 20),
		dev->heaps_hold_anything ? "anything (tier 2)" : "one kind of thing (tier 1)");

	if (!gs_d3d12_heap_init(&dev->heap, dev->device, &fns, block, ceiling))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create the D3D12 heap\n");
		return false;
	}

	return true;
}

/* The heap flags a resource of this description needs. On tier 2 they
 * all share, so everything lands in the same blocks. */
static D3D12_HEAP_FLAGS d12_heap_flags_for_resource(const D3D12_RESOURCE_DESC* desc, bool heaps_hold_anything)
{
	if (heaps_hold_anything)
		return D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;

	if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		return D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

	if (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
		return D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;

	return D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
}

/* One place that turns a resource description into memory: it asks the
 * device how big and how aligned, takes that from the heap, and places
 * the resource at the offset. */
static bool d12_create_placed_resource(gs_device_d3d12* dev, const D3D12_RESOURCE_DESC* desc,
	D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear_value,
	ID3D12Resource** out_resource, gs_d3d12_alloc_t* out_alloc)
{
	D3D12_RESOURCE_ALLOCATION_INFO info;
	const D3D12_HEAP_FLAGS heap_flags = d12_heap_flags_for_resource(desc, dev->heaps_hold_anything);
	gs_d3d12_alloc_t alloc;
	HRESULT hr;

	dev->device->lpVtbl->GetResourceAllocationInfo(dev->device, &info, 0, 1, desc);
	memset(&alloc, 0, sizeof(alloc));

	if (info.SizeInBytes == UINT64_MAX)
		return false;

	if (!gs_d3d12_heap_alloc(&dev->heap, info.SizeInBytes, info.Alignment, (uint32_t)heap_type, (uint32_t)heap_flags,
			&alloc))
	{
		/* Silence here is a black screen and nothing to go on. Say what was
		 * asked for and what the heap is holding. */
		log_cb(RETRO_LOG_ERROR,
			"GS: no room for a %llu KB resource (heap %llu MB reserved, %llu MB used: "
			"%llu MB uploads and readbacks, %llu MB device).\n",
			(unsigned long long)(info.SizeInBytes >> 10), (unsigned long long)(dev->heap.bytes_reserved >> 20),
			(unsigned long long)(dev->heap.bytes_used >> 20), (unsigned long long)(dev->heap.bytes_host >> 20),
			(unsigned long long)(dev->heap.bytes_device >> 20));
		return false;
	}

	hr = ID3D12Device_CreatePlacedResource(dev->device, (ID3D12Heap*)alloc.heap, alloc.offset, desc, state,
		clear_value, &s_iid_resource, (void**)out_resource);
	if (FAILED(hr))
	{
		gs_d3d12_heap_free(&dev->heap, &alloc);
		/* Out of memory is not fatal; the caller retries with less. */
		if (hr != E_OUTOFMEMORY)
			log_cb(RETRO_LOG_ERROR, "CreatePlacedResource() failed with HRESULT %08X\n", (unsigned)hr);
		return false;
	}

	/* The span this resource was placed in may have held another one a
	 * moment ago, and D3D12 requires an aliasing barrier before the new
	 * one is used or the command list is invalid - Close() returns
	 * E_INVALIDARG and the frame never reaches the screen. NULL before
	 * means "whatever was here". */
	{
		D3D12_RESOURCE_BARRIER rb;
		memset(&rb, 0, sizeof(rb));
		rb.Type                      = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
		rb.Flags                     = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		rb.Aliasing.pResourceBefore = NULL;
		rb.Aliasing.pResourceAfter  = *out_resource;
		ID3D12GraphicsCommandList4_ResourceBarrier(d12_init_command_list(dev), 1, &rb);
	}

	/* Name it. The runtime prints the name of a resource it complains
	 * about, and everything this backend places gets one, so anything
	 * still unnamed in a message belongs to the frontend. */
	{
		static unsigned s_placed_serial;
		wchar_t name[96];

		_snwprintf(name, sizeof(name) / sizeof(name[0]), L"GS placed %u: %s %ux%u fmt %u heap %u", ++s_placed_serial,
			(desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) ? L"buffer" : L"texture", (unsigned)desc->Width,
			(unsigned)desc->Height, (unsigned)desc->Format, (unsigned)heap_type);
		name[(sizeof(name) / sizeof(name[0])) - 1] = 0;
		ID3D12Resource_SetName(*out_resource, name);
	}

	*out_alloc = alloc;
	return true;
}

static bool d12_create_fence(gs_device_d3d12* dev)
{
	HRESULT hr = ID3D12Device_CreateFence(dev->device, dev->completed_fence_value, D3D12_FENCE_FLAG_NONE,
		&s_iid_fence, (void**)&dev->fence);
	if (FAILED(hr))
		return false;

	dev->fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (!dev->fence_event)
		return false;

	return true;
}

static bool d12_create_descriptor_heaps(gs_device_d3d12* dev)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC null_srv_desc;

	if (!gs_d3d12_descriptor_heap_manager_create(&dev->descriptor_heap_manager, dev->device,
			D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32768, 0)
		|| !gs_d3d12_descriptor_heap_manager_create(&dev->rtv_heap_manager, dev->device,
			D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 16384, 0)
		|| !gs_d3d12_descriptor_heap_manager_create(&dev->dsv_heap_manager, dev->device,
			D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 16384, 0)
		|| !gs_d3d12_descriptor_heap_manager_create(&dev->sampler_heap_manager, dev->device,
			D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1024, 0))
		return false;

	/* The null SRV descriptor, for unbound textures */
	memset(&null_srv_desc, 0, sizeof(null_srv_desc));
	null_srv_desc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
	null_srv_desc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
	null_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	if (!gs_d3d12_descriptor_heap_manager_allocate(&dev->descriptor_heap_manager, &dev->null_srv_descriptor))
		return false;

	ID3D12Device_CreateShaderResourceView(dev->device, NULL, &null_srv_desc, d12_cpu(&dev->null_srv_descriptor));
	return true;
}

static void d12_move_to_next_command_list(gs_device_d3d12* dev)
{
	const u32 next_command_list = (dev->current_command_list + 1) % GS_D3D12_NUM_COMMAND_LISTS;
	gs_d3d12_command_list* res  = &dev->command_lists[next_command_list];
	ID3D12DescriptorHeap* heaps[2];

	/* We may have to wait if this command list hasn't finished on the GPU. */
	d12_wait_for_fence(dev, res->ready_fence_value);

	dev->current_command_list = next_command_list;
	dev->current_fence_value++;
	res->ready_fence_value      = dev->current_fence_value;
	res->init_command_list_used = false;

	/* Begin command list. */
	ID3D12CommandAllocator_Reset(res->command_allocators[1]);
	ID3D12GraphicsCommandList4_Reset(res->command_lists[1], res->command_allocators[1], NULL);
	gs_d3d12_descriptor_allocator_reset(&res->descriptor_allocator);
	if (gs_d3d12_sampler_allocator_should_reset(&res->sampler_allocator))
		gs_d3d12_sampler_allocator_reset(&res->sampler_allocator);

	heaps[0] = (ID3D12DescriptorHeap*)res->descriptor_allocator.heap;
	heaps[1] = (ID3D12DescriptorHeap*)res->sampler_allocator.alloc.heap;
	ID3D12GraphicsCommandList4_SetDescriptorHeaps(res->command_lists[1], D12_ARRAY_SIZE(heaps), heaps);
}

static bool d12_create_command_lists(gs_device_d3d12* dev)
{
	u32 i, j;

	for (i = 0; i < GS_D3D12_NUM_COMMAND_LISTS; i++)
	{
		gs_d3d12_command_list* res = &dev->command_lists[i];
		HRESULT hr;

		for (j = 0; j < 2; j++)
		{
			hr = ID3D12Device_CreateCommandAllocator(dev->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
				&s_iid_command_allocator, (void**)&res->command_allocators[j]);
			if (FAILED(hr))
				return false;

			hr = ID3D12Device_CreateCommandList(dev->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				res->command_allocators[j], NULL, &s_iid_command_list4, (void**)&res->command_lists[j]);
			if (FAILED(hr))
			{
				log_cb(RETRO_LOG_ERROR, "Failed to create command list: %08X\n", (unsigned)hr);
				return false;
			}

			/* Close the command lists, since the first thing we do is reset them. */
			hr = ID3D12GraphicsCommandList4_Close(res->command_lists[j]);
			if (FAILED(hr))
				return false;
		}

		if (!gs_d3d12_descriptor_allocator_create(&res->descriptor_allocator, dev->device,
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32768))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to create per frame descriptor allocator\n");
			return false;
		}

		if (!gs_d3d12_sampler_allocator_create(&res->sampler_allocator, dev->device, 2048))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to create per frame sampler allocator\n");
			return false;
		}

		/* (as the device is made: what a frame defers does not allocate) */
		res->pending_resources =
			(gs_d3d12_pending_resource*)malloc(D12_PENDING_RESERVE * sizeof(gs_d3d12_pending_resource));
		res->pending_descriptors =
			(gs_d3d12_pending_descriptor*)malloc(D12_PENDING_RESERVE * sizeof(gs_d3d12_pending_descriptor));
		if (!res->pending_resources || !res->pending_descriptors)
			return false;
		res->cap_pending_resources   = D12_PENDING_RESERVE;
		res->cap_pending_descriptors = D12_PENDING_RESERVE;
	}

	d12_move_to_next_command_list(dev);
	return true;
}

static ID3D12GraphicsCommandList4* d12_init_command_list(gs_device_d3d12* dev)
{
	gs_d3d12_command_list* res = &dev->command_lists[dev->current_command_list];
	if (!res->init_command_list_used)
	{
		ID3D12CommandAllocator_Reset(res->command_allocators[0]);
		ID3D12GraphicsCommandList4_Reset(res->command_lists[0], res->command_allocators[0], NULL);
		res->init_command_list_used = true;
	}

	return res->command_lists[0];
}

/* Report a recurring device error without turning a wedged GPU into a
 * multi-gigabyte log. The first few say what happened; after that the
 * count is carried until the next distinct failure. */
static void d12_report_recurring(const char* what, HRESULT hr)
{
	static const char* last_what;
	static HRESULT last_hr;
	static u64 repeats;

	if (what != last_what || hr != last_hr)
	{
		if (repeats > 0)
			log_cb(RETRO_LOG_ERROR, "(previous message repeated %llu more times)\n", (unsigned long long)repeats);
		last_what = what;
		last_hr   = hr;
		repeats   = 0;
		log_cb(RETRO_LOG_ERROR, "%s failed with HRESULT %08X\n", what, (unsigned)hr);
		return;
	}

	repeats++;
	/* One line every ~10 seconds of a 60 Hz stream of failures, so a
	 * report still shows the problem is ongoing. */
	if ((repeats % 600) == 0)
		log_cb(RETRO_LOG_ERROR, "%s failed with HRESULT %08X (%llu times)\n", what, (unsigned)hr,
			(unsigned long long)repeats);
}

/* Whatever the runtime has queued since last time. The text is the
 * point: a failed Close says a command was invalid, this says which. */
static void d12_drain_debug_messages(gs_device_d3d12* dev)
{
	UINT64 count;
	UINT64 i;

	if (!dev->info_queue)
		return;

	count = ID3D12InfoQueue_GetNumStoredMessages(dev->info_queue);
	for (i = 0; i < count; i++)
	{
		SIZE_T length = 0;
		D3D12_MESSAGE* msg;

		if (FAILED(ID3D12InfoQueue_GetMessage(dev->info_queue, i, NULL, &length)) || !length)
			continue;

		/* (only with the debug layer forced on) */
		msg = (D3D12_MESSAGE*)malloc(length);
		if (!msg)
			continue;

		if (SUCCEEDED(ID3D12InfoQueue_GetMessage(dev->info_queue, i, msg, &length)) && msg->pDescription)
			log_cb(RETRO_LOG_ERROR, "GS: D3D12 validation: %s\n", msg->pDescription);

		free(msg);
	}
	ID3D12InfoQueue_ClearStoredMessages(dev->info_queue);
}

static void d12_destroy_pending_resources(gs_device_d3d12* dev, gs_d3d12_command_list* cmdlist)
{
	size_t i;
	for (i = 0; i < cmdlist->num_pending_descriptors; i++)
		gs_d3d12_descriptor_heap_manager_free_index(cmdlist->pending_descriptors[i].manager,
			cmdlist->pending_descriptors[i].index);
	cmdlist->num_pending_descriptors = 0;

	for (i = 0; i < cmdlist->num_pending_resources; i++)
	{
		gs_d3d12_pending_resource* it = &cmdlist->pending_resources[i];
		ID3D12DeviceChild_Release(it->resource);
		/* The heap span goes back now, with the GPU finished with it. */
		if (it->alloc.heap)
			gs_d3d12_heap_free(&dev->heap, &it->alloc);
	}
	cmdlist->num_pending_resources = 0;
}

static void d12_wait_for_fence(gs_device_d3d12* dev, u64 fence)
{
	u32 index, i;

	if (dev->completed_fence_value >= fence)
		return;

	/* Try non-blocking check. */
	dev->completed_fence_value = ID3D12Fence_GetCompletedValue(dev->fence);
	if (dev->completed_fence_value < fence)
	{
		/* Fall back to event. */
		ID3D12Fence_SetEventOnCompletion(dev->fence, fence, dev->fence_event);
		WaitForSingleObject(dev->fence_event, INFINITE);
		dev->completed_fence_value = ID3D12Fence_GetCompletedValue(dev->fence);
	}

	/* Release resources for as many command lists which have completed. */
	index = (dev->current_command_list + 1) % GS_D3D12_NUM_COMMAND_LISTS;
	for (i = 0; i < GS_D3D12_NUM_COMMAND_LISTS; i++)
	{
		gs_d3d12_command_list* res = &dev->command_lists[index];
		if (dev->completed_fence_value < res->ready_fence_value)
			break;

		d12_destroy_pending_resources(dev, res);
		index = (index + 1) % GS_D3D12_NUM_COMMAND_LISTS;
	}
}

static bool d12_context_execute_command_list(gs_device_d3d12* dev, bool wait_for_completion)
{
	gs_d3d12_command_list* res = &dev->command_lists[dev->current_command_list];
	HRESULT hr;
	bool closed_ok = true;
	u32 i;

	/* Anything the runtime objected to while this list was being recorded,
	 * whether or not the Close below fails. */
	d12_drain_debug_messages(dev);

	if (res->init_command_list_used)
	{
		hr = ID3D12GraphicsCommandList4_Close(res->command_lists[0]);
		if (FAILED(hr))
		{
			d12_report_recurring("Closing init command list", hr);
			d12_drain_debug_messages(dev);
			closed_ok = false;
		}
	}

	/* Close and queue command list. */
	if (closed_ok)
	{
		hr = ID3D12GraphicsCommandList4_Close(res->command_lists[1]);
		if (FAILED(hr))
		{
			d12_report_recurring("Closing main command list", hr);
			d12_drain_debug_messages(dev);
			closed_ok = false;
		}
	}

	if (!closed_ok)
	{
		/* Drop the frame, but keep the bookkeeping moving: signal the fence
		 * this list was going to signal - there is no GPU work outstanding
		 * for it, so it completes immediately and the stream buffers can
		 * free their segments - and move to the next command list, which
		 * resets an allocator and a list that are in a known state. */
		ID3D12CommandQueue_Signal(dev->command_queue, dev->fence, res->ready_fence_value);

		/* And the lists themselves are replaced: a list that has failed to
		 * close - or a main list that was then never closed at all - does
		 * not come back. Nothing was submitted from these, so they can
		 * simply go; the allocators are reset when their turn comes. */
		for (i = 0; i < 2; i++)
		{
			D12_RELEASE(res->command_lists[i]);
			hr = ID3D12Device_CreateCommandList(dev->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				res->command_allocators[i], NULL, &s_iid_command_list4, (void**)&res->command_lists[i]);
			if (SUCCEEDED(hr))
				hr = ID3D12GraphicsCommandList4_Close(res->command_lists[i]);
			if (FAILED(hr))
				d12_report_recurring("Replacing a command list", hr);
		}
		res->init_command_list_used = false;

		d12_move_to_next_command_list(dev);
		if (wait_for_completion)
			d12_wait_for_fence(dev, res->ready_fence_value);
		return false;
	}

	if (res->init_command_list_used)
	{
		ID3D12CommandList* execute_lists[2];
		execute_lists[0] = (ID3D12CommandList*)res->command_lists[0];
		execute_lists[1] = (ID3D12CommandList*)res->command_lists[1];
		ID3D12CommandQueue_ExecuteCommandLists(dev->command_queue, 2, execute_lists);
	}
	else
	{
		ID3D12CommandList* execute_lists[1];
		execute_lists[0] = (ID3D12CommandList*)res->command_lists[1];
		ID3D12CommandQueue_ExecuteCommandLists(dev->command_queue, 1, execute_lists);
	}

	/* Update fence when GPU has completed. */
	ID3D12CommandQueue_Signal(dev->command_queue, dev->fence, res->ready_fence_value);

	d12_move_to_next_command_list(dev);
	if (wait_for_completion)
		d12_wait_for_fence(dev, res->ready_fence_value);

	return true;
}

static void d12_invalidate_sampler_groups(gs_device_d3d12* dev)
{
	u32 i;
	for (i = 0; i < GS_D3D12_NUM_COMMAND_LISTS; i++)
		gs_d3d12_sampler_allocator_invalidate(&dev->command_lists[i].sampler_allocator);
}

/* An entry more in a command list's array: grown, past what it was given
 * as the device was made, only by a frame that defers that much */
static bool d12_grow(void** arr, size_t* cap, size_t elem)
{
	const size_t ncap = *cap ? *cap * 2 : D12_PENDING_RESERVE;
	void* p           = realloc(*arr, ncap * elem);
	if (!p)
		return false;
	*arr = p;
	*cap = ncap;
	return true;
}

/* Holds the resource (and its span) until the current command list has run */
static void d12_defer_resource(gs_device_d3d12* dev, const gs_d3d12_alloc_t* alloc, ID3D12DeviceChild* resource)
{
	gs_d3d12_command_list* res = &dev->command_lists[dev->current_command_list];
	gs_d3d12_pending_resource* pr;

	if (!resource)
		return;
	if (res->num_pending_resources == res->cap_pending_resources
		&& !d12_grow((void**)&res->pending_resources, &res->cap_pending_resources, sizeof(*pr)))
		return;

	ID3D12DeviceChild_AddRef(resource);
	pr = &res->pending_resources[res->num_pending_resources++];
	memset(pr, 0, sizeof(*pr));
	if (alloc)
		pr->alloc = *alloc;
	pr->resource = resource;
}

static void d12_defer_descriptor_index(gs_device_d3d12* dev, gs_d3d12_descriptor_heap_manager* manager, u32 index)
{
	gs_d3d12_command_list* res = &dev->command_lists[dev->current_command_list];
	gs_d3d12_pending_descriptor* pd;
	if (res->num_pending_descriptors == res->cap_pending_descriptors
		&& !d12_grow((void**)&res->pending_descriptors, &res->cap_pending_descriptors, sizeof(*pd)))
		return;
	pd          = &res->pending_descriptors[res->num_pending_descriptors++];
	pd->manager = manager;
	pd->index   = index;
}

static void d12_defer_descriptor(gs_device_d3d12* dev, gs_d3d12_descriptor_heap_manager* manager,
	gs_d3d12_descriptor_handle* handle)
{
	if (handle->index == GS_D3D12_DESCRIPTOR_INVALID)
		return;

	d12_defer_descriptor_index(dev, manager, handle->index);
	gs_d3d12_descriptor_handle_clear(handle);
}

/* The expansion index buffer, filled through an upload buffer the init
 * list copies from */
static bool d12_allocate_expand_index_buffer(gs_device_d3d12* dev, u32 size, ID3D12Resource** gpu_buffer,
	gs_d3d12_alloc_t* gpu_alloc)
{
	D3D12_RESOURCE_DESC rd;
	D3D12_RANGE read_range, write_range;
	D3D12_RESOURCE_BARRIER rb;
	ID3D12Resource* cpu_buffer = NULL;
	gs_d3d12_alloc_t cpu_alloc;
	void* mapped;
	HRESULT hr;

	memset(&rd, 0, sizeof(rd));
	rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width            = size;
	rd.Height           = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels        = 1;
	rd.Format           = DXGI_FORMAT_UNKNOWN;
	rd.SampleDesc.Count = 1;
	rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	rd.Flags            = D3D12_RESOURCE_FLAG_NONE;
	memset(&cpu_alloc, 0, sizeof(cpu_alloc));

	if (!d12_create_placed_resource(dev, &rd, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
			&cpu_buffer, &cpu_alloc))
		return false;

	read_range.Begin  = 0;
	read_range.End    = 0;
	write_range.Begin = 0;
	write_range.End   = size;
	hr                = ID3D12Resource_Map(cpu_buffer, 0, &read_range, &mapped);
	if (FAILED(hr))
	{
		d12_defer_resource(dev, &cpu_alloc, (ID3D12DeviceChild*)cpu_buffer);
		ID3D12Resource_Release(cpu_buffer);
		return false;
	}
	gs_device_generate_expansion_index_buffer(mapped);
	ID3D12Resource_Unmap(cpu_buffer, 0, &write_range);

	if (!d12_create_placed_resource(dev, &rd, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, NULL, gpu_buffer,
			gpu_alloc))
	{
		d12_defer_resource(dev, &cpu_alloc, (ID3D12DeviceChild*)cpu_buffer);
		ID3D12Resource_Release(cpu_buffer);
		return false;
	}

	ID3D12GraphicsCommandList4_CopyBufferRegion(d12_init_command_list(dev), *gpu_buffer, 0, cpu_buffer, 0, size);

	memset(&rb, 0, sizeof(rb));
	rb.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	rb.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	rb.Transition.pResource   = *gpu_buffer;
	rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; /* COMMON -> COPY_DEST at first use. */
	rb.Transition.StateAfter  = D3D12_RESOURCE_STATE_INDEX_BUFFER;
	ID3D12GraphicsCommandList4_ResourceBarrier(d12_init_command_list(dev), 1, &rb);

	d12_defer_resource(dev, &cpu_alloc, (ID3D12DeviceChild*)cpu_buffer);
	ID3D12Resource_Release(cpu_buffer);
	return true;
}

/* ==== textures as the device sees them ==== */

static bool d12_is_datm_convert_shader(int i)
{
	return (i == GS_SHADER_CONVERT_DATM_0 || i == GS_SHADER_CONVERT_DATM_1 || i == GS_SHADER_CONVERT_DATM_0_RTA_CORRECTION
		|| i == GS_SHADER_CONVERT_DATM_1_RTA_CORRECTION);
}

static bool d12_is_date_mode_prim_id_init(u32 flag)
{
	return flag == 1 || flag == 2;
}

static const D3D12_PRIMITIVE_TOPOLOGY s_primitive_topology_mapping[3] = {
	D3D_PRIMITIVE_TOPOLOGY_POINTLIST, D3D_PRIMITIVE_TOPOLOGY_LINELIST, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST};

static D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE d12_load_op(gs_texture_d3d12* tex)
{
	if (!tex)
		return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;

	switch (tex->base.m_state)
	{
		case GS_TEXSTATE_CLEARED:
			tex->base.m_state = GS_TEXSTATE_DIRTY;
			return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
		case GS_TEXSTATE_INVALIDATED:
			tex->base.m_state = GS_TEXSTATE_DIRTY;
			return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
		default:
			break;
	}
	return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
}

/* (GSVector4::unorm8) */
static void d12_unorm8(float out[4], u32 c)
{
	int i;
	for (i = 0; i < 4; i++)
		out[i] = (float)((c >> (i * 8)) & 0xFFu) * (1.0f / 255.0f);
}

static void d12_set_clear_color(gs_texture* t, u32 c)
{
	t->m_state             = GS_TEXSTATE_CLEARED;
	t->m_clear_value.color = c;
}

static void d12_transition(gs_texture_d3d12* t, gs_device_d3d12* dev, D3D12_RESOURCE_STATES state)
{
	gs_texture_d3d12_transition(t, (ID3D12GraphicsCommandList*)d12_cmdlist(dev), state);
}

static void d12_commit_clear(gs_texture_d3d12* t)
{
	gs_texture_d3d12_commit_clear(t, NULL);
}

static bool d12_is_rt_or_ds(const gs_texture* t)
{
	return t->m_type == GS_TEXTYPE_RENDER_TARGET || t->m_type == GS_TEXTYPE_DEPTH_STENCIL;
}

/* ==== rectangles ==== */

static void d12_recti_of(gs_recti* out, const gs_rectf* r)
{
	/* (GSVector4i(GSVector4): truncated) */
	out->x = (int)r->x;
	out->y = (int)r->y;
	out->z = (int)r->z;
	out->w = (int)r->w;
}

static void d12_rectf_of(gs_rectf* out, const gs_recti* r)
{
	out->x = (float)r->x;
	out->y = (float)r->y;
	out->z = (float)r->z;
	out->w = (float)r->w;
}

static void d12_rintersect(gs_recti* out, const gs_recti* a, const gs_recti* b)
{
	out->x = a->x > b->x ? a->x : b->x;
	out->y = a->y > b->y ? a->y : b->y;
	out->z = a->z < b->z ? a->z : b->z;
	out->w = a->w < b->w ? a->w : b->w;
}

static bool d12_recti_eq(const gs_recti* a, const gs_recti* b)
{
	return a->x == b->x && a->y == b->y && a->z == b->z && a->w == b->w;
}

static void d12_tex_rect(gs_recti* out, const gs_texture* t)
{
	out->x = 0;
	out->y = 0;
	out->z = t->m_size.x;
	out->w = t->m_size.y;
}

/* (sRect / size.xyxy) */
static void d12_norm_rect(gs_rectf* out, const gs_recti* r, int w, int h)
{
	out->x = (float)r->x / (float)w;
	out->y = (float)r->y / (float)h;
	out->z = (float)r->z / (float)w;
	out->w = (float)r->w / (float)h;
}

/* ==== the cached state ==== */

static void d12_set_vertex_buffer(gs_device_d3d12* dev, D3D12_GPU_VIRTUAL_ADDRESS buffer, size_t size, size_t stride)
{
	if (dev->vertex_buffer.BufferLocation == buffer && dev->vertex_buffer.SizeInBytes == size
		&& dev->vertex_buffer.StrideInBytes == stride)
		return;

	dev->vertex_buffer.BufferLocation = buffer;
	dev->vertex_buffer.SizeInBytes    = (UINT)size;
	dev->vertex_buffer.StrideInBytes  = (UINT)stride;
	dev->dirty_flags |= DIRTY_FLAG_VERTEX_BUFFER;
}

static void d12_set_index_buffer(gs_device_d3d12* dev, D3D12_GPU_VIRTUAL_ADDRESS buffer, size_t size, DXGI_FORMAT type)
{
	if (dev->index_buffer.BufferLocation == buffer && dev->index_buffer.SizeInBytes == size
		&& dev->index_buffer.Format == type)
		return;

	dev->index_buffer.BufferLocation = buffer;
	dev->index_buffer.SizeInBytes    = (UINT)size;
	dev->index_buffer.Format         = type;
	dev->dirty_flags |= DIRTY_FLAG_INDEX_BUFFER;
}

static void d12_set_primitive_topology(gs_device_d3d12* dev, D3D12_PRIMITIVE_TOPOLOGY topology)
{
	if (dev->primitive_topology == topology)
		return;

	dev->primitive_topology = topology;
	dev->dirty_flags |= DIRTY_FLAG_PRIMITIVE_TOPOLOGY;
}

static void d12_set_blend_constants(gs_device_d3d12* dev, u8 color)
{
	if (dev->blend_constant_color == color)
		return;

	dev->blend_constant_color = color;
	dev->dirty_flags |= DIRTY_FLAG_BLEND_CONSTANTS;
}

static void d12_set_stencil_ref(gs_device_d3d12* dev, u8 ref)
{
	if (dev->stencil_ref == ref)
		return;

	dev->stencil_ref = ref;
	dev->dirty_flags |= DIRTY_FLAG_STENCIL_REF;
}

static void d12_set_viewport(gs_device_d3d12* dev, const D3D12_VIEWPORT* viewport)
{
	if (memcmp(viewport, &dev->viewport, sizeof(dev->viewport)) == 0)
		return;

	memcpy(&dev->viewport, viewport, sizeof(dev->viewport));
	dev->dirty_flags |= DIRTY_FLAG_VIEWPORT;
}

static void d12_set_scissor(gs_device_d3d12* dev, const gs_recti* scissor)
{
	if (d12_recti_eq(&dev->scissor, scissor))
		return;

	dev->scissor = *scissor;
	dev->dirty_flags |= DIRTY_FLAG_SCISSOR;
}

static void d12_set_pipeline(gs_device_d3d12* dev, const ID3D12PipelineState* pipeline)
{
	if (dev->current_pipeline == pipeline)
		return;

	dev->current_pipeline = pipeline;
	dev->dirty_flags |= DIRTY_FLAG_PIPELINE;
}

static void d12_invalidate_cached_state(gs_device_d3d12* dev)
{
	dev->dirty_flags |= DIRTY_BASE_STATE | DIRTY_TFX_STATE | DIRTY_UTILITY_STATE | DIRTY_CONSTANT_BUFFER_STATE;
	dev->current_root_signature = GS_D3D12_ROOT_SIGNATURE_UNDEFINED;
	gs_d3d12_descriptor_handle_clear(&dev->utility_texture_cpu);
	gs_d3d12_descriptor_handle_clear(&dev->utility_texture_gpu);
	gs_d3d12_descriptor_handle_clear(&dev->utility_sampler_cpu);
	gs_d3d12_descriptor_handle_clear(&dev->utility_sampler_gpu);
	gs_d3d12_descriptor_handle_clear(&dev->tfx_textures_handle_gpu);
	gs_d3d12_descriptor_handle_clear(&dev->tfx_samplers_handle_gpu);
	gs_d3d12_descriptor_handle_clear(&dev->tfx_rt_textures_handle_gpu);
}

/* ==== render passes ==== */

static void d12_lookup_native_format(unsigned format, DXGI_FORMAT* d3d_format, DXGI_FORMAT* srv_format,
	DXGI_FORMAT* rtv_format, DXGI_FORMAT* dsv_format)
{
	static const DXGI_FORMAT s_format_mapping[GS_TEX_BC7 + 1][4] = {
		{DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN}, /* Invalid */
		{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
			DXGI_FORMAT_UNKNOWN}, /* Color */
		{DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM,
			DXGI_FORMAT_UNKNOWN}, /* ColorClip */
		{DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_UNKNOWN,
			DXGI_FORMAT_D32_FLOAT_S8X24_UINT}, /* DepthStencil */
		{DXGI_FORMAT_A8_UNORM, DXGI_FORMAT_A8_UNORM, DXGI_FORMAT_A8_UNORM, DXGI_FORMAT_UNKNOWN}, /* UNorm8 */
		{DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_UNKNOWN}, /* UInt16 */
		{DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_UNKNOWN}, /* UInt32 */
		{DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN}, /* PrimID */
		{DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN}, /* BC1 */
		{DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN}, /* BC2 */
		{DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN}, /* BC3 */
		{DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN}, /* BC7 */
	};
	const DXGI_FORMAT* mapping = s_format_mapping[format];
	if (d3d_format)
		*d3d_format = mapping[0];
	if (srv_format)
		*srv_format = mapping[1];
	if (rtv_format)
		*rtv_format = mapping[2];
	if (dsv_format)
		*dsv_format = mapping[3];
}

static void d12_end_render_pass(gs_device_d3d12* dev)
{
	if (!dev->in_render_pass)
		return;

	dev->in_render_pass = false;
	/* to render again, we need to reset OM */
	dev->dirty_flags |= DIRTY_FLAG_RENDER_TARGET;
	ID3D12GraphicsCommandList4_EndRenderPass(d12_cmdlist(dev));
}

static void d12_begin_render_pass(gs_device_d3d12* dev, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE color_begin,
	D3D12_RENDER_PASS_ENDING_ACCESS_TYPE color_end, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE depth_begin,
	D3D12_RENDER_PASS_ENDING_ACCESS_TYPE depth_end, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE stencil_begin,
	D3D12_RENDER_PASS_ENDING_ACCESS_TYPE stencil_end, const float* clear_color, float clear_depth, u8 clear_stencil)
{
	D3D12_RENDER_PASS_RENDER_TARGET_DESC rt;
	D3D12_RENDER_PASS_DEPTH_STENCIL_DESC ds;

	if (dev->in_render_pass)
		d12_end_render_pass(dev);

	/* we're setting the RT here. */
	dev->dirty_flags &= ~DIRTY_FLAG_RENDER_TARGET;
	dev->in_render_pass = true;

	memset(&rt, 0, sizeof(rt));
	if (dev->current_render_target)
	{
		rt.cpuDescriptor        = d12_cpu(&dev->current_render_target->write_descriptor);
		rt.EndingAccess.Type    = color_end;
		rt.BeginningAccess.Type = color_begin;
		if (color_begin == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
		{
			d12_lookup_native_format(dev->current_render_target->base.m_format, NULL,
				&rt.BeginningAccess.Clear.ClearValue.Format, NULL, NULL);
			memcpy(rt.BeginningAccess.Clear.ClearValue.Color, clear_color, 4 * sizeof(float));
		}
	}

	memset(&ds, 0, sizeof(ds));
	if (dev->current_depth_target)
	{
		ds.cpuDescriptor             = d12_cpu(&dev->current_depth_target->write_descriptor);
		ds.DepthEndingAccess.Type    = depth_end;
		ds.DepthBeginningAccess.Type = depth_begin;
		if (depth_begin == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
		{
			d12_lookup_native_format(dev->current_depth_target->base.m_format, NULL, NULL, NULL,
				&ds.DepthBeginningAccess.Clear.ClearValue.Format);
			ds.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth = clear_depth;
		}
		ds.StencilEndingAccess.Type    = stencil_end;
		ds.StencilBeginningAccess.Type = stencil_begin;
		if (stencil_begin == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
		{
			d12_lookup_native_format(dev->current_depth_target->base.m_format, NULL, NULL, NULL,
				&ds.StencilBeginningAccess.Clear.ClearValue.Format);
			ds.StencilBeginningAccess.Clear.ClearValue.DepthStencil.Stencil = clear_stencil;
		}
	}

	ID3D12GraphicsCommandList4_BeginRenderPass(d12_cmdlist(dev), dev->current_render_target ? 1 : 0,
		dev->current_render_target ? &rt : NULL, dev->current_depth_target ? &ds : NULL, D3D12_RENDER_PASS_FLAG_NONE);
}

static const float s_d12_zero4[4] = {0.0f, 0.0f, 0.0f, 0.0f};

/* (BeginRenderPass with every access but the two given NO_ACCESS) */
static void d12_begin_render_pass_color(gs_device_d3d12* dev, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE color_begin,
	D3D12_RENDER_PASS_ENDING_ACCESS_TYPE color_end, const float* clear_color)
{
	d12_begin_render_pass(dev, color_begin, color_end, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
		D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
		D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS, clear_color, 0.0f, 0);
}

static void d12_om_set_render_targets(gs_device_d3d12* dev, gs_texture_d3d12* rt, gs_texture_d3d12* ds,
	const gs_recti* scissor)
{
	const gs_texture* sized;
	D3D12_VIEWPORT vp;

	if (dev->current_render_target != rt || dev->current_depth_target != ds)
	{
		/* framebuffer change */
		d12_end_render_pass(dev);
	}
	else if (dev->in_render_pass)
	{
		/* Framebuffer unchanged, but check for clears. Have to restart
		 * render pass, unlike Vulkan. We'll take care of issuing the
		 * actual clear there, because we have to start one anyway. */
		if (rt && rt->base.m_state != GS_TEXSTATE_DIRTY)
		{
			if (rt->base.m_state == GS_TEXSTATE_CLEARED)
				d12_end_render_pass(dev);
			else
				rt->base.m_state = GS_TEXSTATE_DIRTY;
		}
		if (ds && ds->base.m_state != GS_TEXSTATE_DIRTY)
		{
			if (ds->base.m_state == GS_TEXSTATE_CLEARED)
				d12_end_render_pass(dev);
			else
				ds->base.m_state = GS_TEXSTATE_DIRTY;
		}
	}

	dev->current_render_target = rt;
	dev->current_depth_target  = ds;

	if (!dev->in_render_pass)
	{
		if (rt)
			d12_transition(rt, dev, D3D12_RESOURCE_STATE_RENDER_TARGET);
		if (ds)
			d12_transition(ds, dev, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}

	/* This is used to set/initialize the framebuffer for tfx rendering. */
	sized       = rt ? &rt->base : &ds->base;
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width    = (float)sized->m_size.x;
	vp.Height   = (float)sized->m_size.y;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;

	d12_set_viewport(dev, &vp);
	d12_set_scissor(dev, scissor);
}

/* ==== samplers and descriptors ==== */

static bool d12_get_sampler(gs_device_d3d12* dev, gs_d3d12_descriptor_handle* cpu_handle, gs_hw_sampler_selector ss)
{
	static const D3D12_FILTER filters[8] = {
		D3D12_FILTER_MIN_MAG_MIP_POINT,               /* 000 / min=point,mag=point,mip=point */
		D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT,        /* 001 / min=linear,mag=point,mip=point */
		D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT,  /* 010 / min=point,mag=linear,mip=point */
		D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,        /* 011 / min=linear,mag=linear,mip=point */
		D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR,        /* 100 / min=point,mag=point,mip=linear */
		D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR, /* 101 / min=linear,mag=point,mip=linear */
		D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR,        /* 110 / min=point,mag=linear,mip=linear */
		D3D12_FILTER_MIN_MAG_MIP_LINEAR,              /* 111 / min=linear,mag=linear,mip=linear */
	};
	gs_d3d12_descriptor_handle* slot = &dev->samplers[ss.key & 0xFF];
	const int anisotropy             = GSConfig.MaxAnisotropy;
	D3D12_SAMPLER_DESC sd;

	if (D12_HANDLE_VALID(*slot))
	{
		*cpu_handle = *slot;
		return true;
	}

	memset(&sd, 0, sizeof(sd));
	if (anisotropy > 1 && ss.aniso)
		sd.Filter = D3D12_FILTER_ANISOTROPIC;
	else
	{
		/* (gs_hw_sampler_selector's: the minification filter's linear, the
		 * mipmaps filtered, linearly between levels) */
		const bool mip_filtering = ss.triln >= Nearest_Mipmap_Nearest;
		const bool min_linear    = !mip_filtering ? ss.biln != 0 : ss.triln >= Linear_Mipmap_Nearest;
		const bool mip_linear    = ss.triln == Nearest_Mipmap_Linear || ss.triln == Linear_Mipmap_Linear;
		sd.Filter                = filters[((mip_linear ? 1 : 0) << 2) | ((ss.biln ? 1 : 0) << 1) | (min_linear ? 1 : 0)];
	}

	sd.AddressU       = ss.tau ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sd.AddressV       = ss.tav ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sd.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sd.MinLOD         = 0.0f;
	sd.MaxLOD         = (ss.lodclamp || !(ss.triln >= Nearest_Mipmap_Nearest)) ? 0.25f : FLT_MAX;
	sd.MaxAnisotropy  = (UINT)(anisotropy < 1 ? 1 : anisotropy > 16 ? 16 : anisotropy);
	sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;

	if (!gs_d3d12_descriptor_heap_manager_allocate(&dev->sampler_heap_manager, cpu_handle))
		return false;

	ID3D12Device_CreateSampler(dev->device, &sd, d12_cpu(cpu_handle));
	*slot = *cpu_handle;
	return true;
}

static bool d12_get_texture_group_descriptors(gs_device_d3d12* dev, gs_d3d12_descriptor_handle* gpu_handle,
	const gs_d3d12_descriptor_handle* cpu_handles, u32 count)
{
	D3D12_CPU_DESCRIPTOR_HANDLE dst_handle;
	D3D12_CPU_DESCRIPTOR_HANDLE src_handles[GS_D3D12_NUM_TFX_TEXTURES];
	UINT src_sizes[GS_D3D12_NUM_TFX_TEXTURES];
	UINT dst_size = count;
	u32 i;

	if (!gs_d3d12_descriptor_allocator_allocate(&dev->command_lists[dev->current_command_list].descriptor_allocator,
			count, gpu_handle))
		return false;

	if (count == 1)
	{
		ID3D12Device_CopyDescriptorsSimple(dev->device, 1, d12_cpu(gpu_handle), d12_cpu(&cpu_handles[0]),
			D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		return true;
	}

	dst_handle = d12_cpu(gpu_handle);
	for (i = 0; i < count; i++)
	{
		src_handles[i] = d12_cpu(&cpu_handles[i]);
		src_sizes[i]   = 1;
	}
	ID3D12Device_CopyDescriptors(dev->device, 1, &dst_handle, &dst_size, count, src_handles, src_sizes,
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	return true;
}

static void d12_ps_set_shader_resource(gs_device_d3d12* dev, int i, gs_texture* sr, bool check_state)
{
	gs_d3d12_descriptor_handle handle;
	if (sr)
	{
		gs_texture_d3d12* dtex = D12_TEX(sr);
		if (check_state)
		{
			/* Ending render pass due to resource transition */
			if (dtex->resource_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE && dev->in_render_pass)
				d12_end_render_pass(dev);

			d12_commit_clear(dtex);
			d12_transition(dtex, dev, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		dtex->use_fence_counter = dev->current_fence_value;
		handle                  = dtex->srv_descriptor;
	}
	else
		handle = dev->null_texture->srv_descriptor;

	if (D12_HANDLE_EQ(dev->tfx_textures[i], handle))
		return;

	dev->tfx_textures[i] = handle;
	dev->dirty_flags |= (i < 2) ? DIRTY_FLAG_TFX_TEXTURES : DIRTY_FLAG_TFX_RT_TEXTURES;
}

static void d12_ps_set_sampler(gs_device_d3d12* dev, gs_hw_sampler_selector sel)
{
	if (dev->tfx_sampler_sel == sel.key)
		return;

	d12_get_sampler(dev, &dev->tfx_sampler, sel);
	dev->tfx_sampler_sel = sel.key;
	dev->dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS;
}

static void d12_set_utility_root_signature(gs_device_d3d12* dev)
{
	if (dev->current_root_signature == GS_D3D12_ROOT_SIGNATURE_UTILITY)
		return;

	dev->current_root_signature = GS_D3D12_ROOT_SIGNATURE_UTILITY;
	dev->dirty_flags |= DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE | DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE | DIRTY_FLAG_PIPELINE;
	ID3D12GraphicsCommandList4_SetGraphicsRootSignature(d12_cmdlist(dev), dev->utility_root_signature);
}

static void d12_set_utility_texture(gs_device_d3d12* dev, gs_texture* dtex, const gs_d3d12_descriptor_handle* sampler)
{
	gs_d3d12_descriptor_handle handle;
	/* (sampler may be one of the fields reassigned on the way) */
	const gs_d3d12_descriptor_handle smp = *sampler;

	if (dtex)
	{
		gs_texture_d3d12* d12tex = D12_TEX(dtex);
		d12_commit_clear(d12tex);
		d12_transition(d12tex, dev, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		d12tex->use_fence_counter = dev->current_fence_value;
		handle                    = d12tex->srv_descriptor;
	}
	else
		handle = dev->null_texture->srv_descriptor;

	if (!D12_HANDLE_EQ(dev->utility_texture_cpu, handle))
	{
		dev->utility_texture_cpu = handle;
		dev->dirty_flags |= DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE;

		if (!d12_get_texture_group_descriptors(dev, &dev->utility_texture_gpu, &handle, 1))
		{
			d12_execute_command_list_and_restart_render_pass(dev, false);
			d12_set_utility_texture(dev, dtex, &smp);
			return;
		}
	}

	if (!D12_HANDLE_EQ(dev->utility_sampler_cpu, smp))
	{
		dev->utility_sampler_cpu = smp;
		dev->dirty_flags |= DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE;

		if (!gs_d3d12_sampler_allocator_lookup_single(&dev->command_lists[dev->current_command_list].sampler_allocator,
				&dev->utility_sampler_gpu, &smp))
		{
			d12_execute_command_list_and_restart_render_pass(dev, false);
			d12_set_utility_texture(dev, dtex, &smp);
			return;
		}
	}
}

static void d12_set_utility_push_constants(gs_device_d3d12* dev, const void* data, u32 size)
{
	ID3D12GraphicsCommandList4_SetGraphicsRoot32BitConstants(d12_cmdlist(dev), GS_D3D12_UTILITY_PARAM_PUSH_CONSTANTS,
		(size + 3) / sizeof(u32), data, 0);
}

static void d12_unbind_texture(gs_device_d3d12* dev, gs_texture_d3d12* tex)
{
	u32 i;
	for (i = 0; i < GS_D3D12_NUM_TOTAL_TFX_TEXTURES; i++)
	{
		if (D12_HANDLE_EQ(dev->tfx_textures[i], tex->srv_descriptor))
		{
			dev->tfx_textures[i] = dev->null_texture->srv_descriptor;
			dev->dirty_flags |= DIRTY_FLAG_TFX_TEXTURES;
		}
	}
	if (dev->current_render_target == tex)
	{
		d12_end_render_pass(dev);
		dev->current_render_target = NULL;
	}
	if (dev->current_depth_target == tex)
	{
		d12_end_render_pass(dev);
		dev->current_depth_target = NULL;
	}
}

/* ==== state onto the command list ==== */

static void d12_apply_base_state(gs_device_d3d12* dev, u32 flags, ID3D12GraphicsCommandList4* cmdlist)
{
	if (flags & DIRTY_FLAG_VERTEX_BUFFER)
		ID3D12GraphicsCommandList4_IASetVertexBuffers(cmdlist, 0, 1, &dev->vertex_buffer);

	if (flags & DIRTY_FLAG_INDEX_BUFFER)
		ID3D12GraphicsCommandList4_IASetIndexBuffer(cmdlist, &dev->index_buffer);

	if (flags & DIRTY_FLAG_PRIMITIVE_TOPOLOGY)
		ID3D12GraphicsCommandList4_IASetPrimitiveTopology(cmdlist, dev->primitive_topology);

	if (flags & DIRTY_FLAG_PIPELINE)
		ID3D12GraphicsCommandList4_SetPipelineState(cmdlist, (ID3D12PipelineState*)dev->current_pipeline);

	if (flags & DIRTY_FLAG_VIEWPORT)
		ID3D12GraphicsCommandList4_RSSetViewports(cmdlist, 1, &dev->viewport);

	if (flags & DIRTY_FLAG_SCISSOR)
	{
		D3D12_RECT rc;
		rc.left   = dev->scissor.x;
		rc.top    = dev->scissor.y;
		rc.right  = dev->scissor.z;
		rc.bottom = dev->scissor.w;
		ID3D12GraphicsCommandList4_RSSetScissorRects(cmdlist, 1, &rc);
	}

	if (flags & DIRTY_FLAG_BLEND_CONSTANTS)
	{
		const float f = (float)dev->blend_constant_color / 128.0f;
		float col[4];
		col[0] = col[1] = col[2] = col[3] = f;
		ID3D12GraphicsCommandList4_OMSetBlendFactor(cmdlist, col);
	}

	if (flags & DIRTY_FLAG_STENCIL_REF)
		ID3D12GraphicsCommandList4_OMSetStencilRef(cmdlist, dev->stencil_ref);

	if (flags & DIRTY_FLAG_RENDER_TARGET)
	{
		if (dev->current_render_target)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE rtv = d12_cpu(&dev->current_render_target->write_descriptor);
			D3D12_CPU_DESCRIPTOR_HANDLE dsv;
			if (dev->current_depth_target)
				dsv = d12_cpu(&dev->current_depth_target->write_descriptor);
			ID3D12GraphicsCommandList4_OMSetRenderTargets(cmdlist, 1, &rtv, FALSE,
				dev->current_depth_target ? &dsv : NULL);
		}
		else if (dev->current_depth_target)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE dsv = d12_cpu(&dev->current_depth_target->write_descriptor);
			ID3D12GraphicsCommandList4_OMSetRenderTargets(cmdlist, 0, NULL, FALSE, &dsv);
		}
	}
}

static bool d12_apply_tfx_state(gs_device_d3d12* dev, bool already_execed)
{
	ID3D12GraphicsCommandList4* cmdlist;
	u32 flags;

	if (dev->current_root_signature == GS_D3D12_ROOT_SIGNATURE_TFX && dev->dirty_flags == 0)
		return true;

	flags = dev->dirty_flags;
	dev->dirty_flags &= ~(DIRTY_TFX_STATE | DIRTY_CONSTANT_BUFFER_STATE);

	/* do cbuffer first, because it's the most likely to cause an exec */
	if (flags & DIRTY_FLAG_VS_CONSTANT_BUFFER)
	{
		if (!gs_d3d12_stream_buffer_reserve(&dev->vertex_constant_buffer, sizeof(dev->vs_cb_cache),
				D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT))
		{
			if (already_execed)
			{
				log_cb(RETRO_LOG_ERROR, "Failed to reserve vertex uniform space\n");
				return false;
			}

			d12_execute_command_list_and_restart_render_pass(dev, false);
			return d12_apply_tfx_state(dev, true);
		}

		memcpy(dev->vertex_constant_buffer.host_pointer + dev->vertex_constant_buffer.current_offset,
			&dev->vs_cb_cache, sizeof(dev->vs_cb_cache));
		dev->tfx_constant_buffers[0] =
			dev->vertex_constant_buffer.gpu_pointer + dev->vertex_constant_buffer.current_offset;
		gs_d3d12_stream_buffer_commit(&dev->vertex_constant_buffer, sizeof(dev->vs_cb_cache));
		flags |= DIRTY_FLAG_VS_CONSTANT_BUFFER_BINDING;
	}

	if (flags & DIRTY_FLAG_PS_CONSTANT_BUFFER)
	{
		if (!gs_d3d12_stream_buffer_reserve(&dev->pixel_constant_buffer, sizeof(dev->ps_cb_cache),
				D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT))
		{
			if (already_execed)
			{
				log_cb(RETRO_LOG_ERROR, "Failed to reserve pixel uniform space\n");
				return false;
			}

			d12_execute_command_list_and_restart_render_pass(dev, false);
			return d12_apply_tfx_state(dev, true);
		}

		memcpy(dev->pixel_constant_buffer.host_pointer + dev->pixel_constant_buffer.current_offset, &dev->ps_cb_cache,
			sizeof(dev->ps_cb_cache));
		dev->tfx_constant_buffers[1] =
			dev->pixel_constant_buffer.gpu_pointer + dev->pixel_constant_buffer.current_offset;
		gs_d3d12_stream_buffer_commit(&dev->pixel_constant_buffer, sizeof(dev->ps_cb_cache));
		flags |= DIRTY_FLAG_PS_CONSTANT_BUFFER_BINDING;
	}

	if (flags & DIRTY_FLAG_TFX_SAMPLERS)
	{
		if (!gs_d3d12_sampler_allocator_lookup_single(&dev->command_lists[dev->current_command_list].sampler_allocator,
				&dev->tfx_samplers_handle_gpu, &dev->tfx_sampler))
		{
			d12_execute_command_list_and_restart_render_pass(dev, false);
			return d12_apply_tfx_state(dev, true);
		}

		flags |= DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE;
	}

	if (flags & DIRTY_FLAG_TFX_TEXTURES)
	{
		if (!d12_get_texture_group_descriptors(dev, &dev->tfx_textures_handle_gpu, dev->tfx_textures, 2))
		{
			d12_execute_command_list_and_restart_render_pass(dev, false);
			return d12_apply_tfx_state(dev, true);
		}

		flags |= DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE;
	}

	if (flags & DIRTY_FLAG_TFX_RT_TEXTURES)
	{
		if (!d12_get_texture_group_descriptors(dev, &dev->tfx_rt_textures_handle_gpu, dev->tfx_textures + 2, 2))
		{
			d12_execute_command_list_and_restart_render_pass(dev, false);
			return d12_apply_tfx_state(dev, true);
		}

		flags |= DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE_2;
	}

	cmdlist = d12_cmdlist(dev);

	if (dev->current_root_signature != GS_D3D12_ROOT_SIGNATURE_TFX)
	{
		dev->current_root_signature = GS_D3D12_ROOT_SIGNATURE_TFX;
		flags |= DIRTY_FLAG_VS_CONSTANT_BUFFER_BINDING | DIRTY_FLAG_PS_CONSTANT_BUFFER_BINDING
			| DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE | DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE
			| DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE_2 | DIRTY_FLAG_PIPELINE;
		ID3D12GraphicsCommandList4_SetGraphicsRootSignature(cmdlist, dev->tfx_root_signature);
	}

	if (flags & DIRTY_FLAG_VS_CONSTANT_BUFFER_BINDING)
		ID3D12GraphicsCommandList4_SetGraphicsRootConstantBufferView(cmdlist, GS_D3D12_TFX_PARAM_VS_CBV,
			dev->tfx_constant_buffers[0]);
	if (flags & DIRTY_FLAG_PS_CONSTANT_BUFFER_BINDING)
		ID3D12GraphicsCommandList4_SetGraphicsRootConstantBufferView(cmdlist, GS_D3D12_TFX_PARAM_PS_CBV,
			dev->tfx_constant_buffers[1]);
	if (flags & DIRTY_FLAG_VS_VERTEX_BUFFER_BINDING)
		ID3D12GraphicsCommandList4_SetGraphicsRootShaderResourceView(cmdlist, GS_D3D12_TFX_PARAM_VS_SRV,
			dev->vertex_stream_buffer.gpu_pointer + (u64)dev->base.m_vertex.start * 32u /* (GSVertex) */);
	if (flags & DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE)
		ID3D12GraphicsCommandList4_SetGraphicsRootDescriptorTable(cmdlist, GS_D3D12_TFX_PARAM_PS_TEXTURES,
			d12_gpu(&dev->tfx_textures_handle_gpu));
	if (flags & DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE)
		ID3D12GraphicsCommandList4_SetGraphicsRootDescriptorTable(cmdlist, GS_D3D12_TFX_PARAM_PS_SAMPLERS,
			d12_gpu(&dev->tfx_samplers_handle_gpu));
	if (flags & DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE_2)
		ID3D12GraphicsCommandList4_SetGraphicsRootDescriptorTable(cmdlist, GS_D3D12_TFX_PARAM_PS_RT_TEXTURES,
			d12_gpu(&dev->tfx_rt_textures_handle_gpu));

	d12_apply_base_state(dev, flags, cmdlist);
	return true;
}

static bool d12_apply_utility_state(gs_device_d3d12* dev)
{
	ID3D12GraphicsCommandList4* cmdlist;
	u32 flags;

	/* (a utility pipeline that could not be built: no draw) */
	if (!dev->current_pipeline)
		return false;
	if (dev->current_root_signature == GS_D3D12_ROOT_SIGNATURE_UTILITY && dev->dirty_flags == 0)
		return true;

	flags = dev->dirty_flags;
	dev->dirty_flags &= ~DIRTY_UTILITY_STATE;

	cmdlist = d12_cmdlist(dev);

	if (dev->current_root_signature != GS_D3D12_ROOT_SIGNATURE_UTILITY)
	{
		dev->current_root_signature = GS_D3D12_ROOT_SIGNATURE_UTILITY;
		flags |= DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE | DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE | DIRTY_FLAG_PIPELINE;
		ID3D12GraphicsCommandList4_SetGraphicsRootSignature(cmdlist, dev->utility_root_signature);
	}

	if (flags & DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE)
		ID3D12GraphicsCommandList4_SetGraphicsRootDescriptorTable(cmdlist, GS_D3D12_UTILITY_PARAM_PS_TEXTURES,
			d12_gpu(&dev->utility_texture_gpu));
	if (flags & DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE)
		ID3D12GraphicsCommandList4_SetGraphicsRootDescriptorTable(cmdlist, GS_D3D12_UTILITY_PARAM_PS_SAMPLERS,
			d12_gpu(&dev->utility_sampler_gpu));

	d12_apply_base_state(dev, flags, cmdlist);
	return true;
}

/* ==== executing ==== */

static void d12_execute_command_list(gs_device_d3d12* dev, bool wait_for_completion)
{
	d12_end_render_pass(dev);
	d12_context_execute_command_list(dev, wait_for_completion);
	d12_invalidate_cached_state(dev);
}

static void d12_execute_command_list_and_restart_render_pass(gs_device_d3d12* dev, bool wait_for_completion)
{
	const bool was_in_render_pass = dev->in_render_pass;
	d12_end_render_pass(dev);
	d12_context_execute_command_list(dev, wait_for_completion);
	d12_invalidate_cached_state(dev);

	if (was_in_render_pass)
	{
		/* rebind everything except RT, because the RP does that for us */
		d12_apply_base_state(dev, dev->dirty_flags & ~DIRTY_FLAG_RENDER_TARGET, d12_cmdlist(dev));
		dev->dirty_flags &= ~DIRTY_BASE_STATE;

		/* restart render pass */
		d12_begin_render_pass(dev,
			dev->current_render_target ? D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE
									   : D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
			dev->current_render_target ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE
									   : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			dev->current_depth_target ? D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE
									  : D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
			dev->current_depth_target ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE
									  : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			s_d12_zero4, 0.0f, 0);
	}
}

/* ==== uploads ==== */

static void d12_draw_primitive(gs_device_d3d12* dev)
{
	ID3D12GraphicsCommandList4_DrawInstanced(d12_cmdlist(dev), dev->base.m_vertex.count, 1, dev->base.m_vertex.start, 0);
}

static void d12_draw_indexed_primitive(gs_device_d3d12* dev)
{
	ID3D12GraphicsCommandList4_DrawIndexedInstanced(d12_cmdlist(dev), dev->base.m_index.count, 1,
		dev->base.m_index.start, (INT)dev->base.m_vertex.start, 0);
}

static void d12_ia_set_vertex_buffer(gs_device_d3d12* dev, const void* vertex, size_t stride, size_t count)
{
	gs_d3d12_stream_buffer* sb = &dev->vertex_stream_buffer;
	const u32 size             = (u32)stride * (u32)count;
	if (!gs_d3d12_stream_buffer_reserve(sb, size, (u32)stride))
	{
		d12_execute_command_list_and_restart_render_pass(dev, false);
		if (!gs_d3d12_stream_buffer_reserve(sb, size, (u32)stride))
		{
			/* Nothing reserved the space, but the host pointer still points
			 * into the stream buffer, at a region belonging to something
			 * else. Leave the count at zero so the draw that follows submits
			 * nothing, rather than reading whatever the stale offsets point
			 * at. */
			d12_report_recurring("Reserving space for vertices", E_OUTOFMEMORY);
			dev->base.m_vertex.count = 0;
			return;
		}
	}

	dev->base.m_vertex.start = (u32)(sb->current_offset / stride);
	dev->base.m_vertex.count = (u32)count;
	d12_set_vertex_buffer(dev, sb->gpu_pointer, sb->size, stride);

	memcpy(sb->host_pointer + sb->current_offset, vertex, count * stride);
	gs_d3d12_stream_buffer_commit(sb, size);
}

static void d12_ia_set_index_buffer(gs_device_d3d12* dev, const void* index, size_t count)
{
	gs_d3d12_stream_buffer* sb = &dev->index_stream_buffer;
	const u32 size             = (u32)sizeof(u16) * (u32)count;
	if (!gs_d3d12_stream_buffer_reserve(sb, size, sizeof(u16)))
	{
		d12_execute_command_list_and_restart_render_pass(dev, false);
		if (!gs_d3d12_stream_buffer_reserve(sb, size, sizeof(u16)))
		{
			d12_report_recurring("Reserving space for indices", E_OUTOFMEMORY);
			dev->base.m_index.count = 0;
			return;
		}
	}

	dev->base.m_index.start = (u32)(sb->current_offset / sizeof(u16));
	dev->base.m_index.count = (u32)count;
	d12_set_index_buffer(dev, sb->gpu_pointer, sb->size, DXGI_FORMAT_R16_UINT);

	memcpy(sb->host_pointer + sb->current_offset, index, size);
	gs_d3d12_stream_buffer_commit(sb, size);
}

/* ==== the utility pipelines ==== */

static void d12_set_vertex(d12_vertex_pt1* v, float x, float y, float s, float t)
{
	memset(v, 0, sizeof(*v));
	v->p[0] = x;
	v->p[1] = y;
	v->p[2] = 0.5f;
	v->p[3] = 1.0f;
	v->t[0] = s;
	v->t[1] = t;
}

static void d12_add_utility_vertex_attributes(gs_d3d12_gp_builder* gpb)
{
	gs_d3d12_gp_add_vertex_attribute(gpb, "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0);
	gs_d3d12_gp_add_vertex_attribute(gpb, "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16);
	gs_d3d12_gp_add_vertex_attribute(gpb, "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28);
	gs_d3d12_gp_set_topology_type(gpb, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
}

/* The macros of a D3D12 shader: the model's, and DX12 */
static void d12_macros_init(gs_d3d_macros* m, D3D_FEATURE_LEVEL fl)
{
	gs_d3d_macros_init(m, fl);
	gs_d3d_macros_add(m, "DX12", "1");
}

/* A numbered entry point (prefix and a number) into out[32] */
static const char* d12_entry_name(char* out, const char* prefix, unsigned i)
{
	const size_t len = strlen(prefix);
	char digits[12];
	size_t n = 0;
	memcpy(out, prefix, len);
	do
	{
		digits[n++] = (char)('0' + (i % 10));
		i /= 10;
	} while (i);
	{
		size_t k;
		for (k = 0; k < n; k++)
			out[len + k] = digits[n - 1 - k];
	}
	out[len + n] = '\0';
	return out;
}

/* The pixel shader of a utility pipeline: its source, and its entry point
 * (written to entry, 32 bytes, when it has to be made). The vertex shader
 * is convert's vs_main for every one. */
static const char* d12_utility_pixel_shader(u32 slot, const char** source, char* entry)
{
	*source = convert_fx_shader_raw;
	if (slot < GS_D3D12_UTIL_COLOR_COPY)
		return gs_shader_convert_name((int)(slot - GS_D3D12_UTIL_CONVERT));
	if (slot < GS_D3D12_UTIL_HDR_SETUP)
		return gs_shader_convert_name((slot - GS_D3D12_UTIL_COLOR_COPY) < 16 ? GS_SHADER_CONVERT_COPY
																		   : GS_SHADER_CONVERT_RTA_CORRECTION);
	if (slot < GS_D3D12_UTIL_HDR_FINISH)
		return gs_shader_convert_name(GS_SHADER_CONVERT_COLCLIP_INIT);
	if (slot < GS_D3D12_UTIL_DATE_SETUP)
		return gs_shader_convert_name(GS_SHADER_CONVERT_COLCLIP_RESOLVE);
	if (slot < GS_D3D12_UTIL_INTERLACE)
		return d12_entry_name(entry, "ps_stencil_image_init_", (slot - GS_D3D12_UTIL_DATE_SETUP) % 4);
	if (slot < GS_D3D12_UTIL_MERGE)
	{
		*source = interlace_fx_shader_raw;
		return d12_entry_name(entry, "ps_main", slot - GS_D3D12_UTIL_INTERLACE);
	}
	*source = merge_fx_shader_raw;
	return d12_entry_name(entry, "ps_main", slot - GS_D3D12_UTIL_MERGE);
}

/* The description of utility pipeline @slot. The convert family shared
 * one builder, each pipeline made from what the ones before it left set,
 * so its calls are made again in the same order up to @slot: the
 * description comes out byte for byte as it did, and with it the key the
 * driver's cached blob is found under. Every pixel shader set on the way
 * is @ps, which is the one in force when @slot's is made. Reads nothing of
 * the device's but @rs: the warm-up thread calls it too. */
static bool d12_utility_pipeline_desc(gs_d3d12_gp_builder* gpb, u32 slot, ID3D12RootSignature* rs, const ID3DBlob* vs,
	const ID3DBlob* ps)
{
	int i;
	u32 j, ds, datm;

	if (slot >= GS_D3D12_UTIL_INTERLACE)
	{
		const bool merge = slot >= GS_D3D12_UTIL_MERGE;
		d12_add_utility_vertex_attributes(gpb);
		gs_d3d12_gp_set_root_signature(gpb, rs);
		d12_gp_no_cull(gpb);
		d12_gp_no_depth(gpb);
		if (!merge)
			gs_d3d12_gp_set_no_blending(gpb);
		gs_d3d12_gp_set_render_target(gpb, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
		d12_gp_set_vs(gpb, vs);
		d12_gp_set_ps(gpb, ps);
		if (merge)
			gs_d3d12_gp_set_blend(gpb, 0, 1, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD,
				D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_COLOR_WRITE_ENABLE_ALL);
		return slot < GS_D3D12_UTIL_COUNT;
	}

	gs_d3d12_gp_set_root_signature(gpb, rs);
	d12_add_utility_vertex_attributes(gpb);
	d12_gp_no_cull(gpb);
	gs_d3d12_gp_set_no_blending(gpb);
	d12_gp_set_vs(gpb, vs);

	for (i = GS_SHADER_CONVERT_COPY; i < GS_SHADER_CONVERT_COUNT; i++)
	{
		const bool depth = gs_shader_convert_has_depth_output(i);
		const u32 index  = (u32)i;

		switch (i)
		{
			case GS_SHADER_CONVERT_RGBA8_TO_16_BITS:
			case GS_SHADER_CONVERT_FLOAT32_TO_16_BITS:
				gs_d3d12_gp_set_render_target(gpb, 0, DXGI_FORMAT_R16_UINT);
				gs_d3d12_gp_set_depth_stencil_format(gpb, DXGI_FORMAT_UNKNOWN);
				break;
			case GS_SHADER_CONVERT_FLOAT32_TO_32_BITS:
				gs_d3d12_gp_set_render_target(gpb, 0, DXGI_FORMAT_R32_UINT);
				gs_d3d12_gp_set_depth_stencil_format(gpb, DXGI_FORMAT_UNKNOWN);
				break;
			case GS_SHADER_CONVERT_DATM_0:
			case GS_SHADER_CONVERT_DATM_1:
			case GS_SHADER_CONVERT_DATM_0_RTA_CORRECTION:
			case GS_SHADER_CONVERT_DATM_1_RTA_CORRECTION:
				gs_d3d12_gp_clear_render_targets(gpb);
				gs_d3d12_gp_set_depth_stencil_format(gpb, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
				break;
			default:
				if (depth)
					gs_d3d12_gp_clear_render_targets(gpb);
				else
					gs_d3d12_gp_set_render_target(gpb, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
				gs_d3d12_gp_set_depth_stencil_format(gpb, depth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_UNKNOWN);
				break;
		}

		if (d12_is_datm_convert_shader(i))
		{
			D3D12_DEPTH_STENCILOP_DESC sos;
			sos.StencilFailOp      = D3D12_STENCIL_OP_KEEP;
			sos.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
			sos.StencilPassOp      = D3D12_STENCIL_OP_REPLACE;
			sos.StencilFunc        = D3D12_COMPARISON_FUNC_ALWAYS;
			gs_d3d12_gp_set_stencil(gpb, 1, 1, 1, &sos, &sos);
			gs_d3d12_gp_set_depth(gpb, 0, 0, D3D12_COMPARISON_FUNC_ALWAYS);
		}
		else
		{
			gs_d3d12_gp_set_depth(gpb, depth, depth, D3D12_COMPARISON_FUNC_ALWAYS);
			gs_d3d12_gp_set_no_stencil(gpb);
		}

		gs_d3d12_gp_set_color_write_mask(gpb, 0, (u8)gs_shader_convert_write_mask(i));
		d12_gp_set_ps(gpb, ps);
		if (slot == GS_D3D12_UTIL_CONVERT + index)
			return true;

		if (i == GS_SHADER_CONVERT_COPY)
		{
			/* color copy pipelines */
			gs_d3d12_gp_set_render_target(gpb, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
			gs_d3d12_gp_set_depth_stencil_format(gpb, DXGI_FORMAT_UNKNOWN);
			for (j = 0; j < 16; j++)
			{
				gs_d3d12_gp_set_blend(gpb, 0, 0, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE,
					D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, (u8)j);
				if (slot == GS_D3D12_UTIL_COLOR_COPY + j)
					return true;
			}
		}
		else if (i == GS_SHADER_CONVERT_RTA_CORRECTION)
		{
			/* color copy pipelines */
			gs_d3d12_gp_set_render_target(gpb, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
			gs_d3d12_gp_set_depth_stencil_format(gpb, DXGI_FORMAT_UNKNOWN);
			d12_gp_set_ps(gpb, ps);
			for (j = 16; j < 32; j++)
			{
				gs_d3d12_gp_set_blend(gpb, 0, 0, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE,
					D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, (u8)(j - 16));
				if (slot == GS_D3D12_UTIL_COLOR_COPY + j)
					return true;
			}
		}
		else if (i == GS_SHADER_CONVERT_COLCLIP_INIT || i == GS_SHADER_CONVERT_COLCLIP_RESOLVE)
		{
			const bool is_setup = i == GS_SHADER_CONVERT_COLCLIP_INIT;
			for (ds = 0; ds < 2; ds++)
			{
				gs_d3d12_gp_set_render_target(gpb, 0,
					is_setup ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM);
				gs_d3d12_gp_set_depth_stencil_format(gpb, ds ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_UNKNOWN);
				if (slot == (is_setup ? GS_D3D12_UTIL_HDR_SETUP : GS_D3D12_UTIL_HDR_FINISH) + ds)
					return true;
			}
		}
	}

	for (datm = 0; datm < 4; datm++)
	{
		gs_d3d12_gp_set_root_signature(gpb, rs);
		gs_d3d12_gp_set_render_target(gpb, 0, DXGI_FORMAT_R32_FLOAT);
		d12_gp_set_ps(gpb, ps);
		d12_gp_no_depth(gpb);
		gs_d3d12_gp_set_no_stencil(gpb);
		gs_d3d12_gp_set_blend(gpb, 0, 0, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD, D3D12_BLEND_ZERO,
			D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_COLOR_WRITE_ENABLE_RED);

		for (ds = 0; ds < 2; ds++)
		{
			gs_d3d12_gp_set_depth_stencil_format(gpb, ds ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_UNKNOWN);
			if (slot == GS_D3D12_UTIL_DATE_SETUP + ds * 4 + datm)
				return true;
		}
	}
	return false;
}

static ID3DBlob* d12_utility_shader(gs_device_d3d12* dev, unsigned type, const char* source, const char* entry)
{
	d12_macros_init(&dev->sm, dev->shader_cache.feature_level);
	return gs_d3d12_shader_cache_blob(&dev->shader_cache, type, source, strlen(source), gs_d3d_macros_list(&dev->sm),
		entry);
}

/* A utility pipeline built here, through the shader cache. */
static ID3D12PipelineState* d12_create_utility_pipeline(gs_device_d3d12* dev, u32 slot)
{
	char entry_buf[32];
	const char* source;
	const char* entry = d12_utility_pixel_shader(slot, &source, entry_buf);
	ID3DBlob* vs = d12_utility_shader(dev, GS_D3D12_ENTRY_VERTEX_SHADER, convert_fx_shader_raw, "vs_main");
	ID3DBlob* ps = d12_utility_shader(dev, GS_D3D12_ENTRY_PIXEL_SHADER, source, entry);
	ID3D12PipelineState* pso = NULL;
	gs_d3d12_gp_builder gpb;

	gs_d3d12_gp_clear(&gpb);
	if (vs && ps && d12_utility_pipeline_desc(&gpb, slot, dev->utility_root_signature, vs, ps))
		pso = d12_gp_create_cached(dev, &gpb, false);
	D12_RELEASE(vs);
	D12_RELEASE(ps);
	return pso;
}

static bool d12_collect_warmed_tfx_pipelines(gs_device_d3d12* dev, const gs_d3d12_pipeline_selector* want,
	ID3D12PipelineState*** found);

/* A utility pipeline at its first use: the warm-up thread's, else built
 * here once, as device creation built it. One that cannot be built
 * leaves the device lost, and the next device builds them all at its
 * creation, failing it as before; the draws asking meanwhile are dropped
 * (d12_apply_utility_state). */
static ID3D12PipelineState* d12_missing_utility_pipeline(gs_device_d3d12* dev, u32 slot)
{
	if (dev->tfx_warm)
	{
		d12_collect_warmed_tfx_pipelines(dev, NULL, NULL);
		if (dev->utility[slot])
			return dev->utility[slot];
	}
	if (dev->utility_failed)
		return NULL;
	dev->utility[slot] = d12_create_utility_pipeline(dev, slot);
	if (!dev->utility[slot])
	{
		log_cb(RETRO_LOG_ERROR, "GS: utility pipeline %u failed to build\n", slot);
		dev->utility_failed    = true;
		s_utility_build_failed = true;
		dev->device_lost       = true;
	}
	return dev->utility[slot];
}

static ID3D12PipelineState* d12_utility_pipeline(gs_device_d3d12* dev, u32 slot)
{
	ID3D12PipelineState* p = dev->utility[slot];
	return p ? p : d12_missing_utility_pipeline(dev, slot);
}

/* ==== the utility passes ==== */

static void d12_draw_stretch_rect(gs_device_d3d12* dev, const gs_rectf* sRect, const gs_rectf* dRect, int ds_x,
	int ds_y)
{
	/* ia */
	const float left   = dRect->x * 2 / (float)ds_x - 1.0f;
	const float top    = 1.0f - dRect->y * 2 / (float)ds_y;
	const float right  = dRect->z * 2 / (float)ds_x - 1.0f;
	const float bottom = 1.0f - dRect->w * 2 / (float)ds_y;
	d12_vertex_pt1 vertices[4];

	d12_set_vertex(&vertices[0], left, top, sRect->x, sRect->y);
	d12_set_vertex(&vertices[1], right, top, sRect->z, sRect->y);
	d12_set_vertex(&vertices[2], left, bottom, sRect->x, sRect->w);
	d12_set_vertex(&vertices[3], right, bottom, sRect->z, sRect->w);
	d12_ia_set_vertex_buffer(dev, vertices, sizeof(vertices[0]), D12_ARRAY_SIZE(vertices));
	d12_set_primitive_topology(dev, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	if (d12_apply_utility_state(dev))
		d12_draw_primitive(dev);
}

static void d12_begin_render_pass_for_stretch_rect(gs_device_d3d12* dev, gs_texture_d3d12* dTex,
	const gs_recti* dtex_rc, const gs_recti* dst_rc, bool allow_discard)
{
	const D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE load_op = (allow_discard && d12_recti_eq(dst_rc, dtex_rc))
		? D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD
		: d12_load_op(dTex);
	dTex->base.m_state = GS_TEXSTATE_DIRTY;

	if (dTex->base.m_type != GS_TEXTYPE_DEPTH_STENCIL)
	{
		float clear_color[4];
		d12_unorm8(clear_color, dTex->base.m_clear_value.color);
		d12_begin_render_pass_color(dev, load_op, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE, clear_color);
	}
	else
	{
		d12_begin_render_pass(dev, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
			D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS, load_op, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE,
			D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			s_d12_zero4, dTex->base.m_clear_value.depth, 0);
	}
}

static void d12_do_stretch_rect(gs_device_d3d12* dev, gs_texture_d3d12* sTex, const gs_rectf* sRect,
	gs_texture_d3d12* dTex, const gs_rectf* dRect, const ID3D12PipelineState* pipeline, bool linear,
	bool allow_discard)
{
	bool is_present, depth;
	int size_x, size_y;
	gs_recti dtex_rc, dst_rc, drect_i;

	/* A texture made with no render target or depth view cannot be drawn
	 * into: the render pass would name a descriptor that is not there. */
	if (dTex && !D12_HANDLE_VALID(dTex->write_descriptor))
	{
		d12_report_recurring("Drawing into a texture that is not a render target", E_INVALIDARG);
		return;
	}

	if (sTex->resource_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
	{
		/* can't transition in a render pass */
		d12_end_render_pass(dev);
		d12_transition(sTex, dev, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}

	d12_set_utility_root_signature(dev);
	d12_set_utility_texture(dev, &sTex->base, linear ? &dev->linear_sampler_cpu : &dev->point_sampler_cpu);
	d12_set_pipeline(dev, pipeline);

	is_present = (!dTex);
	depth      = (dTex && dTex->base.m_type == GS_TEXTYPE_DEPTH_STENCIL);
	size_x     = is_present ? (int)dev->base.m_window_info.surface_width : dTex->base.m_size.x;
	size_y     = is_present ? (int)dev->base.m_window_info.surface_height : dTex->base.m_size.y;
	dtex_rc.x  = 0;
	dtex_rc.y  = 0;
	dtex_rc.z  = size_x;
	dtex_rc.w  = size_y;
	d12_recti_of(&drect_i, dRect);
	d12_rintersect(&dst_rc, &drect_i, &dtex_rc);

	/* switch rts (which might not end the render pass), so check the bounds */
	if (!is_present)
		d12_om_set_render_targets(dev, depth ? NULL : dTex, depth ? dTex : NULL, &dst_rc);
	else
	{
		/* this is for presenting, we don't want to screw with the
		 * viewport/scissor set by display */
		dev->dirty_flags &= ~(DIRTY_FLAG_RENDER_TARGET | DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR);
	}

	if (!(is_present || dev->in_render_pass))
		d12_begin_render_pass_for_stretch_rect(dev, dTex, &dtex_rc, &dst_rc, allow_discard);

	d12_draw_stretch_rect(dev, sRect, dRect, size_x, size_y);
}

static void d12_dev_stretch_rect(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, int shader, bool linear)
{
	gs_device_d3d12* dev = D12_DEV(d);
	d12_do_stretch_rect(dev, D12_TEX(sTex), sRect, D12_TEX(dTex), dRect,
		d12_utility_pipeline(dev, GS_D3D12_UTIL_CONVERT + (u32)shader), linear, gs_shader_convert_write_mask(shader) == 0xf);
}

static void d12_dev_stretch_rect_mask(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, bool red, bool green, bool blue, bool alpha, int shader)
{
	gs_device_d3d12* dev     = D12_DEV(d);
	const u32 index          = (red ? 1 : 0) | (green ? 2 : 0) | (blue ? 4 : 0) | (alpha ? 8 : 0);
	const u32 rta_offset     = (shader == GS_SHADER_CONVERT_RTA_CORRECTION) ? 16 : 0;
	const bool allow_discard = (index == 0xf);
	d12_do_stretch_rect(dev, D12_TEX(sTex), sRect, D12_TEX(dTex), dRect,
		d12_utility_pipeline(dev, GS_D3D12_UTIL_COLOR_COPY + index + rta_offset), false, allow_discard);
}

static void d12_do_multi_stretch_rects(gs_device_d3d12* dev, const gs_multi_stretch_rect* rects, u32 num_rects,
	gs_texture_d3d12* dTex, int shader)
{
	/* Set up vertices first. */
	const u32 vertex_reserve_size = num_rects * 4 * sizeof(d12_vertex_pt1);
	const u32 index_reserve_size  = num_rects * 6 * sizeof(u16);
	float ds_x, ds_y;
	d12_vertex_pt1* verts;
	u16* idx;
	u32 icount = 0;
	u32 vcount = 0;
	u32 i;
	u32 rta_bit;
	gs_recti rc;

	if (!gs_d3d12_stream_buffer_reserve(&dev->vertex_stream_buffer, vertex_reserve_size, sizeof(d12_vertex_pt1))
		|| !gs_d3d12_stream_buffer_reserve(&dev->index_stream_buffer, index_reserve_size, sizeof(u16)))
	{
		d12_execute_command_list_and_restart_render_pass(dev, false);
		if (!gs_d3d12_stream_buffer_reserve(&dev->vertex_stream_buffer, vertex_reserve_size, sizeof(d12_vertex_pt1))
			|| !gs_d3d12_stream_buffer_reserve(&dev->index_stream_buffer, index_reserve_size, sizeof(u16)))
		{
			/* Filling the buffer anyway would write into a region nothing
			 * reserved. Drop the blit instead. */
			d12_report_recurring("Reserving space for vertices", E_OUTOFMEMORY);
			return;
		}
	}

	/* Pain in the arse because the primitive topology for the pipelines is
	 * all triangle strips. Don't use primitive restart here, it ends up
	 * slower on some drivers. */
	ds_x  = (float)dTex->base.m_size.x;
	ds_y  = (float)dTex->base.m_size.y;
	verts = (d12_vertex_pt1*)(dev->vertex_stream_buffer.host_pointer + dev->vertex_stream_buffer.current_offset);
	idx   = (u16*)(dev->index_stream_buffer.host_pointer + dev->index_stream_buffer.current_offset);
	for (i = 0; i < num_rects; i++)
	{
		const gs_rectf* sRect = &rects[i].src_rect;
		const gs_rectf* dRect = &rects[i].dst_rect;
		const float left      = dRect->x * 2 / ds_x - 1.0f;
		const float top       = 1.0f - dRect->y * 2 / ds_y;
		const float right     = dRect->z * 2 / ds_x - 1.0f;
		const float bottom    = 1.0f - dRect->w * 2 / ds_y;
		const u32 vstart      = vcount;

		d12_set_vertex(&verts[vcount++], left, top, sRect->x, sRect->y);
		d12_set_vertex(&verts[vcount++], right, top, sRect->z, sRect->y);
		d12_set_vertex(&verts[vcount++], left, bottom, sRect->x, sRect->w);
		d12_set_vertex(&verts[vcount++], right, bottom, sRect->z, sRect->w);

		if (i > 0)
			idx[icount++] = (u16)vstart;

		idx[icount++] = (u16)vstart;
		idx[icount++] = (u16)(vstart + 1);
		idx[icount++] = (u16)(vstart + 2);
		idx[icount++] = (u16)(vstart + 3);
		idx[icount++] = (u16)(vstart + 3);
	}

	dev->base.m_vertex.start = dev->vertex_stream_buffer.current_offset / sizeof(d12_vertex_pt1);
	dev->base.m_vertex.count = vcount;
	dev->base.m_index.start  = dev->index_stream_buffer.current_offset / sizeof(u16);
	dev->base.m_index.count  = icount;
	gs_d3d12_stream_buffer_commit(&dev->vertex_stream_buffer, vcount * sizeof(d12_vertex_pt1));
	gs_d3d12_stream_buffer_commit(&dev->index_stream_buffer, icount * sizeof(u16));
	d12_set_vertex_buffer(dev, dev->vertex_stream_buffer.gpu_pointer, dev->vertex_stream_buffer.size,
		sizeof(d12_vertex_pt1));
	d12_set_index_buffer(dev, dev->index_stream_buffer.gpu_pointer, dev->index_stream_buffer.size, DXGI_FORMAT_R16_UINT);

	/* Even though we're batching, a cmdbuffer submit could've messed this up. */
	d12_tex_rect(&rc, &dTex->base);
	d12_om_set_render_targets(dev, dTex->base.m_type == GS_TEXTYPE_RENDER_TARGET ? dTex : NULL,
		dTex->base.m_type == GS_TEXTYPE_DEPTH_STENCIL ? dTex : NULL, &rc);
	if (!dev->in_render_pass)
		d12_begin_render_pass_for_stretch_rect(dev, dTex, &rc, &rc, false);
	d12_set_primitive_topology(dev, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	d12_set_utility_texture(dev, rects[0].src, rects[0].linear ? &dev->linear_sampler_cpu : &dev->point_sampler_cpu);

	rta_bit = (shader == GS_SHADER_CONVERT_RTA_CORRECTION) ? 16 : 0;
	d12_set_pipeline(dev, (rects[0].wmask.wrgba != 0xf)
			? d12_utility_pipeline(dev, GS_D3D12_UTIL_COLOR_COPY + (rects[0].wmask.wrgba | rta_bit))
			: d12_utility_pipeline(dev, GS_D3D12_UTIL_CONVERT + (u32)shader));

	if (d12_apply_utility_state(dev))
		d12_draw_indexed_primitive(dev);
}

static void d12_dev_draw_multi_stretch_rects(gs_device* d, const gs_multi_stretch_rect* rects, u32 num_rects,
	gs_texture* dTex, int shader)
{
	gs_device_d3d12* dev = D12_DEV(d);
	gs_texture* last_tex = rects[0].src;
	bool last_linear     = rects[0].linear;
	u8 last_wmask        = rects[0].wmask.wrgba;
	u32 first            = 0;
	u32 count            = 1;
	u32 i;

	/* Make sure all textures are in shader read only layout, so we don't
	 * need to break the render pass to transition. */
	for (i = 0; i < num_rects; i++)
	{
		gs_texture_d3d12* const stex = D12_TEX(rects[i].src);
		d12_commit_clear(stex);
		if (stex->resource_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
		{
			d12_end_render_pass(dev);
			d12_transition(stex, dev, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
	}

	for (i = 1; i < num_rects; i++)
	{
		if (rects[i].src == last_tex && rects[i].linear == last_linear && rects[i].wmask.wrgba == last_wmask)
		{
			count++;
			continue;
		}

		d12_do_multi_stretch_rects(dev, rects + first, count, D12_TEX(dTex), shader);
		last_tex    = rects[i].src;
		last_linear = rects[i].linear;
		last_wmask  = rects[i].wmask.wrgba;
		first += count;
		count = 1;
	}

	d12_do_multi_stretch_rects(dev, rects + first, count, D12_TEX(dTex), shader);
}

static void d12_dev_copy_rect(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_recti* r, u32 destX, u32 destY)
{
	gs_device_d3d12* dev           = D12_DEV(d);
	gs_texture_d3d12* const sTex12 = D12_TEX(sTex);
	gs_texture_d3d12* const dTex12 = D12_TEX(dTex);
	D3D12_TEXTURE_COPY_LOCATION srcloc, dstloc;
	D3D12_BOX srcbox;
	gs_recti dtex_rc;

	d12_tex_rect(&dtex_rc, dTex);

	if (sTex->m_state == GS_TEXSTATE_CLEARED)
	{
		/* source is cleared. if destination is a render target, we can
		 * carry the clear forward */
		if (d12_is_rt_or_ds(dTex))
		{
			if (d12_recti_eq(&dtex_rc, r))
			{
				/* pass it forward if we're clearing the whole thing */
				dTex->m_state = GS_TEXSTATE_CLEARED;
				if (sTex->m_type == GS_TEXTYPE_DEPTH_STENCIL)
					dTex->m_clear_value.depth = sTex->m_clear_value.depth;
				else
					dTex->m_clear_value.color = sTex->m_clear_value.color;
				return;
			}
			else
			{
				/* otherwise we need to do an attachment clear */
				d12_end_render_pass(dev);

				dTex->m_state = GS_TEXSTATE_DIRTY;

				if (dTex->m_type != GS_TEXTYPE_DEPTH_STENCIL)
				{
					float col[4];
					d12_unorm8(col, sTex->m_clear_value.color);
					d12_transition(dTex12, dev, D3D12_RESOURCE_STATE_RENDER_TARGET);
					ID3D12GraphicsCommandList4_ClearRenderTargetView(d12_cmdlist(dev),
						d12_cpu(&dTex12->write_descriptor), col, 0, NULL);
				}
				else
				{
					d12_transition(dTex12, dev, D3D12_RESOURCE_STATE_DEPTH_WRITE);
					ID3D12GraphicsCommandList4_ClearDepthStencilView(d12_cmdlist(dev),
						d12_cpu(&dTex12->write_descriptor), D3D12_CLEAR_FLAG_DEPTH, sTex->m_clear_value.depth, 0, 0,
						NULL);
				}

				return;
			}
		}

		/* commit the clear to the source first, then do normal copy */
		d12_commit_clear(sTex12);
	}

	/* if the destination has been cleared, and we're not overwriting the
	 * whole thing, commit the clear first (the area outside of where we're
	 * copying to) */
	if (dTex->m_state == GS_TEXSTATE_CLEARED && !d12_recti_eq(&dtex_rc, r))
		d12_commit_clear(dTex12);

	d12_end_render_pass(dev);

	d12_transition(sTex12, dev, D3D12_RESOURCE_STATE_COPY_SOURCE);
	sTex12->use_fence_counter = dev->current_fence_value;
	if (D12_HANDLE_VALID(dev->tfx_textures[0]) && D12_HANDLE_EQ(sTex12->srv_descriptor, dev->tfx_textures[0]))
		d12_ps_set_shader_resource(dev, 0, NULL, false);

	d12_transition(dTex12, dev, D3D12_RESOURCE_STATE_COPY_DEST);
	dTex12->use_fence_counter = dev->current_fence_value;

	srcloc.pResource        = sTex12->resource;
	srcloc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	srcloc.SubresourceIndex = 0;

	dstloc.pResource        = dTex12->resource;
	dstloc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dstloc.SubresourceIndex = 0;

	srcbox.left   = (UINT)r->x;
	srcbox.top    = (UINT)r->y;
	srcbox.front  = 0u;
	srcbox.right  = (UINT)r->z;
	srcbox.bottom = (UINT)r->w;
	srcbox.back   = 1u;
	ID3D12GraphicsCommandList4_CopyTextureRegion(d12_cmdlist(dev), &dstloc, destX, destY, 0, &srcloc, &srcbox);

	dTex->m_state = GS_TEXSTATE_DIRTY;
}

/* How many indices the frontend cycles through: the mask is a mask, so it
 * is one past its highest set bit. Told to the device after each wait so a
 * retired present texture is freed on the frontend's own signal rather
 * than on a guessed frame count (gs_device_age_pool). */
static u32 gs_sync_slots_from_mask(u32 mask)
{
	u32 slots = 0;
	while (mask)
	{
		slots++;
		mask >>= 1;
	}
	return slots ? slots : 1;
}

static DXGI_FORMAT d12_resource_format(ID3D12Resource* r)
{
	D3D12_RESOURCE_DESC desc;
	r->lpVtbl->GetDesc(r, &desc);
	return desc.Format;
}

static void d12_dev_present_rect(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect)
{
	gs_device_d3d12* dev      = D12_DEV(d);
	gs_texture_d3d12* texture = D12_TEX(sTex);
	(void)dTex;
	(void)dRect;

	if (d3d12->interface_version >= RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2 && d3d12->get_sync_index
		&& d3d12->wait_sync_index && d3d12->set_texture_fenced)
	{
		const unsigned index = d3d12->get_sync_index(d3d12->handle);
		const bool blank     = (!sRect->z && !sRect->w);
		u64 value;

		if (index < GS_D3D12_MAX_SYNC_INDICES)
		{
			/* Not ours until the frontend has finished with what this index
			 * carried last time round. */
			d3d12->wait_sync_index(d3d12->handle);
			dev->base.m_sync_slots = d3d12->get_sync_index_mask
				? gs_sync_slots_from_mask(d3d12->get_sync_index_mask(d3d12->handle))
				: 1;
			dev->base.m_sync_waits++;

			/* Every texture presented is drawn over in full each frame, so
			 * the one handed over stays with this index until it comes
			 * round again, and the next frame draws into the one this index
			 * carried, which the wait above has made ours. Nothing is
			 * copied. */
			if (gs_device_swap_present_texture(d, sTex, s_d3d12_present_textures[index]
						? &s_d3d12_present_textures[index]->base
						: NULL))
			{
				s_d3d12_present_textures[index] = texture;
				if (blank)
					d12_set_clear_color(sTex, 0);
				d12_commit_clear(texture);
				d12_transition(texture, dev, d3d12->required_state);

				/* The value the list being recorded signals when it is
				 * submitted, which is the next line: the texture is
				 * complete, and in required_state, once the fence is there. */
				value = dev->current_fence_value;
				d12_execute_command_list(dev, false);

				d3d12->set_texture_fenced(d3d12->handle, texture->resource, d12_resource_format(texture->resource),
					dev->fence, value);
				video_cb(RETRO_HW_FRAME_BUFFER_VALID, (unsigned)sTex->m_size.x, (unsigned)sTex->m_size.y, 0);
				return;
			}
		}
		/* A texture that is none of the device's present targets: version
		 * 1's handoff is still valid on a version 2 interface. */
	}

	d12_commit_clear(texture);
	d12_transition(texture, dev, d3d12->required_state);
	d12_execute_command_list(dev, false);

	/* Blanking enforce, see GSRenderer's VSync */
	if (!sRect->z && !sRect->w)
		d12_set_clear_color(sTex, 0);

	d3d12->set_texture(d3d12->handle, texture->resource, d12_resource_format(texture->resource));
	video_cb(RETRO_HW_FRAME_BUFFER_VALID, (unsigned)sTex->m_size.x, (unsigned)sTex->m_size.y, 0);
}

static const gs_rectf s_d12_zero_rect = {0.0f, 0.0f, 0.0f, 0.0f};

static void d12_dev_update_clut_texture(gs_device* d, gs_texture* sTex, float sScale, float sSample, u32 offsetX,
	u32 offsetY, gs_texture* dTex, u32 dOffset, u32 dSize)
{
	/* match merge cb */
	struct
	{
		float scale;
		float sample; /* BGColor.y */
		float pad1[2];
		u32 offsetX, offsetY, dOffset;
		u32 pad2;
	} cb;
	gs_device_d3d12* dev = D12_DEV(d);
	const int shader     = (dSize == 16) ? GS_SHADER_CONVERT_CLUT_4 : GS_SHADER_CONVERT_CLUT_8;
	gs_rectf dRect;

	memset(&cb, 0, sizeof(cb));
	cb.scale   = sScale;
	cb.sample  = sSample;
	cb.offsetX = offsetX;
	cb.offsetY = offsetY;
	cb.dOffset = dOffset;
	d12_set_utility_root_signature(dev);
	d12_set_utility_push_constants(dev, &cb, sizeof(cb));

	dRect.x = 0.0f;
	dRect.y = 0.0f;
	dRect.z = (float)dSize;
	dRect.w = 1.0f;
	d12_do_stretch_rect(dev, D12_TEX(sTex), &s_d12_zero_rect, D12_TEX(dTex), &dRect,
		d12_utility_pipeline(dev, GS_D3D12_UTIL_CONVERT + (u32)shader), true, true);
}

static void d12_dev_convert_to_indexed_texture(gs_device* d, gs_texture* sTex, float sScale, u32 offsetX,
	u32 offsetY, u32 SBW, u32 SPSM, gs_texture* dTex, u32 DBW, u32 DPSM)
{
	/* match merge cb; seven words, as were pushed */
	struct
	{
		float scale;
		float pad1[3];
		u32 SBW, DBW, PageOffset;
	} cb;
	gs_device_d3d12* dev = D12_DEV(d);
	gs_rectf dRect;
	(void)SPSM;
	(void)DPSM;

	memset(&cb, 0, sizeof(cb));
	cb.scale      = sScale;
	cb.SBW        = SBW;
	cb.DBW        = DBW;
	cb.PageOffset = gs_indexed_conversion_page_offset(offsetX, offsetY, SBW);
	d12_set_utility_root_signature(dev);
	d12_set_utility_push_constants(dev, &cb, sizeof(cb));

	dRect.x = 0.0f;
	dRect.y = 0.0f;
	dRect.z = (float)dTex->m_size.x;
	dRect.w = (float)dTex->m_size.y;
	d12_do_stretch_rect(dev, D12_TEX(sTex), &s_d12_zero_rect, D12_TEX(dTex), &dRect,
		d12_utility_pipeline(dev, GS_D3D12_UTIL_CONVERT + GS_SHADER_CONVERT_RGBA_TO_8I), false, true);
}

static void d12_dev_filtered_downsample_texture(gs_device* d, gs_texture* sTex, gs_texture* dTex, u32 factor,
	const gs_point* clamp_min, const gs_rectf* dRect)
{
	struct
	{
		float weight;
		float pad0[3];
		s32 clamp_min[2];
		s32 downsample_factor;
		s32 pad1;
	} cb;
	gs_device_d3d12* dev = D12_DEV(d);

	memset(&cb, 0, sizeof(cb));
	cb.weight            = (float)(factor * factor);
	cb.clamp_min[0]      = clamp_min->x;
	cb.clamp_min[1]      = clamp_min->y;
	cb.downsample_factor = (s32)factor;
	d12_set_utility_root_signature(dev);
	d12_set_utility_push_constants(dev, &cb, sizeof(cb));

	d12_do_stretch_rect(dev, D12_TEX(sTex), &s_d12_zero_rect, D12_TEX(dTex), dRect,
		d12_utility_pipeline(dev, GS_D3D12_UTIL_CONVERT + GS_SHADER_CONVERT_DOWNSAMPLE_COPY), false, true);
}

static void d12_dev_move_texels(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_rectf* dRect,
	const MoveTexelsConstants* cb)
{
	gs_device_d3d12* dev = D12_DEV(d);
	d12_set_utility_root_signature(dev);
	d12_set_utility_push_constants(dev, cb, sizeof(*cb));

	d12_do_stretch_rect(dev, D12_TEX(sTex), &s_d12_zero_rect, D12_TEX(dTex), dRect,
		d12_utility_pipeline(dev, GS_D3D12_UTIL_CONVERT + GS_SHADER_CONVERT_MOVE_TEXELS), false, true);
}

/* (whether a texture holds anything: drawn into, or cleared to something
 * other than black) */
static bool d12_has_input(const gs_texture* t)
{
	return t
		&& (t->m_state == GS_TEXSTATE_DIRTY || (t->m_state == GS_TEXSTATE_CLEARED || t->m_clear_value.color != 0));
}

static void d12_dev_do_merge(gs_device* d, gs_texture* sTex[3], gs_rectf* sRect, gs_texture* dTex, gs_rectf* dRect,
	const GSRegPMODE* PMODE, const GSRegEXTBUF* EXTBUF, u32 c, bool linear)
{
	static const gs_rectf full_r             = {0.0f, 0.0f, 1.0f, 1.0f};
	gs_device_d3d12* dev                     = D12_DEV(d);
	const bool feedback_write_2              = PMODE->EN2 && sTex[2] != NULL && EXTBUF->FBIN == 1;
	const bool feedback_write_1              = PMODE->EN1 && sTex[2] != NULL && EXTBUF->FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE->SLBG == 1;
	const gs_d3d12_descriptor_handle* sampler = linear ? &dev->linear_sampler_cpu : &dev->point_sampler_cpu;
	bool has_input_0, has_input_1;
	bool dcleared = false;
	int dsize_x, dsize_y, fbsize_x, fbsize_y;
	gs_recti darea, fbarea;
	float cc[4];

	/* Merge the 2 source textures (sTex[0],sTex[1]). Final results go to
	 * dTex. Feedback write will go to sTex[2]. If either 2nd output is
	 * disabled or SLBG is 1, a background color will be used. Note:
	 * background color is also used when outside of the unit rectangle
	 * area */
	d12_end_render_pass(dev);

	/* transition everything before starting the new render pass */
	has_input_0 = d12_has_input(sTex[0]);
	has_input_1 = (PMODE->SLBG == 0 || feedback_write_2_but_blend_bg) && d12_has_input(sTex[1]);
	if (has_input_0)
	{
		d12_commit_clear(D12_TEX(sTex[0]));
		d12_transition(D12_TEX(sTex[0]), dev, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	if (has_input_1)
	{
		d12_commit_clear(D12_TEX(sTex[1]));
		d12_transition(D12_TEX(sTex[1]), dev, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	d12_transition(D12_TEX(dTex), dev, D3D12_RESOURCE_STATE_RENDER_TARGET);

	/* Upload constant to select YUV algo, but skip constant buffer update
	 * if we don't need it */
	if (feedback_write_2 || feedback_write_1 || sTex[0])
	{
		d12_merge_cb uniforms;
		memset(&uniforms, 0, sizeof(uniforms));
		d12_unorm8(uniforms.bg_color, c);
		uniforms.emoda = EXTBUF->EMODA;
		uniforms.emodc = EXTBUF->EMODC;
		d12_set_utility_root_signature(dev);
		d12_set_utility_push_constants(dev, &uniforms, sizeof(uniforms));
	}

	d12_unorm8(cc, c);
	dsize_x = dTex->m_size.x;
	dsize_y = dTex->m_size.y;
	darea.x = 0;
	darea.y = 0;
	darea.z = dsize_x;
	darea.w = dsize_y;
	if (has_input_1 && (PMODE->SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		/* 2nd output is enabled and selected. Copy it to destination so we
		 * can blend it with 1st output. Note: value outside of dRect must
		 * contains the background color (c) */
		d12_om_set_render_targets(dev, D12_TEX(dTex), NULL, &darea);
		d12_set_utility_texture(dev, sTex[1], sampler);
		d12_begin_render_pass_color(dev, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR,
			D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE, cc);
		d12_set_utility_root_signature(dev);
		d12_set_pipeline(dev, d12_utility_pipeline(dev, GS_D3D12_UTIL_CONVERT + GS_SHADER_CONVERT_COPY));
		d12_draw_stretch_rect(dev, &sRect[1], PMODE->SLBG ? &dRect[2] : &dRect[1], dsize_x, dsize_y);
		dTex->m_state = GS_TEXSTATE_DIRTY;
		dcleared      = true;
	}

	/* Upload constant to select YUV algo */
	fbsize_x = sTex[2] ? sTex[2]->m_size.x : 0;
	fbsize_y = sTex[2] ? sTex[2]->m_size.y : 0;
	fbarea.x = 0;
	fbarea.y = 0;
	fbarea.z = fbsize_x;
	fbarea.w = fbsize_y;
	if (feedback_write_2) /* FIXME I'm not sure dRect[1] is always correct */
	{
		gs_recti dr2;
		d12_end_render_pass(dev);
		d12_om_set_render_targets(dev, D12_TEX(sTex[2]), NULL, &fbarea);
		if (dcleared)
			d12_set_utility_texture(dev, dTex, sampler);

		/* sTex[2] can be sTex[0], in which case it might be cleared (e.g. Xenosaga). */
		d12_recti_of(&dr2, &dRect[2]);
		d12_begin_render_pass_for_stretch_rect(dev, D12_TEX(sTex[2]), &fbarea, &dr2, true);
		if (dcleared)
		{
			d12_set_utility_root_signature(dev);
			d12_set_pipeline(dev, d12_utility_pipeline(dev, GS_D3D12_UTIL_CONVERT + GS_SHADER_CONVERT_YUV));
			d12_draw_stretch_rect(dev, &full_r, &dRect[2], fbsize_x, fbsize_y);
		}
		d12_end_render_pass(dev);

		if (sTex[0] == sTex[2])
		{
			/* need a barrier here because of the render pass */
			d12_transition(D12_TEX(sTex[2]), dev, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
	}

	/* Restore background color to process the normal merge */
	if (feedback_write_2_but_blend_bg || !dcleared)
	{
		d12_end_render_pass(dev);
		d12_om_set_render_targets(dev, D12_TEX(dTex), NULL, &darea);
		d12_begin_render_pass_color(dev, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR,
			D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE, cc);
		dTex->m_state = GS_TEXSTATE_DIRTY;
	}
	else if (!dev->in_render_pass)
	{
		d12_om_set_render_targets(dev, D12_TEX(dTex), NULL, &darea);
		d12_begin_render_pass_color(dev, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE,
			D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE, s_d12_zero4);
	}

	if (has_input_0)
	{
		/* 1st output is enabled. It must be blended */
		d12_set_utility_root_signature(dev);
		d12_set_utility_texture(dev, sTex[0], sampler);
		d12_set_pipeline(dev, d12_utility_pipeline(dev, GS_D3D12_UTIL_MERGE + PMODE->MMOD));
		d12_draw_stretch_rect(dev, &sRect[0], &dRect[0], dTex->m_size.x, dTex->m_size.y);
	}

	if (feedback_write_1) /* FIXME I'm not sure dRect[0] is always correct */
	{
		d12_end_render_pass(dev);
		d12_set_utility_root_signature(dev);
		d12_set_pipeline(dev, d12_utility_pipeline(dev, GS_D3D12_UTIL_CONVERT + GS_SHADER_CONVERT_YUV));
		d12_set_utility_texture(dev, dTex, sampler);
		d12_om_set_render_targets(dev, D12_TEX(sTex[2]), NULL, &fbarea);
		d12_begin_render_pass_color(dev, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE,
			D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE, s_d12_zero4);
		d12_draw_stretch_rect(dev, &full_r, &dRect[2], dsize_x, dsize_y);
	}

	d12_end_render_pass(dev);

	/* this texture is going to get used as an input, so make sure we don't
	 * read undefined data */
	d12_commit_clear(D12_TEX(dTex));
	d12_transition(D12_TEX(dTex), dev, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

static void d12_dev_do_interlace(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, int shader, bool linear, const gs_rectf* zrh)
{
	gs_device_d3d12* dev = D12_DEV(d);
	gs_recti rc, dtex_rc, clamped_rc;

	d12_transition(D12_TEX(dTex), dev, D3D12_RESOURCE_STATE_RENDER_TARGET);

	d12_recti_of(&rc, dRect);
	d12_tex_rect(&dtex_rc, dTex);
	d12_rintersect(&clamped_rc, &rc, &dtex_rc);
	d12_end_render_pass(dev);
	d12_om_set_render_targets(dev, D12_TEX(dTex), NULL, &clamped_rc);
	d12_set_utility_root_signature(dev);
	d12_set_utility_texture(dev, sTex, linear ? &dev->linear_sampler_cpu : &dev->point_sampler_cpu);
	d12_begin_render_pass_for_stretch_rect(dev, D12_TEX(dTex), &dtex_rc, &clamped_rc, false);
	d12_set_pipeline(dev, d12_utility_pipeline(dev, GS_D3D12_UTIL_INTERLACE + (u32)shader));
	/* (InterlaceConstantBuffer: ZrH) */
	d12_set_utility_push_constants(dev, zrh, sizeof(*zrh));
	d12_draw_stretch_rect(dev, sRect, dRect, dTex->m_size.x, dTex->m_size.y);
	d12_end_render_pass(dev);

	/* this texture is going to get used as an input, so make sure we don't
	 * read undefined data */
	d12_transition(D12_TEX(dTex), dev, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

/* Assumes that the previous level has been transitioned to PS resource,
 * and the current level has been transitioned to RT. */
static void d12_render_texture_mipmap(gs_device_d3d12* dev, gs_texture_d3d12* texture, u32 dst_level, u32 dst_width,
	u32 dst_height, u32 src_level, u32 src_width, u32 src_height)
{
	const u32 MODIFIED_STATE = DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR | DIRTY_FLAG_RENDER_TARGET;
	gs_d3d12_descriptor_handle rtv_handle, srv_handle;
	D3D12_RENDER_TARGET_VIEW_DESC rtv_desc;
	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
	D3D12_DISCARD_REGION discard_region;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu;
	D3D12_VIEWPORT vp;
	D3D12_RECT scissor;
	ID3D12GraphicsCommandList* cmdlist;
	gs_rectf sr, dr;
	(void)src_width;
	(void)src_height;

	d12_end_render_pass(dev);

	/* We need a temporary SRV and RTV for each mip level. Safe to use the
	 * init buffer after exec, because everything will be done with the
	 * texture. */
	gs_d3d12_descriptor_handle_clear(&rtv_handle);
	while (!gs_d3d12_descriptor_heap_manager_allocate(&dev->rtv_heap_manager, &rtv_handle))
		d12_execute_command_list(dev, false);

	gs_d3d12_descriptor_handle_clear(&srv_handle);
	while (!gs_d3d12_descriptor_heap_manager_allocate(&dev->descriptor_heap_manager, &srv_handle))
		d12_execute_command_list(dev, false);

	/* Setup views. This will be a partial view for the SRV. */
	memset(&rtv_desc, 0, sizeof(rtv_desc));
	rtv_desc.Format             = texture->dxgi_format;
	rtv_desc.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
	rtv_desc.Texture2D.MipSlice = dst_level;
	rtv_desc.Texture2D.PlaneSlice = 0;
	ID3D12Device_CreateRenderTargetView(dev->device, texture->resource, &rtv_desc, d12_cpu(&rtv_handle));

	memset(&srv_desc, 0, sizeof(srv_desc));
	srv_desc.Format                        = texture->dxgi_format;
	srv_desc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv_desc.Texture2D.MostDetailedMip     = src_level;
	srv_desc.Texture2D.MipLevels           = 1;
	srv_desc.Texture2D.PlaneSlice          = 0;
	srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
	ID3D12Device_CreateShaderResourceView(dev->device, texture->resource, &srv_desc, d12_cpu(&srv_handle));

	/* We need to set the descriptors up manually, because we're not going
	 * through the texture. */
	if (!d12_get_texture_group_descriptors(dev, &dev->utility_texture_gpu, &srv_handle, 1))
		d12_execute_command_list(dev, false);
	if (!D12_HANDLE_EQ(dev->utility_sampler_cpu, dev->linear_sampler_cpu))
	{
		dev->dirty_flags |= DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE;
		if (!gs_d3d12_sampler_allocator_lookup_single(&dev->command_lists[dev->current_command_list].sampler_allocator,
				&dev->utility_sampler_gpu, &dev->linear_sampler_cpu))
			d12_execute_command_list(dev, false);
	}

	/* *now* we don't have to worry about running out of anything. */
	cmdlist = (ID3D12GraphicsCommandList*)d12_cmdlist(dev);
	if (texture->resource_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
		gs_texture_d3d12_transition_level(texture, cmdlist, (int)src_level, texture->resource_state,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	if (texture->resource_state != D3D12_RESOURCE_STATE_RENDER_TARGET)
		gs_texture_d3d12_transition_level(texture, cmdlist, (int)dst_level, texture->resource_state,
			D3D12_RESOURCE_STATE_RENDER_TARGET);

	/* We set the state directly here. */
	dev->dirty_flags &= ~MODIFIED_STATE;

	/* Using a render pass is probably a bit overkill. */
	discard_region.NumRects         = 0u;
	discard_region.pRects           = NULL;
	discard_region.FirstSubresource = dst_level;
	discard_region.NumSubresources  = 1u;
	ID3D12GraphicsCommandList_DiscardResource(cmdlist, texture->resource, &discard_region);
	rtv_cpu = d12_cpu(&rtv_handle);
	ID3D12GraphicsCommandList_OMSetRenderTargets(cmdlist, 1, &rtv_cpu, FALSE, NULL);

	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width    = (float)dst_width;
	vp.Height   = (float)dst_height;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	ID3D12GraphicsCommandList_RSSetViewports(cmdlist, 1, &vp);

	scissor.left   = 0;
	scissor.top    = 0;
	scissor.right  = (LONG)dst_width;
	scissor.bottom = (LONG)dst_height;
	ID3D12GraphicsCommandList_RSSetScissorRects(cmdlist, 1, &scissor);

	d12_set_utility_root_signature(dev);
	d12_set_pipeline(dev, d12_utility_pipeline(dev, GS_D3D12_UTIL_CONVERT + GS_SHADER_CONVERT_COPY));
	sr.x = 0.0f;
	sr.y = 0.0f;
	sr.z = 1.0f;
	sr.w = 1.0f;
	dr.x = 0.0f;
	dr.y = 0.0f;
	dr.z = (float)dst_width;
	dr.w = (float)dst_height;
	d12_draw_stretch_rect(dev, &sr, &dr, (int)dst_width, (int)dst_height);

	if (texture->resource_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
		gs_texture_d3d12_transition_level(texture, cmdlist, (int)src_level, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			texture->resource_state);
	if (texture->resource_state != D3D12_RESOURCE_STATE_RENDER_TARGET)
		gs_texture_d3d12_transition_level(texture, cmdlist, (int)dst_level, D3D12_RESOURCE_STATE_RENDER_TARGET,
			texture->resource_state);

	/* Must destroy after current cmdlist. */
	d12_defer_descriptor(dev, &dev->descriptor_heap_manager, &srv_handle);
	d12_defer_descriptor(dev, &dev->rtv_heap_manager, &rtv_handle);

	/* Restore for next normal draw. */
	dev->dirty_flags |= MODIFIED_STATE;
}

/* ==== the draw pipelines ==== */

static size_t d12_selector_hash(const gs_d3d12_pipeline_selector* p)
{
	/* Compared bit for bit: hashed over every byte. */
	return gs_key_hash(p, sizeof(*p));
}

static ID3D12PipelineState** d12_pipeline_find(gs_device_d3d12* dev, const gs_d3d12_pipeline_selector* p)
{
	return (ID3D12PipelineState**)gs_flat_map_find(&dev->tfx_pipelines, p, sizeof(*p), d12_selector_hash(p));
}

/* p's entry: made, null, if there was none (*inserted) */
static ID3D12PipelineState** d12_pipeline_entry(gs_device_d3d12* dev, const gs_d3d12_pipeline_selector* p,
	int* inserted)
{
	return (ID3D12PipelineState**)gs_flat_map_insert(&dev->tfx_pipelines, p, d12_selector_hash(p), inserted);
}

/* pso into p's entry in place of what was (insert_or_assign) */
static ID3D12PipelineState** d12_pipeline_assign(gs_device_d3d12* dev, const gs_d3d12_pipeline_selector* p,
	ID3D12PipelineState* pso)
{
	int inserted;
	ID3D12PipelineState** v = d12_pipeline_entry(dev, p, &inserted);
	if (!v)
	{
		D12_RELEASE(pso);
		return NULL;
	}
	if (!inserted && *v)
		ID3D12PipelineState_Release(*v);
	*v = pso;
	return v;
}

static void d12_tfx_vertex_shader_macros(gs_d3d_macros* sm, gs_hw_vs_selector sel, const char** entry)
{
	gs_d3d_macros_add_int(sm, "VERTEX_SHADER", 1);
	gs_d3d_macros_add_int(sm, "VS_TME", sel.tme);
	gs_d3d_macros_add_int(sm, "VS_FST", sel.fst);
	gs_d3d_macros_add_int(sm, "VS_IIP", sel.iip);
	gs_d3d_macros_add_int(sm, "VS_EXPAND", (int)sel.expand);
	*entry = (sel.expand != GS_HW_VS_EXPAND_NONE) ? "vs_main_expand" : "vs_main";
}

/* A vertex shader's place: point_size changes nothing in it here. */
static u32 d12_tfx_vertex_shader_key(gs_hw_vs_selector sel)
{
	sel.point_size = 0;
	return sel.key;
}

/* (gs_hw_blend_state::IsEffective: CONST_ONE 14, CONST_ZERO 15) */
static bool d12_blend_is_effective(const gs_hw_blend_state* b, gs_hw_color_mask colormask)
{
	return b->enable
		&& (((colormask.key & 7u) && (b->src_factor != 14 || b->dst_factor != 15))
			|| ((colormask.key & 8u) && (b->src_factor_alpha != 14 || b->dst_factor_alpha != 15)));
}

/* The pixel shader a pipeline is made with: without its second colour
 * output, and with blending off, when the colours are masked. */
static gs_hw_ps_selector d12_tfx_effective_ps(const gs_d3d12_pipeline_selector* p, gs_hw_blend_state* bs)
{
	gs_hw_ps_selector pps = p->ps;
	*bs                   = p->bs;
	if (!d12_blend_is_effective(&p->bs, p->cms))
	{
		/* disable blending when colours are masked */
		bs->key       = 0;
		pps.no_color1 = 1;
	}
	return pps;
}

/* The dynamic sibling of a draw pipeline: the pipeline's state, and of
 * its pixel selector only what that state and the dynamic shader's
 * interface take (the rest comes in PsKey). */
static void d12_tfx_sibling_of(gs_d3d12_pipeline_selector* sib, const gs_d3d12_pipeline_selector* p)
{
	gs_hw_blend_state pbs;
	const u32 date = gs_d3d_dynamic_index(&p->ps) % 3u;

	d12_tfx_effective_ps(p, &pbs);
	memcpy(sib, p, sizeof(*sib));
	memset(&sib->ps, 0, sizeof(sib->ps));
	sib->ps.iip        = p->ps.iip;
	sib->ps.date       = (date == 1) ? 1 : (date == 2) ? 3 : 0;
	sib->ps.colclip_hw = p->ps.colclip_hw;
	sib->bs            = pbs;
	sib->dynamic       = 1;
}

/* Every vertex shader a draw can have, and the dynamic pixel shaders, as
 * the game boots: then no draw compiles one. With the shader cache this
 * is a lookup each. */
static bool d12_build_tfx_shaders_ahead(gs_device_d3d12* dev)
{
	const size_t code_len = strlen(tfx_fx_shader_raw);
	u32 key, i;

	for (key = 0; key < D12_ARRAY_SIZE(dev->tfx_vs); key++)
	{
		gs_hw_vs_selector vsel;
		const char* entry_point;
		vsel.key = (u8)key;
		if (vsel.point_size || (vsel.expand != GS_HW_VS_EXPAND_NONE && !dev->base.m_features.vs_expand))
			continue;
		d12_macros_init(&dev->sm, dev->shader_cache.feature_level);
		d12_tfx_vertex_shader_macros(&dev->sm, vsel, &entry_point);
		if (!(dev->tfx_vs[key] = gs_d3d12_shader_cache_blob(&dev->shader_cache, GS_D3D12_ENTRY_VERTEX_SHADER,
				  tfx_fx_shader_raw, code_len, gs_d3d_macros_list(&dev->sm), entry_point)))
			return false;
		dev->tfx_built_ahead++;
	}
	for (i = 0; i < D3D_DYNAMIC_PS_COUNT; i++)
	{
		d12_macros_init(&dev->sm, dev->shader_cache.feature_level);
		gs_d3d_dynamic_macros(&dev->sm, i);
		/* (without them every miss builds its own pipeline, as before) */
		if (!(dev->tfx_ps_dynamic[i] = gs_d3d12_shader_cache_blob(&dev->shader_cache, GS_D3D12_ENTRY_PIXEL_SHADER,
				  tfx_fx_shader_raw, code_len, gs_d3d_macros_list(&dev->sm), "ps_main")))
		{
			log_cb(RETRO_LOG_WARN, "GS: the D3D12 dynamic pixel shaders could not be built\n");
			for (i = 0; i < D3D_DYNAMIC_PS_COUNT; i++)
				D12_RELEASE(dev->tfx_ps_dynamic[i]);
			break;
		}
		dev->tfx_built_ahead++;
	}
	return true;
}

static const ID3DBlob* d12_get_tfx_vertex_shader(gs_device_d3d12* dev, gs_hw_vs_selector sel)
{
	const u32 key = d12_tfx_vertex_shader_key(sel);
	const char* entry_point;
	ID3DBlob** slot;

	if (key < D12_ARRAY_SIZE(dev->tfx_vs) && dev->tfx_vs[key])
		return dev->tfx_vs[key];

	slot = &dev->tfx_vertex_shaders[key & 0xFF];
	if (*slot)
		return *slot;

	d12_macros_init(&dev->sm, dev->shader_cache.feature_level);
	d12_tfx_vertex_shader_macros(&dev->sm, sel, &entry_point);
	*slot = gs_d3d12_shader_cache_blob(&dev->shader_cache, GS_D3D12_ENTRY_VERTEX_SHADER, tfx_fx_shader_raw,
		strlen(tfx_fx_shader_raw), gs_d3d_macros_list(&dev->sm), entry_point);
	dev->tfx_shaders_here++;
	return *slot;
}

static const ID3DBlob* d12_get_tfx_pixel_shader(gs_device_d3d12* dev, const gs_hw_ps_selector* sel)
{
	const size_t hash = gs_key_hash(sel, sizeof(*sel));
	ID3DBlob** found  = (ID3DBlob**)gs_flat_map_find(&dev->tfx_pixel_shaders, sel, sizeof(*sel), hash);
	ID3DBlob* ps;
	int inserted;
	ID3DBlob** v;
	if (found)
		return *found;

	d12_macros_init(&dev->sm, dev->shader_cache.feature_level);
	gs_d3d_ps_macros(&dev->sm, sel);
	ps = gs_d3d12_shader_cache_blob(&dev->shader_cache, GS_D3D12_ENTRY_PIXEL_SHADER, tfx_fx_shader_raw,
		strlen(tfx_fx_shader_raw), gs_d3d_macros_list(&dev->sm), "ps_main");
	dev->tfx_shaders_here++;
	v = (ID3DBlob**)gs_flat_map_insert(&dev->tfx_pixel_shaders, sel, hash, &inserted);
	if (!v)
	{
		D12_RELEASE(ps);
		return NULL;
	}
	*v = ps;
	return ps;
}

/* Everything of a draw pipeline but its shaders' building: the drawing
 * thread and the warm-up thread both. Reads only what the device set up
 * at its creation. */
static void d12_fill_tfx_pipeline(const gs_device_d3d12* dev, gs_d3d12_gp_builder* gpb,
	const gs_d3d12_pipeline_selector* p, const ID3DBlob* vs, const ID3DBlob* ps)
{
	static const D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_lookup[3] = {
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT,    /* Point */
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,     /* Line */
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, /* Triangle */
	};
	gs_hw_blend_state pbs;
	d12_tfx_effective_ps(p, &pbs);

	/* Common state */
	gs_d3d12_gp_set_root_signature(gpb, dev->tfx_root_signature);
	gs_d3d12_gp_set_topology_type(gpb, topology_lookup[p->topology]);
	gs_d3d12_gp_set_rasterization(gpb, D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, 0);
	if (p->rt)
	{
		const unsigned format = d12_is_date_mode_prim_id_init(p->ps.date) ? GS_TEX_PRIMID
			: p->ps.colclip_hw                                            ? GS_TEX_COLOR_CLIP
																		  : GS_TEX_COLOR;
		DXGI_FORMAT native_format;
		d12_lookup_native_format(format, NULL, NULL, &native_format, NULL);
		gs_d3d12_gp_set_render_target(gpb, 0, native_format);
	}
	if (p->ds)
		gs_d3d12_gp_set_depth_stencil_format(gpb, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);

	/* Shaders */
	d12_gp_set_vs(gpb, vs);
	d12_gp_set_ps(gpb, ps);

	/* IA */
	if (p->vs.expand == GS_HW_VS_EXPAND_NONE)
	{
		gs_d3d12_gp_add_vertex_attribute(gpb, "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0);
		gs_d3d12_gp_add_vertex_attribute(gpb, "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 8);
		gs_d3d12_gp_add_vertex_attribute(gpb, "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 12);
		gs_d3d12_gp_add_vertex_attribute(gpb, "POSITION", 0, DXGI_FORMAT_R16G16_UINT, 0, 16);
		gs_d3d12_gp_add_vertex_attribute(gpb, "POSITION", 1, DXGI_FORMAT_R32_UINT, 0, 20);
		gs_d3d12_gp_add_vertex_attribute(gpb, "TEXCOORD", 2, DXGI_FORMAT_R16G16_UINT, 0, 24);
		gs_d3d12_gp_add_vertex_attribute(gpb, "COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28);
	}

	/* DepthStencil */
	if (p->ds)
	{
		static const D3D12_COMPARISON_FUNC ztst[] = {D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_ALWAYS,
			D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_GREATER};
		gs_d3d12_gp_set_depth(gpb, (p->dss.ztst != ZTST_ALWAYS || p->dss.zwe), p->dss.zwe, ztst[p->dss.ztst]);
		if (p->dss.date)
		{
			D3D12_DEPTH_STENCILOP_DESC sos;
			sos.StencilFailOp      = D3D12_STENCIL_OP_KEEP;
			sos.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
			sos.StencilPassOp      = p->dss.date_one ? D3D12_STENCIL_OP_ZERO : D3D12_STENCIL_OP_KEEP;
			sos.StencilFunc        = D3D12_COMPARISON_FUNC_EQUAL;
			gs_d3d12_gp_set_stencil(gpb, 1, 1, 1, &sos, &sos);
		}
	}
	else
		d12_gp_no_depth(gpb);

	/* Blending */
	if (d12_is_date_mode_prim_id_init(p->ps.date))
	{
		/* image DATE prepass */
		gs_d3d12_gp_set_blend(gpb, 0, 1, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_MIN, D3D12_BLEND_ONE,
			D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD, D3D12_COLOR_WRITE_ENABLE_RED);
	}
	else if (pbs.enable)
	{
		static const D3D12_BLEND d3d_blend_factors[16] = {D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR,
			D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_SRC1_COLOR, D3D12_BLEND_INV_SRC1_COLOR,
			D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA,
			D3D12_BLEND_SRC1_ALPHA, D3D12_BLEND_INV_SRC1_ALPHA, D3D12_BLEND_BLEND_FACTOR,
			D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_ONE, D3D12_BLEND_ZERO};
		static const D3D12_BLEND_OP d3d_blend_ops[3] = {
			D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT, D3D12_BLEND_OP_REV_SUBTRACT};

		gs_d3d12_gp_set_blend(gpb, 0, 1, d3d_blend_factors[pbs.src_factor], d3d_blend_factors[pbs.dst_factor],
			d3d_blend_ops[pbs.op], d3d_blend_factors[pbs.src_factor_alpha], d3d_blend_factors[pbs.dst_factor_alpha],
			D3D12_BLEND_OP_ADD, p->cms.wrgba);
	}
	else
	{
		gs_d3d12_gp_set_blend(gpb, 0, 0, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE,
			D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, p->cms.wrgba);
	}
}

static ID3D12PipelineState* d12_create_tfx_pipeline(gs_device_d3d12* dev, const gs_d3d12_pipeline_selector* p)
{
	gs_hw_blend_state pbs;
	const gs_hw_ps_selector pps = d12_tfx_effective_ps(p, &pbs);
	const ID3DBlob* vs          = d12_get_tfx_vertex_shader(dev, p->vs);
	const ID3DBlob* ps          = d12_get_tfx_pixel_shader(dev, &pps);
	gs_d3d12_gp_builder gpb;
	if (!vs || !ps)
		return NULL;

	gs_d3d12_gp_clear(&gpb);
	d12_fill_tfx_pipeline(dev, &gpb, p, vs, ps);
	return d12_gp_create_cached(dev, &gpb, true);
}

/* A dynamic sibling no draw had yet, on the drawing thread: its shaders
 * were all built as the device was created, so this is the driver's part
 * alone, and no cache file is read or written for it. Listed, so that the
 * next start builds it before the first frame. */
static ID3D12PipelineState* d12_create_tfx_sibling(gs_device_d3d12* dev, const gs_d3d12_pipeline_selector* sib)
{
	const ID3DBlob* vs = d12_get_tfx_vertex_shader(dev, sib->vs);
	gs_d3d12_gp_builder gpb;
	ID3D12PipelineState* pso;

	if (!vs)
		return NULL;
	gs_d3d12_gp_clear(&gpb);
	d12_fill_tfx_pipeline(dev, &gpb, sib, vs, dev->tfx_ps_dynamic[gs_d3d_dynamic_index(&sib->ps)]);
	if ((pso = d12_gp_create(dev->device, &gpb, true)))
	{
		dev->tfx_pipelines_here++;
		dev->tfx_siblings_here++;
		gs_warm_list_add(&dev->tfx_list, sib);
	}
	return pso;
}

/* A pipeline no draw had yet, with no warm-up thread or no dynamic
 * shaders: one the warm-up thread built, else built here, and listed for
 * the next start either way it was not listed. */
static ID3D12PipelineState** d12_add_missing_tfx_pipeline(gs_device_d3d12* dev, const gs_d3d12_pipeline_selector* p)
{
	ID3D12PipelineState** it;
	ID3D12PipelineState* pipeline;

	if (dev->tfx_warm && d12_collect_warmed_tfx_pipelines(dev, p, &it))
		return it;

	pipeline = d12_create_tfx_pipeline(dev, p);
	dev->tfx_pipelines_here++;
	if (pipeline)
		gs_warm_list_add(&dev->tfx_list, p);
	return d12_pipeline_assign(dev, p, pipeline);
}

/* A draw whose pipeline is not ready: queued for the warm-up thread when
 * it is not yet, and drawn meanwhile with its dynamic sibling (whose PsKey
 * d12_bind_draw_pipeline sets). A full queue leaves it to the next draw. */
static const ID3D12PipelineState* d12_missed_tfx_pipeline(gs_device_d3d12* dev, const gs_d3d12_pipeline_selector* p,
	bool* dynamic)
{
	ID3D12PipelineState** it;
	gs_d3d12_pipeline_selector sib;
	gs_d3d12_warm_job job;

	if (!dev->tfx_warm || !dev->tfx_ps_dynamic[0])
	{
		it = d12_pipeline_find(dev, p);
		if (!it || !*it)
			it = d12_add_missing_tfx_pipeline(dev, p);
		memcpy(&dev->last_tfx_pipeline_selector, p, sizeof(*p));
		dev->last_tfx_pipeline = it ? *it : NULL;
		return dev->last_tfx_pipeline;
	}

	if (d12_collect_warmed_tfx_pipelines(dev, p, &it) && *it && !dev->test_dynamic)
	{
		memcpy(&dev->last_tfx_pipeline_selector, p, sizeof(*p));
		dev->last_tfx_pipeline = *it;
		return dev->last_tfx_pipeline;
	}
	if (!dev->test_dynamic && !d12_pipeline_find(dev, p))
	{
		memset(&job, 0, sizeof(job));
		memcpy(&job.p, p, sizeof(*p));
		job.utility = GS_D3D12_UTIL_COUNT;
		if (gs_warm_queue(dev->tfx_warm, &job))
		{
			int inserted;
			/* (null until it lands) */
			d12_pipeline_entry(dev, p, &inserted);
			gs_warm_list_add(&dev->tfx_list, p);
			dev->tfx_queued++;
		}
	}

	d12_tfx_sibling_of(&sib, p);
	it = d12_pipeline_find(dev, &sib);
	if (!it || !*it)
	{
		ID3D12PipelineState* pso = d12_create_tfx_sibling(dev, &sib);
		if (!pso)
			return NULL;
		if (!(it = d12_pipeline_assign(dev, &sib, pso)))
			return NULL;
	}
	/* (looked for again by the next draw: its own may land) */
	dev->last_tfx_pipeline = NULL;
	dev->tfx_sibling_draws++;
	*dynamic = true;
	return *it;
}

static const ID3D12PipelineState* d12_get_tfx_pipeline(gs_device_d3d12* dev, const gs_d3d12_pipeline_selector* p,
	bool* dynamic)
{
	ID3D12PipelineState** it;

	*dynamic = false;
	if (dev->last_tfx_pipeline && !memcmp(&dev->last_tfx_pipeline_selector, p, sizeof(*p)))
		return dev->last_tfx_pipeline;

	it = d12_pipeline_find(dev, p);
	if (!it || !*it || dev->test_dynamic)
		return d12_missed_tfx_pipeline(dev, p, dynamic);

	memcpy(&dev->last_tfx_pipeline_selector, p, sizeof(*p));
	dev->last_tfx_pipeline = *it;
	return dev->last_tfx_pipeline;
}

/* ==== draw pipelines built ahead ==== */

static int d12_build_warmed_tfx_pipeline(void* ud, const void* job, void* result);

static bool d12_start_tfx_warm(gs_device_d3d12* dev, size_t* first)
{
	const char* path = gs_d3d12_shader_cache_pipeline_list_path(&dev->shader_cache);
	gs_d3d12_warm_job* jobs;
	size_t i, n, at;

	gs_warm_list_free(&dev->tfx_list);
	dev->tfx_list_loaded = 0;
	if (path && *path && !gs_warm_list_load(&dev->tfx_list, path, TFX_LIST_VERSION) && path_is_valid(path))
		log_cb(RETRO_LOG_WARN, "GS: ignoring the pipeline list '%s' (another version, or damaged)\n", path);
	dev->tfx_list_loaded = dev->tfx_list.count;

	n = GS_D3D12_UTIL_COUNT + dev->tfx_list.count;
	if (!(jobs = (gs_d3d12_warm_job*)calloc(n, sizeof(gs_d3d12_warm_job))))
		return false;
	for (i = 0; i < GS_D3D12_UTIL_COUNT; i++)
		jobs[i].utility = (u32)i;
	/* the listed siblings first: the first frame waits for them and the
	 * utility pipelines, and for nothing else */
	at = GS_D3D12_UTIL_COUNT;
	for (i = 0; i < dev->tfx_list.count; i++)
	{
		const gs_d3d12_pipeline_selector* p =
			(const gs_d3d12_pipeline_selector*)(dev->tfx_list.recs + i * dev->tfx_list.rec_size);
		if (!p->dynamic || !dev->tfx_ps_dynamic[0])
			continue;
		memcpy(&jobs[at].p, p, sizeof(*p));
		jobs[at++].utility = GS_D3D12_UTIL_COUNT;
	}
	*first = at;
	for (i = 0; i < dev->tfx_list.count; i++)
	{
		const gs_d3d12_pipeline_selector* p =
			(const gs_d3d12_pipeline_selector*)(dev->tfx_list.recs + i * dev->tfx_list.rec_size);
		if (p->dynamic)
			continue;
		memcpy(&jobs[at].p, p, sizeof(*p));
		jobs[at++].utility = GS_D3D12_UTIL_COUNT;
	}

	if (gs_d3d12_cache_snapshot_open(&dev->shader_cache, &dev->tfx_warm_cache))
		dev->tfx_warm = gs_warm_start(jobs, sizeof(gs_d3d12_warm_job), at, D3D12_TFX_QUEUED,
			sizeof(gs_d3d12_warm_result), d12_build_warmed_tfx_pipeline, dev);
	else
		log_cb(RETRO_LOG_WARN, "GS: the shader cache could not be opened for the warm-up thread\n");
	free(jobs);
	if (!dev->tfx_warm)
		gs_d3d12_cache_snapshot_close(&dev->tfx_warm_cache);
	return dev->tfx_warm != NULL;
}

/* What the warm-up thread built that the cache did not have, kept to be
 * added to it at teardown rather than written while drawing. */
static void d12_keep_tfx_warm_blobs(gs_device_d3d12* dev, const gs_d3d12_warm_result* r)
{
	if (!r->vs_blob && !r->ps_blob && !r->pso_blob)
		return;
	if (dev->tfx_warm_blob_count == dev->tfx_warm_blob_cap)
	{
		const size_t cap = dev->tfx_warm_blob_cap ? dev->tfx_warm_blob_cap * 2 : 64;
		gs_d3d12_warm_result* blobs =
			(gs_d3d12_warm_result*)realloc(dev->tfx_warm_blobs, cap * sizeof(gs_d3d12_warm_result));
		if (!blobs)
		{
			if (r->vs_blob)
				ID3D10Blob_Release(r->vs_blob);
			if (r->ps_blob)
				ID3D10Blob_Release(r->ps_blob);
			if (r->pso_blob)
				ID3D10Blob_Release(r->pso_blob);
			return;
		}
		dev->tfx_warm_blobs    = blobs;
		dev->tfx_warm_blob_cap = cap;
	}
	memcpy(&dev->tfx_warm_blobs[dev->tfx_warm_blob_count++], r, sizeof(*r));
}

/* Takes what the warm-up thread finished into the map; a pipeline the
 * drawing thread built meanwhile keeps its own. True, and where it is,
 * when @want was among them. */
static bool d12_collect_warmed_tfx_pipelines(gs_device_d3d12* dev, const gs_d3d12_pipeline_selector* want,
	ID3D12PipelineState*** found)
{
	gs_d3d12_warm_result r;
	bool hit = false;

	while (gs_warm_take(dev->tfx_warm, &r))
	{
		ID3D12PipelineState** v;
		int inserted;
		if (r.utility < GS_D3D12_UTIL_COUNT)
		{
			/* a utility pipeline first used before the thread built it keeps
			 * the one built then */
			if (!dev->utility[r.utility])
				dev->utility[r.utility] = r.pso;
			else
				ID3D12PipelineState_Release(r.pso);
			r.pso = NULL;
			d12_keep_tfx_warm_blobs(dev, &r);
			continue;
		}
		v = d12_pipeline_entry(dev, &r.p, &inserted);
		/* (a queued one waits null; one built here meanwhile keeps its own) */
		if (v && !*v)
			*v = r.pso;
		else
			ID3D12PipelineState_Release(r.pso);
		if (want && !memcmp(&r.p, want, sizeof(*want)))
			hit = true;
		r.pso = NULL;
		d12_keep_tfx_warm_blobs(dev, &r);
	}
	/* Looked up once the map has stopped growing: an insertion that grows
	 * it moves every entry. */
	if (hit)
		*found = d12_pipeline_find(dev, want);
	return hit;
}

/* Before anything the warm-up thread uses goes: the thread is joined,
 * what it finished is taken (and destroyed with the rest), its blobs go
 * to the cache, and the list is saved when this session added to it. */
static void d12_stop_tfx_warm(gs_device_d3d12* dev)
{
	size_t i;

	if (dev->tfx_warm)
	{
		gs_warm_stop(dev->tfx_warm);
		d12_collect_warmed_tfx_pipelines(dev, NULL, NULL);
		gs_warm_free(dev->tfx_warm);
		dev->tfx_warm = NULL;
		gs_d3d12_cache_snapshot_close(&dev->tfx_warm_cache);
	}

	for (i = 0; i < dev->tfx_warm_blob_count; i++)
	{
		gs_d3d12_warm_result* r = &dev->tfx_warm_blobs[i];
		if (r->vs_blob)
		{
			gs_d3d12_shader_cache_add_shader(&dev->shader_cache, &r->vs_key, r->vs_blob);
			ID3D10Blob_Release(r->vs_blob);
		}
		if (r->ps_blob)
		{
			gs_d3d12_shader_cache_add_shader(&dev->shader_cache, &r->ps_key, r->ps_blob);
			ID3D10Blob_Release(r->ps_blob);
		}
		if (r->pso_blob)
		{
			gs_d3d12_shader_cache_add_pipeline(&dev->shader_cache, &r->pso_key, r->pso_blob);
			ID3D10Blob_Release(r->pso_blob);
		}
	}
	free(dev->tfx_warm_blobs);
	dev->tfx_warm_blobs      = NULL;
	dev->tfx_warm_blob_count = 0;
	dev->tfx_warm_blob_cap   = 0;

	if (dev->tfx_list.count > dev->tfx_list_loaded)
	{
		const char* path = gs_d3d12_shader_cache_pipeline_list_path(&dev->shader_cache);
		gs_warm_list_unique(&dev->tfx_list);
		if (path && *path)
			gs_warm_list_save(&dev->tfx_list, path, TFX_LIST_VERSION);
	}
	gs_warm_list_free(&dev->tfx_list);
	dev->tfx_list_loaded = 0;
}

/* The warm-up thread: a shader from the cache as it stood when the thread
 * started, else compiled - and then handed back in @built, for the cache. */
static ID3DBlob* d12_warm_shader(gs_device_d3d12* dev, unsigned type, const char* source, gs_d3d_macros* sm,
	const char* entry, gs_d3d12_cache_key* key, ID3DBlob** built)
{
	const size_t len = strlen(source);
	ID3DBlob* blob;

	gs_d3d12_shader_key(key, type, source, len, gs_d3d_macros_list(sm), entry);
	if ((blob = gs_d3d12_cache_snapshot_read(&dev->tfx_warm_cache, key)))
		return blob;
	blob = gs_d3d_compile_shader(type, dev->shader_cache.feature_level, dev->shader_cache.debug, source, len,
		gs_d3d_macros_list(sm), entry);
	if (blob)
	{
		*built = blob;
		ID3D10Blob_AddRef(*built);
	}
	return blob;
}

/* The warm-up thread: a pipeline from the driver's cached blob when that
 * still holds, else built whole - and its blob handed back in @built. */
static ID3D12PipelineState* d12_warm_pipeline(gs_device_d3d12* dev, D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
	gs_d3d12_cache_key* key, ID3DBlob** built)
{
	ID3DBlob* cached         = NULL;
	ID3D12PipelineState* pso = NULL;

	gs_d3d12_graphics_pipeline_key(key, desc);
	if ((cached = gs_d3d12_cache_snapshot_read(&dev->tfx_warm_cache, key)))
	{
		desc->CachedPSO.pCachedBlob           = ID3D10Blob_GetBufferPointer(cached);
		desc->CachedPSO.CachedBlobSizeInBytes = ID3D10Blob_GetBufferSize(cached);
		if (FAILED(ID3D12Device_CreateGraphicsPipelineState(dev->device, desc, &s_iid_pipeline_state,
				(void**)&pso)))
			pso = NULL;
		desc->CachedPSO.pCachedBlob           = NULL;
		desc->CachedPSO.CachedBlobSizeInBytes = 0;
		ID3D10Blob_Release(cached);
	}
	if (!pso)
	{
		ID3DBlob* blob = NULL;
		if (FAILED(ID3D12Device_CreateGraphicsPipelineState(dev->device, desc, &s_iid_pipeline_state, (void**)&pso)))
			return NULL;
		if (SUCCEEDED(ID3D12PipelineState_GetCachedBlob(pso, &blob)))
			*built = blob;
	}
	return pso;
}

static int d12_build_warmed_utility_pipeline(gs_device_d3d12* dev, u32 slot, gs_d3d12_warm_result* r)
{
	char entry_buf[32];
	const char* source;
	const char* entry = d12_utility_pixel_shader(slot, &source, entry_buf);
	gs_d3d12_gp_builder gpb;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	ID3DBlob *vs, *ps;
	ID3D12PipelineState* pso = NULL;

	d12_macros_init(&dev->warm_sm, dev->shader_cache.feature_level);
	vs = d12_warm_shader(dev, GS_D3D12_ENTRY_VERTEX_SHADER, convert_fx_shader_raw, &dev->warm_sm, "vs_main", &r->vs_key,
		&r->vs_blob);
	ps = d12_warm_shader(dev, GS_D3D12_ENTRY_PIXEL_SHADER, source, &dev->warm_sm, entry, &r->ps_key, &r->ps_blob);
	gs_d3d12_gp_clear(&gpb);
	if (vs && ps && d12_utility_pipeline_desc(&gpb, slot, dev->utility_root_signature, vs, ps))
	{
		desc = gpb.desc;
		pso  = d12_warm_pipeline(dev, &desc, &r->pso_key, &r->pso_blob);
	}
	D12_RELEASE(vs);
	D12_RELEASE(ps);
	if (!pso)
		return 0;
	r->pso = pso;
	return 1;
}

/* The warm-up thread: one job - a utility pipeline, or a listed draw
 * pipeline with its shaders. */
static int d12_build_warmed_tfx_pipeline(void* ud, const void* job, void* result)
{
	gs_device_d3d12* dev              = (gs_device_d3d12*)ud;
	const gs_d3d12_warm_job* j        = (const gs_d3d12_warm_job*)job;
	const gs_d3d12_pipeline_selector* p = &j->p;
	gs_d3d12_warm_result* r           = (gs_d3d12_warm_result*)result;
	const D3D_FEATURE_LEVEL fl        = dev->shader_cache.feature_level;
	gs_hw_blend_state pbs;
	gs_hw_ps_selector pps;
	gs_d3d12_gp_builder gpb;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	ID3DBlob* vs = NULL;
	ID3DBlob* ps = NULL;
	ID3D12PipelineState* pso;
	const u32 vkey = d12_tfx_vertex_shader_key(p->vs);
	int built      = 0;

	memset(r, 0, sizeof(*r));
	r->utility = j->utility;
	if (j->utility < GS_D3D12_UTIL_COUNT)
		built = d12_build_warmed_utility_pipeline(dev, j->utility, r);
	else
	{
		memcpy(&r->p, p, sizeof(*p));
		/* (a selector no draw could make: the tables it indexes end there) */
		if (p->topology > 2 || p->bs.op > 2)
			return 0;
		pps = d12_tfx_effective_ps(p, &pbs);
		/* (the device's own built ahead: not changed while this runs) */
		if (vkey < D12_ARRAY_SIZE(dev->tfx_vs) && dev->tfx_vs[vkey])
		{
			vs = dev->tfx_vs[vkey];
			ID3D10Blob_AddRef(vs);
		}
		else
		{
			const char* entry;
			d12_macros_init(&dev->warm_sm, fl);
			d12_tfx_vertex_shader_macros(&dev->warm_sm, p->vs, &entry);
			vs = d12_warm_shader(dev, GS_D3D12_ENTRY_VERTEX_SHADER, tfx_fx_shader_raw, &dev->warm_sm, entry, &r->vs_key,
				&r->vs_blob);
		}
		if (vs && p->dynamic)
		{
			ps = dev->tfx_ps_dynamic[gs_d3d_dynamic_index(&p->ps)];
			if (ps)
				ID3D10Blob_AddRef(ps);
		}
		else if (vs)
		{
			d12_macros_init(&dev->warm_sm, fl);
			gs_d3d_ps_macros(&dev->warm_sm, &pps);
			ps = d12_warm_shader(dev, GS_D3D12_ENTRY_PIXEL_SHADER, tfx_fx_shader_raw, &dev->warm_sm, "ps_main",
				&r->ps_key, &r->ps_blob);
		}
		if (vs && ps)
		{
			gs_d3d12_gp_clear(&gpb);
			d12_fill_tfx_pipeline(dev, &gpb, p, vs, ps);
			desc = gpb.desc;
			if ((pso = d12_warm_pipeline(dev, &desc, &r->pso_key, &r->pso_blob)))
			{
				r->pso = pso;
				built  = 1;
			}
		}
		D12_RELEASE(vs);
		D12_RELEASE(ps);
	}
	if (!built)
	{
		if (r->vs_blob)
			ID3D10Blob_Release(r->vs_blob);
		if (r->ps_blob)
			ID3D10Blob_Release(r->ps_blob);
		if (r->pso_blob)
			ID3D10Blob_Release(r->pso_blob);
	}
	return built;
}

static bool d12_bind_draw_pipeline(gs_device_d3d12* dev, const gs_d3d12_pipeline_selector* p)
{
	bool dynamic;
	const ID3D12PipelineState* pipeline = d12_get_tfx_pipeline(dev, p, &dynamic);
	if (!pipeline)
		return false;

	d12_set_pipeline(dev, pipeline);

	if (!d12_apply_tfx_state(dev, false))
		return false;
	/* (after the root signature is bound: its arguments go with it) */
	if (dynamic)
	{
		gs_hw_blend_state pbs;
		const gs_hw_ps_selector pps = d12_tfx_effective_ps(p, &pbs);
		u32 key[4];
		gs_d3d_dynamic_key(&pps, key);
		ID3D12GraphicsCommandList4_SetGraphicsRoot32BitConstants(d12_cmdlist(dev), GS_D3D12_TFX_PARAM_PS_KEY, 4, key, 0);
	}
	return true;
}

static void d12_initialize_state(gs_device_d3d12* dev)
{
	u32 i;
	for (i = 0; i < GS_D3D12_NUM_TOTAL_TFX_TEXTURES; i++)
		dev->tfx_textures[i] = dev->null_texture->srv_descriptor;
	dev->tfx_sampler_sel = 0; /* (SamplerSelector::Point) */

	d12_invalidate_cached_state(dev);
}

static void d12_initialize_samplers(gs_device_d3d12* dev)
{
	gs_hw_sampler_selector point, linear, tfx;
	bool result;
	point.key  = 0;
	linear.key = 0;
	linear.biln = 1;
	tfx.key    = (u8)dev->tfx_sampler_sel;

	result = d12_get_sampler(dev, &dev->point_sampler_cpu, point);
	result = result && d12_get_sampler(dev, &dev->linear_sampler_cpu, linear);
	result = result && d12_get_sampler(dev, &dev->tfx_sampler, tfx);

	if (!result)
		log_cb(RETRO_LOG_ERROR, "Failed to initialize samplers\n");
}

static void d12_dev_clear_sampler_cache(gs_device* d)
{
	gs_device_d3d12* dev = D12_DEV(d);
	u32 i;
	d12_execute_command_list(dev, false);
	for (i = 0; i < D12_ARRAY_SIZE(dev->samplers); i++)
		if (D12_HANDLE_VALID(dev->samplers[i]))
		{
			gs_d3d12_descriptor_heap_manager_free_index(&dev->sampler_heap_manager, dev->samplers[i].index);
			gs_d3d12_descriptor_handle_clear(&dev->samplers[i]);
		}
	d12_invalidate_sampler_groups(dev);
	d12_initialize_samplers(dev);

	dev->utility_sampler_gpu = dev->point_sampler_cpu;
	gs_d3d12_descriptor_handle_clear(&dev->tfx_samplers_handle_gpu);
	dev->dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS;
}

/* ==== the HW draw ==== */

static void d12_set_vs_constant_buffer(gs_device_d3d12* dev, const gs_hw_vs_cb* cb)
{
	if (memcmp(&dev->vs_cb_cache, cb, sizeof(*cb)))
	{
		memcpy(&dev->vs_cb_cache, cb, sizeof(*cb));
		dev->dirty_flags |= DIRTY_FLAG_VS_CONSTANT_BUFFER;
	}
}

static void d12_set_ps_constant_buffer(gs_device_d3d12* dev, const gs_hw_ps_cb* cb)
{
	if (memcmp(&dev->ps_cb_cache, cb, sizeof(*cb)))
	{
		memcpy(&dev->ps_cb_cache, cb, sizeof(*cb));
		dev->dirty_flags |= DIRTY_FLAG_PS_CONSTANT_BUFFER;
	}
}

/* The quad over bbox in a target of w by h (DATE's) */
static void d12_date_quad(d12_vertex_pt1 v[4], const gs_recti* bbox, int w, int h)
{
	gs_rectf src, dst;
	d12_norm_rect(&src, bbox, w, h);
	dst.x = src.x * 2.0f - 1.0f;
	dst.y = src.y * 2.0f - 1.0f;
	dst.z = src.z * 2.0f - 1.0f;
	dst.w = src.w * 2.0f - 1.0f;
	d12_set_vertex(&v[0], dst.x, -dst.y, src.x, src.y);
	d12_set_vertex(&v[1], dst.z, -dst.y, src.z, src.y);
	d12_set_vertex(&v[2], dst.x, -dst.w, src.x, src.w);
	d12_set_vertex(&v[3], dst.z, -dst.w, src.z, src.w);
}

static void d12_setup_date(gs_device_d3d12* dev, gs_texture* rt, gs_texture* ds, int datm, const gs_recti* bbox)
{
	d12_vertex_pt1 vertices[4];
	d12_date_quad(vertices, bbox, ds->m_size.x, ds->m_size.y);

	/* sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii
	 * shadows, rumble roses shadows, persona4 shadows */
	d12_end_render_pass(dev);
	d12_set_utility_texture(dev, rt, &dev->point_sampler_cpu);
	d12_om_set_render_targets(dev, NULL, D12_TEX(ds), bbox);
	d12_ia_set_vertex_buffer(dev, vertices, sizeof(vertices[0]), 4);
	d12_set_primitive_topology(dev, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	d12_set_pipeline(dev, d12_utility_pipeline(dev, GS_D3D12_UTIL_CONVERT + (u32)gs_set_datm_shader(datm)));
	d12_set_stencil_ref(dev, 1);
	d12_begin_render_pass(dev, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
		D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE,
		D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR,
		D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE, s_d12_zero4, 0.0f, 0);
	if (d12_apply_utility_state(dev))
		d12_draw_primitive(dev);

	d12_end_render_pass(dev);
}

static void d12_upload_hw_draw_vertices_and_indices(gs_device_d3d12* dev, const gs_hw_draw_config* config)
{
	d12_ia_set_vertex_buffer(dev, config->verts, 32 /* (GSVertex) */, config->nverts);

	/* Update SRV in root signature directly, rather than using a uniform
	 * for base vertex. */
	if (config->vs.expand != GS_HW_VS_EXPAND_NONE)
		dev->dirty_flags |= DIRTY_FLAG_VS_VERTEX_BUFFER_BINDING;

	if (config->vs.expand == GS_HW_VS_EXPAND_POINT || config->vs.expand == GS_HW_VS_EXPAND_SPRITE)
	{
		dev->base.m_index.start = 0;
		dev->base.m_index.count = config->nindices;
		d12_set_index_buffer(dev, ID3D12Resource_GetGPUVirtualAddress(dev->expand_index_buffer),
			GS_DEVICE_EXPAND_BUFFER_SIZE, DXGI_FORMAT_R16_UINT);
	}
	else
		d12_ia_set_index_buffer(dev, config->indices, config->nindices);
}

static gs_texture_d3d12* d12_setup_primitive_tracking_date(gs_device_d3d12* dev, gs_hw_draw_config* config,
	gs_d3d12_pipeline_selector* pipe)
{
	/* How this is done:
	 * - can't put a barrier for the image in the middle of the normal
	 *   render pass, so that's out
	 * - so, instead of just filling the int texture with INT_MAX, we sample
	 *   the RT and use -1 for failing values
	 * - then, instead of sampling the RT with DATE=1/2, we just do a min()
	 *   without it, the -1 gets preserved
	 * - then, the DATE=3 draw is done as normal */
	const int rtsize_x = config->rt->m_size.x;
	const int rtsize_y = config->rt->m_size.y;
	gs_texture_d3d12* image =
		D12_TEX(gs_device_create_render_target(&dev->base, rtsize_x, rtsize_y, GS_TEX_PRIMID, false));
	gs_d3d12_pipeline_selector init_pipe;
	d12_vertex_pt1 vertices[4];
	if (!image)
		return NULL;

	d12_end_render_pass(dev);

	/* setup the fill quad to prefill with existing alpha values */
	d12_set_utility_texture(dev, config->rt, &dev->point_sampler_cpu);
	d12_om_set_render_targets(dev, image, D12_TEX(config->ds), &config->drawarea);

	/* if the depth target has been cleared, we need to preserve that clear */
	d12_begin_render_pass(dev, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD,
		D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE,
		config->ds ? d12_load_op(D12_TEX(config->ds)) : D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
		config->ds ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
		D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS, s_d12_zero4,
		config->ds ? config->ds->m_clear_value.depth : 0.0f, 0);

	/* draw the quad to prefill the image */
	d12_date_quad(vertices, &config->drawarea, rtsize_x, rtsize_y);
	d12_set_utility_root_signature(dev);
	d12_set_primitive_topology(dev, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	d12_set_pipeline(dev, d12_utility_pipeline(dev, GS_D3D12_UTIL_DATE_SETUP + pipe->ds * 4 + (u32)config->datm));
	d12_ia_set_vertex_buffer(dev, vertices, sizeof(vertices[0]), D12_ARRAY_SIZE(vertices));
	if (d12_apply_utility_state(dev))
		d12_draw_primitive(dev);

	/* image is now filled with either -1 or INT_MAX, so now we can do the prepass */
	d12_set_primitive_topology(dev, s_primitive_topology_mapping[config->topology]);
	d12_upload_hw_draw_vertices_and_indices(dev, config);

	/* cut down the configuration for the prepass, we don't need blending
	 * or any feedback loop */
	memcpy(&init_pipe, &dev->pipeline_selector, sizeof(init_pipe));
	init_pipe.dss.zwe   = 0;
	init_pipe.cms.wrgba = 0;
	init_pipe.bs.key    = 0;
	init_pipe.rt        = 1;
	init_pipe.ps.blend_a = init_pipe.ps.blend_b = init_pipe.ps.blend_c = init_pipe.ps.blend_d = 0;
	init_pipe.ps.no_color  = 0;
	init_pipe.ps.no_color1 = 1;
	if (d12_bind_draw_pipeline(dev, &init_pipe))
		d12_draw_indexed_primitive(dev);

	/* image is initialized/prepass is done, so finish up and get ready to
	 * do the "real" draw */
	d12_end_render_pass(dev);

	/* .. by setting it to DATE=3 */
	pipe->ps.date                     = 3;
	config->alpha_second_pass.ps.date = 3;

	/* and bind the image to the primitive sampler */
	d12_transition(image, dev, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	d12_ps_set_shader_resource(dev, 3, &image->base, false);
	return image;
}

static void d12_update_hw_pipeline_selector(gs_device_d3d12* dev, const gs_hw_draw_config* config)
{
	gs_d3d12_pipeline_selector* p = &dev->pipeline_selector;
	p->vs.key       = config->vs.key;
	p->ps.key_hi    = config->ps.key_hi;
	p->ps.key_lo    = config->ps.key_lo;
	p->dss.key      = config->depth.key;
	p->bs.key       = config->blend.key;
	p->bs.constant  = 0; /* don't dupe states with different alpha values */
	p->cms.key      = config->colormask.key;
	p->topology     = config->topology;
	p->rt           = config->rt != NULL;
	p->ds           = config->ds != NULL;
}

/* The colour clip target resolved into the draw's target over the update
 * area, and given back */
static void d12_colclip_resolve(gs_device_d3d12* dev, gs_hw_draw_config* config, gs_texture_d3d12* hdr_rt,
	gs_texture_d3d12* draw_ds, int rtsize_x, int rtsize_y)
{
	gs_texture_d3d12* draw_rt = D12_TEX(config->rt);
	gs_rectf sRect, dRect;
	float clear_color[4];

	d12_end_render_pass(dev);
	d12_transition(hdr_rt, dev, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	d12_om_set_render_targets(dev, draw_rt, draw_ds, &config->scissor);

	/* if this target was cleared and never drawn to, perform the clear as
	 * part of the resolve here. */
	d12_unorm8(clear_color, draw_rt->base.m_clear_value.color);
	d12_begin_render_pass(dev, d12_load_op(draw_rt), D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE,
		d12_load_op(draw_ds),
		draw_ds ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
		D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS, clear_color,
		0.0f, 0);

	d12_norm_rect(&sRect, &config->colclip_update_area, rtsize_x, rtsize_y);
	d12_rectf_of(&dRect, &config->colclip_update_area);
	d12_set_pipeline(dev, d12_utility_pipeline(dev, GS_D3D12_UTIL_HDR_FINISH + dev->pipeline_selector.ds));
	d12_set_utility_texture(dev, &hdr_rt->base, &dev->point_sampler_cpu);
	d12_draw_stretch_rect(dev, &sRect, &dRect, rtsize_x, rtsize_y);

	gs_device_recycle(&dev->base, &hdr_rt->base);
	dev->base.m_colclip_rt = NULL;
}

static void d12_dev_render_hw(gs_device* d, struct GSHWDrawConfig* cfg)
{
	gs_device_d3d12* dev            = D12_DEV(d);
	gs_hw_draw_config* config       = (gs_hw_draw_config*)cfg;
	gs_d3d12_pipeline_selector* pipe = &dev->pipeline_selector;
	/* Destination Alpha Setup */
	const bool stencil_DATE = (config->destination_alpha == GS_HW_DATE_STENCIL
		|| config->destination_alpha == GS_HW_DATE_STENCIL_ONE);
	gs_texture_d3d12* hdr_rt        = D12_TEX(dev->base.m_colclip_rt);
	gs_texture_d3d12* draw_rt       = D12_TEX(config->rt);
	gs_texture_d3d12* draw_ds       = D12_TEX(config->ds);
	gs_texture_d3d12* draw_rt_clone = NULL;
	gs_texture_d3d12* date_image    = NULL;
	const gs_texture* sized         = config->rt ? config->rt : config->ds;
	const int rtsize_x              = sized->m_size.x;
	const int rtsize_y              = sized->m_size.y;

	/* figure out the pipeline */
	d12_update_hw_pipeline_selector(dev, config);

	/* now blit the colclip texture back to the original target */
	if (hdr_rt)
	{
		if (config->colclip_mode == GS_HW_COLCLIP_EARLY_RESOLVE)
		{
			d12_colclip_resolve(dev, config, hdr_rt, draw_ds, rtsize_x, rtsize_y);
			draw_rt = D12_TEX(config->rt);
			/* (given back: what follows draws as if there had been none) */
			hdr_rt = NULL;
		}
		else
		{
			draw_rt            = hdr_rt;
			pipe->ps.colclip_hw = 1;
		}
	}

	if (stencil_DATE)
	{
		d12_setup_date(dev, (gs_texture*)draw_rt, config->ds, (int)config->datm, &config->drawarea);
		/* The draw after the depth-only stencil pass binds the vertex stream
		 * again: some drivers drop the binding across that pass. */
		dev->dirty_flags |= DIRTY_FLAG_VERTEX_BUFFER;
	}

	/* stream buffer in first, in case we need to exec */
	d12_set_vs_constant_buffer(dev, &config->cb_vs);
	d12_set_ps_constant_buffer(dev, &config->cb_ps);

	/* bind textures before checking the render pass, in case we need to
	 * transition them */
	if (config->tex)
	{
		d12_ps_set_shader_resource(dev, 0, config->tex, config->tex != config->rt);
		d12_ps_set_sampler(dev, config->sampler);
	}
	if (config->pal)
		d12_ps_set_shader_resource(dev, 1, config->pal, true);

	if (config->blend.constant_enable)
		d12_set_blend_constants(dev, config->blend.constant);

	/* Primitive ID tracking DATE setup. */
	if (config->destination_alpha == GS_HW_DATE_PRIMID)
	{
		gs_texture* backup_rt = config->rt;
		config->rt            = (gs_texture*)draw_rt;
		date_image            = d12_setup_primitive_tracking_date(dev, config, pipe);
		config->rt            = backup_rt;
		if (!date_image)
		{
			log_cb(RETRO_LOG_INFO, "Failed to allocate DATE image, aborting draw.\n");
			return;
		}
	}

	if (config->require_one_barrier)
	{
		/* requires a copy of the RT */
		draw_rt_clone = D12_TEX(gs_device_create_texture(d, rtsize_x, rtsize_y, 1,
			hdr_rt ? GS_TEX_COLOR_CLIP : GS_TEX_COLOR, true));
		if (draw_rt_clone)
		{
			d12_end_render_pass(dev);

			draw_rt_clone->base.m_state = GS_TEXSTATE_INVALIDATED;
			d12_dev_copy_rect(d, (gs_texture*)draw_rt, &draw_rt_clone->base, &config->drawarea, (u32)config->drawarea.x,
				(u32)config->drawarea.y);
			d12_ps_set_shader_resource(dev, 2, &draw_rt_clone->base, true);
		}
	}

	/* Switch to colclip target for colclip rendering */
	if (pipe->ps.colclip_hw)
	{
		if (!hdr_rt)
		{
			config->colclip_update_area = config->drawarea;

			d12_end_render_pass(dev);

			hdr_rt = D12_TEX(gs_device_create_render_target(d, rtsize_x, rtsize_y, GS_TEX_COLOR_CLIP, false));
			if (!hdr_rt)
			{
				log_cb(RETRO_LOG_INFO, "Failed to allocate HDR render target, aborting draw.\n");

				if (date_image)
					gs_device_recycle(d, &date_image->base);

				return;
			}

			dev->base.m_colclip_rt = &hdr_rt->base;

			/* propagate clear value through if the colclip render is the first */
			if (draw_rt->base.m_state == GS_TEXSTATE_CLEARED)
				d12_set_clear_color(&hdr_rt->base, draw_rt->base.m_clear_value.color);
			else if (draw_rt->base.m_state == GS_TEXSTATE_DIRTY)
				d12_transition(draw_rt, dev, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			/* we're not drawing to the RT, so we can use it as a source */
			if (config->require_one_barrier)
				d12_ps_set_shader_resource(dev, 2, (gs_texture*)draw_rt, true);
		}

		draw_rt = hdr_rt;
	}

	/* clear texture binding when it's bound to RT or DS */
	if ((draw_rt && D12_HANDLE_EQ(draw_rt->srv_descriptor, dev->tfx_textures[0]))
		|| (config->ds && D12_HANDLE_EQ(D12_TEX(config->ds)->srv_descriptor, dev->tfx_textures[0])))
		d12_ps_set_shader_resource(dev, 0, NULL, false);

	/* avoid restarting the render pass just to switch from rt+depth to rt
	 * and vice versa */
	if (dev->in_render_pass && (dev->current_render_target == draw_rt || dev->current_depth_target == draw_ds))
	{
		/* keep the depth even if doing HDR draws, because the next draw
		 * will probably re-enable depth */
		if (!draw_rt && dev->current_render_target && config->tex != &dev->current_render_target->base
			&& dev->current_render_target->base.m_size.x == draw_ds->base.m_size.x
			&& dev->current_render_target->base.m_size.y == draw_ds->base.m_size.y)
		{
			draw_rt                     = dev->current_render_target;
			dev->pipeline_selector.rt   = 1;
		}
	}
	else if (!draw_ds && dev->current_depth_target && config->tex != &dev->current_depth_target->base
		&& dev->current_depth_target->base.m_size.x == draw_rt->base.m_size.x
		&& dev->current_depth_target->base.m_size.y == draw_rt->base.m_size.y)
	{
		draw_ds                   = dev->current_depth_target;
		dev->pipeline_selector.ds = 1;
	}

	d12_om_set_render_targets(dev, draw_rt, draw_ds, &config->scissor);

	/* Begin render pass if new target or out of the area. */
	if (!dev->in_render_pass)
	{
		float clear_color[4];
		if (draw_rt)
			d12_unorm8(clear_color, draw_rt->base.m_clear_value.color);
		else
			memset(clear_color, 0, sizeof(clear_color));
		if (pipe->ps.colclip_hw)
		{
			/* Denormalize clear color for HDR. */
			clear_color[0] *= 255.0f / 65535.0f;
			clear_color[1] *= 255.0f / 65535.0f;
			clear_color[2] *= 255.0f / 65535.0f;
			clear_color[3] *= 1.0f;
		}
		d12_begin_render_pass(dev, d12_load_op(draw_rt),
			draw_rt ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			d12_load_op(draw_ds),
			draw_ds ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			stencil_DATE ? D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE
						 : D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
			stencil_DATE ? (draw_rt_clone ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE
										  : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD)
						 : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			clear_color, draw_ds ? draw_ds->base.m_clear_value.depth : 0.0f, 1);
	}

	/* rt -> hdr blit if enabled */
	if (hdr_rt
		&& (config->colclip_mode == GS_HW_COLCLIP_CONVERT_ONLY
			|| config->colclip_mode == GS_HW_COLCLIP_CONVERT_AND_RESOLVE)
		&& config->rt->m_state == GS_TEXSTATE_DIRTY)
	{
		gs_recti area;
		gs_rectf drawareaf, sRect;
		d12_set_utility_texture(dev, config->rt, &dev->point_sampler_cpu);
		d12_set_pipeline(dev, d12_utility_pipeline(dev, GS_D3D12_UTIL_HDR_SETUP + pipe->ds));

		if (config->colclip_mode == GS_HW_COLCLIP_CONVERT_ONLY)
		{
			area.x = 0;
			area.y = 0;
			area.z = rtsize_x;
			area.w = rtsize_y;
		}
		else
			area = config->drawarea;
		d12_rectf_of(&drawareaf, &area);
		d12_norm_rect(&sRect, &area, rtsize_x, rtsize_y);
		d12_draw_stretch_rect(dev, &sRect, &drawareaf, rtsize_x, rtsize_y);
	}

	/* VB/IB upload, if we did DATE setup and it's not HDR this has already
	 * been done */
	d12_set_primitive_topology(dev, s_primitive_topology_mapping[config->topology]);
	if (!date_image || hdr_rt)
		d12_upload_hw_draw_vertices_and_indices(dev, config);

	/* now we can do the actual draw */
	if (d12_bind_draw_pipeline(dev, pipe))
		d12_draw_indexed_primitive(dev);

	/* blend second pass */
	if (config->blend_multi_pass.enable)
	{
		if (config->blend_multi_pass.blend.constant_enable)
			d12_set_blend_constants(dev, config->blend_multi_pass.blend.constant);

		pipe->bs          = config->blend_multi_pass.blend;
		pipe->ps.blend_hw = config->blend_multi_pass.blend_hw;
		pipe->ps.dither   = config->blend_multi_pass.dither;
		if (d12_bind_draw_pipeline(dev, pipe))
			d12_draw_indexed_primitive(dev);
	}

	/* and the alpha pass */
	if (config->alpha_second_pass.enable)
	{
		/* cbuffer will definitely be dirty if aref changes, no need to check it */
		if (config->cb_ps.FogColor_AREF.w != config->alpha_second_pass.ps_aref)
		{
			config->cb_ps.FogColor_AREF.w = config->alpha_second_pass.ps_aref;
			d12_set_ps_constant_buffer(dev, &config->cb_ps);
		}

		pipe->ps  = config->alpha_second_pass.ps;
		pipe->cms = config->alpha_second_pass.colormask;
		pipe->dss = config->alpha_second_pass.depth;
		pipe->bs  = config->blend;
		if (d12_bind_draw_pipeline(dev, pipe))
			d12_draw_indexed_primitive(dev);
	}

	if (draw_rt_clone)
		gs_device_recycle(d, &draw_rt_clone->base);

	if (date_image)
		gs_device_recycle(d, &date_image->base);

	/* now blit the hdr texture back to the original target */
	if (hdr_rt)
	{
		/* (GSVector4i::runion) */
		gs_recti* u = &config->colclip_update_area;
		u->x        = u->x < config->drawarea.x ? u->x : config->drawarea.x;
		u->y        = u->y < config->drawarea.y ? u->y : config->drawarea.y;
		u->z        = u->z > config->drawarea.z ? u->z : config->drawarea.z;
		u->w        = u->w > config->drawarea.w ? u->w : config->drawarea.w;

		if (config->colclip_mode == GS_HW_COLCLIP_RESOLVE_ONLY
			|| config->colclip_mode == GS_HW_COLCLIP_CONVERT_AND_RESOLVE)
			d12_colclip_resolve(dev, config, hdr_rt, draw_ds, rtsize_x, rtsize_y);
	}
}

/* ==== the device ==== */

static bool d12_check_features(gs_device_d3d12* dev)
{
	gs_device_features* f = &dev->base.m_features;
	const u32 vendorID    = d12_adapter_vendor_id(dev);
	const bool isAMD      = (vendorID == 0x1002 || vendorID == 0x1022);

	f->texture_barrier       = false;
	f->broken_point_sampler  = isAMD;
	f->primitive_id          = true;
	f->prefer_new_textures   = true;
	f->provoking_vertex_last = false;
	f->point_expand          = false;
	f->line_expand           = false;
	f->framebuffer_fetch     = false;
	f->clip_control          = true;
	f->stencil_buffer        = true;
	f->test_and_sample_depth = false;
	f->vs_expand             = !GSConfig.DisableVertexShaderExpand;

	f->dxt_textures = d12_supports_texture_format(dev, DXGI_FORMAT_BC1_UNORM)
		&& d12_supports_texture_format(dev, DXGI_FORMAT_BC2_UNORM)
		&& d12_supports_texture_format(dev, DXGI_FORMAT_BC3_UNORM);
	f->bptc_textures = d12_supports_texture_format(dev, DXGI_FORMAT_BC7_UNORM);

	return true;
}

static bool d12_create_null_texture(gs_device_d3d12* dev)
{
	dev->null_texture = gs_texture_d3d12_create(GS_TEXTYPE_TEXTURE, GS_TEX_COLOR, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN);
	if (!dev->null_texture)
		return false;

	gs_texture_d3d12_transition(dev->null_texture, (ID3D12GraphicsCommandList*)d12_cmdlist(dev),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	return true;
}

static bool d12_create_buffers(gs_device_d3d12* dev)
{
	if (!gs_d3d12_stream_buffer_create(&dev->vertex_stream_buffer, GS_D3D12_VERTEX_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate vertex buffer\n");
		return false;
	}

	if (!gs_d3d12_stream_buffer_create(&dev->index_stream_buffer, GS_D3D12_INDEX_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate index buffer\n");
		return false;
	}

	if (!gs_d3d12_stream_buffer_create(&dev->vertex_constant_buffer, GS_D3D12_VERTEX_UNIFORM_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate vertex uniform buffer\n");
		return false;
	}

	if (!gs_d3d12_stream_buffer_create(&dev->pixel_constant_buffer, GS_D3D12_FRAGMENT_UNIFORM_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate fragment uniform buffer\n");
		return false;
	}

	if (!d12_allocate_expand_index_buffer(dev, GS_DEVICE_EXPAND_BUFFER_SIZE, &dev->expand_index_buffer,
			&dev->expand_index_buffer_alloc))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate expansion index buffer\n");
		return false;
	}

	return true;
}

static bool d12_create_root_signatures(gs_device_d3d12* dev)
{
	gs_d3d12_rs_builder rsb;
	gs_d3d12_rs_clear(&rsb);

	/* Convert Pipeline Layout */
	gs_d3d12_rs_set_input_assembler_flag(&rsb);
	gs_d3d12_rs_add_32bit_constants(&rsb, 0, GS_D3D12_CONVERT_PUSH_CONSTANTS_SIZE / sizeof(u32),
		D3D12_SHADER_VISIBILITY_ALL);
	gs_d3d12_rs_add_descriptor_table(&rsb, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, GS_D3D12_NUM_UTILITY_SAMPLERS,
		D3D12_SHADER_VISIBILITY_PIXEL);
	gs_d3d12_rs_add_descriptor_table(&rsb, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, GS_D3D12_NUM_UTILITY_SAMPLERS,
		D3D12_SHADER_VISIBILITY_PIXEL);
	if (!(dev->utility_root_signature = d12_rs_create(dev, &rsb)))
		return false;

	/* Draw/TFX Pipeline Layout */
	gs_d3d12_rs_set_input_assembler_flag(&rsb);
	gs_d3d12_rs_add_cbv(&rsb, 0, D3D12_SHADER_VISIBILITY_ALL);
	gs_d3d12_rs_add_cbv(&rsb, 1, D3D12_SHADER_VISIBILITY_PIXEL);
	gs_d3d12_rs_add_srv(&rsb, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	gs_d3d12_rs_add_descriptor_table(&rsb, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 2, D3D12_SHADER_VISIBILITY_PIXEL);
	gs_d3d12_rs_add_descriptor_table(&rsb, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, GS_D3D12_NUM_TFX_SAMPLERS,
		D3D12_SHADER_VISIBILITY_PIXEL);
	gs_d3d12_rs_add_descriptor_table(&rsb, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 2, D3D12_SHADER_VISIBILITY_PIXEL);
	gs_d3d12_rs_add_32bit_constants(&rsb, 2, 4, D3D12_SHADER_VISIBILITY_PIXEL); /* PsKey: a dynamic sibling's */
	if (!(dev->tfx_root_signature = d12_rs_create(dev, &rsb)))
		return false;
	return true;
}

static void d12_destroy_resources(gs_device_d3d12* dev);

static void d12_dev_destroy(gs_device* d)
{
	gs_device_d3d12* dev = D12_DEV(d);

	gs_device_destroy_base(d);
	/* With the pool's textures, and like them ahead of the last submit
	 * below. The frontend holds its own reference to any it still shows. */
	gs_d3d12_free_present_textures();

	if (dev->command_lists[dev->current_command_list].command_lists[1])
	{
		d12_end_render_pass(dev);
		d12_execute_command_list(dev, true);
	}

	d12_destroy_resources(dev);
	d12_unload_library();
}

static bool d12_dev_create(gs_device* d)
{
	gs_device_d3d12* dev = D12_DEV(d);
	size_t first         = 0;
	bool warming;
	u32 slot;
	LUID luid;

	dev->dxgi_factory = gs_d3d_create_factory(GSConfig.UseDebugDevice);
	if (!dev->dxgi_factory)
		return false;

	if (!d12_load_library())
	{
		log_cb(RETRO_LOG_ERROR, "Failed to load D3D12 library\n");
		return false;
	}

	d3d12 = NULL;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void**)&d3d12) || !d3d12)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to get HW rendering interface!\n");
		return false;
	}

	/* Version 1, or version 2 if gs_d3d12_negotiate_hw_interface() asked
	 * for it and the frontend has it. Anything else was not asked for. */
	if (d3d12->interface_version != RETRO_HW_RENDER_INTERFACE_D3D12_VERSION
		&& d3d12->interface_version != RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2)
	{
		log_cb(RETRO_LOG_ERROR, "HW render interface mismatch, expected %u or %u, got %u!\n",
			RETRO_HW_RENDER_INTERFACE_D3D12_VERSION, RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2,
			d3d12->interface_version);
		return false;
	}

	D12_RELEASE(dev->device);
	dev->device = d3d12->device;
	ID3D12Device_AddRef(dev->device);

	/* If something forced the debug layer on before the frontend made this
	 * device, say what it objects to rather than letting it break: left as
	 * a break, the first validation error is an unhandled exception inside
	 * the SDK layers and the process is gone before anything is logged. */
	if (FAILED(ID3D12Device_QueryInterface(dev->device, &s_iid_info_queue, (void**)&dev->info_queue)))
		dev->info_queue = NULL;
	else
	{
		ID3D12InfoQueue_SetBreakOnSeverity(dev->info_queue, D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
		ID3D12InfoQueue_SetBreakOnSeverity(dev->info_queue, D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
		ID3D12InfoQueue_SetBreakOnSeverity(dev->info_queue, D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
		log_cb(RETRO_LOG_INFO, "GS: D3D12 debug layer is on; validation messages go to this log.\n");
	}

	dev->device->lpVtbl->GetAdapterLuid(dev->device, &luid);

	if (FAILED(IDXGIFactory5_EnumAdapterByLuid(dev->dxgi_factory, luid, &s_iid_dxgi_adapter, (void**)&dev->adapter)))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to get lookup adapter by device LUID\n");
		d12_dev_destroy(d);
		return false;
	}

	D12_RELEASE(dev->command_queue);
	dev->command_queue = d3d12->queue;
	ID3D12CommandQueue_AddRef(dev->command_queue);

	if (!d12_create_heap(dev) || !d12_create_fence(dev) || !d12_create_descriptor_heaps(dev)
		|| !d12_create_command_lists(dev))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create D3D12 context\n");
		d12_dev_destroy(d);
		return false;
	}

	if (!gs_d3d12_stream_buffer_create(&dev->texture_stream_buffer, GS_D3D12_TEXTURE_UPLOAD_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create D3D12 context\n");
		d12_dev_destroy(d);
		return false;
	}

	if (!d12_check_features(dev))
	{
		log_cb(RETRO_LOG_ERROR, "Your GPU does not support the required D3D12 features.\n");
		return false;
	}

	gs_device_acquire_window(d);

	if (!gs_d3d12_shader_cache_open(&dev->shader_cache, dev->feature_level, GSConfig.UseDebugDevice,
			GSConfig.DisableShaderCache ? NULL : EmuFolders.Cache))
		log_cb(RETRO_LOG_WARN, "Shader cache failed to open.\n");

	if (!d12_create_null_texture(dev))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create dummy texture\n");
		return false;
	}

	if (!d12_create_root_signatures(dev))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create pipeline layouts\n");
		return false;
	}

	if (!d12_build_tfx_shaders_ahead(dev))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to compile the draw vertex shaders\n");
		return false;
	}

	/* The pixel shaders' and pipelines' maps, so that none grows as a game
	 * draws */
	gs_flat_map_reserve(&dev->tfx_pixel_shaders, D12_PS_RESERVE);
	gs_flat_map_reserve(&dev->tfx_pipelines, D12_PIPELINES_RESERVE);

	/* The utility pipelines, then the listed siblings and draw pipelines,
	 * from here on a thread of their own, which also builds what a draw
	 * queues; the first frame waits for the utility pipelines and the
	 * listed siblings. Without the thread, or after one of them could not
	 * be built on the last device, the utility pipelines here, so a device
	 * that cannot have them is not made. */
	warming           = d12_start_tfx_warm(dev, &first);
	dev->test_dynamic = warming && dev->tfx_ps_dynamic[0] && getenv("LRPS2_TEST_D3D12_DYNAMIC");

	if (!d12_create_buffers(dev))
		return false;

	if (warming)
	{
		gs_warm_wait(dev->tfx_warm, first);
		d12_collect_warmed_tfx_pipelines(dev, NULL, NULL);
	}
	for (slot = 0; slot < GS_D3D12_UTIL_COUNT; slot++)
	{
		if (dev->utility[slot] && !s_utility_build_failed)
			continue;
		D12_RELEASE(dev->utility[slot]);
		if (!(dev->utility[slot] = d12_create_utility_pipeline(dev, slot)))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to compile utility pipelines\n");
			return false;
		}
	}
	s_utility_build_failed = false;

	d12_initialize_state(dev);
	d12_initialize_samplers(dev);
	return true;
}

static void d12_release_map(gs_flat_map_t* map)
{
	size_t i;
	for (i = 0; i < GS_FLAT_MAP_SLOTS(map); i++)
		if (GS_FLAT_MAP_USED(map, i))
		{
			IUnknown** v = (IUnknown**)GS_FLAT_MAP_VALUE(map, i);
			if (*v)
				IUnknown_Release(*v);
		}
	gs_flat_map_clear(map);
}

static void d12_destroy_resources(gs_device_d3d12* dev)
{
	size_t i;

	/* First: the warm-up thread reads the shaders built ahead. */
	d12_stop_tfx_warm(dev);
	if (dev->tfx_built_ahead || dev->tfx_pipelines_here)
		log_cb(RETRO_LOG_INFO, "D3D12 pipelines: %u shaders compiled on the render thread, %u pipelines built there (%u of them dynamic siblings), %u shaders built as the device was created, %u pipelines queued for the warm-up thread; %u draws drawn with a sibling\n",
			dev->tfx_shaders_here, dev->tfx_pipelines_here, dev->tfx_siblings_here, dev->tfx_built_ahead,
			dev->tfx_queued, dev->tfx_sibling_draws);
	dev->tfx_shaders_here = dev->tfx_pipelines_here = dev->tfx_siblings_here = 0;
	dev->tfx_built_ahead = dev->tfx_queued = dev->tfx_sibling_draws = 0;

	/* (the GPU is done with them: the device waited for its last list) */
	d12_release_map(&dev->tfx_pipelines);
	dev->last_tfx_pipeline = NULL;
	d12_release_map(&dev->tfx_pixel_shaders);
	for (i = 0; i < D12_ARRAY_SIZE(dev->tfx_vertex_shaders); i++)
		D12_RELEASE(dev->tfx_vertex_shaders[i]);
	for (i = 0; i < D12_ARRAY_SIZE(dev->tfx_vs); i++)
		D12_RELEASE(dev->tfx_vs[i]);
	for (i = 0; i < D3D_DYNAMIC_PS_COUNT; i++)
		D12_RELEASE(dev->tfx_ps_dynamic[i]);
	for (i = 0; i < GS_D3D12_UTIL_COUNT; i++)
		D12_RELEASE(dev->utility[i]);
	dev->utility_failed = false;

	gs_d3d12_descriptor_handle_clear(&dev->linear_sampler_cpu);
	gs_d3d12_descriptor_handle_clear(&dev->point_sampler_cpu);
	for (i = 0; i < D12_ARRAY_SIZE(dev->samplers); i++)
		d12_defer_descriptor(dev, &dev->sampler_heap_manager, &dev->samplers[i]);
	d12_invalidate_sampler_groups(dev);

	D12_RELEASE(dev->expand_index_buffer);
	gs_d3d12_heap_free(&dev->heap, &dev->expand_index_buffer_alloc);
	memset(&dev->expand_index_buffer_alloc, 0, sizeof(dev->expand_index_buffer_alloc));
	gs_d3d12_stream_buffer_destroy(&dev->texture_stream_buffer, 0);
	gs_d3d12_stream_buffer_destroy(&dev->pixel_constant_buffer, 0);
	gs_d3d12_stream_buffer_destroy(&dev->vertex_constant_buffer, 0);
	gs_d3d12_stream_buffer_destroy(&dev->index_stream_buffer, 0);
	gs_d3d12_stream_buffer_destroy(&dev->vertex_stream_buffer, 0);

	D12_RELEASE(dev->utility_root_signature);
	D12_RELEASE(dev->tfx_root_signature);

	if (dev->null_texture)
	{
		gs_texture_d3d12_destroy(dev->null_texture, 0);
		dev->null_texture->base.m_ops->free(&dev->null_texture->base);
		dev->null_texture = NULL;
	}

	gs_d3d12_shader_cache_close(&dev->shader_cache);

	/* What the lists still hold: nothing of it is in use any more. */
	for (i = 0; i < GS_D3D12_NUM_COMMAND_LISTS; i++)
		d12_destroy_pending_resources(dev, &dev->command_lists[i]);

	gs_d3d12_descriptor_heap_manager_free(&dev->descriptor_heap_manager, &dev->null_srv_descriptor);
	gs_d3d12_descriptor_heap_manager_destroy(&dev->sampler_heap_manager);
	gs_d3d12_descriptor_heap_manager_destroy(&dev->dsv_heap_manager);
	gs_d3d12_descriptor_heap_manager_destroy(&dev->rtv_heap_manager);
	gs_d3d12_descriptor_heap_manager_destroy(&dev->descriptor_heap_manager);
	for (i = 0; i < GS_D3D12_NUM_COMMAND_LISTS; i++)
	{
		gs_d3d12_command_list* res = &dev->command_lists[i];
		u32 j;
		for (j = 0; j < 2; j++)
		{
			D12_RELEASE(res->command_allocators[j]);
			D12_RELEASE(res->command_lists[j]);
		}
		gs_d3d12_descriptor_allocator_destroy(&res->descriptor_allocator);
		gs_d3d12_sampler_allocator_destroy(&res->sampler_allocator);
		free(res->pending_resources);
		free(res->pending_descriptors);
		memset(res, 0, sizeof(*res));
		gs_d3d12_descriptor_allocator_init(&res->descriptor_allocator);
		gs_d3d12_sampler_allocator_init(&res->sampler_allocator, GS_D3D12_SAMPLER_GROUP_SIZE);
	}
	dev->current_command_list  = 0;
	dev->completed_fence_value = 0;
	dev->current_fence_value   = 0;
	if (dev->fence_event)
	{
		CloseHandle(dev->fence_event);
		dev->fence_event = NULL;
	}
	D12_RELEASE(dev->fence);

	gs_d3d12_heap_shutdown(&dev->heap);
	D12_RELEASE(dev->command_queue);
	D12_RELEASE(dev->info_queue);
	D12_RELEASE(dev->adapter);
	D12_RELEASE(dev->dxgi_factory);
	D12_RELEASE(dev->device);
}

static void d12_dev_free(gs_device* d)
{
	gs_device_d3d12* dev = D12_DEV(d);
	u32 i;
	gs_flat_map_free(&dev->tfx_pixel_shaders);
	gs_flat_map_free(&dev->tfx_pipelines);
	for (i = 0; i < GS_D3D12_NUM_COMMAND_LISTS; i++)
	{
		free(dev->command_lists[i].pending_resources);
		free(dev->command_lists[i].pending_descriptors);
	}
	gs_d3d12_shader_cache_close(&dev->shader_cache);
	gs_device_deinit(d);
	memalign_free(dev);
}

static void d12_dev_reset_api_state(gs_device* d)
{
	d12_end_render_pass(D12_DEV(d));
}

static void d12_dev_restore_api_state(gs_device* d)
{
	d12_invalidate_cached_state(D12_DEV(d));
}

static int d12_dev_get_render_api(const gs_device* d)
{
	(void)d;
	return GS_RENDER_API_D3D12;
}

static int d12_dev_begin_present(gs_device* d, bool frame_skip)
{
	gs_device_d3d12* dev = D12_DEV(d);
	(void)frame_skip;
	d12_end_render_pass(dev);

	if (dev->device_lost)
		return GS_PRESENT_DEVICE_LOST;
	return GS_PRESENT_OK;
}

static void d12_dev_end_present(gs_device* d)
{
	d12_invalidate_cached_state(D12_DEV(d));
}

static gs_texture* d12_dev_create_surface(gs_device* d, unsigned type, int width, int height, int levels,
	unsigned format)
{
	gs_device_d3d12* dev  = D12_DEV(d);
	const int max         = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
	const u32 clamped_w   = (u32)(width < 1 ? 1 : width > max ? max : width);
	const u32 clamped_h   = (u32)(height < 1 ? 1 : height > max ? max : height);
	DXGI_FORMAT dxgi_format, srv_format, rtv_format, dsv_format, uav_format;
	gs_texture_d3d12* tex;

	d12_lookup_native_format(format, &dxgi_format, &srv_format, &rtv_format, &dsv_format);
	uav_format = (type == GS_TEXTYPE_RW_TEXTURE) ? dxgi_format : DXGI_FORMAT_UNKNOWN;

	tex = gs_texture_d3d12_create(type, format, (int)clamped_w, (int)clamped_h, levels, dxgi_format, srv_format,
		rtv_format, dsv_format, uav_format);
	if (!tex)
	{
		/* We're probably out of vram, try flushing the command buffer to
		 * release pending textures. */
		gs_device_purge_pool(d);
		d12_execute_command_list_and_restart_render_pass(dev, true);
		tex = gs_texture_d3d12_create(type, format, (int)clamped_w, (int)clamped_h, levels, dxgi_format, srv_format,
			rtv_format, dsv_format, uav_format);
	}

	return tex ? &tex->base : NULL;
}

static void* d12_dev_create_download_texture(gs_device* d, u32 width, u32 height, unsigned format)
{
	(void)d;
	return gs_download_texture_d3d12_create(width, height, format);
}

static const struct gs_device_ops s_d3d12_device_ops = {
	d12_dev_free,
	d12_dev_create,
	d12_dev_destroy,
	d12_dev_reset_api_state,
	d12_dev_restore_api_state,
	d12_dev_get_render_api,
	d12_dev_begin_present,
	d12_dev_end_present,
	d12_dev_create_surface,
	d12_dev_create_download_texture,
	d12_dev_do_merge,
	d12_dev_do_interlace,
	d12_dev_copy_rect,
	d12_dev_stretch_rect,
	d12_dev_stretch_rect_mask,
	d12_dev_present_rect,
	d12_dev_draw_multi_stretch_rects,
	d12_dev_update_clut_texture,
	d12_dev_convert_to_indexed_texture,
	d12_dev_filtered_downsample_texture,
	d12_dev_move_texels,
	d12_dev_render_hw,
	d12_dev_clear_sampler_cache
};

/* (gs_device_open) */
gs_device* gs_device_new_d3d12(void)
{
	gs_device_d3d12* dev = (gs_device_d3d12*)memalign_alloc(32, sizeof(gs_device_d3d12));
	u32 i;
	if (!dev)
		return NULL;
	memset(dev, 0, sizeof(*dev));
	gs_device_init(&dev->base);
	dev->base.m_ops = &s_d3d12_device_ops;

	dev->feature_level           = D3D_FEATURE_LEVEL_11_0;
	dev->current_command_list    = GS_D3D12_NUM_COMMAND_LISTS - 1;
	dev->viewport.Width          = 1.0f;
	dev->viewport.Height         = 1.0f;
	dev->viewport.MaxDepth       = 1.0f;

	for (i = 0; i < GS_D3D12_NUM_COMMAND_LISTS; i++)
	{
		gs_d3d12_descriptor_allocator_init(&dev->command_lists[i].descriptor_allocator);
		gs_d3d12_sampler_allocator_init(&dev->command_lists[i].sampler_allocator, GS_D3D12_SAMPLER_GROUP_SIZE);
	}
	gs_d3d12_descriptor_heap_manager_init(&dev->descriptor_heap_manager);
	gs_d3d12_descriptor_heap_manager_init(&dev->rtv_heap_manager);
	gs_d3d12_descriptor_heap_manager_init(&dev->dsv_heap_manager);
	gs_d3d12_descriptor_heap_manager_init(&dev->sampler_heap_manager);
	gs_d3d12_descriptor_handle_clear(&dev->null_srv_descriptor);
	gs_d3d12_stream_buffer_init(&dev->texture_stream_buffer);
	gs_d3d12_stream_buffer_init(&dev->vertex_stream_buffer);
	gs_d3d12_stream_buffer_init(&dev->index_stream_buffer);
	gs_d3d12_stream_buffer_init(&dev->vertex_constant_buffer);
	gs_d3d12_stream_buffer_init(&dev->pixel_constant_buffer);
	gs_d3d12_descriptor_handle_clear(&dev->point_sampler_cpu);
	gs_d3d12_descriptor_handle_clear(&dev->linear_sampler_cpu);
	for (i = 0; i < D12_ARRAY_SIZE(dev->samplers); i++)
		gs_d3d12_descriptor_handle_clear(&dev->samplers[i]);
	for (i = 0; i < GS_D3D12_NUM_TOTAL_TFX_TEXTURES; i++)
		gs_d3d12_descriptor_handle_clear(&dev->tfx_textures[i]);
	gs_d3d12_descriptor_handle_clear(&dev->tfx_sampler);
	gs_d3d12_descriptor_handle_clear(&dev->tfx_textures_handle_gpu);
	gs_d3d12_descriptor_handle_clear(&dev->tfx_samplers_handle_gpu);
	gs_d3d12_descriptor_handle_clear(&dev->tfx_rt_textures_handle_gpu);
	gs_d3d12_descriptor_handle_clear(&dev->utility_texture_cpu);
	gs_d3d12_descriptor_handle_clear(&dev->utility_texture_gpu);
	gs_d3d12_descriptor_handle_clear(&dev->utility_sampler_cpu);
	gs_d3d12_descriptor_handle_clear(&dev->utility_sampler_gpu);
	gs_warm_list_init(&dev->tfx_list, sizeof(gs_d3d12_pipeline_selector));
	gs_flat_map_init(&dev->tfx_pixel_shaders, sizeof(gs_hw_ps_selector), sizeof(ID3DBlob*));
	gs_flat_map_init(&dev->tfx_pipelines, sizeof(gs_d3d12_pipeline_selector), sizeof(ID3D12PipelineState*));
	gs_d3d12_shader_cache_init(&dev->shader_cache);
	return &dev->base;
}
