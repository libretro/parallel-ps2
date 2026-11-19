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

#ifndef GS_DEVICE12_H
#define GS_DEVICE12_H

/* The Direct3D 12 device (gs_device_new_d3d12): the frontend's device and
 * queue, through the table in GSDevice12.c */

#include <boolean.h>

#include "common/Pcsx2Defs.h"
#include "GS/Renderers/Common/GSDeviceBase.h"
#include "GS/Renderers/Common/GSFlatMap.h"
#include "GS/Renderers/Common/GSHWDrawConfig.h"
#include "GS/Renderers/DX11/D3D.h"
#include "GS/Renderers/DX11/D3DDynamicPS.h"
#include "D3D12DescriptorHeapManager.h"
#include "D3D12ShaderCache.h"
#include "D3D12StreamBuffer.h"
#include "GSD3D12Heap.h"
#include "GSD3D12Warm.h"
#include "GSTexture12.h"

#include <d3d12.h>
#include <dxgi1_5.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Command lists: one is recorded while the others run */
#define GS_D3D12_NUM_COMMAND_LISTS 3
/* Textures that don't fit into this buffer are uploaded with a staging buffer */
#define GS_D3D12_TEXTURE_UPLOAD_BUFFER_SIZE (64 * 1024 * 1024)
#define GS_D3D12_SAMPLER_GROUP_SIZE 2

#define GS_D3D12_NUM_TFX_CONSTANT_BUFFERS 2
#define GS_D3D12_NUM_TFX_TEXTURES 2
#define GS_D3D12_NUM_TFX_RT_TEXTURES 2
#define GS_D3D12_NUM_TOTAL_TFX_TEXTURES (GS_D3D12_NUM_TFX_TEXTURES + GS_D3D12_NUM_TFX_RT_TEXTURES)
#define GS_D3D12_NUM_TFX_SAMPLERS 1
#define GS_D3D12_NUM_UTILITY_SAMPLERS 1
#define GS_D3D12_CONVERT_PUSH_CONSTANTS_SIZE 96

#define GS_D3D12_VERTEX_BUFFER_SIZE (32 * 1024 * 1024)
#define GS_D3D12_INDEX_BUFFER_SIZE (16 * 1024 * 1024)
#define GS_D3D12_VERTEX_UNIFORM_BUFFER_SIZE (8 * 1024 * 1024)
#define GS_D3D12_FRAGMENT_UNIFORM_BUFFER_SIZE (8 * 1024 * 1024)

#define GS_D3D12_TFX_PARAM_VS_CBV 0
#define GS_D3D12_TFX_PARAM_PS_CBV 1
#define GS_D3D12_TFX_PARAM_VS_SRV 2
#define GS_D3D12_TFX_PARAM_PS_TEXTURES 3
#define GS_D3D12_TFX_PARAM_PS_SAMPLERS 4
#define GS_D3D12_TFX_PARAM_PS_RT_TEXTURES 5
#define GS_D3D12_TFX_PARAM_PS_KEY 6

#define GS_D3D12_UTILITY_PARAM_PUSH_CONSTANTS 0
#define GS_D3D12_UTILITY_PARAM_PS_TEXTURES 1
#define GS_D3D12_UTILITY_PARAM_PS_SAMPLERS 2

/* The utility pipelines, one table: built on the warm-up thread from the
 * device's creation on, each taken at its first use, or built then when
 * the thread has not got to it. */
#define GS_D3D12_UTIL_CONVERT 0 /* [shader convert] */
#define GS_D3D12_UTIL_COLOR_COPY (GS_D3D12_UTIL_CONVERT + GS_SHADER_CONVERT_COUNT) /* [write mask | 16 for RTA] */
#define GS_D3D12_UTIL_HDR_SETUP (GS_D3D12_UTIL_COLOR_COPY + 32)                    /* [depth] */
#define GS_D3D12_UTIL_HDR_FINISH (GS_D3D12_UTIL_HDR_SETUP + 2)                     /* [depth] */
#define GS_D3D12_UTIL_DATE_SETUP (GS_D3D12_UTIL_HDR_FINISH + 2)                    /* [depth * 4 + datm] */
#define GS_D3D12_UTIL_INTERLACE (GS_D3D12_UTIL_DATE_SETUP + 8)                     /* [shader] */
#define GS_D3D12_UTIL_MERGE (GS_D3D12_UTIL_INTERLACE + GS_SHADER_INTERLACE_COUNT)  /* [MMOD] */
#define GS_D3D12_UTIL_COUNT (GS_D3D12_UTIL_MERGE + 2)

/* A draw pipeline: compared and hashed as its bytes, kept in the
 * pipeline list as them (zeroed before it is filled, padding included) */
