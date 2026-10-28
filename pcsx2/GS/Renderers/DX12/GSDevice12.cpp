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

#include "GS/GS.h"
#include "../../../FormatString.h"
#include "common/Pcsx2Defs.h"
#include <cfloat>
#include "GS/GSUtil.h"
#include "GS/Renderers/DX11/D3D.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include "Host.h"
#include "ShaderCacheVersion.h"

#include "common/General.h"
#include "common/Align.h"
#include "D3D12Builders.h"
#include "D3D12ShaderCache.h"


#include <limits>
#include <algorithm>
#include <array>
#include <dxgi1_4.h>
#include <queue>
#include <vector>

#include <libretro_d3d.h>
#include <file/file_path.h>

extern "C" {

extern char tfx_fx_shader_raw[];
extern char merge_fx_shader_raw[];
extern char convert_fx_shader_raw[];
extern char interlace_fx_shader_raw[];

}

retro_hw_render_interface_d3d12 *d3d12;
extern retro_environment_t environ_cb;
extern retro_video_refresh_t video_cb;

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
 * submitted and the frontend records nothing here.
 *
 * Plain C on purpose: a struct that lives for the process, a function
 * with C linkage main.cpp calls before SET_HW_RENDER, and an array. */

#define GS_D3D12_MAX_SYNC_INDICES 32

static struct retro_hw_render_context_negotiation_interface_d3d12 s_d3d12_negotiation;
static GSTexture12* s_d3d12_present_textures[GS_D3D12_MAX_SYNC_INDICES];

extern "C" void gs_d3d12_negotiate_hw_interface(retro_environment_t cb)
{
	struct retro_hw_render_context_negotiation_interface probe;

	if (!cb)
		return;

	/* A frontend that does not know the type answers version 0, or does
	 * not answer; then nothing has been asked for and version 1 is what
	 * GET_HW_RENDER_INTERFACE returns, as it always has. */
	probe.interface_type    = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D12;
	probe.interface_version = 0;
	if (   !cb(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT, &probe)
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
		delete s_d3d12_present_textures[i];
		s_d3d12_present_textures[i] = NULL;
	}
}

using namespace D3D12;

// Private D3D12 state
static HMODULE s_d3d12_library;
static PFN_D3D12_CREATE_DEVICE s_d3d12_create_device;
static PFN_D3D12_GET_DEBUG_INTERFACE s_d3d12_get_debug_interface;
static PFN_D3D12_SERIALIZE_ROOT_SIGNATURE s_d3d12_serialize_root_signature;

static bool LoadD3D12Library()
{
	if ((s_d3d12_library = LoadLibraryW(L"d3d12.dll")) == nullptr ||
		(s_d3d12_create_device =
				reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(s_d3d12_library, "D3D12CreateDevice"))) == nullptr ||
		(s_d3d12_get_debug_interface = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(
			 GetProcAddress(s_d3d12_library, "D3D12GetDebugInterface"))) == nullptr ||
		(s_d3d12_serialize_root_signature = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
			 GetProcAddress(s_d3d12_library, "D3D12SerializeRootSignature"))) == nullptr)
	{
		log_cb(RETRO_LOG_ERROR, "d3d12.dll could not be loaded.\n");
		s_d3d12_create_device = nullptr;
		s_d3d12_get_debug_interface = nullptr;
		s_d3d12_serialize_root_signature = nullptr;
		if (s_d3d12_library)
			FreeLibrary(s_d3d12_library);
		s_d3d12_library = nullptr;
		return false;
	}

	return true;
}

static void UnloadD3D12Library()
{
	s_d3d12_serialize_root_signature = nullptr;
	s_d3d12_get_debug_interface = nullptr;
	s_d3d12_create_device = nullptr;
	if (s_d3d12_library)
	{
		FreeLibrary(s_d3d12_library);
		s_d3d12_library = nullptr;
	}
}

GSDevice12::ComPtr<ID3DBlob> GSDevice12::SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc)
{
	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> error_blob;
	const HRESULT hr = s_d3d12_serialize_root_signature(desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(),
		error_blob.put());
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "D3D12SerializeRootSignature() failed: %08X\n", hr);
		if (error_blob)
			log_cb(RETRO_LOG_ERROR, "%s\n", error_blob->GetBufferPointer());

		return {};
	}

	return blob;
}

GSDevice12::ComPtr<ID3D12RootSignature> GSDevice12::CreateRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc)
{
	ComPtr<ID3DBlob> blob = SerializeRootSignature(desc);
	if (!blob)
		return {};

	ComPtr<ID3D12RootSignature> rs;
	const HRESULT hr =
		m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(rs.put()));
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "CreateRootSignature() failed: %08X\n", hr);
		return {};
	}

	return rs;
}

bool GSDevice12::SupportsTextureFormat(DXGI_FORMAT format)
{
	constexpr u32 required = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;

	D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {format};
	return SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
		   (support.Support1 & required) == required;
}

u32 GSDevice12::GetAdapterVendorID() const
{
	if (!m_adapter)
		return 0;

	DXGI_ADAPTER_DESC desc;
	if (FAILED(m_adapter->GetDesc(&desc)))
		return 0;

	return desc.VendorId;
}

/* The device calls the heap needs, so that GSD3D12Heap.c needs no
 * d3d12.h and can be built and tested without a GPU. */
static int gs_d3d12_create_heap_cb(void *device, uint64_t size, uint32_t heap_type,
	uint32_t heap_flags, void **out_heap)
{
	ID3D12Device*   dev = static_cast<ID3D12Device*>(device);
	ID3D12Heap*     heap = nullptr;
	D3D12_HEAP_DESC hd = {};

	hd.SizeInBytes           = size;
	hd.Properties.Type       = static_cast<D3D12_HEAP_TYPE>(heap_type);
	hd.Alignment             = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	hd.Flags                 = static_cast<D3D12_HEAP_FLAGS>(heap_flags);

	if (FAILED(dev->CreateHeap(&hd, IID_PPV_ARGS(&heap))) || !heap)
		return 0;

	*out_heap = heap;
	return 1;
}

static void gs_d3d12_release_heap_cb(void *device, void *heap)
{
	(void)device;
	if (heap)
		static_cast<ID3D12Heap*>(heap)->Release();
}

bool GSDevice12::CreateHeap()
{
	D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
	gs_d3d12_heap_fns_t fns;
	const float scale = GSConfig.UpscaleMultiplier > 0.0f ? GSConfig.UpscaleMultiplier : 1.0f;
	u64 block;
	u64 ceiling;

	/* Tier 1 keeps buffers, render targets and other textures in heaps
	 * of their own; tier 2 takes them all in one, which means fewer
	 * heaps and less of each one wasted. */
	m_heaps_hold_anything = false;
	if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
		m_heaps_hold_anything = (options.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2);

	/* Heaps big enough that a frame's targets come out of one of them,
	 * so that making a target is a placement inside a heap that exists
	 * rather than a heap of its own. Vulkan sizes its blocks the same
	 * way (GSDeviceVK::CreateTargetMemoryPool). */
	block = (u64)((float)VM_SIZE * scale * scale) * 2u;
	if (block < 64ull * 1024ull * 1024ull)
		block = 64ull * 1024ull * 1024ull;
	/* 64 MB, not 512, for the reasons the Vulkan side found the hard
	 * way: a block is only given back when it is entirely empty, which
	 * at 512 MB never happens once allocations are scattered through
	 * it, so trim returns nothing and the tails of the blocks are
	 * memory nothing can reach. It is worse here than there, because a
	 * resource-heap-tier-1 device needs separate blocks for buffers,
	 * for render targets and depth stencils, and for other textures -
	 * three 512 MB blocks before a frame has drawn anything. */
	if (block > 64ull * 1024ull * 1024ull)
		block = 64ull * 1024ull * 1024ull;

	/* And the same ceiling, for the same reason: the budgets above the
	 * heap already come to more than half of it, and a ceiling under
	 * that sum refuses allocations the renderer is entitled to make.
	 * Never fewer than four blocks, or at native it would be smaller
	 * than one block. */
	ceiling = (u64)((float)VM_SIZE * scale * scale) * 16u;
	if (ceiling < block * 4u)
		ceiling = block * 4u;

	fns.create_heap  = gs_d3d12_create_heap_cb;
	fns.release_heap = gs_d3d12_release_heap_cb;

	log_cb(RETRO_LOG_INFO,
		"GS: D3D12 heap, %llu MB blocks, %llu MB ceiling, heaps hold %s.\n",
		(unsigned long long)(block >> 20),
		(unsigned long long)(ceiling >> 20),
		m_heaps_hold_anything ? "anything (tier 2)" : "one kind of thing (tier 1)");

	if (!gs_d3d12_heap_init(&m_heap, m_device.get(), &fns, block, ceiling))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create the D3D12 heap\n");
		return false;
	}

	return true;
}

/* The heap flags a resource of this description needs. On tier 2 they
 * all share, so everything lands in the same blocks. */
static D3D12_HEAP_FLAGS GetHeapFlagsForResource(const D3D12_RESOURCE_DESC* desc, bool heaps_hold_anything)
{
	if (heaps_hold_anything)
		return D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;

	if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		return D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

	if (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
		return D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;

	return D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
}

bool GSDevice12::CreatePlacedResource(const D3D12_RESOURCE_DESC* desc, D3D12_HEAP_TYPE heap_type,
	D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear_value,
	ID3D12Resource** out_resource, gs_d3d12_alloc_t* out_alloc)
{
	const D3D12_RESOURCE_ALLOCATION_INFO info = m_device->GetResourceAllocationInfo(0, 1, desc);
	const D3D12_HEAP_FLAGS heap_flags = GetHeapFlagsForResource(desc, m_heaps_hold_anything);
	gs_d3d12_alloc_t alloc = {};
	HRESULT hr;

	if (info.SizeInBytes == UINT64_MAX)
		return false;

	if (!gs_d3d12_heap_alloc(&m_heap, info.SizeInBytes, info.Alignment,
			static_cast<uint32_t>(heap_type), static_cast<uint32_t>(heap_flags), &alloc))
	{
		/* Silence here is a black screen and nothing to go on, which is
		 * how the Vulkan side of this cost a week. Say what was asked
		 * for and what the heap is holding. */
		log_cb(RETRO_LOG_ERROR,
			"GS: no room for a %llu KB resource (heap %llu MB reserved, %llu MB used: "
			"%llu MB uploads and readbacks, %llu MB device).\n",
			(unsigned long long)(info.SizeInBytes >> 10),
			(unsigned long long)(m_heap.bytes_reserved >> 20),
			(unsigned long long)(m_heap.bytes_used >> 20),
			(unsigned long long)(m_heap.bytes_host >> 20),
			(unsigned long long)(m_heap.bytes_device >> 20));
		return false;
	}

	hr = m_device->CreatePlacedResource(static_cast<ID3D12Heap*>(alloc.heap), alloc.offset,
		desc, state, clear_value, IID_PPV_ARGS(out_resource));
	if (FAILED(hr))
	{
		gs_d3d12_heap_free(&m_heap, &alloc);
		/* Out of memory is not fatal; the caller retries with less. */
		if (hr != E_OUTOFMEMORY)
			log_cb(RETRO_LOG_ERROR, "CreatePlacedResource() failed with HRESULT %08X\n", hr);
		return false;
	}

	/* The span this resource was placed in may have held another one a
	 * moment ago, and D3D12 requires an aliasing barrier before the new
	 * one is used or the command list is invalid - Close() returns
	 * E_INVALIDARG and the frame never reaches the screen. A committed
	 * resource owned its memory and never needed this, which is why it
	 * appears now and only in games that churn through targets fast
	 * enough to reuse a span. NULL before means "whatever was here". */
	{
		D3D12_RESOURCE_BARRIER rb = {D3D12_RESOURCE_BARRIER_TYPE_ALIASING,
			D3D12_RESOURCE_BARRIER_FLAG_NONE};
		rb.Aliasing.pResourceBefore = nullptr;
		rb.Aliasing.pResourceAfter  = *out_resource;
		GetInitCommandList()->ResourceBarrier(1, &rb);
	}

	/* Name it. The runtime prints the name of a resource it complains
	 * about, and "Unnamed ID3D12Resource Object" in a validation
	 * message is the difference between knowing whose resource it is
	 * and guessing. Everything this backend places gets one, so
	 * anything still unnamed in a message belongs to the frontend. */
	{
		static unsigned s_placed_serial;
		wchar_t name[96];

		_snwprintf(name, sizeof(name) / sizeof(name[0]),
			L"GS placed %u: %s %ux%u fmt %u heap %u",
			++s_placed_serial,
			(desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) ? L"buffer" : L"texture",
			(unsigned)desc->Width, (unsigned)desc->Height,
			(unsigned)desc->Format, (unsigned)heap_type);
		name[(sizeof(name) / sizeof(name[0])) - 1] = 0;
		(*out_resource)->SetName(name);
	}

	*out_alloc = alloc;
	return true;
}

bool GSDevice12::CreateFence()
{
	HRESULT hr = m_device->CreateFence(m_completed_fence_value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
	if (FAILED(hr))
		return false;

	m_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!m_fence_event)
		return false;

	return true;
}

bool GSDevice12::CreateDescriptorHeaps()
{
	static constexpr size_t MAX_SRVS = 32768;
	static constexpr size_t MAX_RTVS = 16384;
	static constexpr size_t MAX_DSVS = 16384;
	static constexpr size_t MAX_CPU_SAMPLERS = 1024;

	if (!m_descriptor_heap_manager.Create(m_device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, MAX_SRVS, false) ||
		!m_rtv_heap_manager.Create(m_device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, MAX_RTVS, false) ||
		!m_dsv_heap_manager.Create(m_device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, MAX_DSVS, false) ||
		!m_sampler_heap_manager.Create(m_device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, MAX_CPU_SAMPLERS, false))
		return false;

	// Allocate null SRV descriptor for unbound textures.
	constexpr D3D12_SHADER_RESOURCE_VIEW_DESC null_srv_desc = {DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_SRV_DIMENSION_TEXTURE2D,
		D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};

	if (!m_descriptor_heap_manager.Allocate(&m_null_srv_descriptor))
		return false;

	m_device->CreateShaderResourceView(nullptr, &null_srv_desc, m_null_srv_descriptor.cpu_handle);
	return true;
}

bool GSDevice12::CreateCommandLists()
{
	static constexpr size_t MAX_GPU_SRVS = 32768;
	static constexpr size_t MAX_GPU_SAMPLERS = 2048;

	for (u32 i = 0; i < NUM_COMMAND_LISTS; i++)
	{
		CommandListResources& res = m_command_lists[i];
		HRESULT hr;

		for (u32 i = 0; i < 2; i++)
		{
			hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
				IID_PPV_ARGS(res.command_allocators[i].put()));
			if (FAILED(hr))
				return false;

			hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				res.command_allocators[i].get(), nullptr,
				IID_PPV_ARGS(res.command_lists[i].put()));
			if (FAILED(hr))
			{
				log_cb(RETRO_LOG_ERROR, "Failed to create command list: %08X\n", hr);
				return false;
			}

			// Close the command lists, since the first thing we do is reset them.
			hr = res.command_lists[i]->Close();
			if (FAILED(hr))
				return false;
		}

		if (!res.descriptor_allocator.Create(m_device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, MAX_GPU_SRVS))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to create per frame descriptor allocator\n");
			return false;
		}

		if (!res.sampler_allocator.Create(m_device.get(), MAX_GPU_SAMPLERS))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to create per frame sampler allocator\n");
			return false;
		}
	}

	MoveToNextCommandList();
	return true;
}

void GSDevice12::MoveToNextCommandList()
{
	const int next_command_list = (m_current_command_list + 1) % NUM_COMMAND_LISTS;

	// We may have to wait if this command list hasn't finished on the GPU.
	CommandListResources& res = m_command_lists[next_command_list];
	WaitForFence(res.ready_fence_value);

	m_current_command_list = next_command_list;
	m_current_fence_value++;
	res.ready_fence_value = m_current_fence_value;
	res.init_command_list_used = false;

	// Begin command list.
	res.command_allocators[1]->Reset();
	res.command_lists[1]->Reset(res.command_allocators[1].get(), nullptr);
	res.descriptor_allocator.Reset();
	if (res.sampler_allocator.ShouldReset())
		res.sampler_allocator.Reset();

	ID3D12DescriptorHeap* heaps[2] = {res.descriptor_allocator.GetDescriptorHeap(), res.sampler_allocator.GetDescriptorHeap()};
	res.command_lists[1]->SetDescriptorHeaps(C89_ARRAY_SIZE(heaps), heaps);

}

ID3D12GraphicsCommandList4* GSDevice12::GetInitCommandList()
{
	CommandListResources& res = m_command_lists[m_current_command_list];
	if (!res.init_command_list_used)
	{
		HRESULT hr = res.command_allocators[0]->Reset();
		res.command_lists[0]->Reset(res.command_allocators[0].get(), nullptr);
		res.init_command_list_used = true;
	}

	return res.command_lists[0].get();
}

/* Report a recurring device error without turning a wedged GPU into a
 * multi-gigabyte log.  The first few say what happened; after that the
 * count is carried until the next distinct failure. */
void GSDevice12::ReportRecurring(const char* what, HRESULT hr)
{
	static const char* last_what;
	static HRESULT     last_hr;
	static u64         repeats;

	if (what != last_what || hr != last_hr)
	{
		if (repeats > 0)
			log_cb(RETRO_LOG_ERROR, "(previous message repeated %llu more times)\n",
					static_cast<unsigned long long>(repeats));
		last_what = what;
		last_hr   = hr;
		repeats   = 0;
		log_cb(RETRO_LOG_ERROR, "%s failed with HRESULT %08X\n", what, hr);
		return;
	}

	repeats++;
	/* One line every ~10 seconds of a 60 Hz stream of failures, so a
	 * report still shows the problem is ongoing. */
	if ((repeats % 600) == 0)
		log_cb(RETRO_LOG_ERROR, "%s failed with HRESULT %08X (%llu times)\n", what, hr,
				static_cast<unsigned long long>(repeats));
}

