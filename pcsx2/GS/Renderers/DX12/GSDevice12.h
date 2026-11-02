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

#pragma once

#include "common/Pcsx2Defs.h"
#include "GS/Renderers/Common/GSKeyHash.h"
#include "common/RedtapeWindows.h"

#include "D3D12DescriptorHeapManager.h"
#include "D3D12StreamBuffer.h"
#include "GSTexture12.h"
#include "D3D12ShaderCache.h"
#include "D3D12StreamBuffer.h"

#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSDevice.h"

#include <array>
#include <unordered_map>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <memory>
#include <vector>
#include <functional>
#include "common/RedtapeWilCom.h"

struct IDXGIAdapter;
namespace D3D12 { class GraphicsPipelineBuilder; }
struct IDXGIFactory;

#include "GSD3D12Heap.h"
#include "GSD3D12Warm.h"

class GSDevice12 final : public GSDevice
{
	/* The table this device answers through is a set of free functions
	 * in its own .cpp (gs_device_ops, Common/GSDevice.h); they call what
	 * used to be reached through a vtable, whatever its access. */
	friend struct GSDevice12_ops_access;
public:
	template <typename T>
	using ComPtr = wil::com_ptr_nothrow<T>;

	enum : u32
       {
	       /// Number of command lists. One is being built while the other(s) are executed.
	       NUM_COMMAND_LISTS = 3,

	       /// Textures that don't fit into this buffer will be uploaded with a staging buffer.
	       TEXTURE_UPLOAD_BUFFER_SIZE = 64 * 1024 * 1024,

	       /// Maximum number of samples in a single allocation group.
	       SAMPLER_GROUP_SIZE = 2,

	       /// Start/End timestamp queries.
	       NUM_TIMESTAMP_QUERIES_PER_CMDLIST = 2
       };

	struct alignas(8) PipelineSelector
	{
		GSHWDrawConfig::PSSelector ps;

		union
		{
			struct
			{
				u32 topology : 2;
				u32 rt : 1;
				u32 ds : 1;
			};

			u32 key;
		};

		GSHWDrawConfig::BlendState bs;
		GSHWDrawConfig::VSSelector vs;
		GSHWDrawConfig::DepthStencilSelector dss;
		GSHWDrawConfig::ColorMaskSelector cms;
		u8 pad;

		__fi bool operator==(const PipelineSelector& p) const { return BitEqual(*this, p); }
		__fi bool operator!=(const PipelineSelector& p) const { return !BitEqual(*this, p); }

		__fi PipelineSelector() { memset(this, 0, sizeof(*this)); }
	};

	struct PipelineSelectorHash
	{
		std::size_t operator()(const PipelineSelector& e) const noexcept
		{
			/* Compared bit for bit: hashed over every byte. */
			return gs_key_hash(&e, sizeof(e));
		}
	};

	class ShaderMacro
	{
		struct mcstr
		{
			const char *name, *def;
			mcstr(const char* n, const char* d)
				: name(n)
				, def(d)
			{
			}
		};

		struct mstring
		{
			std::string name, def;
			mstring(const char* n, std::string d)
				: name(n)
				, def(d)
			{
			}
		};

		std::vector<mstring> mlist;
		std::vector<mcstr> mout;

	public:
		ShaderMacro(D3D_FEATURE_LEVEL fl);
		void AddMacro(const char* n, int d);
		void AddMacro(const char* n, std::string d);
		D3D_SHADER_MACRO* GetPtr(void);
	};

	enum : u32
	{
		NUM_TFX_CONSTANT_BUFFERS = 2,
		NUM_TFX_TEXTURES = 2,
		NUM_TFX_RT_TEXTURES = 2,
		NUM_TOTAL_TFX_TEXTURES = NUM_TFX_TEXTURES + NUM_TFX_RT_TEXTURES,
		NUM_TFX_SAMPLERS = 1,
		NUM_UTILITY_TEXTURES = 1,
		NUM_UTILITY_SAMPLERS = 1,
		CONVERT_PUSH_CONSTANTS_SIZE = 96,