typedef struct PCSX2_ALIGN(8) gs_d3d12_pipeline_selector
{
	gs_hw_ps_selector ps;
	union
	{
		struct
		{
			u32 topology : 2;
			u32 rt : 1;
			u32 ds : 1;
			/* a dynamic sibling: ps holds only what the pipeline's state
			 * and the shader's interface take from it */
			u32 dynamic : 1;
		};
		u32 key;
	};
	gs_hw_blend_state bs;
	gs_hw_vs_selector vs;
	gs_hw_depth_selector dss;
	gs_hw_color_mask cms;
	u8 pad;
} gs_d3d12_pipeline_selector;

/* A resource a command list holds until it has run, with the span of the
 * heap it is in (a zeroed alloc: it came from somewhere else) */
typedef struct gs_d3d12_pending_resource
{
	gs_d3d12_alloc_t alloc;
	ID3D12DeviceChild* resource;
} gs_d3d12_pending_resource;

/* A descriptor a command list holds until it has run */
typedef struct gs_d3d12_pending_descriptor
{
	gs_d3d12_descriptor_heap_manager* manager;
	u32 index;
} gs_d3d12_pending_descriptor;

typedef struct gs_d3d12_command_list
{
	ID3D12CommandAllocator* command_allocators[2];
	ID3D12GraphicsCommandList4* command_lists[2]; /* [0] init, [1] draws */
	gs_d3d12_descriptor_allocator descriptor_allocator;
	gs_d3d12_sampler_allocator sampler_allocator;
	/* reserved as the device is made; grown only past that */
	gs_d3d12_pending_resource* pending_resources;
	size_t num_pending_resources;
	size_t cap_pending_resources;
	gs_d3d12_pending_descriptor* pending_descriptors;
	size_t num_pending_descriptors;
	size_t cap_pending_descriptors;
	u64 ready_fence_value;
	bool init_command_list_used;
} gs_d3d12_command_list;

/* A result of the warm-up thread: the pipeline, and what it built that the
 * cache had not (null when it had), for the cache */
typedef struct gs_d3d12_warm_result
{
	gs_d3d12_cache_key vs_key;
	gs_d3d12_cache_key ps_key;
	gs_d3d12_cache_key pso_key;
	ID3D12PipelineState* pso;
	ID3DBlob* vs_blob;
	ID3DBlob* ps_blob;
	ID3DBlob* pso_blob;
	gs_d3d12_pipeline_selector p;
	u32 utility; /* a GS_D3D12_UTIL_ slot, or GS_D3D12_UTIL_COUNT: the draw pipeline p */
	u32 pad;
} gs_d3d12_warm_result;

/* a job of the warm-up thread: the same two */
typedef struct gs_d3d12_warm_job
{
	gs_d3d12_pipeline_selector p;
	u32 utility;
	u32 pad;
} gs_d3d12_warm_job;

#define GS_D3D12_ROOT_SIGNATURE_UNDEFINED 0
#define GS_D3D12_ROOT_SIGNATURE_TFX 1
#define GS_D3D12_ROOT_SIGNATURE_UTILITY 2