bool GSDevice12::ContextExecuteCommandList(bool wait_for_completion)
{
	CommandListResources& res = m_command_lists[m_current_command_list];
	HRESULT hr;
	bool    closed_ok = true;

	/* Anything the runtime objected to while this list was being
	 * recorded, whether or not the Close below fails. */
	DrainDebugMessages();

	if (res.init_command_list_used)
	{
		hr = res.command_lists[0]->Close();
		if (FAILED(hr))
		{
			ReportRecurring("Closing init command list", hr);
			DrainDebugMessages();
			closed_ok = false;
		}
	}

	// Close and queue command list.
	if (closed_ok)
	{
		hr = res.command_lists[1]->Close();
		if (FAILED(hr))
		{
			ReportRecurring("Closing main command list", hr);
			DrainDebugMessages();
			closed_ok = false;
		}
	}

	if (!closed_ok)
	{
		/* This used to return here, which is what turned one failed
		 * close into a device that never worked again.  Nothing was
		 * submitted, the fence was never signalled and the command
		 * list index never advanced, so the next call re-entered with
		 * init_command_list_used still set and closed an
		 * already-closed list - E_FAIL, once per attempt, forever.
		 * The stream buffers were caught in the same net: they only
		 * reclaim space when the completed fence passes the value a
		 * segment was tagged with, so a fence that stops advancing
		 * means "Failed to reserve space for vertices" and "Failed to
		 * reserve texture upload memory" from then on, and the
		 * handlers for those call back into this function to make
		 * room.
		 *
		 * Drop the frame, but keep the bookkeeping moving: signal the
		 * fence this list was going to signal - there is no GPU work
		 * outstanding for it, so it completes immediately and the
		 * stream buffers can free their segments - and move to the
		 * next command list, which resets an allocator and a list that
		 * are in a known state.  The frame is lost either way; this
		 * way the next one has somewhere to go. */
		m_command_queue->Signal(m_fence.get(), res.ready_fence_value);

		/* And the lists themselves are replaced. Moving on was not
		 * enough: a list that has failed to close - or, when it was the
		 * init list that failed, a main list that was then never closed
		 * at all - does not come back. The next time round Reset() is
		 * refused, whatever is recorded goes nowhere, and Close() fails
		 * again, with E_FAIL now and for good. There are three of these,
		 * so three bad frames in a row - one invalid command recorded on
		 * each, which a texture re-sent every frame will do - left the
		 * renderer with nothing it could submit on, ever: the log shows
		 * E_INVALIDARG three times and then E_FAIL thousands of times,
		 * the core still presenting the last picture it made, until the
		 * frontend rebuilt the device. Nothing was submitted from these,
		 * so they can simply go; the allocators are reset when their turn
		 * comes, as always. */
		for (u32 i = 0; i < 2; i++)
		{
			res.command_lists[i].reset();
			hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				res.command_allocators[i].get(), nullptr,
				IID_PPV_ARGS(res.command_lists[i].put()));
			if (SUCCEEDED(hr))
				hr = res.command_lists[i]->Close();
			if (FAILED(hr))
				ReportRecurring("Replacing a command list", hr);
		}
		res.init_command_list_used = false;

		MoveToNextCommandList();
		if (wait_for_completion)
			WaitForFence(res.ready_fence_value);
		return false;
	}

	if (res.init_command_list_used)
	{
		const std::array<ID3D12CommandList*, 2> execute_lists{res.command_lists[0].get(), res.command_lists[1].get()};
		m_command_queue->ExecuteCommandLists(static_cast<UINT>(execute_lists.size()), execute_lists.data());
	}
	else
	{
		const std::array<ID3D12CommandList*, 1> execute_lists{res.command_lists[1].get()};
		m_command_queue->ExecuteCommandLists(static_cast<UINT>(execute_lists.size()), execute_lists.data());
	}

	// Update fence when GPU has completed.
	hr = m_command_queue->Signal(m_fence.get(), res.ready_fence_value);

	MoveToNextCommandList();
	if (wait_for_completion)
		WaitForFence(res.ready_fence_value);

	return true;
}

void GSDevice12::InvalidateSamplerGroups()
{
	for (CommandListResources& res : m_command_lists)
		res.sampler_allocator.InvalidateCache();
}

void GSDevice12::DeferObjectDestruction(ID3D12DeviceChild* resource)
{
	if (!resource)
		return;

	resource->AddRef();
	{
		PendingResource pr = {};
		pr.resource = resource;
		m_command_lists[m_current_command_list].pending_resources.push_back(pr);
	}
}

void GSDevice12::DeferResourceDestruction(const gs_d3d12_alloc_t* alloc, ID3D12Resource* resource)
{
	PendingResource pr = {};

	if (!resource)
		return;

	resource->AddRef();
	if (alloc)
		pr.alloc = *alloc;
	pr.resource = resource;
	m_command_lists[m_current_command_list].pending_resources.push_back(pr);
}

void GSDevice12::DeferDescriptorDestruction(D3D12DescriptorHeapManager& manager, u32 index)
{
	m_command_lists[m_current_command_list].pending_descriptors.emplace_back(manager, index);
}

void GSDevice12::DeferDescriptorDestruction(D3D12DescriptorHeapManager& manager, D3D12DescriptorHandle* handle)
{
	if (handle->index == D3D12DescriptorHandle::INVALID_INDEX)
		return;

	m_command_lists[m_current_command_list].pending_descriptors.emplace_back(manager, handle->index);
	handle->Clear();
}

void GSDevice12::DestroyPendingResources(CommandListResources& cmdlist)
{
	for (const auto& dd : cmdlist.pending_descriptors)
		dd.first.Free(dd.second);
	cmdlist.pending_descriptors.clear();

	for (const auto& it : cmdlist.pending_resources)
	{
		it.resource->Release();
		/* The heap span goes back now, with the GPU finished with it. */
		if (it.alloc.heap)
			gs_d3d12_heap_free(&m_heap, &it.alloc);
	}
	cmdlist.pending_resources.clear();
}

void GSDevice12::WaitForFence(u64 fence)
{
	if (m_completed_fence_value >= fence)
		return;

	// Try non-blocking check.
	m_completed_fence_value = m_fence->GetCompletedValue();
	if (m_completed_fence_value < fence)
	{
		// Fall back to event.
		HRESULT hr = m_fence->SetEventOnCompletion(fence, m_fence_event);
		WaitForSingleObject(m_fence_event, INFINITE);
		m_completed_fence_value = m_fence->GetCompletedValue();
	}

	// Release resources for as many command lists which have completed.
	u32 index = (m_current_command_list + 1) % NUM_COMMAND_LISTS;
	for (u32 i = 0; i < NUM_COMMAND_LISTS; i++)
	{
		CommandListResources& res = m_command_lists[index];
		if (m_completed_fence_value < res.ready_fence_value)
			break;

		DestroyPendingResources(res);
		index = (index + 1) % NUM_COMMAND_LISTS;
	}
}

void GSDevice12::WaitForGPUIdle()
{
	u32 index = (m_current_command_list + 1) % NUM_COMMAND_LISTS;
	for (u32 i = 0; i < (NUM_COMMAND_LISTS - 1); i++)
	{
		WaitForFence(m_command_lists[index].ready_fence_value);
		index = (index + 1) % NUM_COMMAND_LISTS;
	}
}

bool GSDevice12::AllocatePreinitializedGPUBuffer(u32 size, ID3D12Resource** gpu_buffer,
	gs_d3d12_alloc_t* gpu_alloc, const std::function<void(void*)>& fill_callback)
{
	// Try to place the fixed index buffer in GPU local memory.
	// Use the staging buffer to copy into it.
	const D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1,
		DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
		D3D12_RESOURCE_FLAG_NONE};

	ComPtr<ID3D12Resource> cpu_buffer;
	gs_d3d12_alloc_t cpu_alloc = {};
	HRESULT hr;

	if (!CreatePlacedResource(&rd, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr, cpu_buffer.put(), &cpu_alloc))
		return false;

	static constexpr const D3D12_RANGE read_range = {};
	const D3D12_RANGE write_range = {0, size};
	void* mapped;
	hr = cpu_buffer->Map(0, &read_range, &mapped);
	if (FAILED(hr))
	{
		DeferResourceDestruction(&cpu_alloc, cpu_buffer.get());
		return false;
	}
	fill_callback(mapped);
	cpu_buffer->Unmap(0, &write_range);

	if (!CreatePlacedResource(&rd, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
			nullptr, gpu_buffer, gpu_alloc))
	{
		DeferResourceDestruction(&cpu_alloc, cpu_buffer.get());
		return false;
	}

	GetInitCommandList()->CopyBufferRegion(*gpu_buffer, 0, cpu_buffer.get(), 0, size);

	D3D12_RESOURCE_BARRIER rb = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE};
	rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	rb.Transition.pResource = *gpu_buffer;
	rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; // COMMON -> COPY_DEST at first use.
	rb.Transition.StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER;
	GetInitCommandList()->ResourceBarrier(1, &rb);

	DeferResourceDestruction(&cpu_alloc, cpu_buffer.get());
	return true;
}

static bool IsDATMConvertShader(ShaderConvert i)
{
	return (i == ShaderConvert::DATM_0 || i == ShaderConvert::DATM_1 || i == ShaderConvert::DATM_0_RTA_CORRECTION || i == ShaderConvert::DATM_1_RTA_CORRECTION);
}
static bool IsDATEModePrimIDInit(u32 flag)
{
	return flag == 1 || flag == 2;
}

static constexpr std::array<D3D12_PRIMITIVE_TOPOLOGY, 3> s_primitive_topology_mapping = {
	{D3D_PRIMITIVE_TOPOLOGY_POINTLIST, D3D_PRIMITIVE_TOPOLOGY_LINELIST, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}};

static D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE GetLoadOpForTexture(GSTexture12* tex)
{
	if (!tex)
		return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;

	// clang-format off
	switch (tex->GetState())
	{
	case GSTexture12::State::Cleared:       tex->SetState(GSTexture::State::Dirty); return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
	case GSTexture12::State::Invalidated:   tex->SetState(GSTexture::State::Dirty); return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
	case GSTexture12::State::Dirty:
	default:                                break;
	}
	// clang-format on
	return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
}

GSDevice12::ShaderMacro::ShaderMacro(D3D_FEATURE_LEVEL fl)
{
	switch (fl)
	{
		case D3D_FEATURE_LEVEL_10_0:
			mlist.emplace_back("SHADER_MODEL", "0x400");
			break;
		case D3D_FEATURE_LEVEL_10_1:
			mlist.emplace_back("SHADER_MODEL", "0x401");
			break;
		case D3D_FEATURE_LEVEL_11_0:
		default:
			mlist.emplace_back("SHADER_MODEL", "0x500");
			break;
	}
	mlist.emplace_back("DX12", "1");
}

void GSDevice12::ShaderMacro::AddMacro(const char* n, int d)
{
	AddMacro(n, std::to_string(d));
}

void GSDevice12::ShaderMacro::AddMacro(const char* n, std::string d)
{
	mlist.emplace_back(n, std::move(d));
}

D3D_SHADER_MACRO* GSDevice12::ShaderMacro::GetPtr(void)
{
	mout.clear();

	for (auto& i : mlist)
		mout.emplace_back(i.name.c_str(), i.def.c_str());

	mout.emplace_back(nullptr, nullptr);
	return (D3D_SHADER_MACRO*)mout.data();
}

/* The table GSDevice calls this device through (gs_device_ops,
 * Common/GSDevice.h): every entry is this class's function of that
 * name, and each was a virtual function. */
GS_DEVICE_OPS_DEFINE(GSDevice12, d3d12);

GSDevice12::GSDevice12()
{
	m_ops = &s_d3d12_device_ops;
}

GSDevice12::~GSDevice12()
{
}

RenderAPI GSDevice12::GetRenderAPI() const
{
	return RenderAPI::D3D12;
}

bool GSDevice12::Create()
{
	if (!CreateBase())
		return false;

	m_dxgi_factory = D3D::CreateFactory(GSConfig.UseDebugDevice);
	if (!m_dxgi_factory)
		return false;

	if (!LoadD3D12Library())
	{
		log_cb(RETRO_LOG_ERROR, "Failed to load D3D12 library\n");
		return false;
	}

	d3d12 = nullptr;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void **)&d3d12) || !d3d12) {
		log_cb(RETRO_LOG_ERROR, "Failed to get HW rendering interface!\n");
		return false;
	}

	/* Version 1, or version 2 if gs_d3d12_negotiate_hw_interface() asked
	 * for it and the frontend has it. Anything else was not asked for. */
	if (   d3d12->interface_version != RETRO_HW_RENDER_INTERFACE_D3D12_VERSION
	    && d3d12->interface_version != RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2) {
		log_cb(RETRO_LOG_ERROR, "HW render interface mismatch, expected %u or %u, got %u!\n", RETRO_HW_RENDER_INTERFACE_D3D12_VERSION, RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2, d3d12->interface_version);
		return false;
	}

	m_device = d3d12->device;

	/* If something forced the debug layer on before the frontend made
	 * this device - dxcpl, or a RetroArch built for it - then say what
	 * it objects to rather than letting it break. Left as a break, the
	 * first validation error is an unhandled exception inside
	 * D3D12SDKLayers and the process is gone before anything is logged,
	 * which is the whole of what a report has to go on. */
	if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(m_info_queue.put()))))
		m_info_queue.reset();
	else
	{
		m_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
		m_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR,      FALSE);
		m_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING,    FALSE);
		log_cb(RETRO_LOG_INFO,
			"GS: D3D12 debug layer is on; validation messages go to this log.\n");
	}

	const LUID luid(m_device->GetAdapterLuid());

	if (FAILED(m_dxgi_factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(m_adapter.put()))))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to get lookup adapter by device LUID\n");
		Destroy();
		return false;
	}

	m_command_queue = d3d12->queue;

	if (
		
		   !CreateHeap()
		|| !CreateFence()
		|| !CreateDescriptorHeaps() 
		|| !CreateCommandLists())
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create D3D12 context\n");
		Destroy();
		return false;
	}

	if (!m_texture_stream_buffer.Create(TEXTURE_UPLOAD_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create D3D12 context\n");
		Destroy();
		return false;
	}

	if (!CheckFeatures())
	{
		log_cb(RETRO_LOG_ERROR, "Your GPU does not support the required D3D12 features.\n");
		return false;
	}

	AcquireWindow();

	if (!m_shader_cache.Open(m_feature_level, GSConfig.UseDebugDevice))
		log_cb(RETRO_LOG_WARN, "Shader cache failed to open.\n");

	if (!CreateNullTexture())
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create dummy texture\n");
		return false;
	}

	if (!CreateRootSignatures())
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create pipeline layouts\n");
		return false;
	}

	/* The utility pipelines, then the listed draw pipelines, from here on
	 * a thread of their own; without one, or after one of them could not
	 * be built on the last device, the utility pipelines here, so a
	 * device that cannot have them is not made. */
	const bool warming = StartTFXWarm();

	if (!CreateBuffers())
		return false;

	if (!warming || s_utility_build_failed)
	{
		for (u32 slot = 0; slot < UTIL_COUNT; slot++)
		{
			if (!(m_utility[slot] = CreateUtilityPipeline(slot)))
			{
				log_cb(RETRO_LOG_ERROR, "Failed to compile utility pipelines\n");
				return false;
			}
		}
		s_utility_build_failed = false;
	}

	InitializeState();
	InitializeSamplers();
	return true;
}

void GSDevice12::Destroy()
{
	DestroyBase();
	/* With the pool's textures, and like them ahead of the last submit
	 * below. The frontend holds its own reference to any it still shows. */
	gs_d3d12_free_present_textures();

	if (GetCommandList())
	{
		EndRenderPass();
		ExecuteCommandList(true);
	}

	DestroyResources();
	UnloadD3D12Library();
}

GSDevice::PresentResult GSDevice12::BeginPresent(bool frame_skip)
{
	EndRenderPass();

	if (m_device_lost)
		return PresentResult::DeviceLost;
	return PresentResult::OK;
}

void GSDevice12::EndPresent()
{
	InvalidateCachedState();
}

bool GSDevice12::CheckFeatures()
{
	const u32 vendorID = GetAdapterVendorID();
	const bool isAMD = (vendorID == 0x1002 || vendorID == 0x1022);

	m_features.texture_barrier = false;
	m_features.broken_point_sampler = isAMD;
	m_features.primitive_id = true;
	m_features.prefer_new_textures = true;
	m_features.provoking_vertex_last = false;
	m_features.point_expand = false;
	m_features.line_expand = false;
	m_features.framebuffer_fetch = false;
	m_features.clip_control = true;
	m_features.stencil_buffer = true;
	m_features.test_and_sample_depth = false;
	m_features.vs_expand = !GSConfig.DisableVertexShaderExpand;

	m_features.dxt_textures = SupportsTextureFormat(DXGI_FORMAT_BC1_UNORM) &&
							  SupportsTextureFormat(DXGI_FORMAT_BC2_UNORM) &&
							  SupportsTextureFormat(DXGI_FORMAT_BC3_UNORM);
	m_features.bptc_textures = SupportsTextureFormat(DXGI_FORMAT_BC7_UNORM);

	return true;
}

void GSDevice12::DrawPrimitive()
{
	GetCommandList()->DrawInstanced(m_vertex.count, 1, m_vertex.start, 0);
}

void GSDevice12::DrawIndexedPrimitive()
{
	GetCommandList()->DrawIndexedInstanced(m_index.count, 1, m_index.start, m_vertex.start, 0);
}

void GSDevice12::DrawIndexedPrimitive(int offset, int count)
{
	GetCommandList()->DrawIndexedInstanced(count, 1, m_index.start + offset, m_vertex.start, 0);
}

void GSDevice12::LookupNativeFormat(GSTexture::Format format, DXGI_FORMAT* d3d_format, DXGI_FORMAT* srv_format, DXGI_FORMAT* rtv_format, DXGI_FORMAT* dsv_format) const
{
	static constexpr std::array<std::array<DXGI_FORMAT, 4>, static_cast<int>(GSTexture::Format::BC7) + 1>
		s_format_mapping = {{
			{DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN}, // Invalid
			{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
				DXGI_FORMAT_UNKNOWN}, // Color
			{DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM,
				DXGI_FORMAT_UNKNOWN}, // ColorClip
			{DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_UNKNOWN,
				DXGI_FORMAT_D32_FLOAT_S8X24_UINT}, // DepthStencil
			{DXGI_FORMAT_A8_UNORM, DXGI_FORMAT_A8_UNORM, DXGI_FORMAT_A8_UNORM, DXGI_FORMAT_UNKNOWN}, // UNorm8
			{DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_UNKNOWN}, // UInt16
			{DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_UNKNOWN}, // UInt32
			{DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN}, // Int32
			{DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN}, // BC1
			{DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN}, // BC2
			{DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN}, // BC3
			{DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN}, // BC7
		}};

	const auto& mapping = s_format_mapping[static_cast<int>(format)];
	if (d3d_format)
		*d3d_format = mapping[0];
	if (srv_format)
		*srv_format = mapping[1];
	if (rtv_format)
		*rtv_format = mapping[2];
	if (dsv_format)
		*dsv_format = mapping[3];
}

GSTexture* GSDevice12::CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{
	const u32 clamped_width = static_cast<u32>(pcsx2_clamp_i(width, 1, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION));
	const u32 clamped_height = static_cast<u32>(pcsx2_clamp_i(height, 1, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION));

	DXGI_FORMAT dxgi_format, srv_format, rtv_format, dsv_format;
	LookupNativeFormat(format, &dxgi_format, &srv_format, &rtv_format, &dsv_format);

	const DXGI_FORMAT uav_format = (type == GSTexture::Type::RWTexture) ? dxgi_format : DXGI_FORMAT_UNKNOWN;

	std::unique_ptr<GSTexture12> tex(GSTexture12::Create(type, format, clamped_width, clamped_height, levels,
		dxgi_format, srv_format, rtv_format, dsv_format, uav_format));
	if (!tex)
	{
		// We're probably out of vram, try flushing the command buffer to release pending textures.
		PurgePool();
		ExecuteCommandListAndRestartRenderPass(true);
		tex = GSTexture12::Create(type, format, clamped_width, clamped_height, levels, dxgi_format, srv_format, rtv_format, dsv_format,
				uav_format);
	}

	return tex.release();
}

GSDownloadTexturePtr GSDevice12::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return GSDownloadTexturePtr(GSDownloadTexture12::Create(width, height, format).release());
}