		VERTEX_BUFFER_SIZE = 32 * 1024 * 1024,
		INDEX_BUFFER_SIZE = 16 * 1024 * 1024,
		VERTEX_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
		FRAGMENT_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,

		TFX_ROOT_SIGNATURE_PARAM_VS_CBV = 0,
		TFX_ROOT_SIGNATURE_PARAM_PS_CBV = 1,
		TFX_ROOT_SIGNATURE_PARAM_VS_SRV = 2,
		TFX_ROOT_SIGNATURE_PARAM_PS_TEXTURES = 3,
		TFX_ROOT_SIGNATURE_PARAM_PS_SAMPLERS = 4,
		TFX_ROOT_SIGNATURE_PARAM_PS_RT_TEXTURES = 5,

		UTILITY_ROOT_SIGNATURE_PARAM_PUSH_CONSTANTS = 0,
		UTILITY_ROOT_SIGNATURE_PARAM_PS_TEXTURES = 1,
		UTILITY_ROOT_SIGNATURE_PARAM_PS_SAMPLERS = 2
	};

private:
	ComPtr<IDXGIFactory5> m_dxgi_factory;

	bool m_device_lost = false;
	bool m_utility_failed = false;
	/* a utility pipeline failed at first use: the next device builds them at its creation */
	static bool s_utility_build_failed;

	ComPtr<ID3D12RootSignature> m_tfx_root_signature;
	ComPtr<ID3D12RootSignature> m_utility_root_signature;

	D3D12StreamBuffer m_vertex_stream_buffer;
	D3D12StreamBuffer m_index_stream_buffer;
	D3D12StreamBuffer m_vertex_constant_buffer;
	D3D12StreamBuffer m_pixel_constant_buffer;
	ComPtr<ID3D12Resource> m_expand_index_buffer;
	gs_d3d12_alloc_t m_expand_index_buffer_alloc = {};

	D3D12DescriptorHandle m_point_sampler_cpu;
	D3D12DescriptorHandle m_linear_sampler_cpu;

	std::unordered_map<u32, D3D12DescriptorHandle> m_samplers;

	std::array<ComPtr<ID3D12PipelineState>, static_cast<int>(PresentShader::Count)> m_present{};

	/* The utility pipelines, one table: built on the warm-up thread from
	 * the device's creation on, each taken at its first use, or built
	 * then when the thread has not got to it. */
	enum : u32
	{
		UTIL_CONVERT    = 0,                                                   // [ShaderConvert]
		UTIL_COLOR_COPY = UTIL_CONVERT + static_cast<u32>(ShaderConvert::Count), // [write mask | 16 for RTA]
		UTIL_HDR_SETUP  = UTIL_COLOR_COPY + 32,                                // [depth]
		UTIL_HDR_FINISH = UTIL_HDR_SETUP + 2,                                  // [depth]
		UTIL_DATE_SETUP = UTIL_HDR_FINISH + 2,                                 // [depth * 4 + datm]
		UTIL_INTERLACE  = UTIL_DATE_SETUP + 8,                                 // [shader]
		UTIL_MERGE      = UTIL_INTERLACE + NUM_INTERLACE_SHADERS,              // [MMOD]
		UTIL_COUNT      = UTIL_MERGE + 2,
	};
	std::array<ComPtr<ID3D12PipelineState>, UTIL_COUNT> m_utility{};