typedef struct gs_device_d3d12
{
	gs_device base;

	IDXGIFactory5* dxgi_factory;
	IDXGIAdapter* adapter;
	/* Non-null only when the debug layer was forced on before the
	 * frontend made the device - the core cannot enable it, the device is
	 * not ours. When it is on, this is what turns a validation error into
	 * a line in the libretro log instead of a breakpoint that takes the
	 * process with it. */
	ID3D12InfoQueue* info_queue;
	ID3D12Device* device;
	ID3D12CommandQueue* command_queue;
	gs_d3d12_heap_t heap;
	/* Tier 1 wants buffers, render targets and other textures in heaps of
	 * their own; tier 2 takes them all in one. */
	bool heaps_hold_anything;
	bool device_lost;
	bool utility_failed;
	bool in_render_pass;
	/* LRPS2_TEST_D3D12_DYNAMIC: every draw with its dynamic sibling */
	bool test_dynamic;

	ID3D12Fence* fence;
	HANDLE fence_event;
	u32 current_fence_value;
	u64 completed_fence_value;

	gs_d3d12_command_list command_lists[GS_D3D12_NUM_COMMAND_LISTS];
	u32 current_command_list;

	gs_d3d12_descriptor_heap_manager descriptor_heap_manager;
	gs_d3d12_descriptor_heap_manager rtv_heap_manager;
	gs_d3d12_descriptor_heap_manager dsv_heap_manager;
	gs_d3d12_descriptor_heap_manager sampler_heap_manager;
	gs_d3d12_descriptor_handle null_srv_descriptor;
	gs_d3d12_stream_buffer texture_stream_buffer;

	D3D_FEATURE_LEVEL feature_level;

	ID3D12RootSignature* tfx_root_signature;
	ID3D12RootSignature* utility_root_signature;

	gs_d3d12_stream_buffer vertex_stream_buffer;
	gs_d3d12_stream_buffer index_stream_buffer;
	gs_d3d12_stream_buffer vertex_constant_buffer;
	gs_d3d12_stream_buffer pixel_constant_buffer;
	ID3D12Resource* expand_index_buffer;
	gs_d3d12_alloc_t expand_index_buffer_alloc;

	gs_d3d12_descriptor_handle point_sampler_cpu;
	gs_d3d12_descriptor_handle linear_sampler_cpu;
	/* by sampler selector; an index of GS_D3D12_DESCRIPTOR_INVALID: none */
	gs_d3d12_descriptor_handle samplers[256];

	ID3D12PipelineState* utility[GS_D3D12_UTIL_COUNT];

	/* Every vertex shader a draw can have (by its selector's key) and the
	 * dynamic pixel shaders, built as the device is created and not
	 * changed after: the warm-up thread reads them. */
	ID3DBlob* tfx_vs[64];
	ID3DBlob* tfx_ps_dynamic[D3D_DYNAMIC_PS_COUNT];
	/* the drawing thread's: shaders only a device without a warm-up
	 * thread builds as it draws */
	ID3DBlob* tfx_vertex_shaders[256];
	gs_flat_map_t tfx_pixel_shaders; /* gs_hw_ps_selector -> ID3DBlob* */
	gs_flat_map_t tfx_pipelines;     /* gs_d3d12_pipeline_selector -> ID3D12PipelineState* */
	/* The draws of one frame come in runs that share a pipeline, and the
	 * hash of a selector costs more than the comparison: the last one, and
	 * the map left alone while the selector has not changed. */
	gs_d3d12_pipeline_selector last_tfx_pipeline_selector;
	const ID3D12PipelineState* last_tfx_pipeline;

	/* Counted for the log at teardown: shaders compiled and pipelines
	 * built on the drawing thread (siblings among them), built as the
	 * device was created, queued for the warm-up thread, and draws drawn
	 * with a sibling. */
	u32 tfx_shaders_here;
	u32 tfx_pipelines_here;
	u32 tfx_siblings_here;
	u32 tfx_built_ahead;
	u32 tfx_queued;
	u32 tfx_sibling_draws;

	/* The draw pipelines a session made, kept beside the shader cache, and
	 * built again at the next start on a thread of their own (see
	 * GSD3D12Warm.h) */
	gs_warm_list_t tfx_list;
	size_t tfx_list_loaded;
	gs_warm_t* tfx_warm;
	gs_d3d12_cache_snapshot tfx_warm_cache;
	/* the blobs the warm-up thread built, added to the cache at teardown */
	gs_d3d12_warm_result* tfx_warm_blobs;
	size_t tfx_warm_blob_count;
	size_t tfx_warm_blob_cap;

	gs_hw_vs_cb vs_cb_cache;
	gs_hw_ps_cb ps_cb_cache;

	gs_d3d12_shader_cache shader_cache;

	/* Which bindings and state are to be set before the next draw */
	u32 dirty_flags;

	/* input assembly */
	D3D12_VERTEX_BUFFER_VIEW vertex_buffer;
	D3D12_INDEX_BUFFER_VIEW index_buffer;
	D3D12_PRIMITIVE_TOPOLOGY primitive_topology;

	gs_texture_d3d12* current_render_target;
	gs_texture_d3d12* current_depth_target;

	D3D12_VIEWPORT viewport;
	gs_recti scissor;
	u8 blend_constant_color;
	u8 stencil_ref;

	D3D12_GPU_VIRTUAL_ADDRESS tfx_constant_buffers[GS_D3D12_NUM_TFX_CONSTANT_BUFFERS];
	gs_d3d12_descriptor_handle tfx_textures[GS_D3D12_NUM_TOTAL_TFX_TEXTURES];
	gs_d3d12_descriptor_handle tfx_sampler;
	u32 tfx_sampler_sel;
	gs_d3d12_descriptor_handle tfx_textures_handle_gpu;
	gs_d3d12_descriptor_handle tfx_samplers_handle_gpu;
	gs_d3d12_descriptor_handle tfx_rt_textures_handle_gpu;

	gs_d3d12_descriptor_handle utility_texture_cpu;
	gs_d3d12_descriptor_handle utility_texture_gpu;
	gs_d3d12_descriptor_handle utility_sampler_cpu;
	gs_d3d12_descriptor_handle utility_sampler_gpu;

	int current_root_signature; /* GS_D3D12_ROOT_SIGNATURE_ */
	const ID3D12PipelineState* current_pipeline;

	gs_texture_d3d12* null_texture;

	/* the draw's pipeline selector, kept so that it is not zeroed each draw */
	gs_d3d12_pipeline_selector pipeline_selector;

	/* the shader macros: the drawing thread's, and the warm-up thread's */
	gs_d3d_macros sm;
	gs_d3d_macros warm_sm;
} gs_device_d3d12;

#ifdef __cplusplus
}
#endif

#endif