void GSDevice12::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
	GSTexture12* const sTex12 = static_cast<GSTexture12*>(sTex);
	GSTexture12* const dTex12 = static_cast<GSTexture12*>(dTex);
	const GSVector4i dtex_rc(0, 0, dTex12->GetWidth(), dTex12->GetHeight());

	if (sTex12->GetState() == GSTexture::State::Cleared)
	{
		// source is cleared. if destination is a render target, we can carry the clear forward
		if (dTex12->IsRenderTargetOrDepthStencil())
		{
			if (dtex_rc.eq(r))
			{
				// pass it forward if we're clearing the whole thing
				if (sTex12->IsDepthStencil())
					dTex12->SetClearDepth(sTex12->GetClearDepth());
				else
					dTex12->SetClearColor(sTex12->GetClearColor());

				return;
			}
			else
			{
				// otherwise we need to do an attachment clear
				EndRenderPass();

				dTex12->SetState(GSTexture::State::Dirty);

				if (dTex12->GetType() != GSTexture::Type::DepthStencil)
				{
					dTex12->TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);
					GetCommandList()->ClearRenderTargetView(dTex12->GetWriteDescriptor(),
						sTex12->GetUNormClearColor().v, 0, nullptr);
				}
				else
				{
					dTex12->TransitionToState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
					GetCommandList()->ClearDepthStencilView(dTex12->GetWriteDescriptor(),
						D3D12_CLEAR_FLAG_DEPTH, sTex12->GetClearDepth(), 0, 0, nullptr);
				}

				return;
			}
		}

		// commit the clear to the source first, then do normal copy
		sTex12->CommitClear();
	}

	// if the destination has been cleared, and we're not overwriting the whole thing, commit the clear first
	// (the area outside of where we're copying to)
	if (dTex12->GetState() == GSTexture::State::Cleared && !dtex_rc.eq(r))
		dTex12->CommitClear();

	EndRenderPass();

	sTex12->TransitionToState(D3D12_RESOURCE_STATE_COPY_SOURCE);
	sTex12->SetUseFenceCounter(GetCurrentFenceValue());
	if (m_tfx_textures[0] && sTex12->GetSRVDescriptor() == m_tfx_textures[0])
		PSSetShaderResource(0, nullptr, false);

	dTex12->TransitionToState(D3D12_RESOURCE_STATE_COPY_DEST);
	dTex12->SetUseFenceCounter(GetCurrentFenceValue());

	D3D12_TEXTURE_COPY_LOCATION srcloc;
	srcloc.pResource = sTex12->GetResource();
	srcloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	srcloc.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION dstloc;
	dstloc.pResource = dTex12->GetResource();
	dstloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dstloc.SubresourceIndex = 0;

	const D3D12_BOX srcbox{static_cast<UINT>(r.left), static_cast<UINT>(r.top), 0u,
		static_cast<UINT>(r.right), static_cast<UINT>(r.bottom), 1u};
	GetCommandList()->CopyTextureRegion(
		&dstloc, destX, destY, 0,
		&srcloc, &srcbox);

	dTex12->SetState(GSTexture::State::Dirty);
}

void GSDevice12::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvert shader /* = ShaderConvert::COPY */, bool linear /* = true */)
{
	DoStretchRect(static_cast<GSTexture12*>(sTex), sRect, static_cast<GSTexture12*>(dTex), dRect,
		UtilityPipeline(UTIL_CONVERT + static_cast<int>(shader)), linear, ShaderConvertWriteMask(shader) == 0xf);
}

void GSDevice12::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red,
	bool green, bool blue, bool alpha, ShaderConvert shader)
{
	const u32 index = (red ? 1 : 0) | (green ? 2 : 0) | (blue ? 4 : 0) | (alpha ? 8 : 0);
	int rta_offset = (shader == ShaderConvert::RTA_CORRECTION) ? 16 : 0;
	const bool allow_discard = (index == 0xf);
	DoStretchRect(static_cast<GSTexture12*>(sTex), sRect, static_cast<GSTexture12*>(dTex), dRect,
		UtilityPipeline(UTIL_COLOR_COPY + index + rta_offset), false, allow_discard);
}

/* How many indices the frontend cycles through: the mask is a mask, so
 * it is one past its highest set bit. Told to GSDevice after each wait
 * so a retired present texture is freed on the frontend's own signal
 * rather than on a guessed frame count (GSDevice::AgePool). */
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

void GSDevice12::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect)
{
	GSTexture12* texture = (GSTexture12*)sTex;

	if (   d3d12->interface_version >= RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2
	    && d3d12->get_sync_index && d3d12->wait_sync_index && d3d12->set_texture_fenced)
	{
		const unsigned index = d3d12->get_sync_index(d3d12->handle);
		const bool     blank = (!sRect.right && !sRect.bottom);
		u64            value;

		if (index < GS_D3D12_MAX_SYNC_INDICES)
		{
			/* Not ours until the frontend has finished with what this index
			 * carried last time round. */
			d3d12->wait_sync_index(d3d12->handle);
			SyncIndexWaited(d3d12->get_sync_index_mask
					? gs_sync_slots_from_mask(d3d12->get_sync_index_mask(d3d12->handle))
					: 1);

			/* Every texture presented is drawn over in full each frame, so
			 * the one handed over stays with this index until it comes
			 * round again, and the next frame draws into the one this
			 * index carried, which the wait above has made ours. Nothing is
			 * copied. */
			if (SwapPresentTexture(texture, s_d3d12_present_textures[index]))
			{
				s_d3d12_present_textures[index] = texture;
				if (blank)
					ClearRenderTarget(texture, 0);
				texture->CommitClear();
				texture->TransitionToState(d3d12->required_state);

				/* The value the list being recorded signals when it is
				 * submitted, which is the next line: the texture is
				 * complete, and in required_state, once the fence is there. */
				value = GetCurrentFenceValue();
				ExecuteCommandList(false);

				d3d12->set_texture_fenced(d3d12->handle, texture->GetResource(),
					texture->GetResource()->GetDesc().Format, m_fence.get(), value);
				video_cb(RETRO_HW_FRAME_BUFFER_VALID, texture->GetWidth(), texture->GetHeight(), 0);
				return;
			}
		}
		/* A texture that is none of the device's present targets: version
		 * 1's handoff is still valid on a version 2 interface. */
	}

	texture->CommitClear();
	texture->TransitionToState(d3d12->required_state);
	ExecuteCommandList(false);

	/* Blanking enforce, see 'GSRenderer::VSync()' */
	if (!sRect.right && !sRect.bottom)
		ClearRenderTarget(texture, 0);

	d3d12->set_texture(d3d12->handle, texture->GetResource(), texture->GetResource()->GetDesc().Format);
	video_cb(RETRO_HW_FRAME_BUFFER_VALID, texture->GetWidth(), texture->GetHeight(), 0);
}

void GSDevice12::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize, u32 samples)
{
	// match merge cb
	struct Uniforms
	{
		float scale;
		float samples; // BGColor.y
		float pad1[2];
		u32 offsetX, offsetY, dOffset;
		u32 pad2;
	};
	const Uniforms cb = {sScale, static_cast<float>(samples), {}, offsetX, offsetY, dOffset};
	SetUtilityRootSignature();
	SetUtilityPushConstants(&cb, sizeof(cb));

	const GSVector4 dRect(0, 0, dSize * samples, samples);
	const ShaderConvert shader = (dSize == 16) ? ShaderConvert::CLUT_4 : ShaderConvert::CLUT_8;
	DoStretchRect(static_cast<GSTexture12*>(sTex), GSVector4::zero(), static_cast<GSTexture12*>(dTex), dRect,
		UtilityPipeline(UTIL_CONVERT + static_cast<int>(shader)), false, true);
}

void GSDevice12::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
	// match merge cb
	struct Uniforms
	{
		float scale;
		float pad1[3];
		u32 SBW, DBW, PageOffset;
	};

	const Uniforms cb = {sScale, {}, SBW, DBW, IndexedConversionPageOffset(offsetX, offsetY, SBW)};
	SetUtilityRootSignature();
	SetUtilityPushConstants(&cb, sizeof(cb));

	const GSVector4 dRect(0, 0, dTex->GetWidth(), dTex->GetHeight());
	const ShaderConvert shader = ShaderConvert::RGBA_TO_8I;
	DoStretchRect(static_cast<GSTexture12*>(sTex), GSVector4::zero(), static_cast<GSTexture12*>(dTex), dRect,
		UtilityPipeline(UTIL_CONVERT + static_cast<int>(shader)), false, true);
}

void GSDevice12::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect)
{
	struct Uniforms
	{
		float weight;
		float pad0[3];
		GSVector2i clamp_min;
		int downsample_factor;
		int pad1;
	};

	const Uniforms cb = {
		static_cast<float>(downsample_factor * downsample_factor), {}, clamp_min, static_cast<int>(downsample_factor), 0};
	SetUtilityRootSignature();
	SetUtilityPushConstants(&cb, sizeof(cb));

	const ShaderConvert shader = ShaderConvert::DOWNSAMPLE_COPY;
	DoStretchRect(static_cast<GSTexture12*>(sTex), GSVector4::zero(), static_cast<GSTexture12*>(dTex), dRect,
		UtilityPipeline(UTIL_CONVERT + static_cast<int>(shader)), false, true);
}

void GSDevice12::MoveTexels(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const MoveTexelsConstants& cb)
{
	SetUtilityRootSignature();
	SetUtilityPushConstants(&cb, sizeof(cb));

	const ShaderConvert shader = ShaderConvert::MOVE_TEXELS;
	DoStretchRect(static_cast<GSTexture12*>(sTex), GSVector4::zero(), static_cast<GSTexture12*>(dTex), dRect,
		UtilityPipeline(UTIL_CONVERT + static_cast<int>(shader)), false, true);
}

void GSDevice12::DrawMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader)
{
	GSTexture* last_tex = rects[0].src;
	bool last_linear = rects[0].linear;
	u8 last_wmask = rects[0].wmask.wrgba;

	u32 first = 0;
	u32 count = 1;

	// Make sure all textures are in shader read only layout, so we don't need to break
	// the render pass to transition.
	for (u32 i = 0; i < num_rects; i++)
	{
		GSTexture12* const stex = static_cast<GSTexture12*>(rects[i].src);
		stex->CommitClear();
		if (stex->GetResourceState() != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
		{
			EndRenderPass();
			stex->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
	}

	for (u32 i = 1; i < num_rects; i++)
	{
		if (rects[i].src == last_tex && rects[i].linear == last_linear && rects[i].wmask.wrgba == last_wmask)
		{
			count++;
			continue;
		}

		DoMultiStretchRects(rects + first, count, static_cast<GSTexture12*>(dTex), shader);
		last_tex = rects[i].src;
		last_linear = rects[i].linear;
		last_wmask = rects[i].wmask.wrgba;
		first += count;
		count = 1;
	}

	DoMultiStretchRects(rects + first, count, static_cast<GSTexture12*>(dTex), shader);
}

void GSDevice12::DoMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTexture12* dTex, ShaderConvert shader)
{
	// Set up vertices first.
	const u32 vertex_reserve_size = num_rects * 4 * sizeof(GSVertexPT1);
	const u32 index_reserve_size = num_rects * 6 * sizeof(u16);
	if (!m_vertex_stream_buffer.ReserveMemory(vertex_reserve_size, sizeof(GSVertexPT1)) ||
		!m_index_stream_buffer.ReserveMemory(index_reserve_size, sizeof(u16)))
	{
		ExecuteCommandListAndRestartRenderPass(false);
		if (!m_vertex_stream_buffer.ReserveMemory(vertex_reserve_size, sizeof(GSVertexPT1)) ||
			!m_index_stream_buffer.ReserveMemory(index_reserve_size, sizeof(u16)))
		{
			/* Reporting the failure and then filling the buffer anyway
			 * is worse than the failure: GetCurrentHostPointer() still
			 * points into the stream buffer, at a region nothing
			 * reserved, and the loop below writes num_rects worth of
			 * vertices and indices into it.  Once the device is wedged
			 * this ran every frame.  Drop the blit instead. */
			GSDevice12::ReportRecurring("Reserving space for vertices", E_OUTOFMEMORY);
			return;
		}
	}

	// Pain in the arse because the primitive topology for the pipelines is all triangle strips.
	// Don't use primitive restart here, it ends up slower on some drivers.
	const GSVector2 ds(static_cast<float>(dTex->GetWidth()), static_cast<float>(dTex->GetHeight()));
	GSVertexPT1* verts = reinterpret_cast<GSVertexPT1*>(m_vertex_stream_buffer.GetCurrentHostPointer());
	u16* idx = reinterpret_cast<u16*>(m_index_stream_buffer.GetCurrentHostPointer());
	u32 icount = 0;
	u32 vcount = 0;
	for (u32 i = 0; i < num_rects; i++)
	{
		const GSVector4& sRect = rects[i].src_rect;
		const GSVector4& dRect = rects[i].dst_rect;

		const float left = dRect.x * 2 / ds.x - 1.0f;
		const float top = 1.0f - dRect.y * 2 / ds.y;
		const float right = dRect.z * 2 / ds.x - 1.0f;
		const float bottom = 1.0f - dRect.w * 2 / ds.y;

		const u32 vstart = vcount;
		verts[vcount++] = {GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)};
		verts[vcount++] = {GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)};
		verts[vcount++] = {GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)};
		verts[vcount++] = {GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)};

		if (i > 0)
			idx[icount++] = vstart;

		idx[icount++] = vstart;
		idx[icount++] = vstart + 1;
		idx[icount++] = vstart + 2;
		idx[icount++] = vstart + 3;
		idx[icount++] = vstart + 3;
	};

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / sizeof(GSVertexPT1);
	m_vertex.count = vcount;
	m_index.start = m_index_stream_buffer.GetCurrentOffset() / sizeof(u16);
	m_index.count = icount;
	m_vertex_stream_buffer.CommitMemory(vcount * sizeof(GSVertexPT1));
	m_index_stream_buffer.CommitMemory(icount * sizeof(u16));
	SetVertexBuffer(m_vertex_stream_buffer.GetGPUPointer(), m_vertex_stream_buffer.GetSize(), sizeof(GSVertexPT1));
	SetIndexBuffer(m_index_stream_buffer.GetGPUPointer(), m_index_stream_buffer.GetSize(), DXGI_FORMAT_R16_UINT);

	// Even though we're batching, a cmdbuffer submit could've messed this up.
	const GSVector4i rc(dTex->GetRect());
	OMSetRenderTargets(dTex->IsRenderTarget() ? dTex : nullptr, dTex->IsDepthStencil() ? dTex : nullptr, rc);
	if (!InRenderPass())
		BeginRenderPassForStretchRect(dTex, rc, rc, false);
	SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	SetUtilityTexture(rects[0].src, rects[0].linear ? m_linear_sampler_cpu : m_point_sampler_cpu);

	int rta_bit = (shader == ShaderConvert::RTA_CORRECTION) ? 16 : 0;
	SetPipeline((rects[0].wmask.wrgba != 0xf) ? UtilityPipeline(UTIL_COLOR_COPY + (rects[0].wmask.wrgba | rta_bit)) : UtilityPipeline(UTIL_CONVERT + static_cast<int>(shader)));

	if (ApplyUtilityState())
		DrawIndexedPrimitive();
}

void GSDevice12::BeginRenderPassForStretchRect(
	GSTexture12* dTex, const GSVector4i& dtex_rc, const GSVector4i& dst_rc, bool allow_discard)
{
	const D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE load_op = (allow_discard && dst_rc.eq(dtex_rc)) ?
																D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD :
																GetLoadOpForTexture(dTex);
	dTex->SetState(GSTexture::State::Dirty);

	if (dTex->GetType() != GSTexture::Type::DepthStencil)
	{
		BeginRenderPass(load_op, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE,
			D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			dTex->GetUNormClearColor());
	}
	else
	{
		BeginRenderPass(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
			D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS, load_op, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE,
			D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			GSVector4::zero(), dTex->GetClearDepth());
	}
}

void GSDevice12::DoStretchRect(GSTexture12* sTex, const GSVector4& sRect, GSTexture12* dTex, const GSVector4& dRect,
	const ID3D12PipelineState* pipeline, bool linear, bool allow_discard)
{
	if (sTex->GetResourceState() != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
	{
		// can't transition in a render pass
		EndRenderPass();
		sTex->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}

	SetUtilityRootSignature();
	SetUtilityTexture(sTex, linear ? m_linear_sampler_cpu : m_point_sampler_cpu);
	SetPipeline(pipeline);

	const bool is_present = (!dTex);
	const bool depth = (dTex && dTex->GetType() == GSTexture::Type::DepthStencil);
	const GSVector2i size(is_present ? GSVector2i(GetWindowWidth(), GetWindowHeight()) : dTex->GetSize());
	const GSVector4i dtex_rc(0, 0, size.x, size.y);
	const GSVector4i dst_rc(GSVector4i(dRect).rintersect(dtex_rc));

	// switch rts (which might not end the render pass), so check the bounds
	if (!is_present)
	{
		OMSetRenderTargets(depth ? nullptr : dTex, depth ? dTex : nullptr, dst_rc);
	}
	else
	{
		// this is for presenting, we don't want to screw with the viewport/scissor set by display
		m_dirty_flags &= ~(DIRTY_FLAG_RENDER_TARGET | DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR);
	}

	const bool drawing_to_current_rt = (is_present || InRenderPass());
	if (!drawing_to_current_rt)
		BeginRenderPassForStretchRect(dTex, dtex_rc, dst_rc, allow_discard);

	DrawStretchRect(sRect, dRect, size);
}

void GSDevice12::DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds)
{
	// ia
	const float left = dRect.x * 2 / ds.x - 1.0f;
	const float top = 1.0f - dRect.y * 2 / ds.y;
	const float right = dRect.z * 2 / ds.x - 1.0f;
	const float bottom = 1.0f - dRect.w * 2 / ds.y;

	GSVertexPT1 vertices[] = {
		{GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)},
		{GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)},
		{GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)},
		{GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)},
	};
	IASetVertexBuffer(vertices, sizeof(vertices[0]), C89_ARRAY_SIZE(vertices));
	SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	if (ApplyUtilityState())
		DrawPrimitive();
}