	std::unordered_map<u32, ComPtr<ID3DBlob>> m_tfx_vertex_shaders;
	std::unordered_map<GSHWDrawConfig::PSSelector, ComPtr<ID3DBlob>, GSHWDrawConfig::PSSelectorHash> m_tfx_pixel_shaders;
	typedef std::unordered_map<PipelineSelector, ComPtr<ID3D12PipelineState>, PipelineSelectorHash> TFXPipelineMap;
	TFXPipelineMap m_tfx_pipelines;
	/* The draws of one frame come in runs that share a pipeline, and the
	 * hash of a selector costs more than the comparison: remember the
	 * last one and skip the map while the selector has not changed. */
	PipelineSelector m_last_tfx_pipeline_selector;
	const ID3D12PipelineState* m_last_tfx_pipeline = nullptr;

	/* The draw pipelines a session made, kept beside the shader cache, and
	 * built again at the next start on a thread of their own (see
	 * GSD3D12Warm.h). A result of that thread: the pipeline, and what it
	 * built that the cache had not (null when it had), for the cache. */
	struct TFXWarmResult
	{
		D3D12ShaderCache::CacheIndexKey vs_key;
		D3D12ShaderCache::CacheIndexKey ps_key;
		D3D12ShaderCache::CacheIndexKey pso_key;
		ID3D12PipelineState* pso;
		ID3DBlob* vs_blob;
		ID3DBlob* ps_blob;
		ID3DBlob* pso_blob;
		PipelineSelector p;
		u32 utility;   /* a UTIL_ slot, or UTIL_COUNT: the draw pipeline p */
		u32 pad;
	};
	/* a job of the warm-up thread: the same two */
	struct WarmJob
	{
		PipelineSelector p;
		u32 utility;
		u32 pad;
	};
	gs_warm_list_t m_tfx_list = {nullptr, sizeof(PipelineSelector), 0, 0};
	size_t m_tfx_list_loaded = 0;
	gs_warm_t* m_tfx_warm = nullptr;
	D3D12ShaderCache::Snapshot m_tfx_warm_cache = {};
	/* the blobs the warm-up thread built, added to the cache at teardown */
	TFXWarmResult* m_tfx_warm_blobs = nullptr;
	size_t m_tfx_warm_blob_count = 0;
	size_t m_tfx_warm_blob_cap = 0;

	GSHWDrawConfig::VSConstantBuffer m_vs_cb_cache;
	GSHWDrawConfig::PSConstantBuffer m_ps_cb_cache;

	D3D12ShaderCache m_shader_cache;

	void LookupNativeFormat(GSTexture::Format format, DXGI_FORMAT* d3d_format, DXGI_FORMAT* srv_format, DXGI_FORMAT* rtv_format, DXGI_FORMAT* dsv_format) const;