void GSDevice12::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear)
{
	const GSVector4 full_r(0.0f, 0.0f, 1.0f, 1.0f);
	const bool feedback_write_2 = PMODE.EN2 && sTex[2] != nullptr && EXTBUF.FBIN == 1;
	const bool feedback_write_1 = PMODE.EN1 && sTex[2] != nullptr && EXTBUF.FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE.SLBG == 1;
	const D3D12DescriptorHandle& sampler = linear ? m_linear_sampler_cpu : m_point_sampler_cpu;
	// Merge the 2 source textures (sTex[0],sTex[1]). Final results go to dTex. Feedback write will go to sTex[2].
	// If either 2nd output is disabled or SLBG is 1, a background color will be used.
	// Note: background color is also used when outside of the unit rectangle area
	EndRenderPass();

	// transition everything before starting the new render pass
	const bool has_input_0 =
		(sTex[0] && (sTex[0]->GetState() == GSTexture::State::Dirty ||
						(sTex[0]->GetState() == GSTexture::State::Cleared || sTex[0]->GetClearColor() != 0)));
	const bool has_input_1 = (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg) && sTex[1] &&
							 (sTex[1]->GetState() == GSTexture::State::Dirty ||
								 (sTex[1]->GetState() == GSTexture::State::Cleared || sTex[1]->GetClearColor() != 0));
	if (has_input_0)
	{
		static_cast<GSTexture12*>(sTex[0])->CommitClear();	
		static_cast<GSTexture12*>(sTex[0])->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	if (has_input_1)
	{
		static_cast<GSTexture12*>(sTex[1])->CommitClear();
		static_cast<GSTexture12*>(sTex[1])->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	static_cast<GSTexture12*>(dTex)->TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);

	// Upload constant to select YUV algo, but skip constant buffer update if we don't need it
	if (feedback_write_2 || feedback_write_1 || sTex[0])
	{
		SetUtilityRootSignature();
		const MergeConstantBuffer uniforms = {GSVector4::unorm8(c), EXTBUF.EMODA, EXTBUF.EMODC};
		SetUtilityPushConstants(&uniforms, sizeof(uniforms));
	}

	const GSVector2i dsize(dTex->GetSize());
	const GSVector4i darea(0, 0, dsize.x, dsize.y);
	bool dcleared = false;
	if (has_input_1 && (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		// 2nd output is enabled and selected. Copy it to destination so we can blend it with 1st output
		// Note: value outside of dRect must contains the background color (c)
		OMSetRenderTargets(dTex, nullptr, darea);
		SetUtilityTexture(sTex[1], sampler);
		BeginRenderPass(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR,
			D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
			D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
			D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS, GSVector4::unorm8(c));
		SetUtilityRootSignature();
		SetPipeline(UtilityPipeline(UTIL_CONVERT + static_cast<int>(ShaderConvert::COPY)));
		DrawStretchRect(sRect[1], PMODE.SLBG ? dRect[2] : dRect[1], dsize);
		dTex->SetState(GSTexture::State::Dirty);
		dcleared = true;
	}

	// Upload constant to select YUV algo
	const GSVector2i fbsize(sTex[2] ? sTex[2]->GetSize() : GSVector2i(0, 0));
	const GSVector4i fbarea(0, 0, fbsize.x, fbsize.y);
	if (feedback_write_2) // FIXME I'm not sure dRect[1] is always correct
	{
		EndRenderPass();
		OMSetRenderTargets(sTex[2], nullptr, fbarea);
		if (dcleared)
			SetUtilityTexture(dTex, sampler);

		// sTex[2] can be sTex[0], in which case it might be cleared (e.g. Xenosaga).
		BeginRenderPassForStretchRect(static_cast<GSTexture12*>(sTex[2]), fbarea, GSVector4i(dRect[2]));
		if (dcleared)
		{
			SetUtilityRootSignature();
			SetPipeline(UtilityPipeline(UTIL_CONVERT + static_cast<int>(ShaderConvert::YUV)));
			DrawStretchRect(full_r, dRect[2], fbsize);
		}
		EndRenderPass();

		if (sTex[0] == sTex[2])
		{
			// need a barrier here because of the render pass
			static_cast<GSTexture12*>(sTex[2])->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
	}

	// Restore background color to process the normal merge
	if (feedback_write_2_but_blend_bg || !dcleared)
	{
		EndRenderPass();
		OMSetRenderTargets(dTex, nullptr, darea);
		BeginRenderPass(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE,
			D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			GSVector4::unorm8(c));
		dTex->SetState(GSTexture::State::Dirty);
	}
	else if (!InRenderPass())
	{
		OMSetRenderTargets(dTex, nullptr, darea);
		BeginRenderPass(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE);
	}

	if (has_input_0)
	{
		// 1st output is enabled. It must be blended
		SetUtilityRootSignature();
		SetUtilityTexture(sTex[0], sampler);
		SetPipeline(UtilityPipeline(UTIL_MERGE + PMODE.MMOD));
		DrawStretchRect(sRect[0], dRect[0], dTex->GetSize());
	}

	if (feedback_write_1) // FIXME I'm not sure dRect[0] is always correct
	{
		EndRenderPass();
		SetUtilityRootSignature();
		SetPipeline(UtilityPipeline(UTIL_CONVERT + static_cast<int>(ShaderConvert::YUV)));
		SetUtilityTexture(dTex, sampler);
		OMSetRenderTargets(sTex[2], nullptr, fbarea);
		BeginRenderPass(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE);
		DrawStretchRect(full_r, dRect[2], dsize);
	}

	EndRenderPass();

	// this texture is going to get used as an input, so make sure we don't read undefined data
	static_cast<GSTexture12*>(dTex)->CommitClear();
	static_cast<GSTexture12*>(dTex)->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void GSDevice12::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb)
{
	static_cast<GSTexture12*>(dTex)->TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);

	const GSVector4i rc = GSVector4i(dRect);
	const GSVector4i dtex_rc = dTex->GetRect();
	const GSVector4i clamped_rc = rc.rintersect(dtex_rc);
	EndRenderPass();
	OMSetRenderTargets(dTex, nullptr, clamped_rc);
	SetUtilityRootSignature();
	SetUtilityTexture(sTex, linear ? m_linear_sampler_cpu : m_point_sampler_cpu);
	BeginRenderPassForStretchRect(static_cast<GSTexture12*>(dTex), dTex->GetRect(), clamped_rc, false);
	SetPipeline(UtilityPipeline(UTIL_INTERLACE + static_cast<int>(shader)));
	SetUtilityPushConstants(&cb, sizeof(cb));
	DrawStretchRect(sRect, dRect, dTex->GetSize());
	EndRenderPass();

	// this texture is going to get used as an input, so make sure we don't read undefined data
	static_cast<GSTexture12*>(dTex)->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void GSDevice12::IASetVertexBuffer(const void* vertex, size_t stride, size_t count)
{
	const u32 size = static_cast<u32>(stride) * static_cast<u32>(count);
	if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride)))
	{
		ExecuteCommandListAndRestartRenderPass(false);
		if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride)))
		{
			/* Nothing reserved the space, but GetCurrentHostPointer() is
			 * still a valid pointer into the stream buffer, so the write
			 * below would land in a region belonging to something else -
			 * and CommitMemory would then advance the buffer as though it
			 * had been reserved.  This is the main draw path, not a blit:
			 * on a wedged device it ran for every draw of every frame.
			 *
			 * Leave the count at zero so the draw that follows submits
			 * nothing, rather than reading whatever the stale offsets
			 * point at. */
			GSDevice12::ReportRecurring("Reserving space for vertices", E_OUTOFMEMORY);
			m_vertex.count = 0;
			return;
		}
	}

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / stride;
	m_vertex.count = count;
	SetVertexBuffer(m_vertex_stream_buffer.GetGPUPointer(), m_vertex_stream_buffer.GetSize(), stride);

	GSVector4i::storent(m_vertex_stream_buffer.GetCurrentHostPointer(), vertex, count * stride);
	m_vertex_stream_buffer.CommitMemory(size);
}

void GSDevice12::IASetIndexBuffer(const void* index, size_t count)
{
	const u32 size = sizeof(u16) * static_cast<u32>(count);
	if (!m_index_stream_buffer.ReserveMemory(size, sizeof(u16)))
	{
		ExecuteCommandListAndRestartRenderPass(false);
		if (!m_index_stream_buffer.ReserveMemory(size, sizeof(u16)))
		{
			/* Nothing reserved the space, but GetCurrentHostPointer() is
			 * still a valid pointer into the stream buffer, so the write
			 * below would land in a region belonging to something else -
			 * and CommitMemory would then advance the buffer as though it
			 * had been reserved.  This is the main draw path, not a blit:
			 * on a wedged device it ran for every draw of every frame.
			 *
			 * Leave the count at zero so the draw that follows submits
			 * nothing, rather than reading whatever the stale offsets
			 * point at. */
			GSDevice12::ReportRecurring("Reserving space for indices", E_OUTOFMEMORY);
			m_index.count = 0;
			return;
		}
	}

	m_index.start = m_index_stream_buffer.GetCurrentOffset() / sizeof(u16);
	m_index.count = count;
	SetIndexBuffer(m_index_stream_buffer.GetGPUPointer(), m_index_stream_buffer.GetSize(), DXGI_FORMAT_R16_UINT);

	memcpy(m_index_stream_buffer.GetCurrentHostPointer(), index, size);
	m_index_stream_buffer.CommitMemory(size);
}

void GSDevice12::OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i& scissor)
{
	GSTexture12* vkRt = static_cast<GSTexture12*>(rt);
	GSTexture12* vkDs = static_cast<GSTexture12*>(ds);

	if (m_current_render_target != vkRt || m_current_depth_target != vkDs)
	{
		// framebuffer change
		EndRenderPass();
	}
	else if (InRenderPass())
	{
		// Framebuffer unchanged, but check for clears. Have to restart render pass, unlike Vulkan.
		// We'll take care of issuing the actual clear there, because we have to start one anyway.
		if (vkRt && vkRt->GetState() != GSTexture::State::Dirty)
		{
			if (vkRt->GetState() == GSTexture::State::Cleared)
				EndRenderPass();
			else
				vkRt->SetState(GSTexture::State::Dirty);
		}
		if (vkDs && vkDs->GetState() != GSTexture::State::Dirty)
		{
			if (vkDs->GetState() == GSTexture::State::Cleared)
				EndRenderPass();
			else
				vkDs->SetState(GSTexture::State::Dirty);
		}
	}

	m_current_render_target = vkRt;
	m_current_depth_target = vkDs;

	if (!InRenderPass())
	{
		if (vkRt)
			vkRt->TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);
		if (vkDs)
			vkDs->TransitionToState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}

	// This is used to set/initialize the framebuffer for tfx rendering.
	const GSVector2i size = vkRt ? vkRt->GetSize() : vkDs->GetSize();
	const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(size.x), static_cast<float>(size.y), 0.0f, 1.0f};

	SetViewport(vp);
	SetScissor(scissor);
}

bool GSDevice12::GetSampler(D3D12DescriptorHandle* cpu_handle, GSHWDrawConfig::SamplerSelector ss)
{
	const auto it = m_samplers.find(ss.key);
	if (it != m_samplers.end())
	{
		*cpu_handle = it->second;
		return true;
	}

	D3D12_SAMPLER_DESC sd = {};
	const int anisotropy = GSConfig.MaxAnisotropy;
	if (anisotropy > 1 && ss.aniso)
	{
		sd.Filter = D3D12_FILTER_ANISOTROPIC;
	}
	else
	{
		static constexpr std::array<D3D12_FILTER, 8> filters = {{
			D3D12_FILTER_MIN_MAG_MIP_POINT, // 000 / min=point,mag=point,mip=point
			D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT, // 001 / min=linear,mag=point,mip=point
			D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT, // 010 / min=point,mag=linear,mip=point
			D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, // 011 / min=linear,mag=linear,mip=point
			D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR, // 100 / min=point,mag=point,mip=linear
			D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR, // 101 / min=linear,mag=point,mip=linear
			D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR, // 110 / min=point,mag=linear,mip=linear
			D3D12_FILTER_MIN_MAG_MIP_LINEAR, // 111 / min=linear,mag=linear,mip=linear
		}};

		const u8 index = (static_cast<u8>(ss.IsMipFilterLinear()) << 2) |
						 (static_cast<u8>(ss.IsMagFilterLinear()) << 1) |
						 static_cast<u8>(ss.IsMinFilterLinear());
		sd.Filter = filters[index];
	}

	sd.AddressU = ss.tau ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sd.AddressV = ss.tav ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sd.MinLOD = 0.0f;
	sd.MaxLOD = (ss.lodclamp || !ss.UseMipmapFiltering()) ? 0.25f : FLT_MAX;
	sd.MaxAnisotropy = pcsx2_clamp_i(GSConfig.MaxAnisotropy, 1, 16);
	sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;

	if (!GetSamplerHeapManager().Allocate(cpu_handle))
		return false;

	m_device.get()->CreateSampler(&sd, *cpu_handle);
	m_samplers.emplace(ss.key, *cpu_handle);
	return true;
}

void GSDevice12::ClearSamplerCache()
{
	ExecuteCommandList(false);
	for (const auto& it : m_samplers)
		m_sampler_heap_manager.Free(it.second.index);
	m_samplers.clear();
	InvalidateSamplerGroups();
	InitializeSamplers();

	m_utility_sampler_gpu = m_point_sampler_cpu;
	m_tfx_samplers_handle_gpu.Clear();
	m_dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS;
}

bool GSDevice12::GetTextureGroupDescriptors(D3D12DescriptorHandle* gpu_handle, const D3D12DescriptorHandle* cpu_handles, u32 count)
{
	if (!GetDescriptorAllocator().Allocate(count, gpu_handle))
		return false;

	if (count == 1)
	{
		m_device.get()->CopyDescriptorsSimple(1, *gpu_handle, cpu_handles[0], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		return true;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE dst_handle = *gpu_handle;
	D3D12_CPU_DESCRIPTOR_HANDLE src_handles[NUM_TFX_TEXTURES];
	UINT src_sizes[NUM_TFX_TEXTURES];
	for (u32 i = 0; i < count; i++)
	{
		src_handles[i] = cpu_handles[i];
		src_sizes[i] = 1;
	}
	m_device.get()->CopyDescriptors(1, &dst_handle, &count, count, src_handles, src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	return true;
}

static void AddUtilityVertexAttributes(D3D12::GraphicsPipelineBuilder& gpb)
{
	gpb.AddVertexAttribute("POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0);
	gpb.AddVertexAttribute("TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16);
	gpb.AddVertexAttribute("COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28);
	gpb.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
}

GSDevice12::ComPtr<ID3DBlob> GSDevice12::GetUtilityVertexShader(const std::string& source, const char* entry_point)
{
	ShaderMacro sm_model(m_shader_cache.GetFeatureLevel());
	return m_shader_cache.GetVertexShader(source, sm_model.GetPtr(), entry_point);
}

GSDevice12::ComPtr<ID3DBlob> GSDevice12::GetUtilityPixelShader(const std::string& source, const char* entry_point)
{
	ShaderMacro sm_model(m_shader_cache.GetFeatureLevel());
	return m_shader_cache.GetPixelShader(source, sm_model.GetPtr(), entry_point);
}

GSDevice12::ComPtr<ID3DBlob> GSDevice12::GetUtilityVertexShader(const char *source, size_t len, const char* entry_point)
{
	ShaderMacro sm_model(m_shader_cache.GetFeatureLevel());
	return m_shader_cache.GetVertexShader(source, len, sm_model.GetPtr(), entry_point);
}

GSDevice12::ComPtr<ID3DBlob> GSDevice12::GetUtilityPixelShader(const char *source, size_t len, const char* entry_point)
{
	ShaderMacro sm_model(m_shader_cache.GetFeatureLevel());
	return m_shader_cache.GetPixelShader(source, len, sm_model.GetPtr(), entry_point);
}

bool GSDevice12::CreateNullTexture()
{
	m_null_texture =
		GSTexture12::Create(GSTexture::Type::Texture, GSTexture::Format::Color, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
			DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN);
	if (!m_null_texture)
		return false;

	m_null_texture->TransitionToState(GetCommandList(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	return true;
}

bool GSDevice12::CreateBuffers()
{
	if (!m_vertex_stream_buffer.Create(VERTEX_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate vertex buffer\n");
		return false;
	}

	if (!m_index_stream_buffer.Create(INDEX_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate index buffer\n");
		return false;
	}

	if (!m_vertex_constant_buffer.Create(VERTEX_UNIFORM_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate vertex uniform buffer\n");
		return false;
	}

	if (!m_pixel_constant_buffer.Create(FRAGMENT_UNIFORM_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate fragment uniform buffer\n");
		return false;
	}

	if (!AllocatePreinitializedGPUBuffer(EXPAND_BUFFER_SIZE, &m_expand_index_buffer,
			&m_expand_index_buffer_alloc, &GSDevice::GenerateExpansionIndexBuffer))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate expansion index buffer\n");
		return false;
	}

	return true;
}

bool GSDevice12::CreateRootSignatures()
{
	D3D12::RootSignatureBuilder rsb;

	//////////////////////////////////////////////////////////////////////////
	// Convert Pipeline Layout
	//////////////////////////////////////////////////////////////////////////
	rsb.SetInputAssemblerFlag();
	rsb.Add32BitConstants(0, CONVERT_PUSH_CONSTANTS_SIZE / sizeof(u32), D3D12_SHADER_VISIBILITY_ALL);
	rsb.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, NUM_UTILITY_SAMPLERS, D3D12_SHADER_VISIBILITY_PIXEL);
	rsb.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, NUM_UTILITY_SAMPLERS, D3D12_SHADER_VISIBILITY_PIXEL);
	if (!(m_utility_root_signature = rsb.Create()))
		return false;

	//////////////////////////////////////////////////////////////////////////
	// Draw/TFX Pipeline Layout
	//////////////////////////////////////////////////////////////////////////
	rsb.SetInputAssemblerFlag();
	rsb.AddCBVParameter(0, D3D12_SHADER_VISIBILITY_ALL);
	rsb.AddCBVParameter(1, D3D12_SHADER_VISIBILITY_PIXEL);
	rsb.AddSRVParameter(0, D3D12_SHADER_VISIBILITY_VERTEX);
	rsb.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 2, D3D12_SHADER_VISIBILITY_PIXEL);
	rsb.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, NUM_TFX_SAMPLERS, D3D12_SHADER_VISIBILITY_PIXEL);
	rsb.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 2, D3D12_SHADER_VISIBILITY_PIXEL);
	if (!(m_tfx_root_signature = rsb.Create()))
		return false;
	return true;
}

/* The pixel shader of a utility pipeline: its source, and its entry
 * point (written to @entry when it has to be made). The vertex shader is
 * convert's vs_main for every one. */
const char* GSDevice12::UtilityPixelShader(u32 slot, const char** source, char* entry, size_t entry_size)
{
	*source = convert_fx_shader_raw;
	if (slot < UTIL_COLOR_COPY)
		return shaderName(static_cast<ShaderConvert>(slot - UTIL_CONVERT));
	if (slot < UTIL_HDR_SETUP)
		return shaderName((slot - UTIL_COLOR_COPY) < 16 ? ShaderConvert::COPY : ShaderConvert::RTA_CORRECTION);
	if (slot < UTIL_HDR_FINISH)
		return shaderName(ShaderConvert::COLCLIP_INIT);
	if (slot < UTIL_DATE_SETUP)
		return shaderName(ShaderConvert::COLCLIP_RESOLVE);
	if (slot < UTIL_INTERLACE)
	{
		snprintf(entry, entry_size, "ps_stencil_image_init_%u", (slot - UTIL_DATE_SETUP) % 4);
		return entry;
	}
	if (slot < UTIL_MERGE)
	{
		*source = interlace_fx_shader_raw;
		snprintf(entry, entry_size, "ps_main%u", slot - UTIL_INTERLACE);
		return entry;
	}
	*source = merge_fx_shader_raw;
	snprintf(entry, entry_size, "ps_main%u", slot - UTIL_MERGE);
	return entry;
}

/* The description of utility pipeline @slot. The convert family shared
 * one builder, each pipeline made from what the ones before it left set,
 * so its calls are made again in the same order up to @slot: the
 * description comes out byte for byte as it did, and with it the key
 * the driver's cached blob is found under. Every pixel shader set on the
 * way is @ps, which is the one in force when @slot's is made. Reads
 * nothing of the device's but @rs: the warm-up thread calls it too. */
bool GSDevice12::UtilityPipelineDesc(D3D12::GraphicsPipelineBuilder& gpb, u32 slot, ID3D12RootSignature* rs,
	const ID3DBlob* vs, const ID3DBlob* ps)
{
	if (slot >= UTIL_INTERLACE)
	{
		const bool merge = slot >= UTIL_MERGE;
		AddUtilityVertexAttributes(gpb);
		gpb.SetRootSignature(rs);
		gpb.SetNoCullRasterizationState();
		gpb.SetNoDepthTestState();
		if (!merge)
			gpb.SetNoBlendingState();
		gpb.SetRenderTarget(0, DXGI_FORMAT_R8G8B8A8_UNORM);
		gpb.SetVertexShader(vs);
		gpb.SetPixelShader(ps);
		if (merge)
			gpb.SetBlendState(0, true, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA,
				D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD);
		return slot < UTIL_COUNT;
	}

	gpb.SetRootSignature(rs);
	AddUtilityVertexAttributes(gpb);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetVertexShader(vs);

	for (ShaderConvert i = ShaderConvert::COPY; static_cast<int>(i) < static_cast<int>(ShaderConvert::Count);
		 i = static_cast<ShaderConvert>(static_cast<int>(i) + 1))
	{
		const bool depth = HasDepthOutput(i);
		const u32 index = static_cast<u32>(i);

		switch (i)
		{
			case ShaderConvert::RGBA8_TO_16_BITS:
			case ShaderConvert::FLOAT32_TO_16_BITS:
			{
				gpb.SetRenderTarget(0, DXGI_FORMAT_R16_UINT);
				gpb.SetDepthStencilFormat(DXGI_FORMAT_UNKNOWN);
			}
			break;
			case ShaderConvert::FLOAT32_TO_32_BITS:
			{
				gpb.SetRenderTarget(0, DXGI_FORMAT_R32_UINT);
				gpb.SetDepthStencilFormat(DXGI_FORMAT_UNKNOWN);
			}
			break;
			case ShaderConvert::DATM_0:
			case ShaderConvert::DATM_1:
			case ShaderConvert::DATM_0_RTA_CORRECTION:
			case ShaderConvert::DATM_1_RTA_CORRECTION:
			{
				gpb.ClearRenderTargets();
				gpb.SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
			}
			break;
			default:
			{
				depth ? gpb.ClearRenderTargets() : gpb.SetRenderTarget(0, DXGI_FORMAT_R8G8B8A8_UNORM);
				gpb.SetDepthStencilFormat(depth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_UNKNOWN);
			}
			break;
		}

		if (IsDATMConvertShader(i))
		{
			const D3D12_DEPTH_STENCILOP_DESC sos = {
				D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS};
			gpb.SetStencilState(true, 1, 1, sos, sos);
			gpb.SetDepthState(false, false, D3D12_COMPARISON_FUNC_ALWAYS);
		}
		else
		{
			gpb.SetDepthState(depth, depth, D3D12_COMPARISON_FUNC_ALWAYS);
			gpb.SetNoStencilState();
		}

		gpb.SetColorWriteMask(0, ShaderConvertWriteMask(i));
		gpb.SetPixelShader(ps);
		if (slot == UTIL_CONVERT + index)
			return true;

		if (i == ShaderConvert::COPY)
		{
			// color copy pipelines
			gpb.SetRenderTarget(0, DXGI_FORMAT_R8G8B8A8_UNORM);
			gpb.SetDepthStencilFormat(DXGI_FORMAT_UNKNOWN);
			for (u32 j = 0; j < 16; j++)
			{
				gpb.SetBlendState(0, false, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, static_cast<u8>(j));
				if (slot == UTIL_COLOR_COPY + j)
					return true;
			}
		}
		else if (i == ShaderConvert::RTA_CORRECTION)
		{
			// color copy pipelines
			gpb.SetRenderTarget(0, DXGI_FORMAT_R8G8B8A8_UNORM);
			gpb.SetDepthStencilFormat(DXGI_FORMAT_UNKNOWN);
			gpb.SetPixelShader(ps);
			for (u32 j = 16; j < 32; j++)
			{
				gpb.SetBlendState(0, false, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE,
					D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, static_cast<u8>(j - 16));
				if (slot == UTIL_COLOR_COPY + j)
					return true;
			}
		}
		else if (i == ShaderConvert::COLCLIP_INIT || i == ShaderConvert::COLCLIP_RESOLVE)
		{
			const bool is_setup = i == ShaderConvert::COLCLIP_INIT;
			for (u32 ds = 0; ds < 2; ds++)
			{
				gpb.SetRenderTarget(0, is_setup ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM);
				gpb.SetDepthStencilFormat(ds ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_UNKNOWN);
				if (slot == (is_setup ? UTIL_HDR_SETUP : UTIL_HDR_FINISH) + ds)
					return true;
			}
		}
	}

	for (u32 datm = 0; datm < 4; datm++)
	{
		gpb.SetRootSignature(rs);
		gpb.SetRenderTarget(0, DXGI_FORMAT_R32_FLOAT);
		gpb.SetPixelShader(ps);
		gpb.SetNoDepthTestState();
		gpb.SetNoStencilState();
		gpb.SetBlendState(0, false, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD,
			D3D12_BLEND_ZERO, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_COLOR_WRITE_ENABLE_RED);

		for (u32 ds = 0; ds < 2; ds++)
		{
			gpb.SetDepthStencilFormat(ds ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_UNKNOWN);
			if (slot == UTIL_DATE_SETUP + ds * 4 + datm)
				return true;
		}
	}
	return false;
}

/* A utility pipeline built here, through the shader cache. */
GSDevice12::ComPtr<ID3D12PipelineState> GSDevice12::CreateUtilityPipeline(u32 slot)
{
	char entry_buf[32];
	const char* source;
	const char* entry = UtilityPixelShader(slot, &source, entry_buf, sizeof(entry_buf));
	ComPtr<ID3DBlob> vs(GetUtilityVertexShader(convert_fx_shader_raw, strlen(convert_fx_shader_raw), "vs_main"));
	ComPtr<ID3DBlob> ps(GetUtilityPixelShader(source, strlen(source), entry));
	D3D12::GraphicsPipelineBuilder gpb;

	if (!vs || !ps || !UtilityPipelineDesc(gpb, slot, m_utility_root_signature.get(), vs.get(), ps.get()))
		return {};
	return gpb.Create(m_device.get(), m_shader_cache, false);
}

/* A utility pipeline at its first use: the warm-up thread's, else built
 * here once, as device creation built it. One that cannot be built
 * leaves the device lost, and the next device builds them all at its
 * creation, failing it as before; the draws asking meanwhile are
 * dropped (ApplyUtilityState). */
ID3D12PipelineState* GSDevice12::MissingUtilityPipeline(u32 slot)
{
	if (m_tfx_warm)
	{
		CollectWarmedTFXPipelines(nullptr, nullptr);
		if (m_utility[slot])
			return m_utility[slot].get();
	}
	if (m_utility_failed)
		return nullptr;
	m_utility[slot] = CreateUtilityPipeline(slot);
	if (!m_utility[slot])
	{
		log_cb(RETRO_LOG_ERROR, "GS: utility pipeline %u failed to build\n", slot);
		m_utility_failed       = true;
		s_utility_build_failed = true;
		m_device_lost          = true;
	}
	return m_utility[slot].get();
}

/* Whatever the runtime has queued since last time. The text is the
 * point: a failed Close says a command was invalid, this says which. */
void GSDevice12::DrainDebugMessages()
{
	UINT64 count;
	UINT64 i;

	if (!m_info_queue)
		return;

	count = m_info_queue->GetNumStoredMessages();
	for (i = 0; i < count; i++)
	{
		SIZE_T         length = 0;
		D3D12_MESSAGE *msg;

		if (FAILED(m_info_queue->GetMessage(i, NULL, &length)) || !length)
			continue;

		msg = (D3D12_MESSAGE*)malloc(length);
		if (!msg)
			continue;

		if (SUCCEEDED(m_info_queue->GetMessage(i, msg, &length)) && msg->pDescription)
			log_cb(RETRO_LOG_ERROR, "GS: D3D12 validation: %s\n", msg->pDescription);

		free(msg);
	}
	m_info_queue->ClearStoredMessages();
}

void GSDevice12::DestroyResources()
{
	StopTFXWarm();

	for (auto& it : m_tfx_pipelines)
		DeferObjectDestruction(it.second.get());
	m_tfx_pipelines.clear();
	m_last_tfx_pipeline = nullptr;
	m_tfx_pixel_shaders.clear();
	m_tfx_vertex_shaders.clear();
	m_present = {};
	m_utility = {};
	m_utility_failed = false;

	m_linear_sampler_cpu.Clear();
	m_point_sampler_cpu.Clear();

	for (auto& it : m_samplers)
		DeferDescriptorDestruction(GetSamplerHeapManager(), &it.second);
	DeferDescriptorDestruction(GetSamplerHeapManager(), &m_linear_sampler_cpu);
	DeferDescriptorDestruction(GetSamplerHeapManager(), &m_point_sampler_cpu);
	InvalidateSamplerGroups();

	m_expand_index_buffer.reset();
	gs_d3d12_heap_free(&m_heap, &m_expand_index_buffer_alloc);
	m_expand_index_buffer_alloc = gs_d3d12_alloc_t();
	m_texture_stream_buffer.Destroy(false);
	m_pixel_constant_buffer.Destroy(false);
	m_vertex_constant_buffer.Destroy(false);
	m_index_stream_buffer.Destroy(false);
	m_vertex_stream_buffer.Destroy(false);

	m_utility_root_signature.reset();
	m_tfx_root_signature.reset();

	if (m_null_texture)
	{
		m_null_texture->Destroy(false);
		m_null_texture.reset();
	}

	m_shader_cache.Close();

	m_descriptor_heap_manager.Free(&m_null_srv_descriptor);
	m_sampler_heap_manager.Destroy();
	m_dsv_heap_manager.Destroy();
	m_rtv_heap_manager.Destroy();
	m_descriptor_heap_manager.Destroy();
	m_command_lists = {};
	m_current_command_list = 0;
	m_completed_fence_value = 0;
	m_current_fence_value = 0;
	if (m_fence_event)
	{
		CloseHandle(m_fence_event);
		m_fence_event = {};
	}

	gs_d3d12_heap_shutdown(&m_heap);
	m_command_queue.reset();
	m_debug_interface.reset();
	m_device.reset();
}

/* The macros a draw shader is compiled with, from its selector: the same
 * for the drawing thread and the warm-up thread. Returns the entry point. */
static const char* TFXVertexShaderMacros(GSDevice12::ShaderMacro& sm, GSHWDrawConfig::VSSelector sel)
{
	sm.AddMacro("VERTEX_SHADER", 1);
	sm.AddMacro("VS_TME", sel.tme);
	sm.AddMacro("VS_FST", sel.fst);
	sm.AddMacro("VS_IIP", sel.iip);
	sm.AddMacro("VS_EXPAND", static_cast<int>(sel.expand));
	return (sel.expand != GSHWDrawConfig::VSExpand::None) ? "vs_main_expand" : "vs_main";
}

static void TFXPixelShaderMacros(GSDevice12::ShaderMacro& sm, const GSHWDrawConfig::PSSelector& sel)
{
	sm.AddMacro("PIXEL_SHADER", 1);
	sm.AddMacro("PS_FST", sel.fst);
	sm.AddMacro("PS_WMS", sel.wms);
	sm.AddMacro("PS_WMT", sel.wmt);
	sm.AddMacro("PS_ADJS", sel.adjs);
	sm.AddMacro("PS_ADJT", sel.adjt);
	sm.AddMacro("PS_AEM_FMT", sel.aem_fmt);
	sm.AddMacro("PS_AEM", sel.aem);
	sm.AddMacro("PS_TFX", sel.tfx);
	sm.AddMacro("PS_TCC", sel.tcc);
	sm.AddMacro("PS_DATE", sel.date);
	sm.AddMacro("PS_ATST", static_cast<u32>(sel.atst));
	sm.AddMacro("PS_AFAIL", static_cast<u32>(sel.afail));
	sm.AddMacro("PS_FOG", sel.fog);
	sm.AddMacro("PS_IIP", sel.iip);
	sm.AddMacro("PS_BLEND_HW", sel.blend_hw);
	sm.AddMacro("PS_A_MASKED", sel.a_masked);
	sm.AddMacro("PS_FBA", sel.fba);
	sm.AddMacro("PS_FBMASK", sel.fbmask);
	sm.AddMacro("PS_LTF", sel.ltf);
	sm.AddMacro("PS_TCOFFSETHACK", sel.tcoffsethack);
	sm.AddMacro("PS_POINT_SAMPLER", sel.point_sampler);
	sm.AddMacro("PS_REGION_RECT", sel.region_rect);
	sm.AddMacro("PS_SHUFFLE", sel.shuffle);
	sm.AddMacro("PS_SHUFFLE_SAME", sel.shuffle_same);
	sm.AddMacro("PS_PROCESS_BA", sel.process_ba);
	sm.AddMacro("PS_PROCESS_RG", sel.process_rg);
	sm.AddMacro("PS_SHUFFLE_ACROSS", sel.shuffle_across);
	sm.AddMacro("PS_READ16_SRC", sel.real16src);
	sm.AddMacro("PS_CHANNEL_FETCH", sel.channel);
	sm.AddMacro("PS_TALES_OF_ABYSS_HLE", sel.tales_of_abyss_hle);
	sm.AddMacro("PS_URBAN_CHAOS_HLE", sel.urban_chaos_hle);
	sm.AddMacro("PS_DST_FMT", sel.dst_fmt);
	sm.AddMacro("PS_DEPTH_FMT", sel.depth_fmt);
	sm.AddMacro("PS_PAL_FMT", sel.pal_fmt);
	sm.AddMacro("PS_COLCLIP_HW", sel.colclip_hw);
	sm.AddMacro("PS_RTA_CORRECTION", sel.rta_correction);
	sm.AddMacro("PS_RTA_SRC_CORRECTION", sel.rta_source_correction);
	sm.AddMacro("PS_COLCLIP", sel.colclip);
	sm.AddMacro("PS_BLEND_A", sel.blend_a);
	sm.AddMacro("PS_BLEND_B", sel.blend_b);
	sm.AddMacro("PS_BLEND_C", sel.blend_c);
	sm.AddMacro("PS_BLEND_D", sel.blend_d);
	sm.AddMacro("PS_BLEND_MIX", sel.blend_mix);
	sm.AddMacro("PS_ROUND_INV", sel.round_inv);
	sm.AddMacro("PS_FIXED_ONE_A", sel.fixed_one_a);
	sm.AddMacro("PS_PABE", sel.pabe);
	sm.AddMacro("PS_DITHER", sel.dither);
	sm.AddMacro("PS_DITHER_ADJUST", sel.dither_adjust);
	sm.AddMacro("PS_ZCLAMP", sel.zclamp);
	sm.AddMacro("PS_SCANMSK", sel.scanmsk);
	sm.AddMacro("PS_SAMPLE_MAP", sel.sample_map);
	sm.AddMacro("PS_NATIVE_TAPS", sel.native_taps);
	sm.AddMacro("PS_AUTOMATIC_LOD", sel.automatic_lod);
	sm.AddMacro("PS_MANUAL_LOD", sel.manual_lod);
	sm.AddMacro("PS_TEX_IS_FB", sel.tex_is_fb);
	sm.AddMacro("PS_NO_COLOR", sel.no_color);
	sm.AddMacro("PS_NO_COLOR1", sel.no_color1);
}

/* The pixel shader a pipeline is made with: without its second colour
 * output, and with blending off, when the colours are masked. */
static GSHWDrawConfig::PSSelector TFXEffectivePS(const GSDevice12::PipelineSelector& p, GSHWDrawConfig::BlendState* bs)
{
	GSHWDrawConfig::PSSelector pps{p.ps};
	*bs = p.bs;
	if (!p.bs.IsEffective(p.cms))
	{
		// disable blending when colours are masked
		*bs = {};
		pps.no_color1 = true;
	}
	return pps;
}

const ID3DBlob* GSDevice12::GetTFXVertexShader(GSHWDrawConfig::VSSelector sel)
{
	auto it = m_tfx_vertex_shaders.find(sel.key);
	if (it != m_tfx_vertex_shaders.end())
		return it->second.get();

	ShaderMacro sm(m_shader_cache.GetFeatureLevel());
	const char* entry_point = TFXVertexShaderMacros(sm, sel);
	ComPtr<ID3DBlob> vs(m_shader_cache.GetVertexShader(tfx_fx_shader_raw, strlen(tfx_fx_shader_raw), sm.GetPtr(), entry_point));
	it = m_tfx_vertex_shaders.emplace(sel.key, std::move(vs)).first;
	return it->second.get();
}

const ID3DBlob* GSDevice12::GetTFXPixelShader(const GSHWDrawConfig::PSSelector& sel)
{
	auto it = m_tfx_pixel_shaders.find(sel);
	if (it != m_tfx_pixel_shaders.end())
		return it->second.get();

	ShaderMacro sm(m_shader_cache.GetFeatureLevel());
	TFXPixelShaderMacros(sm, sel);
	ComPtr<ID3DBlob> ps(m_shader_cache.GetPixelShader(tfx_fx_shader_raw, strlen(tfx_fx_shader_raw), sm.GetPtr(), "ps_main"));
	it = m_tfx_pixel_shaders.emplace(sel, std::move(ps)).first;
	return it->second.get();
}

/* Everything of a draw pipeline but its shaders' building: the drawing
 * thread and the warm-up thread both. Reads only what the device set up
 * at its creation. */
void GSDevice12::FillTFXPipeline(D3D12::GraphicsPipelineBuilder& gpb, const PipelineSelector& p,
	const ID3DBlob* vs, const ID3DBlob* ps) const
{
	static constexpr std::array<D3D12_PRIMITIVE_TOPOLOGY_TYPE, 3> topology_lookup = {{
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, // Point
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, // Line
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, // Triangle
	}};

	GSHWDrawConfig::BlendState pbs;
	TFXEffectivePS(p, &pbs);

	// Common state
	gpb.SetRootSignature(m_tfx_root_signature.get());
	gpb.SetPrimitiveTopologyType(topology_lookup[p.topology]);
	gpb.SetRasterizationState(D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, false);
	if (p.rt)
	{
		const GSTexture::Format format = IsDATEModePrimIDInit(p.ps.date) ?
			GSTexture::Format::PrimID :
			(p.ps.colclip_hw ? GSTexture::Format::ColorClip : GSTexture::Format::Color);

		DXGI_FORMAT native_format;
		LookupNativeFormat(format, nullptr, nullptr, &native_format, nullptr);
		gpb.SetRenderTarget(0, native_format);
	}
	if (p.ds)
		gpb.SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT_S8X24_UINT);

	// Shaders
	gpb.SetVertexShader(vs);
	gpb.SetPixelShader(ps);

	// IA
	if (p.vs.expand == GSHWDrawConfig::VSExpand::None)
	{
		gpb.AddVertexAttribute("TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0);
		gpb.AddVertexAttribute("COLOR", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 8);
		gpb.AddVertexAttribute("TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 12);
		gpb.AddVertexAttribute("POSITION", 0, DXGI_FORMAT_R16G16_UINT, 0, 16);
		gpb.AddVertexAttribute("POSITION", 1, DXGI_FORMAT_R32_UINT, 0, 20);
		gpb.AddVertexAttribute("TEXCOORD", 2, DXGI_FORMAT_R16G16_UINT, 0, 24);
		gpb.AddVertexAttribute("COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28);
	}

	// DepthStencil
	if (p.ds)
	{
		static const D3D12_COMPARISON_FUNC ztst[] = {
			D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_GREATER};
		gpb.SetDepthState((p.dss.ztst != ZTST_ALWAYS || p.dss.zwe), p.dss.zwe, ztst[p.dss.ztst]);
		if (p.dss.date)
		{
			const D3D12_DEPTH_STENCILOP_DESC sos{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
				p.dss.date_one ? D3D12_STENCIL_OP_ZERO : D3D12_STENCIL_OP_KEEP,
				D3D12_COMPARISON_FUNC_EQUAL};
			gpb.SetStencilState(true, 1, 1, sos, sos);
		}
	}
	else
	{
		gpb.SetNoDepthTestState();
	}

	// Blending
	if (IsDATEModePrimIDInit(p.ps.date))
	{
		// image DATE prepass
		gpb.SetBlendState(0, true, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_MIN, D3D12_BLEND_ONE,
			D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD, D3D12_COLOR_WRITE_ENABLE_RED);
	}
	else if (pbs.enable)
	{
		// clang-format off
		static constexpr std::array<D3D12_BLEND, 16> d3d_blend_factors = { {
			D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR,
			D3D12_BLEND_SRC1_COLOR, D3D12_BLEND_INV_SRC1_COLOR, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA,
			D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_SRC1_ALPHA, D3D12_BLEND_INV_SRC1_ALPHA,
			D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_ONE, D3D12_BLEND_ZERO
		} };
		static constexpr std::array<D3D12_BLEND_OP, 3> d3d_blend_ops = { {
			D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT, D3D12_BLEND_OP_REV_SUBTRACT
		} };
		// clang-format on

		gpb.SetBlendState(0, true, d3d_blend_factors[pbs.src_factor], d3d_blend_factors[pbs.dst_factor],
			d3d_blend_ops[pbs.op], d3d_blend_factors[pbs.src_factor_alpha], d3d_blend_factors[pbs.dst_factor_alpha],
			D3D12_BLEND_OP_ADD, p.cms.wrgba);
	}
	else
	{
		gpb.SetBlendState(0, false, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
			D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, p.cms.wrgba);
	}

}

GSDevice12::ComPtr<ID3D12PipelineState> GSDevice12::CreateTFXPipeline(const PipelineSelector& p)
{
	GSHWDrawConfig::BlendState pbs;
	const GSHWDrawConfig::PSSelector pps(TFXEffectivePS(p, &pbs));
	const ID3DBlob* vs = GetTFXVertexShader(p.vs);
	const ID3DBlob* ps = GetTFXPixelShader(pps);
	if (!vs || !ps)
		return nullptr;

	D3D12::GraphicsPipelineBuilder gpb;
	FillTFXPipeline(gpb, p, vs, ps);
	ComPtr<ID3D12PipelineState> pipeline(gpb.Create(m_device.get(), m_shader_cache));
	return pipeline;
}

/* A pipeline no draw had yet: one the warm-up thread built, else built
 * here, and listed for the next start either way it was not listed. */
GSDevice12::TFXPipelineMap::iterator GSDevice12::AddMissingTFXPipeline(const PipelineSelector& p)
{
	TFXPipelineMap::iterator it;

	if (m_tfx_warm && CollectWarmedTFXPipelines(&p, &it))
		return it;

	ComPtr<ID3D12PipelineState> pipeline(CreateTFXPipeline(p));
	if (pipeline)
		gs_warm_list_add(&m_tfx_list, &p);
	return m_tfx_pipelines.emplace(p, std::move(pipeline)).first;
}

const ID3D12PipelineState* GSDevice12::GetTFXPipeline(const PipelineSelector& p)
{
	if (m_last_tfx_pipeline && m_last_tfx_pipeline_selector == p)
		return m_last_tfx_pipeline;

	TFXPipelineMap::iterator it = m_tfx_pipelines.find(p);
	if (it == m_tfx_pipelines.end())
		it = AddMissingTFXPipeline(p);

	m_last_tfx_pipeline_selector = p;
	m_last_tfx_pipeline          = it->second.get();
	return m_last_tfx_pipeline;
}

/* ---- draw pipelines built ahead ---- */

/* Bumped with the shaders, and whenever what a selector stands for
 * changes; the record size is checked as well. */
static constexpr u32 TFX_LIST_VERSION = (SHADER_CACHE_VERSION << 8) | 1;

bool GSDevice12::s_utility_build_failed = false;

bool GSDevice12::StartTFXWarm()
{
	const std::string path(m_shader_cache.GetPipelineListFileName());
	WarmJob* jobs;
	size_t   i, n;

	gs_warm_list_free(&m_tfx_list);
	m_tfx_list_loaded = 0;
	if (!path.empty() && !gs_warm_list_load(&m_tfx_list, path.c_str(), TFX_LIST_VERSION)
			&& path_is_valid(path.c_str()))
		log_cb(RETRO_LOG_WARN, "GS: ignoring the pipeline list '%s' (another version, or damaged)\n", path.c_str());
	m_tfx_list_loaded = m_tfx_list.count;

	n = UTIL_COUNT + m_tfx_list.count;
	if (!(jobs = (WarmJob*)calloc(n, sizeof(WarmJob))))
		return false;
	for (i = 0; i < UTIL_COUNT; i++)
		jobs[i].utility = (u32)i;
	for (i = 0; i < m_tfx_list.count; i++)
	{
		memcpy((void*)&jobs[UTIL_COUNT + i].p, m_tfx_list.recs + i * m_tfx_list.rec_size, sizeof(PipelineSelector));
		jobs[UTIL_COUNT + i].utility = UTIL_COUNT;
	}

	if (m_shader_cache.OpenSnapshot(&m_tfx_warm_cache))
		m_tfx_warm = gs_warm_start(jobs, sizeof(WarmJob), n, sizeof(TFXWarmResult), BuildWarmedTFXPipeline, this);
	else
		log_cb(RETRO_LOG_WARN, "GS: the shader cache could not be opened for the warm-up thread\n");
	free(jobs);
	if (!m_tfx_warm)
		D3D12ShaderCache::CloseSnapshot(&m_tfx_warm_cache);
	return m_tfx_warm != nullptr;
}

/* Takes what the warm-up thread finished into the map; a pipeline the
 * drawing thread built meanwhile keeps its own. True, and where it is,
 * when @want was among them. (The map is only searched by
 * GetTFXPipeline, which keeps its search inline.) */
bool GSDevice12::CollectWarmedTFXPipelines(const PipelineSelector* want, TFXPipelineMap::iterator* found)
{
	TFXWarmResult r;
	bool          hit = false;

	while (gs_warm_take(m_tfx_warm, &r))
	{
		if (r.utility < UTIL_COUNT)
		{
			/* a utility pipeline first used before the thread built it
			 * keeps the one built then */
			if (!m_utility[r.utility])
				*m_utility[r.utility].put() = r.pso;
			else
				r.pso->Release();
			r.pso = nullptr;
			KeepTFXWarmBlobs(r);
			continue;
		}
		std::pair<TFXPipelineMap::iterator, bool> ins(
			m_tfx_pipelines.emplace(r.p, ComPtr<ID3D12PipelineState>()));
		if (ins.second)
			*ins.first->second.put() = r.pso;
		else
			r.pso->Release();
		if (want && !hit && r.p == *want)
		{
			*found = ins.first;
			hit    = true;
		}
		r.pso = nullptr;
		KeepTFXWarmBlobs(r);
	}
	return hit;
}

/* What the warm-up thread built that the cache did not have, kept to be
 * added to it at teardown rather than written while drawing. */
void GSDevice12::KeepTFXWarmBlobs(const TFXWarmResult& r)
{
	if (!r.vs_blob && !r.ps_blob && !r.pso_blob)
		return;
	if (m_tfx_warm_blob_count == m_tfx_warm_blob_cap)
	{
		const size_t cap = m_tfx_warm_blob_cap ? m_tfx_warm_blob_cap * 2 : 64;
		TFXWarmResult* blobs = (TFXWarmResult*)realloc(m_tfx_warm_blobs, cap * sizeof(TFXWarmResult));
		if (!blobs)
		{
			if (r.vs_blob)
				r.vs_blob->Release();
			if (r.ps_blob)
				r.ps_blob->Release();
			if (r.pso_blob)
				r.pso_blob->Release();
			return;
		}
		m_tfx_warm_blobs    = blobs;
		m_tfx_warm_blob_cap = cap;
	}
	memcpy((void*)&m_tfx_warm_blobs[m_tfx_warm_blob_count++], (const void*)&r, sizeof(r));
}

/* Before anything the warm-up thread uses goes: the thread is joined, what
 * it finished is taken (and destroyed with the rest), its blobs go to the
 * cache, and the list is saved when this session added to it. */
void GSDevice12::StopTFXWarm()
{
	size_t i;

	if (m_tfx_warm)
	{
		gs_warm_stop(m_tfx_warm);
		CollectWarmedTFXPipelines(nullptr, nullptr);
		gs_warm_free(m_tfx_warm);
		m_tfx_warm = nullptr;
		D3D12ShaderCache::CloseSnapshot(&m_tfx_warm_cache);
	}

	for (i = 0; i < m_tfx_warm_blob_count; i++)
	{
		TFXWarmResult* r = &m_tfx_warm_blobs[i];
		if (r->vs_blob)
		{
			m_shader_cache.AddShaderBlobIfMissing(r->vs_key, r->vs_blob);
			r->vs_blob->Release();
		}
		if (r->ps_blob)
		{
			m_shader_cache.AddShaderBlobIfMissing(r->ps_key, r->ps_blob);
			r->ps_blob->Release();
		}
		if (r->pso_blob)
		{
			m_shader_cache.AddPipelineBlobIfMissing(r->pso_key, r->pso_blob);
			r->pso_blob->Release();
		}
	}
	free(m_tfx_warm_blobs);
	m_tfx_warm_blobs     = nullptr;
	m_tfx_warm_blob_count = 0;
	m_tfx_warm_blob_cap  = 0;

	if (m_tfx_list.count > m_tfx_list_loaded)
	{
		const std::string path(m_shader_cache.GetPipelineListFileName());
		gs_warm_list_unique(&m_tfx_list);
		if (!path.empty())
			gs_warm_list_save(&m_tfx_list, path.c_str(), TFX_LIST_VERSION);
	}
	gs_warm_list_free(&m_tfx_list);
	m_tfx_list_loaded = 0;
}

/* The warm-up thread: one listed pipeline, its shaders from the cache as
 * it stood at the start or compiled, its pipeline from the driver's
 * cached blob when that still holds, else built whole. */
/* The warm-up thread: a shader from the cache as it stood when the thread
 * started, else compiled - and then handed back in @built, for the cache. */
GSDevice12::ComPtr<ID3DBlob> GSDevice12::WarmShader(GSDevice12* self, D3D12ShaderCache::EntryType type,
	const char* source, ShaderMacro& sm, const char* entry, D3D12ShaderCache::CacheIndexKey* key, ID3DBlob** built)
{
	const size_t len = strlen(source);
	ComPtr<ID3DBlob> blob;

	*key = D3D12ShaderCache::GetShaderCacheKey(type, source, len, sm.GetPtr(), entry);
	if ((blob = D3D12ShaderCache::ReadSnapshot(&self->m_tfx_warm_cache, *key)))
		return blob;
	blob = D3D::CompileShader(type == D3D12ShaderCache::EntryType::VertexShader ? D3D::ShaderType::Vertex : D3D::ShaderType::Pixel,
		self->m_shader_cache.GetFeatureLevel(), self->m_shader_cache.IsDebug(),
		std::string_view(source, len), sm.GetPtr(), entry);
	if (blob)
	{
		*built = blob.get();
		(*built)->AddRef();
	}
	return blob;
}

/* The warm-up thread: a pipeline from the driver's cached blob when that
 * still holds, else built whole - and its blob handed back in @built. */
GSDevice12::ComPtr<ID3D12PipelineState> GSDevice12::WarmPipeline(GSDevice12* self,
	D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, D3D12ShaderCache::CacheIndexKey* key, ID3DBlob** built)
{
	ComPtr<ID3DBlob> cached;
	ComPtr<ID3D12PipelineState> pso;

	*key = D3D12ShaderCache::GetPipelineCacheKey(desc);
	if ((cached = D3D12ShaderCache::ReadSnapshot(&self->m_tfx_warm_cache, *key)))
	{
		desc.CachedPSO.pCachedBlob           = cached->GetBufferPointer();
		desc.CachedPSO.CachedBlobSizeInBytes = cached->GetBufferSize();
		if (FAILED(self->m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso.put()))))
			pso.reset();
		desc.CachedPSO.pCachedBlob           = nullptr;
		desc.CachedPSO.CachedBlobSizeInBytes = 0;
	}
	if (!pso)
	{
		ComPtr<ID3DBlob> blob;
		if (FAILED(self->m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso.put()))))
			return {};
		if (SUCCEEDED(pso->GetCachedBlob(blob.put())))
			*built = blob.detach();
	}
	return pso;
}

int GSDevice12::BuildWarmedUtilityPipeline(GSDevice12* self, u32 slot, TFXWarmResult* r)
{
	char entry_buf[32];
	const char* source;
	const char* entry = UtilityPixelShader(slot, &source, entry_buf, sizeof(entry_buf));
	ShaderMacro sm(self->m_shader_cache.GetFeatureLevel());
	D3D12::GraphicsPipelineBuilder gpb;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	ComPtr<ID3DBlob> vs, ps;
	ComPtr<ID3D12PipelineState> pso;

	vs = WarmShader(self, D3D12ShaderCache::EntryType::VertexShader, convert_fx_shader_raw, sm, "vs_main", &r->vs_key, &r->vs_blob);
	ps = WarmShader(self, D3D12ShaderCache::EntryType::PixelShader, source, sm, entry, &r->ps_key, &r->ps_blob);
	if (vs && ps && UtilityPipelineDesc(gpb, slot, self->m_utility_root_signature.get(), vs.get(), ps.get()))
	{
		desc = gpb.GetDesc();
		pso  = WarmPipeline(self, desc, &r->pso_key, &r->pso_blob);
	}
	if (!pso)
		return 0;
	r->pso = pso.detach();
	return 1;
}

/* The warm-up thread: one job - a utility pipeline, or a listed draw
 * pipeline with its shaders. */
int GSDevice12::BuildWarmedTFXPipeline(void* ud, const void* job, void* result)
{
	GSDevice12* self           = static_cast<GSDevice12*>(ud);
	const WarmJob* j           = static_cast<const WarmJob*>(job);
	const PipelineSelector* p  = &j->p;
	TFXWarmResult* r           = static_cast<TFXWarmResult*>(result);
	const D3D_FEATURE_LEVEL fl = self->m_shader_cache.GetFeatureLevel();
	GSHWDrawConfig::BlendState pbs;
	GSHWDrawConfig::PSSelector pps;
	D3D12::GraphicsPipelineBuilder gpb;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	ComPtr<ID3DBlob> vs, ps;
	ComPtr<ID3D12PipelineState> pso;
	int built = 0;

	memset((void*)r, 0, sizeof(*r));
	r->utility = j->utility;
	if (j->utility < UTIL_COUNT)
		built = BuildWarmedUtilityPipeline(self, j->utility, r);
	else
	{
		memcpy((void*)&r->p, (const void*)p, sizeof(*p));
		/* (a selector no draw could make: the tables it indexes end there) */
		if (p->topology > 2 || p->bs.op > 2)
			return 0;
		pps = TFXEffectivePS(*p, &pbs);
		{
			ShaderMacro sm(fl);
			const char* entry = TFXVertexShaderMacros(sm, p->vs);
			vs = WarmShader(self, D3D12ShaderCache::EntryType::VertexShader, tfx_fx_shader_raw, sm, entry, &r->vs_key, &r->vs_blob);
		}
		if (vs)
		{
			ShaderMacro sm(fl);
			TFXPixelShaderMacros(sm, pps);
			ps = WarmShader(self, D3D12ShaderCache::EntryType::PixelShader, tfx_fx_shader_raw, sm, "ps_main", &r->ps_key, &r->ps_blob);
		}
		if (vs && ps)
		{
			self->FillTFXPipeline(gpb, *p, vs.get(), ps.get());
			desc = gpb.GetDesc();
			if ((pso = WarmPipeline(self, desc, &r->pso_key, &r->pso_blob)))
			{
				r->pso = pso.detach();
				built  = 1;
			}
		}
	}
	if (!built)
	{
		if (r->vs_blob)
			r->vs_blob->Release();
		if (r->ps_blob)
			r->ps_blob->Release();
		if (r->pso_blob)
			r->pso_blob->Release();
	}
	return built;
}

bool GSDevice12::BindDrawPipeline(const PipelineSelector& p)
{
	const ID3D12PipelineState* pipeline = GetTFXPipeline(p);
	if (!pipeline)
		return false;

	SetPipeline(pipeline);

	return ApplyTFXState();
}

void GSDevice12::InitializeState()
{
	for (u32 i = 0; i < NUM_TOTAL_TFX_TEXTURES; i++)
		m_tfx_textures[i] = m_null_texture->GetSRVDescriptor();
	m_tfx_sampler_sel = GSHWDrawConfig::SamplerSelector::Point().key;

	InvalidateCachedState();
}

void GSDevice12::InitializeSamplers()
{
	bool result = GetSampler(&m_point_sampler_cpu, GSHWDrawConfig::SamplerSelector::Point());
	result = result && GetSampler(&m_linear_sampler_cpu, GSHWDrawConfig::SamplerSelector::Linear());
	result = result && GetSampler(&m_tfx_sampler, m_tfx_sampler_sel);

	if (!result)
		log_cb(RETRO_LOG_ERROR, "Failed to initialize samplers\n");
}

void GSDevice12::ExecuteCommandList(bool wait_for_completion)
{
	EndRenderPass();
	ContextExecuteCommandList(wait_for_completion);
	InvalidateCachedState();
}

void GSDevice12::ExecuteCommandListAndRestartRenderPass(bool wait_for_completion)
{
	const bool was_in_render_pass = InRenderPass();
	EndRenderPass();
	ContextExecuteCommandList(wait_for_completion);
	InvalidateCachedState();

	if (was_in_render_pass)
	{
		// rebind everything except RT, because the RP does that for us
		ApplyBaseState(m_dirty_flags & ~DIRTY_FLAG_RENDER_TARGET, GetCommandList());
		m_dirty_flags &= ~DIRTY_BASE_STATE;

		// restart render pass
		BeginRenderPass(
			m_current_render_target ? D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
			m_current_render_target ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			m_current_depth_target ? D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
			m_current_depth_target ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS);
	}
}

void GSDevice12::ExecuteCommandListForReadback()
{
	ExecuteCommandList(true);
}

void GSDevice12::InvalidateCachedState()
{
	m_dirty_flags |= DIRTY_BASE_STATE | DIRTY_TFX_STATE | DIRTY_UTILITY_STATE | DIRTY_CONSTANT_BUFFER_STATE;
	m_current_root_signature = RootSignature::Undefined;
	m_utility_texture_cpu.Clear();
	m_utility_texture_gpu.Clear();
	m_utility_sampler_cpu.Clear();
	m_utility_sampler_gpu.Clear();
	m_tfx_textures_handle_gpu.Clear();
	m_tfx_samplers_handle_gpu.Clear();
	m_tfx_rt_textures_handle_gpu.Clear();
}

void GSDevice12::SetVertexBuffer(D3D12_GPU_VIRTUAL_ADDRESS buffer, size_t size, size_t stride)
{
	if (m_vertex_buffer.BufferLocation == buffer && m_vertex_buffer.SizeInBytes == size && m_vertex_buffer.StrideInBytes == stride)
		return;

	m_vertex_buffer.BufferLocation = buffer;
	m_vertex_buffer.SizeInBytes = size;
	m_vertex_buffer.StrideInBytes = stride;
	m_dirty_flags |= DIRTY_FLAG_VERTEX_BUFFER;
}

void GSDevice12::SetIndexBuffer(D3D12_GPU_VIRTUAL_ADDRESS buffer, size_t size, DXGI_FORMAT type)
{
	if (m_index_buffer.BufferLocation == buffer && m_index_buffer.SizeInBytes == size && m_index_buffer.Format == type)
		return;

	m_index_buffer.BufferLocation = buffer;
	m_index_buffer.SizeInBytes = size;
	m_index_buffer.Format = type;
	m_dirty_flags |= DIRTY_FLAG_INDEX_BUFFER;
}

void GSDevice12::SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology)
{
	if (m_primitive_topology == topology)
		return;

	m_primitive_topology = topology;
	m_dirty_flags |= DIRTY_FLAG_PRIMITIVE_TOPOLOGY;
}

void GSDevice12::SetBlendConstants(u8 color)
{
	if (m_blend_constant_color == color)
		return;

	m_blend_constant_color = color;
	m_dirty_flags |= DIRTY_FLAG_BLEND_CONSTANTS;
}

void GSDevice12::SetStencilRef(u8 ref)
{
	if (m_stencil_ref == ref)
		return;

	m_stencil_ref = ref;
	m_dirty_flags |= DIRTY_FLAG_STENCIL_REF;
}

void GSDevice12::PSSetShaderResource(int i, GSTexture* sr, bool check_state)
{
	D3D12DescriptorHandle handle;
	if (sr)
	{
		GSTexture12* dtex = static_cast<GSTexture12*>(sr);
		if (check_state)
		{
			/* Ending render pass due to resource transition */
			if (dtex->GetResourceState() != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE && InRenderPass())
				EndRenderPass();

			dtex->CommitClear();
			dtex->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		dtex->SetUseFenceCounter(GetCurrentFenceValue());
		handle = dtex->GetSRVDescriptor();
	}
	else
	{
		handle = m_null_texture->GetSRVDescriptor();
	}

	if (m_tfx_textures[i] == handle)
		return;

	m_tfx_textures[i] = handle;
	m_dirty_flags |= (i < 2) ? DIRTY_FLAG_TFX_TEXTURES : DIRTY_FLAG_TFX_RT_TEXTURES;
}

void GSDevice12::PSSetSampler(GSHWDrawConfig::SamplerSelector sel)
{
	if (m_tfx_sampler_sel == sel.key)
		return;

	GetSampler(&m_tfx_sampler, sel);
	m_tfx_sampler_sel = sel.key;
	m_dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS;
}

void GSDevice12::SetUtilityRootSignature()
{
	if (m_current_root_signature == RootSignature::Utility)
		return;

	m_current_root_signature = RootSignature::Utility;
	m_dirty_flags |= DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE | DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE | DIRTY_FLAG_PIPELINE;
	GetCommandList()->SetGraphicsRootSignature(m_utility_root_signature.get());
}

void GSDevice12::SetUtilityTexture(GSTexture* dtex, const D3D12DescriptorHandle& sampler)
{
	D3D12DescriptorHandle handle;
	if (dtex)
	{
		GSTexture12* d12tex = static_cast<GSTexture12*>(dtex);
		d12tex->CommitClear();
		d12tex->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		d12tex->SetUseFenceCounter(GetCurrentFenceValue());
		handle = d12tex->GetSRVDescriptor();
	}
	else
	{
		handle = m_null_texture->GetSRVDescriptor();
	}

	if (m_utility_texture_cpu != handle)
	{
		m_utility_texture_cpu = handle;
		m_dirty_flags |= DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE;

		if (!GetTextureGroupDescriptors(&m_utility_texture_gpu, &handle, 1))
		{
			ExecuteCommandListAndRestartRenderPass(false);
			SetUtilityTexture(dtex, sampler);
			return;
		}
	}

	if (m_utility_sampler_cpu != sampler)
	{
		m_utility_sampler_cpu = sampler;
		m_dirty_flags |= DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE;

		if (!GetSamplerAllocator().LookupSingle(&m_utility_sampler_gpu, sampler))
		{
			ExecuteCommandListAndRestartRenderPass(false);
			SetUtilityTexture(dtex, sampler);
			return;
		}
	}
}

void GSDevice12::SetUtilityPushConstants(const void* data, u32 size)
{
	GetCommandList()->SetGraphicsRoot32BitConstants(UTILITY_ROOT_SIGNATURE_PARAM_PUSH_CONSTANTS, (size + 3) / sizeof(u32), data, 0);
}

void GSDevice12::UnbindTexture(GSTexture12* tex)
{
	for (u32 i = 0; i < NUM_TOTAL_TFX_TEXTURES; i++)
	{
		if (m_tfx_textures[i] == tex->GetSRVDescriptor())
		{
			m_tfx_textures[i] = m_null_texture->GetSRVDescriptor();
			m_dirty_flags    |= DIRTY_FLAG_TFX_TEXTURES;
		}
	}
	if (m_current_render_target == tex)
	{
		EndRenderPass();
		m_current_render_target = nullptr;
	}
	if (m_current_depth_target == tex)
	{
		EndRenderPass();
		m_current_depth_target = nullptr;
	}
}

void GSDevice12::RenderTextureMipmap(GSTexture12* texture,
	u32 dst_level, u32 dst_width, u32 dst_height, u32 src_level, u32 src_width, u32 src_height)
{
	EndRenderPass();

	// we need a temporary SRV and RTV for each mip level
	// Safe to use the init buffer after exec, because everything will be done with the texture.
	D3D12DescriptorHandle rtv_handle;
	while (!GetRTVHeapManager().Allocate(&rtv_handle))
		ExecuteCommandList(false);

	D3D12DescriptorHandle srv_handle;
	while (!GetDescriptorHeapManager().Allocate(&srv_handle))
		ExecuteCommandList(false);

	// Setup views. This will be a partial view for the SRV.
	D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {texture->GetDXGIFormat(), D3D12_RTV_DIMENSION_TEXTURE2D};
	rtv_desc.Texture2D = {dst_level, 0u};
	m_device.get()->CreateRenderTargetView(texture->GetResource(), &rtv_desc, rtv_handle);

	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {texture->GetDXGIFormat(), D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
	srv_desc.Texture2D = {src_level, 1u, 0u, 0.0f};
	m_device.get()->CreateShaderResourceView(texture->GetResource(), &srv_desc, srv_handle);

	// We need to set the descriptors up manually, because we're not going through GSTexture.
	if (!GetTextureGroupDescriptors(&m_utility_texture_gpu, &srv_handle, 1))
		ExecuteCommandList(false);
	if (m_utility_sampler_cpu != m_linear_sampler_cpu)
	{
		m_dirty_flags |= DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE;
		if (!GetSamplerAllocator().LookupSingle(&m_utility_sampler_gpu, m_linear_sampler_cpu))
			ExecuteCommandList(false);
	}

	// *now* we don't have to worry about running out of anything.
	ID3D12GraphicsCommandList* cmdlist = GetCommandList();
	if (texture->GetResourceState() != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
		texture->TransitionSubresourceToState(cmdlist, src_level, texture->GetResourceState(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	if (texture->GetResourceState() != D3D12_RESOURCE_STATE_RENDER_TARGET)
		texture->TransitionSubresourceToState(cmdlist, dst_level, texture->GetResourceState(), D3D12_RESOURCE_STATE_RENDER_TARGET);

	// We set the state directly here.
	constexpr u32 MODIFIED_STATE = DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR | DIRTY_FLAG_RENDER_TARGET;
	m_dirty_flags &= ~MODIFIED_STATE;

	// Using a render pass is probably a bit overkill.
	const D3D12_DISCARD_REGION discard_region = {0u, nullptr, dst_level, 1u};
	cmdlist->DiscardResource(texture->GetResource(), &discard_region);
	cmdlist->OMSetRenderTargets(1, &rtv_handle.cpu_handle, FALSE, nullptr);

	const D3D12_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(dst_width), static_cast<float>(dst_height), 0.0f, 1.0f};
	cmdlist->RSSetViewports(1, &vp);

	const D3D12_RECT scissor = {0, 0, static_cast<LONG>(dst_width), static_cast<LONG>(dst_height)};
	cmdlist->RSSetScissorRects(1, &scissor);

	SetUtilityRootSignature();
	SetPipeline(UtilityPipeline(UTIL_CONVERT + static_cast<int>(ShaderConvert::COPY)));
	DrawStretchRect(GSVector4(0.0f, 0.0f, 1.0f, 1.0f),
		GSVector4(0.0f, 0.0f, static_cast<float>(dst_width), static_cast<float>(dst_height)),
		GSVector2i(dst_width, dst_height));

	if (texture->GetResourceState() != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
		texture->TransitionSubresourceToState(cmdlist, src_level, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, texture->GetResourceState());
	if (texture->GetResourceState() != D3D12_RESOURCE_STATE_RENDER_TARGET)
		texture->TransitionSubresourceToState(cmdlist, dst_level, D3D12_RESOURCE_STATE_RENDER_TARGET, texture->GetResourceState());

	// Must destroy after current cmdlist.
	DeferDescriptorDestruction(GetDescriptorHeapManager(), &srv_handle);
	DeferDescriptorDestruction(GetRTVHeapManager(), &rtv_handle);

	// Restore for next normal draw.
	m_dirty_flags |= MODIFIED_STATE;
}

bool GSDevice12::InRenderPass()
{
	return m_in_render_pass;
}

void GSDevice12::BeginRenderPass(
	D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE color_begin, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE color_end,
	D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE depth_begin, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE depth_end,
	D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE stencil_begin,
	D3D12_RENDER_PASS_ENDING_ACCESS_TYPE stencil_end, GSVector4 clear_color, float clear_depth, u8 clear_stencil)
{
	if (m_in_render_pass)
		EndRenderPass();

	// we're setting the RT here.
	m_dirty_flags &= ~DIRTY_FLAG_RENDER_TARGET;
	m_in_render_pass = true;

	D3D12_RENDER_PASS_RENDER_TARGET_DESC rt = {};
	if (m_current_render_target)
	{
		rt.cpuDescriptor = m_current_render_target->GetWriteDescriptor();
		rt.EndingAccess.Type = color_end;
		rt.BeginningAccess.Type = color_begin;
		if (color_begin == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
		{
			LookupNativeFormat(m_current_render_target->GetFormat(), nullptr, &rt.BeginningAccess.Clear.ClearValue.Format, nullptr, nullptr);
			GSVector4::store<false>(rt.BeginningAccess.Clear.ClearValue.Color, clear_color);
		}
	}

	D3D12_RENDER_PASS_DEPTH_STENCIL_DESC ds = {};
	if (m_current_depth_target)
	{
		ds.cpuDescriptor = m_current_depth_target->GetWriteDescriptor();
		ds.DepthEndingAccess.Type = depth_end;
		ds.DepthBeginningAccess.Type = depth_begin;
		if (depth_begin == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
		{
			LookupNativeFormat(m_current_depth_target->GetFormat(), nullptr, nullptr, nullptr, &ds.DepthBeginningAccess.Clear.ClearValue.Format);
			ds.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth = clear_depth;
		}
		ds.StencilEndingAccess.Type = stencil_end;
		ds.StencilBeginningAccess.Type = stencil_begin;
		if (stencil_begin == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
		{
			LookupNativeFormat(m_current_depth_target->GetFormat(), nullptr, nullptr, nullptr, &ds.StencilBeginningAccess.Clear.ClearValue.Format);
			ds.StencilBeginningAccess.Clear.ClearValue.DepthStencil.Stencil = clear_stencil;
		}
	}

	GetCommandList()->BeginRenderPass(
		m_current_render_target ? 1 : 0, m_current_render_target ? &rt : nullptr,
		m_current_depth_target ? &ds : nullptr, D3D12_RENDER_PASS_FLAG_NONE);
}

void GSDevice12::EndRenderPass()
{
	if (!m_in_render_pass)
		return;

	m_in_render_pass = false;
	// to render again, we need to reset OM
	m_dirty_flags   |= DIRTY_FLAG_RENDER_TARGET;
	GetCommandList()->EndRenderPass();

}

void GSDevice12::SetViewport(const D3D12_VIEWPORT& viewport)
{
	if (memcmp(&viewport, &m_viewport, sizeof(m_viewport)) == 0)
		return;

	memcpy(&m_viewport, &viewport, sizeof(m_viewport));
	m_dirty_flags |= DIRTY_FLAG_VIEWPORT;
}

void GSDevice12::SetScissor(const GSVector4i& scissor)
{
	if (m_scissor.eq(scissor))
		return;

	m_scissor = scissor;
	m_dirty_flags |= DIRTY_FLAG_SCISSOR;
}

void GSDevice12::SetPipeline(const ID3D12PipelineState* pipeline)
{
	if (m_current_pipeline == pipeline)
		return;

	m_current_pipeline = pipeline;
	m_dirty_flags |= DIRTY_FLAG_PIPELINE;
}

__ri void GSDevice12::ApplyBaseState(u32 flags, ID3D12GraphicsCommandList* cmdlist)
{
	if (flags & DIRTY_FLAG_VERTEX_BUFFER)
		cmdlist->IASetVertexBuffers(0, 1, &m_vertex_buffer);

	if (flags & DIRTY_FLAG_INDEX_BUFFER)
		cmdlist->IASetIndexBuffer(&m_index_buffer);

	if (flags & DIRTY_FLAG_PRIMITIVE_TOPOLOGY)
		cmdlist->IASetPrimitiveTopology(m_primitive_topology);

	if (flags & DIRTY_FLAG_PIPELINE)
		cmdlist->SetPipelineState(const_cast<ID3D12PipelineState*>(m_current_pipeline));

	if (flags & DIRTY_FLAG_VIEWPORT)
		cmdlist->RSSetViewports(1, &m_viewport);

	if (flags & DIRTY_FLAG_SCISSOR)
	{
		const D3D12_RECT rc{m_scissor.x, m_scissor.y, m_scissor.z, m_scissor.w};
		cmdlist->RSSetScissorRects(1, &rc);
	}

	if (flags & DIRTY_FLAG_BLEND_CONSTANTS)
	{
		const GSVector4 col(static_cast<float>(m_blend_constant_color) / 128.0f);
		cmdlist->OMSetBlendFactor(col.v);
	}

	if (flags & DIRTY_FLAG_STENCIL_REF)
		cmdlist->OMSetStencilRef(m_stencil_ref);

	if (flags & DIRTY_FLAG_RENDER_TARGET)
	{
		if (m_current_render_target)
		{
			cmdlist->OMSetRenderTargets(1, &m_current_render_target->GetWriteDescriptor().cpu_handle, FALSE,
				m_current_depth_target ? &m_current_depth_target->GetWriteDescriptor().cpu_handle : nullptr);
		}
		else if (m_current_depth_target)
		{
			cmdlist->OMSetRenderTargets(0, nullptr, FALSE, &m_current_depth_target->GetWriteDescriptor().cpu_handle);
		}
	}
}

bool GSDevice12::ApplyTFXState(bool already_execed)
{
	if (m_current_root_signature == RootSignature::TFX && m_dirty_flags == 0)
		return true;

	u32 flags = m_dirty_flags;
	m_dirty_flags &= ~(DIRTY_TFX_STATE | DIRTY_CONSTANT_BUFFER_STATE);

	// do cbuffer first, because it's the most likely to cause an exec
	if (flags & DIRTY_FLAG_VS_CONSTANT_BUFFER)
	{
		if (!m_vertex_constant_buffer.ReserveMemory(
				sizeof(m_vs_cb_cache), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT))
		{
			if (already_execed)
			{
				log_cb(RETRO_LOG_ERROR, "Failed to reserve vertex uniform space\n");
				return false;
			}

			ExecuteCommandListAndRestartRenderPass(false);
			return ApplyTFXState(true);
		}

		memcpy(m_vertex_constant_buffer.GetCurrentHostPointer(), &m_vs_cb_cache, sizeof(m_vs_cb_cache));
		m_tfx_constant_buffers[0] = m_vertex_constant_buffer.GetCurrentGPUPointer();
		m_vertex_constant_buffer.CommitMemory(sizeof(m_vs_cb_cache));
		flags |= DIRTY_FLAG_VS_CONSTANT_BUFFER_BINDING;
	}

	if (flags & DIRTY_FLAG_PS_CONSTANT_BUFFER)
	{
		if (!m_pixel_constant_buffer.ReserveMemory(
				sizeof(m_ps_cb_cache), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT))
		{
			if (already_execed)
			{
				log_cb(RETRO_LOG_ERROR, "Failed to reserve pixel uniform space\n");
				return false;
			}

			ExecuteCommandListAndRestartRenderPass(false);
			return ApplyTFXState(true);
		}

		memcpy(m_pixel_constant_buffer.GetCurrentHostPointer(), &m_ps_cb_cache, sizeof(m_ps_cb_cache));
		m_tfx_constant_buffers[1] = m_pixel_constant_buffer.GetCurrentGPUPointer();
		m_pixel_constant_buffer.CommitMemory(sizeof(m_ps_cb_cache));
		flags |= DIRTY_FLAG_PS_CONSTANT_BUFFER_BINDING;
	}

	if (flags & DIRTY_FLAG_TFX_SAMPLERS)
	{
		if (!GetSamplerAllocator().LookupSingle(&m_tfx_samplers_handle_gpu, m_tfx_sampler))
		{
			ExecuteCommandListAndRestartRenderPass(false);
			return ApplyTFXState(true);
		}

		flags |= DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE;
	}

	if (flags & DIRTY_FLAG_TFX_TEXTURES)
	{
		if (!GetTextureGroupDescriptors(&m_tfx_textures_handle_gpu, m_tfx_textures.data(), 2))
		{
			ExecuteCommandListAndRestartRenderPass(false);
			return ApplyTFXState(true);
		}

		flags |= DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE;
	}

	if (flags & DIRTY_FLAG_TFX_RT_TEXTURES)
	{
		if (!GetTextureGroupDescriptors(&m_tfx_rt_textures_handle_gpu, m_tfx_textures.data() + 2, 2))
		{
			ExecuteCommandListAndRestartRenderPass(false);
			return ApplyTFXState(true);
		}

		flags |= DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE_2;
	}

	ID3D12GraphicsCommandList* cmdlist = GetCommandList();

	if (m_current_root_signature != RootSignature::TFX)
	{
		m_current_root_signature = RootSignature::TFX;
		flags |= DIRTY_FLAG_VS_CONSTANT_BUFFER_BINDING | DIRTY_FLAG_PS_CONSTANT_BUFFER_BINDING |
				 DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE | DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE | DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE_2 |
				 DIRTY_FLAG_PIPELINE;
		cmdlist->SetGraphicsRootSignature(m_tfx_root_signature.get());
	}

	if (flags & DIRTY_FLAG_VS_CONSTANT_BUFFER_BINDING)
		cmdlist->SetGraphicsRootConstantBufferView(TFX_ROOT_SIGNATURE_PARAM_VS_CBV, m_tfx_constant_buffers[0]);
	if (flags & DIRTY_FLAG_PS_CONSTANT_BUFFER_BINDING)
		cmdlist->SetGraphicsRootConstantBufferView(TFX_ROOT_SIGNATURE_PARAM_PS_CBV, m_tfx_constant_buffers[1]);
	if (flags & DIRTY_FLAG_VS_VERTEX_BUFFER_BINDING)
	{
		cmdlist->SetGraphicsRootShaderResourceView(TFX_ROOT_SIGNATURE_PARAM_VS_SRV,
			m_vertex_stream_buffer.GetGPUPointer() + m_vertex.start * sizeof(GSVertex));
	}
	if (flags & DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE)
		cmdlist->SetGraphicsRootDescriptorTable(TFX_ROOT_SIGNATURE_PARAM_PS_TEXTURES, m_tfx_textures_handle_gpu);
	if (flags & DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE)
		cmdlist->SetGraphicsRootDescriptorTable(TFX_ROOT_SIGNATURE_PARAM_PS_SAMPLERS, m_tfx_samplers_handle_gpu);
	if (flags & DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE_2)
		cmdlist->SetGraphicsRootDescriptorTable(TFX_ROOT_SIGNATURE_PARAM_PS_RT_TEXTURES, m_tfx_rt_textures_handle_gpu);

	ApplyBaseState(flags, cmdlist);
	return true;
}

bool GSDevice12::ApplyUtilityState(bool already_execed)
{
	/* (a utility pipeline that could not be built: no draw) */
	if (unlikely(!m_current_pipeline))
		return false;
	if (m_current_root_signature == RootSignature::Utility && m_dirty_flags == 0)
		return true;

	u32 flags = m_dirty_flags;
	m_dirty_flags &= ~DIRTY_UTILITY_STATE;

	ID3D12GraphicsCommandList* cmdlist = GetCommandList();

	if (m_current_root_signature != RootSignature::Utility)
	{
		m_current_root_signature = RootSignature::Utility;
		flags |= DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE | DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE | DIRTY_FLAG_PIPELINE;
		cmdlist->SetGraphicsRootSignature(m_utility_root_signature.get());
	}

	if (flags & DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE)
		cmdlist->SetGraphicsRootDescriptorTable(UTILITY_ROOT_SIGNATURE_PARAM_PS_TEXTURES, m_utility_texture_gpu);
	if (flags & DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE)
		cmdlist->SetGraphicsRootDescriptorTable(UTILITY_ROOT_SIGNATURE_PARAM_PS_SAMPLERS, m_utility_sampler_gpu);

	ApplyBaseState(flags, cmdlist);
	return true;
}

void GSDevice12::SetVSConstantBuffer(const GSHWDrawConfig::VSConstantBuffer& cb)
{
	if (m_vs_cb_cache.Update(cb))
		m_dirty_flags |= DIRTY_FLAG_VS_CONSTANT_BUFFER;
}

void GSDevice12::SetPSConstantBuffer(const GSHWDrawConfig::PSConstantBuffer& cb)
{
	if (m_ps_cb_cache.Update(cb))
		m_dirty_flags |= DIRTY_FLAG_PS_CONSTANT_BUFFER;
}

void GSDevice12::SetupDATE(GSTexture* rt, GSTexture* ds, SetDATM datm, const GSVector4i& bbox)
{
	const GSVector2i size(ds->GetSize());
	const GSVector4 src = GSVector4(bbox) / GSVector4(size).xyxy();
	const GSVector4 dst = src * 2.0f - 1.0f;
	const GSVertexPT1 vertices[] = {
		{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
		{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
		{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
		{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
	};

	// sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii shadows, rumble roses shadows, persona4 shadows
	EndRenderPass();
	SetUtilityTexture(rt, m_point_sampler_cpu);
	OMSetRenderTargets(nullptr, ds, bbox);
	IASetVertexBuffer(vertices, sizeof(vertices[0]), 4);
	SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	SetPipeline(UtilityPipeline(UTIL_CONVERT + SetDATMShader(datm)));
	SetStencilRef(1);
	BeginRenderPass(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
		D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE,
		D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE,
		GSVector4::zero(), 0.0f, 0);
	if (ApplyUtilityState())
		DrawPrimitive();

	EndRenderPass();
}

GSTexture12* GSDevice12::SetupPrimitiveTrackingDATE(GSHWDrawConfig& config, PipelineSelector& pipe)
{
	// How this is done:
	// - can't put a barrier for the image in the middle of the normal render pass, so that's out
	// - so, instead of just filling the int texture with INT_MAX, we sample the RT and use -1 for failing values
	// - then, instead of sampling the RT with DATE=1/2, we just do a min() without it, the -1 gets preserved
	// - then, the DATE=3 draw is done as normal
	const GSVector2i rtsize(config.rt->GetSize());
	GSTexture12* image =
		static_cast<GSTexture12*>(CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::PrimID, false));
	if (!image)
		return nullptr;

	EndRenderPass();

	// setup the fill quad to prefill with existing alpha values
	SetUtilityTexture(config.rt, m_point_sampler_cpu);
	OMSetRenderTargets(image, config.ds, config.drawarea);

	// if the depth target has been cleared, we need to preserve that clear
	BeginRenderPass(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE,
		config.ds ? GetLoadOpForTexture(static_cast<GSTexture12*>(config.ds)) :
					D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
		config.ds ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
		D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
		GSVector4::zero(), config.ds ? config.ds->GetClearDepth() : 0.0f);

	// draw the quad to prefill the image
	const GSVector4 src = GSVector4(config.drawarea) / GSVector4(rtsize).xyxy();
	const GSVector4 dst = src * 2.0f - 1.0f;
	const GSVertexPT1 vertices[] = {
		{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
		{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
		{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
		{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
	};
	SetUtilityRootSignature();
	SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	SetPipeline(UtilityPipeline(UTIL_DATE_SETUP + pipe.ds * 4 + static_cast<u8>(config.datm)));
	IASetVertexBuffer(vertices, sizeof(vertices[0]), C89_ARRAY_SIZE(vertices));
	if (ApplyUtilityState())
		DrawPrimitive();

	// image is now filled with either -1 or INT_MAX, so now we can do the prepass
	SetPrimitiveTopology(s_primitive_topology_mapping[static_cast<u8>(config.topology)]);
	UploadHWDrawVerticesAndIndices(config);

	// cut down the configuration for the prepass, we don't need blending or any feedback loop
	PipelineSelector init_pipe(m_pipeline_selector);
	init_pipe.dss.zwe = false;
	init_pipe.cms.wrgba = 0;
	init_pipe.bs = {};
	init_pipe.rt = true;
	init_pipe.ps.blend_a = init_pipe.ps.blend_b = init_pipe.ps.blend_c = init_pipe.ps.blend_d = false;
	init_pipe.ps.no_color = false;
	init_pipe.ps.no_color1 = true;
	if (BindDrawPipeline(init_pipe))
		DrawIndexedPrimitive();

	// image is initialized/prepass is done, so finish up and get ready to do the "real" draw
	EndRenderPass();

	// .. by setting it to DATE=3
	pipe.ps.date = 3;
	config.alpha_second_pass.ps.date = 3;

	// and bind the image to the primitive sampler
	image->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	PSSetShaderResource(3, image, false);
	return image;
}

void GSDevice12::RenderHW(GSHWDrawConfig& config)
{
	// Destination Alpha Setup
	const bool stencil_DATE =
		(config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::Stencil ||
			config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::StencilOne);

	GSTexture12* hdr_rt = static_cast<GSTexture12*>(g_gs_device->GetColorClipTexture());
	GSTexture12* draw_rt = static_cast<GSTexture12*>(config.rt);
	GSTexture12* draw_ds = static_cast<GSTexture12*>(config.ds);
	GSTexture12* draw_rt_clone = nullptr;

	// Align the render area to 128x128, hopefully avoiding render pass restarts for small render area changes (e.g. Ratchet and Clank).
	const GSVector2i rtsize(config.rt ? config.rt->GetSize() : config.ds->GetSize());

	PipelineSelector& pipe = m_pipeline_selector;

	// figure out the pipeline
	UpdateHWPipelineSelector(config);

	// now blit the colclip texture back to the original target
	if (hdr_rt)
	{
		if (config.colclip_mode == GSHWDrawConfig::ColClipMode::EarlyResolve)
		{
			EndRenderPass();
			hdr_rt->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			draw_rt = static_cast<GSTexture12*>(config.rt);
			OMSetRenderTargets(draw_rt, draw_ds, config.scissor);

			// if this target was cleared and never drawn to, perform the clear as part of the resolve here.
			BeginRenderPass(GetLoadOpForTexture(draw_rt), D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE,
				GetLoadOpForTexture(draw_ds),
				draw_ds ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
				D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
				draw_rt->GetUNormClearColor(), 0.0f, 0);

			const GSVector4 sRect(GSVector4(config.colclip_update_area) / GSVector4(rtsize.x, rtsize.y).xyxy());
			SetPipeline(UtilityPipeline(UTIL_HDR_FINISH + pipe.ds));
			SetUtilityTexture(hdr_rt, m_point_sampler_cpu);
			DrawStretchRect(sRect, GSVector4(config.colclip_update_area), rtsize);

			Recycle(hdr_rt);
			g_gs_device->SetColorClipTexture(nullptr);
		}
		else
		{
			draw_rt = hdr_rt;
			pipe.ps.colclip_hw = 1;
		}
	}

	if (stencil_DATE)
		SetupDATE(draw_rt, config.ds, config.datm, config.drawarea);

	// stream buffer in first, in case we need to exec
	SetVSConstantBuffer(config.cb_vs);
	SetPSConstantBuffer(config.cb_ps);

	// bind textures before checking the render pass, in case we need to transition them
	if (config.tex)
	{
		PSSetShaderResource(0, config.tex, config.tex != config.rt);
		PSSetSampler(config.sampler);
	}
	if (config.pal)
		PSSetShaderResource(1, config.pal, true);

	if (config.blend.constant_enable)
		SetBlendConstants(config.blend.constant);

	// Primitive ID tracking DATE setup.
	GSTexture12* date_image = nullptr;
	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		GSTexture* backup_rt = config.rt;
		config.rt = draw_rt;
		date_image = SetupPrimitiveTrackingDATE(config, pipe);
		config.rt = backup_rt;
		if (!date_image)
		{
			log_cb(RETRO_LOG_INFO, "Failed to allocate DATE image, aborting draw.\n");
			return;
		}
	}

	if (config.require_one_barrier)
	{
		// requires a copy of the RT
		draw_rt_clone = static_cast<GSTexture12*>(CreateTexture(rtsize.x, rtsize.y, 1, hdr_rt ? GSTexture::Format::ColorClip : GSTexture::Format::Color, true));
		if (draw_rt_clone)
		{
			EndRenderPass();

			draw_rt_clone->SetState(GSTexture::State::Invalidated);
			CopyRect(draw_rt, draw_rt_clone, config.drawarea, config.drawarea.left, config.drawarea.top);
			PSSetShaderResource(2, draw_rt_clone, true);
		}
	}

	// Switch to colclip target for colclip rendering
	if (pipe.ps.colclip_hw)
	{
		if (!hdr_rt)
		{
			config.colclip_update_area = config.drawarea;

			EndRenderPass();

			hdr_rt = static_cast<GSTexture12*>(CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::ColorClip, false));
			if (!hdr_rt)
			{
				log_cb(RETRO_LOG_INFO, "Failed to allocate HDR render target, aborting draw.\n");

				if (date_image)
					Recycle(date_image);

				return;
			}

			g_gs_device->SetColorClipTexture(static_cast<GSTexture*>(hdr_rt));

			// propagate clear value through if the colclip render is the first
			if (draw_rt->GetState() == GSTexture::State::Cleared)
			{
				hdr_rt->SetState(GSTexture::State::Cleared);
				hdr_rt->SetClearColor(draw_rt->GetClearColor());
			}
			else if (draw_rt->GetState() == GSTexture::State::Dirty)
			{
				draw_rt->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}

			// we're not drawing to the RT, so we can use it as a source
			if (config.require_one_barrier)
				PSSetShaderResource(2, draw_rt, true);
		}

		draw_rt = hdr_rt;
	}

	// clear texture binding when it's bound to RT or DS
	if (((draw_rt && static_cast<GSTexture12*>(draw_rt)->GetSRVDescriptor() == m_tfx_textures[0]) ||
			(config.ds && static_cast<GSTexture12*>(config.ds)->GetSRVDescriptor() == m_tfx_textures[0])))
	{
		PSSetShaderResource(0, nullptr, false);
	}

	// avoid restarting the render pass just to switch from rt+depth to rt and vice versa
	if (InRenderPass() && (m_current_render_target == draw_rt || m_current_depth_target == draw_ds))
	{
		// avoid restarting the render pass just to switch from rt+depth to rt and vice versa
		// keep the depth even if doing HDR draws, because the next draw will probably re-enable depth
		if (!draw_rt && m_current_render_target && config.tex != m_current_render_target &&
			m_current_render_target->GetSize() == draw_ds->GetSize())
		{
			draw_rt = m_current_render_target;
			m_pipeline_selector.rt = true;
		}
	}
	else if (!draw_ds && m_current_depth_target && config.tex != m_current_depth_target &&
			 m_current_depth_target->GetSize() == draw_rt->GetSize())
	{
		draw_ds = m_current_depth_target;
		m_pipeline_selector.ds = true;
	}

	OMSetRenderTargets(draw_rt, draw_ds, config.scissor);

	// Begin render pass if new target or out of the area.
	if (!m_in_render_pass)
	{
		GSVector4 clear_color = draw_rt ? draw_rt->GetUNormClearColor() : GSVector4::zero();
		if (pipe.ps.colclip_hw)
		{
			// Denormalize clear color for HDR.
			clear_color *= GSVector4::cxpr(255.0f / 65535.0f, 255.0f / 65535.0f, 255.0f / 65535.0f, 1.0f);
		}
		BeginRenderPass(GetLoadOpForTexture(draw_rt),
			draw_rt ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			GetLoadOpForTexture(draw_ds),
			draw_ds ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			stencil_DATE ? D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE :
						   D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
			stencil_DATE ? (draw_rt_clone ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE :
											D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD) :
						   D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
			clear_color, draw_ds ? draw_ds->GetClearDepth() : 0.0f, 1);
	}

	// rt -> hdr blit if enabled
	if (hdr_rt && (config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly || config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve) && config.rt->GetState() == GSTexture::State::Dirty)
	{
		SetUtilityTexture(static_cast<GSTexture12*>(config.rt), m_point_sampler_cpu);
		SetPipeline(UtilityPipeline(UTIL_HDR_SETUP + pipe.ds));

		const GSVector4 drawareaf = GSVector4((config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly) ? GSVector4i::loadh(rtsize) : config.drawarea);
		const GSVector4 sRect(drawareaf / GSVector4(rtsize.x, rtsize.y).xyxy());
		DrawStretchRect(sRect, GSVector4(drawareaf), rtsize);
	}

	// VB/IB upload, if we did DATE setup and it's not HDR this has already been done
	SetPrimitiveTopology(s_primitive_topology_mapping[static_cast<u8>(config.topology)]);
	if (!date_image || hdr_rt)
		UploadHWDrawVerticesAndIndices(config);

	// now we can do the actual draw
	if (BindDrawPipeline(pipe))
		DrawIndexedPrimitive();

	// blend second pass
	if (config.blend_multi_pass.enable)
	{
		if (config.blend_multi_pass.blend.constant_enable)
			SetBlendConstants(config.blend_multi_pass.blend.constant);

		pipe.bs = config.blend_multi_pass.blend;
		pipe.ps.blend_hw = config.blend_multi_pass.blend_hw;
		pipe.ps.dither = config.blend_multi_pass.dither;
		if (BindDrawPipeline(pipe))
			DrawIndexedPrimitive();
	}

	// and the alpha pass
	if (config.alpha_second_pass.enable)
	{
		// cbuffer will definitely be dirty if aref changes, no need to check it
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			SetPSConstantBuffer(config.cb_ps);
		}

		pipe.ps = config.alpha_second_pass.ps;
		pipe.cms = config.alpha_second_pass.colormask;
		pipe.dss = config.alpha_second_pass.depth;
		pipe.bs = config.blend;
		if (BindDrawPipeline(pipe))
			DrawIndexedPrimitive();
	}

	if (draw_rt_clone)
		Recycle(draw_rt_clone);

	if (date_image)
		Recycle(date_image);

	// now blit the hdr texture back to the original target
	if (hdr_rt)
	{
		config.colclip_update_area = config.colclip_update_area.runion(config.drawarea);

		if ((config.colclip_mode == GSHWDrawConfig::ColClipMode::ResolveOnly || config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve))
		{
			EndRenderPass();
			hdr_rt->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			draw_rt = static_cast<GSTexture12*>(config.rt);
			OMSetRenderTargets(draw_rt, draw_ds, config.scissor);

			// if this target was cleared and never drawn to, perform the clear as part of the resolve here.
			BeginRenderPass(GetLoadOpForTexture(draw_rt), D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE,
				GetLoadOpForTexture(draw_ds),
				draw_ds ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
				D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
				draw_rt->GetUNormClearColor(), 0.0f, 0);

			const GSVector4 sRect(GSVector4(config.colclip_update_area) / GSVector4(rtsize.x, rtsize.y).xyxy());
			SetPipeline(UtilityPipeline(UTIL_HDR_FINISH + pipe.ds));
			SetUtilityTexture(hdr_rt, m_point_sampler_cpu);
			DrawStretchRect(sRect, GSVector4(config.colclip_update_area), rtsize);

			Recycle(hdr_rt);
			g_gs_device->SetColorClipTexture(nullptr);
		}
	}
}

void GSDevice12::UpdateHWPipelineSelector(GSHWDrawConfig& config)
{
	m_pipeline_selector.vs.key = config.vs.key;
	m_pipeline_selector.ps.key_hi = config.ps.key_hi;
	m_pipeline_selector.ps.key_lo = config.ps.key_lo;
	m_pipeline_selector.dss.key = config.depth.key;
	m_pipeline_selector.bs.key = config.blend.key;
	m_pipeline_selector.bs.constant = 0; // don't dupe states with different alpha values
	m_pipeline_selector.cms.key = config.colormask.key;
	m_pipeline_selector.topology = static_cast<u32>(config.topology);
	m_pipeline_selector.rt = config.rt != nullptr;
	m_pipeline_selector.ds = config.ds != nullptr;
}

void GSDevice12::UploadHWDrawVerticesAndIndices(const GSHWDrawConfig& config)
{
	IASetVertexBuffer(config.verts, sizeof(GSVertex), config.nverts);

	// Update SRV in root signature directly, rather than using a uniform for base vertex.
	if (config.vs.expand != GSHWDrawConfig::VSExpand::None)
		m_dirty_flags |= DIRTY_FLAG_VS_VERTEX_BUFFER_BINDING;

	if (config.vs.UseExpandIndexBuffer())
	{
		m_index.start = 0;
		m_index.count = config.nindices;
		SetIndexBuffer(m_expand_index_buffer->GetGPUVirtualAddress(), EXPAND_BUFFER_SIZE, DXGI_FORMAT_R16_UINT);
	}
	else
	{
		IASetIndexBuffer(config.indices, config.nindices);
	}
}

void GSDevice12::ResetAPIState()
{
	EndRenderPass();
}

void GSDevice12::RestoreAPIState()
{
	InvalidateCachedState();
}