	GSTexture* CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format);

	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE,
		const GSRegEXTBUF& EXTBUF, u32 c, const bool linear);
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb);

	bool GetSampler(D3D12DescriptorHandle* cpu_handle, GSHWDrawConfig::SamplerSelector ss);
	void ClearSamplerCache();
	bool GetTextureGroupDescriptors(D3D12DescriptorHandle* gpu_handle, const D3D12DescriptorHandle* cpu_handles, u32 count);

	const ID3DBlob* GetTFXVertexShader(GSHWDrawConfig::VSSelector sel);
	const ID3DBlob* GetTFXPixelShader(const GSHWDrawConfig::PSSelector& sel);
	ComPtr<ID3D12PipelineState> CreateTFXPipeline(const PipelineSelector& p);
	const ID3D12PipelineState* GetTFXPipeline(const PipelineSelector& p);
	__noinline TFXPipelineMap::iterator AddMissingTFXPipeline(const PipelineSelector& p);
	void FillTFXPipeline(D3D12::GraphicsPipelineBuilder& gpb, const PipelineSelector& p,
		const ID3DBlob* vs, const ID3DBlob* ps) const;
	bool StartTFXWarm();
	void StopTFXWarm();
	bool CollectWarmedTFXPipelines(const PipelineSelector* want, TFXPipelineMap::iterator* found);
	void KeepTFXWarmBlobs(const TFXWarmResult& r);
	static int BuildWarmedTFXPipeline(void* ud, const void* job, void* result);
	static int BuildWarmedUtilityPipeline(GSDevice12* self, u32 slot, TFXWarmResult* r);
	static ComPtr<ID3DBlob> WarmShader(GSDevice12* self, D3D12ShaderCache::EntryType type, const char* source,
		ShaderMacro& sm, const char* entry, D3D12ShaderCache::CacheIndexKey* key, ID3DBlob** built);
	static ComPtr<ID3D12PipelineState> WarmPipeline(GSDevice12* self, D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
		D3D12ShaderCache::CacheIndexKey* key, ID3DBlob** built);

	__fi ID3D12PipelineState* UtilityPipeline(u32 slot)
	{
		ID3D12PipelineState* p = m_utility[slot].get();
		return likely(p) ? p : MissingUtilityPipeline(slot);
	}
	__noinline ID3D12PipelineState* MissingUtilityPipeline(u32 slot);
	ComPtr<ID3D12PipelineState> CreateUtilityPipeline(u32 slot);
	static const char* UtilityPixelShader(u32 slot, const char** source, char* entry, size_t entry_size);
	static bool UtilityPipelineDesc(D3D12::GraphicsPipelineBuilder& gpb, u32 slot, ID3D12RootSignature* rs,
		const ID3DBlob* vs, const ID3DBlob* ps);

	ComPtr<ID3DBlob> GetUtilityVertexShader(const std::string& source, const char* entry_point);
	ComPtr<ID3DBlob> GetUtilityPixelShader(const std::string& source, const char* entry_point);
	ComPtr<ID3DBlob> GetUtilityVertexShader(const char *source, size_t len, const char* entry_point);
	ComPtr<ID3DBlob> GetUtilityPixelShader(const char *source, size_t len, const char* entry_point);

	bool CheckFeatures();
	bool CreateNullTexture();
	bool CreateBuffers();
	bool CreateRootSignatures();


	void DestroyResources();
	void DrainDebugMessages();

public:
	GSDevice12();
	~GSDevice12();

	__fi static GSDevice12* GetInstance() { return static_cast<GSDevice12*>(g_gs_device); }

	RenderAPI GetRenderAPI() const;

	bool Create();
	void Destroy();

	PresentResult BeginPresent(bool frame_skip);
	void EndPresent();

	void DrawPrimitive();
	void DrawIndexedPrimitive();
	void DrawIndexedPrimitive(int offset, int count);

	GSDownloadTexturePtr CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format);

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY);

	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvert shader = ShaderConvert::COPY, bool linear = true);
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red,
		bool green, bool blue, bool alpha, ShaderConvert shader = ShaderConvert::COPY);
	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect);
	void UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize, u32 samples);
	void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM);
	void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect);
	void MoveTexels(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const MoveTexelsConstants& cb);

	void DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader);
	void DoMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture12* dTex, ShaderConvert shader);

	void BeginRenderPassForStretchRect(
		GSTexture12* dTex, const GSVector4i& dtex_rc, const GSVector4i& dst_rc, bool allow_discard = true);
	void DoStretchRect(GSTexture12* sTex, const GSVector4& sRect, GSTexture12* dTex, const GSVector4& dRect,
		const ID3D12PipelineState* pipeline, bool linear, bool allow_discard);
	void DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds);

	void SetupDATE(GSTexture* rt, GSTexture* ds, SetDATM datm, const GSVector4i& bbox);
	GSTexture12* SetupPrimitiveTrackingDATE(GSHWDrawConfig& config, PipelineSelector& pipe);

	void IASetVertexBuffer(const void* vertex, size_t stride, size_t count);
	void IASetIndexBuffer(const void* index, size_t count);

	void PSSetShaderResource(int i, GSTexture* sr, bool check_state);
	void PSSetSampler(GSHWDrawConfig::SamplerSelector sel);

	void OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i& scissor);

	void SetVSConstantBuffer(const GSHWDrawConfig::VSConstantBuffer& cb);
	void SetPSConstantBuffer(const GSHWDrawConfig::PSConstantBuffer& cb);
	bool BindDrawPipeline(const PipelineSelector& p);

	void RenderHW(GSHWDrawConfig& config);
	void UpdateHWPipelineSelector(GSHWDrawConfig& config);
	void UploadHWDrawVerticesAndIndices(const GSHWDrawConfig& config);

	/// Ends any render pass, executes the command buffer, and invalidates cached state.
	void ExecuteCommandList(bool wait_for_completion);
	bool ContextExecuteCommandList(bool wait_for_completion);
	void ExecuteCommandListAndRestartRenderPass(bool wait_for_completion);
	void ExecuteCommandListForReadback();

	/// Set dirty flags on everything to force re-bind at next draw time.
	void InvalidateCachedState();

	/// Binds all dirty state to the command buffer.
	bool ApplyUtilityState(bool already_execed = false);
	bool ApplyTFXState(bool already_execed = false);

	void SetVertexBuffer(D3D12_GPU_VIRTUAL_ADDRESS buffer, size_t size, size_t stride);
	void SetIndexBuffer(D3D12_GPU_VIRTUAL_ADDRESS buffer, size_t size, DXGI_FORMAT type);
	void SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology);
	void SetBlendConstants(u8 color);
	void SetStencilRef(u8 ref);

	void SetUtilityRootSignature();
	void SetUtilityTexture(GSTexture* tex, const D3D12DescriptorHandle& sampler);
	void SetUtilityPushConstants(const void* data, u32 size);
	void UnbindTexture(GSTexture12* tex);

	// Assumes that the previous level has been transitioned to PS resource,
	// and the current level has been transitioned to RT.
	void RenderTextureMipmap(GSTexture12 *texture,
		u32 dst_level, u32 dst_width, u32 dst_height, u32 src_level, u32 src_width, u32 src_height);

	// Ends a render pass if we're currently in one.
	// When Bind() is next called, the pass will be restarted.
	// Calling this function is allowed even if a pass has not begun.
	bool InRenderPass();
	void BeginRenderPass(
		D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE color_begin = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
		D3D12_RENDER_PASS_ENDING_ACCESS_TYPE color_end = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
		D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE depth_begin = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
		D3D12_RENDER_PASS_ENDING_ACCESS_TYPE depth_end = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
		D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE stencil_begin = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
		D3D12_RENDER_PASS_ENDING_ACCESS_TYPE stencil_end = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS,
		GSVector4 clear_color = GSVector4::zero(), float clear_depth = 0.0f, u8 clear_stencil = 0);
	void EndRenderPass();

	void SetViewport(const D3D12_VIEWPORT& viewport);
	void SetScissor(const GSVector4i& scissor);
	void SetPipeline(const ID3D12PipelineState* pipeline);

       __fi IDXGIAdapter* GetAdapter() const { return m_adapter.get(); }
       __fi ID3D12Device* GetDevice() const { return m_device.get(); }
       __fi gs_d3d12_heap_t* GetHeap() { return &m_heap; }

       /* One place that turns a resource description into memory: it
        * asks the device how big and how aligned, takes that from the
        * heap, and places the resource at the offset. What
        * D3D12MA::CreateResource did, without the library deciding when
        * to take a heap from the driver. */
       bool CreatePlacedResource(const D3D12_RESOURCE_DESC* desc, D3D12_HEAP_TYPE heap_type,
               D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear_value,
               ID3D12Resource** out_resource, gs_d3d12_alloc_t* out_alloc);

       /// Returns the PCI vendor ID of the device, if known.
       u32 GetAdapterVendorID() const;

       /// Returns the current command list, commands can be recorded directly.
       ID3D12GraphicsCommandList4* GetCommandList() const
       {
	       return m_command_lists[m_current_command_list].command_lists[1].get();
       }

       /// Returns the init command list for uploading.
       ID3D12GraphicsCommandList4* GetInitCommandList();

       /// Returns the per-frame SRV/CBV/UAV allocator.
       D3D12DescriptorAllocator& GetDescriptorAllocator()
       {
	       return m_command_lists[m_current_command_list].descriptor_allocator;
       }

       /// Returns the per-frame sampler allocator.
       D3D12GroupedSamplerAllocator<SAMPLER_GROUP_SIZE>& GetSamplerAllocator()
       {
	       return m_command_lists[m_current_command_list].sampler_allocator;
       }

       /// Invalidates GPU-side sampler caches for all command lists. Call after you've freed samplers,
       /// and are going to re-use the handles from GetSamplerHeapManager().
       void InvalidateSamplerGroups();

       // Descriptor manager access.
       D3D12DescriptorHeapManager& GetDescriptorHeapManager() { return m_descriptor_heap_manager; }
       D3D12DescriptorHeapManager& GetRTVHeapManager() { return m_rtv_heap_manager; }
       D3D12DescriptorHeapManager& GetDSVHeapManager() { return m_dsv_heap_manager; }
       D3D12DescriptorHeapManager& GetSamplerHeapManager() { return m_sampler_heap_manager; }
       const D3D12DescriptorHandle& GetNullSRVDescriptor() const { return m_null_srv_descriptor; }
       D3D12StreamBuffer& GetTextureStreamBuffer() { return m_texture_stream_buffer; }

       // Root signature access.
       ComPtr<ID3DBlob> SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc);
       ComPtr<ID3D12RootSignature> CreateRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc);

       /// Fence value for current command list.
       /* Rate-limited device-error reporting.  A wedged device fails the
        * same way every frame; without this a report is a log file the
        * size of the disc image that provoked it. */
       static void ReportRecurring(const char* what, HRESULT hr);

       u64 GetCurrentFenceValue() const { return m_current_fence_value; }

       /// Last "completed" fence.
       u64 GetCompletedFenceValue() const { return m_completed_fence_value; }

       /// Feature level to use when compiling shaders.
       D3D_FEATURE_LEVEL GetFeatureLevel() const { return m_feature_level; }

       /// Test for support for the specified texture format.
       bool SupportsTextureFormat(DXGI_FORMAT format);

       /// Waits for a specific fence.
       void WaitForFence(u64 fence);

       /// Waits for any in-flight command buffers to complete.
       void WaitForGPUIdle();

       /// Defers destruction of a D3D resource (associates it with the current list).
       void DeferObjectDestruction(ID3D12DeviceChild* resource);

       /// Defers destruction of a D3D resource (associates it with the current list).
       void DeferResourceDestruction(const gs_d3d12_alloc_t* alloc, ID3D12Resource* resource);

       /// Defers destruction of a descriptor handle (associates it with the current list).
       void DeferDescriptorDestruction(D3D12DescriptorHeapManager& manager, u32 index);
       void DeferDescriptorDestruction(D3D12DescriptorHeapManager& manager, D3D12DescriptorHandle* handle);

       // Allocates a temporary CPU staging buffer, fires the callback with it to populate, then copies to a GPU buffer.
       bool AllocatePreinitializedGPUBuffer(u32 size, ID3D12Resource** gpu_buffer, gs_d3d12_alloc_t* gpu_alloc,
		       const std::function<void(void*)>& fill_callback);

private:
	enum DIRTY_FLAG : u32
	{
		DIRTY_FLAG_VS_CONSTANT_BUFFER = (1 << 0),
		DIRTY_FLAG_PS_CONSTANT_BUFFER = (1 << 1),
		DIRTY_FLAG_TFX_TEXTURES = (1 << 2),
		DIRTY_FLAG_TFX_SAMPLERS = (1 << 3),
		DIRTY_FLAG_TFX_RT_TEXTURES = (1 << 4),

		DIRTY_FLAG_VS_CONSTANT_BUFFER_BINDING = (1 << 5),
		DIRTY_FLAG_PS_CONSTANT_BUFFER_BINDING = (1 << 6),
		DIRTY_FLAG_VS_VERTEX_BUFFER_BINDING = (1 << 7),
		DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE = (1 << 8),
		DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE = (1 << 9),
		DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE_2 = (1 << 10),

		DIRTY_FLAG_VERTEX_BUFFER = (1 << 11),
		DIRTY_FLAG_INDEX_BUFFER = (1 << 12),
		DIRTY_FLAG_PRIMITIVE_TOPOLOGY = (1 << 13),
		DIRTY_FLAG_VIEWPORT = (1 << 14),
		DIRTY_FLAG_SCISSOR = (1 << 15),
		DIRTY_FLAG_RENDER_TARGET = (1 << 16),
		DIRTY_FLAG_PIPELINE = (1 << 17),
		DIRTY_FLAG_BLEND_CONSTANTS = (1 << 18),
		DIRTY_FLAG_STENCIL_REF = (1 << 19),

		DIRTY_BASE_STATE = DIRTY_FLAG_VS_CONSTANT_BUFFER_BINDING | DIRTY_FLAG_PS_CONSTANT_BUFFER_BINDING |
						   DIRTY_FLAG_VS_VERTEX_BUFFER_BINDING | DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE |
						   DIRTY_FLAG_SAMPLERS_DESCRIPTOR_TABLE | DIRTY_FLAG_TEXTURES_DESCRIPTOR_TABLE_2 |
						   DIRTY_FLAG_VERTEX_BUFFER | DIRTY_FLAG_INDEX_BUFFER | DIRTY_FLAG_PRIMITIVE_TOPOLOGY |
						   DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR | DIRTY_FLAG_RENDER_TARGET | DIRTY_FLAG_PIPELINE |
						   DIRTY_FLAG_BLEND_CONSTANTS | DIRTY_FLAG_STENCIL_REF,

		DIRTY_TFX_STATE = DIRTY_BASE_STATE | DIRTY_FLAG_TFX_TEXTURES | DIRTY_FLAG_TFX_SAMPLERS | DIRTY_FLAG_TFX_RT_TEXTURES,
		DIRTY_UTILITY_STATE = DIRTY_BASE_STATE,
		DIRTY_CONSTANT_BUFFER_STATE = DIRTY_FLAG_VS_CONSTANT_BUFFER | DIRTY_FLAG_PS_CONSTANT_BUFFER,
	};

	enum class RootSignature
	{
		Undefined,
		TFX,
		Utility
	};

	void InitializeState();
	void InitializeSamplers();

	void ApplyBaseState(u32 flags, ID3D12GraphicsCommandList* cmdlist);

       /* A resource waiting on a command list owns its span of the heap
        * until the GPU is done with it; the span goes back when the list
        * retires, not when the caller let go. A zeroed alloc means the
        * resource came from somewhere else. Named at class scope because
        * the functions that queue one are members of the device, not of
        * the list. */
       struct PendingResource
       {
	       gs_d3d12_alloc_t   alloc;
	       ID3D12DeviceChild* resource;
       };

       struct CommandListResources
       {
	       std::array<ComPtr<ID3D12CommandAllocator>, 2> command_allocators;
	       std::array<ComPtr<ID3D12GraphicsCommandList4>, 2> command_lists;
	       D3D12DescriptorAllocator descriptor_allocator;
	       D3D12GroupedSamplerAllocator<SAMPLER_GROUP_SIZE> sampler_allocator;
	       std::vector<PendingResource> pending_resources;
	       std::vector<std::pair<D3D12DescriptorHeapManager&, u32>> pending_descriptors;
	       u64 ready_fence_value = 0;
	       bool init_command_list_used = false;
       };

       bool CreateHeap();
       bool CreateFence();
       bool CreateDescriptorHeaps();
       bool CreateCommandLists();
       void MoveToNextCommandList();
       void DestroyPendingResources(CommandListResources& cmdlist);

       ComPtr<IDXGIAdapter> m_adapter;
       ComPtr<ID3D12Debug> m_debug_interface;
       /* Non-null only when the debug layer was forced on before the
        * frontend made the device - the core cannot enable it, the
        * device is not ours. When it is on, this is what turns a
        * validation error into a line in the libretro log instead of a
        * breakpoint that takes the process with it. */
       ComPtr<ID3D12InfoQueue> m_info_queue;
       ComPtr<ID3D12Device> m_device;
       ComPtr<ID3D12CommandQueue> m_command_queue;
       gs_d3d12_heap_t m_heap = {};
       /* Tier 1 wants buffers, render targets and other textures in
        * heaps of their own; tier 2 takes them all in one. */
       bool m_heaps_hold_anything = false;

       ComPtr<ID3D12Fence> m_fence;
       HANDLE m_fence_event = {};
       u32 m_current_fence_value = 0;
       u64 m_completed_fence_value = 0;

       std::array<CommandListResources, NUM_COMMAND_LISTS> m_command_lists;
       u32 m_current_command_list = NUM_COMMAND_LISTS - 1;

       D3D12DescriptorHeapManager m_descriptor_heap_manager;
       D3D12DescriptorHeapManager m_rtv_heap_manager;
       D3D12DescriptorHeapManager m_dsv_heap_manager;
       D3D12DescriptorHeapManager m_sampler_heap_manager;
       D3D12DescriptorHandle m_null_srv_descriptor;
       D3D12StreamBuffer m_texture_stream_buffer;

       D3D_FEATURE_LEVEL m_feature_level = D3D_FEATURE_LEVEL_11_0;

	// Which bindings/state has to be updated before the next draw.
	u32 m_dirty_flags = 0;

	// input assembly
	D3D12_VERTEX_BUFFER_VIEW m_vertex_buffer = {};
	D3D12_INDEX_BUFFER_VIEW m_index_buffer = {};
	D3D12_PRIMITIVE_TOPOLOGY m_primitive_topology = {};

	GSTexture12* m_current_render_target = nullptr;
	GSTexture12* m_current_depth_target = nullptr;

	D3D12_VIEWPORT m_viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
	GSVector4i m_scissor = GSVector4i::zero();
	u8 m_blend_constant_color = 0;
	u8 m_stencil_ref = 0;
	bool m_in_render_pass = false;

	std::array<D3D12_GPU_VIRTUAL_ADDRESS, NUM_TFX_CONSTANT_BUFFERS> m_tfx_constant_buffers{};
	std::array<D3D12DescriptorHandle, NUM_TOTAL_TFX_TEXTURES> m_tfx_textures{};
	D3D12DescriptorHandle m_tfx_sampler;
	u32 m_tfx_sampler_sel = 0;
	D3D12DescriptorHandle m_tfx_textures_handle_gpu;
	D3D12DescriptorHandle m_tfx_samplers_handle_gpu;
	D3D12DescriptorHandle m_tfx_rt_textures_handle_gpu;

	D3D12DescriptorHandle m_utility_texture_cpu;
	D3D12DescriptorHandle m_utility_texture_gpu;
	D3D12DescriptorHandle m_utility_sampler_cpu;
	D3D12DescriptorHandle m_utility_sampler_gpu;

	RootSignature m_current_root_signature = RootSignature::Undefined;
	const ID3D12PipelineState* m_current_pipeline = nullptr;

	std::unique_ptr<GSTexture12> m_null_texture;

	// current pipeline selector - we save this in the struct to avoid re-zeroing it every draw
	PipelineSelector m_pipeline_selector = {};

	void ResetAPIState();
	void RestoreAPIState();
};
