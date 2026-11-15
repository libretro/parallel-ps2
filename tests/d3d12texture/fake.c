/* The fake device and D3D12 objects (fake.h), in C's view of the COM
 * interfaces: built the same for the C textures and for the C++ oracle. */
#define COBJMACROS
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <d3d12.h>

#include "fake.h"

unsigned long long fk_hash = 1469598103934665603ull;
unsigned long long fk_fence = 1, fk_completed = 0;

void fk_h(const void* p, size_t n)
{
	const unsigned char* c = (const unsigned char*)p;
	size_t i;
	for (i = 0; i < n; i++)
	{
		fk_hash ^= c[i];
		fk_hash *= 1099511628211ull;
	}
}
void fk_hu(unsigned long long v) { fk_h(&v, sizeof(v)); }

/* --- resources --- */

typedef struct fk_res
{
	ID3D12ResourceVtbl* lpVtbl;
	ULONG refs;
	unsigned long long id;
	unsigned char* mem;
	size_t size;
} fk_res;

static unsigned long long fk_next_id = 1;
static int fk_live;
static ID3D12ResourceVtbl fk_res_vtbl;

unsigned long long fk_resource_id(const void* resource)
{
	return resource ? ((const fk_res*)resource)->id : 0;
}

static ULONG STDMETHODCALLTYPE fk_res_addref(ID3D12Resource* t) { return ++((fk_res*)t)->refs; }
static ULONG STDMETHODCALLTYPE fk_res_release(ID3D12Resource* t)
{
	fk_res* r = (fk_res*)t;
	if (--r->refs)
		return r->refs;
	fk_hu(0xDEAD0000ull ^ r->id);
	fk_live--;
	free(r->mem);
	free(r);
	return 0;
}
static HRESULT STDMETHODCALLTYPE fk_res_map(ID3D12Resource* t, UINT sub, const D3D12_RANGE* range, void** p)
{
	fk_res* r = (fk_res*)t;
	fk_hu(0x3A90000ull ^ r->id);
	fk_hu(sub);
	if (range)
	{
		fk_hu(range->Begin);
		fk_hu(range->End);
	}
	if (!r->mem)
		return E_FAIL;
	*p = r->mem;
	return S_OK;
}
static void STDMETHODCALLTYPE fk_res_unmap(ID3D12Resource* t, UINT sub, const D3D12_RANGE* range)
{
	fk_hu(0x07A90000ull ^ ((fk_res*)t)->id);
	fk_hu(sub);
	if (range)
	{
		fk_hu(range->Begin);
		fk_hu(range->End);
	}
}
static D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE fk_res_gpuva(ID3D12Resource* t)
{
	return ((fk_res*)t)->id << 32;
}

static fk_res* fk_new_res(size_t size)
{
	fk_res* r = (fk_res*)calloc(1, sizeof(*r));
	fk_res_vtbl.AddRef               = fk_res_addref;
	fk_res_vtbl.Release              = fk_res_release;
	fk_res_vtbl.Map                  = fk_res_map;
	fk_res_vtbl.Unmap                = fk_res_unmap;
	fk_res_vtbl.GetGPUVirtualAddress = fk_res_gpuva;
	r->lpVtbl = &fk_res_vtbl;
	r->refs   = 1;
	r->id     = fk_next_id++;
	if (size)
	{
		r->mem  = (unsigned char*)calloc(1, size);
		r->size = size;
	}
	fk_live++;
	return r;
}

int fk_live_resources(void) { return fk_live; }

/* Placed: a texture wider than 2048 has no room; a buffer gets memory */
int fk_create_placed(const void* vdesc, unsigned heap_type, unsigned state, const void* vclear, void** resource,
	gs_d3d12_alloc_t* alloc)
{
	const D3D12_RESOURCE_DESC* desc = (const D3D12_RESOURCE_DESC*)vdesc;
	const D3D12_CLEAR_VALUE* clear  = (const D3D12_CLEAR_VALUE*)vclear;
	fk_res* r;

	fk_hu(0xC2EA7Eull);
	fk_hu(desc->Dimension); fk_hu(desc->Width); fk_hu(desc->Height); fk_hu(desc->DepthOrArraySize);
	fk_hu(desc->MipLevels); fk_hu(desc->Format); fk_hu(desc->SampleDesc.Count); fk_hu(desc->Layout);
	fk_hu(desc->Flags); fk_hu(heap_type); fk_hu(state);
	fk_hu(clear ? (unsigned long long)clear->Format : 0xFFFFull);
	if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc->Width > 2048)
		return 0;
	r = fk_new_res(desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? (size_t)desc->Width : 0);
	alloc->heap      = (void*)(size_t)0x1000;
	alloc->offset    = r->id * 0x10000;
	alloc->size      = desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? desc->Width : 0x10000;
	alloc->heap_type = heap_type;
	*resource        = r;
	return 1;
}

/* deferred: a reference kept until the list that named it completes */
typedef struct fk_pending
{
	unsigned long long fence;
	fk_res* res;
	unsigned kind, index;
} fk_pending;
static fk_pending fk_pend[1 << 16];
static unsigned fk_npend;

void fk_defer_resource(const gs_d3d12_alloc_t* alloc, void* resource)
{
	if (!resource)
		return;
	fk_hu(0xDEF00000ull ^ ((fk_res*)resource)->id);
	fk_hu(alloc ? alloc->offset : 0xFFFF);
	ID3D12Resource_AddRef((ID3D12Resource*)resource);
	fk_pend[fk_npend].fence = fk_fence;
	fk_pend[fk_npend].res   = (fk_res*)resource;
	fk_npend++;
}

static gs_d3d12_descriptor_heap_manager fk_heaps[3];

gs_d3d12_descriptor_heap_manager* fk_heap(unsigned kind) { return &fk_heaps[kind]; }

void fk_defer_descriptor(unsigned kind, unsigned index)
{
	fk_hu(0xDE5C0000ull ^ (kind << 12) ^ index);
	fk_pend[fk_npend].fence = fk_fence;
	fk_pend[fk_npend].res   = NULL;
	fk_pend[fk_npend].kind  = kind;
	fk_pend[fk_npend].index = index;
	fk_npend++;
}

static void fk_retire(void)
{
	unsigned i, j = 0;
	for (i = 0; i < fk_npend; i++)
	{
		if (fk_pend[i].fence > fk_completed)
		{
			fk_pend[j++] = fk_pend[i];
			continue;
		}
		if (fk_pend[i].res)
			ID3D12Resource_Release((ID3D12Resource*)fk_pend[i].res);
		else
			gs_d3d12_descriptor_heap_manager_free_index(&fk_heaps[fk_pend[i].kind], fk_pend[i].index);
	}
	fk_npend = j;
}

void fk_end_render_pass(void) { fk_hu(0xE2D); }
void fk_execute(int wait)
{
	fk_hu(0xE8E); fk_hu((unsigned)wait);
	fk_fence++;
	if (wait)
		fk_completed = fk_fence - 1;
	fk_retire();
}
void fk_execute_for_readback(void)
{
	fk_hu(0x2EADBAC);
	fk_execute(1);
}
void fk_wait(unsigned long long counter)
{
	fk_hu(0xA17); fk_hu(counter);
	fk_completed = counter;
	fk_retire();
}
void fk_report(long hr) { fk_hu(0x2E9000ull ^ (unsigned long)hr); }
void fk_render_mipmap(const void* resource, unsigned dst_level, unsigned dst_w, unsigned dst_h, unsigned src_level,
	unsigned src_w, unsigned src_h)
{
	fk_hu(0x319A9ull ^ fk_resource_id(resource));
	fk_hu(dst_level); fk_hu(dst_w); fk_hu(dst_h); fk_hu(src_level); fk_hu(src_w); fk_hu(src_h);
}

/* --- the device's views --- */

static void STDMETHODCALLTYPE fk_srv(ID3D12Device* d, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
	D3D12_CPU_DESCRIPTOR_HANDLE h)
{
	(void)d;
	fk_hu(0x52Bull ^ fk_resource_id(r)); fk_hu(desc->Format); fk_hu(desc->ViewDimension);
	fk_hu(desc->Shader4ComponentMapping); fk_hu(desc->Texture2D.MostDetailedMip); fk_hu(desc->Texture2D.MipLevels);
	fk_hu(desc->Texture2D.PlaneSlice); fk_hu(h.ptr);
}
static void STDMETHODCALLTYPE fk_rtv(ID3D12Device* d, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
	D3D12_CPU_DESCRIPTOR_HANDLE h)
{
	(void)d;
	fk_hu(0x27Bull ^ fk_resource_id(r)); fk_hu(desc->Format); fk_hu(desc->ViewDimension);
	fk_hu(desc->Texture2D.MipSlice); fk_hu(h.ptr);
}
static void STDMETHODCALLTYPE fk_dsv(ID3D12Device* d, ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
	D3D12_CPU_DESCRIPTOR_HANDLE h)
{
	(void)d;
	fk_hu(0xD5Bull ^ fk_resource_id(r)); fk_hu(desc->Format); fk_hu(desc->ViewDimension); fk_hu(desc->Flags);
	fk_hu(desc->Texture2D.MipSlice); fk_hu(h.ptr);
}
static void STDMETHODCALLTYPE fk_uav(ID3D12Device* d, ID3D12Resource* r, ID3D12Resource* counter,
	const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
	(void)d;
	fk_hu(0xAABull ^ fk_resource_id(r)); fk_hu(fk_resource_id(counter)); fk_hu(desc->Format);
	fk_hu(desc->ViewDimension); fk_hu(desc->Texture2D.MipSlice); fk_hu(h.ptr);
}

static ID3D12DeviceVtbl fk_dev_vtbl;
static struct { ID3D12DeviceVtbl* lpVtbl; } fk_dev;
void* fk_device(void) { return &fk_dev; }

/* --- command lists --- */

typedef struct fk_cl
{
	ID3D12GraphicsCommandListVtbl* lpVtbl;
	unsigned id;
} fk_cl;
static ID3D12GraphicsCommandListVtbl fk_cl_vtbl;
static fk_cl fk_list = {NULL, 1}, fk_init_list = {NULL, 2};

void* fk_command_list(void) { return &fk_list; }
void* fk_init_command_list(void) { return &fk_init_list; }

static void STDMETHODCALLTYPE fk_barrier(ID3D12GraphicsCommandList* l, UINT n, const D3D12_RESOURCE_BARRIER* b)
{
	UINT i;
	fk_hu(0xBA22ull ^ ((fk_cl*)l)->id);
	for (i = 0; i < n; i++)
	{
		fk_hu(b[i].Type); fk_hu(b[i].Flags); fk_hu(fk_resource_id(b[i].Transition.pResource));
		fk_hu(b[i].Transition.Subresource); fk_hu(b[i].Transition.StateBefore); fk_hu(b[i].Transition.StateAfter);
	}
}

static void fk_hash_location(const D3D12_TEXTURE_COPY_LOCATION* loc)
{
	fk_hu(fk_resource_id(loc->pResource)); fk_hu(loc->Type);
	if (loc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
		fk_hu(loc->SubresourceIndex);
	else
	{
		fk_hu(loc->PlacedFootprint.Offset); fk_hu(loc->PlacedFootprint.Footprint.Format);
		fk_hu(loc->PlacedFootprint.Footprint.Width); fk_hu(loc->PlacedFootprint.Footprint.Height);
		fk_hu(loc->PlacedFootprint.Footprint.Depth); fk_hu(loc->PlacedFootprint.Footprint.RowPitch);
	}
}

static void STDMETHODCALLTYPE fk_copy_region(ID3D12GraphicsCommandList* l, const D3D12_TEXTURE_COPY_LOCATION* dst,
	UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box)
{
	fk_hu(0xC0B9ull ^ ((fk_cl*)l)->id);
	fk_hash_location(dst);
	fk_hu(x); fk_hu(y); fk_hu(z);
	fk_hash_location(src);
	if (box)
	{
		fk_hu(box->left); fk_hu(box->top); fk_hu(box->front); fk_hu(box->right); fk_hu(box->bottom); fk_hu(box->back);
	}
	/* the bytes an upload reads */
	if (src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && ((fk_res*)src->pResource)->mem)
	{
		const fk_res* r = (const fk_res*)src->pResource;
		size_t at       = (size_t)src->PlacedFootprint.Offset;
		size_t n        = (size_t)src->PlacedFootprint.Footprint.RowPitch * src->PlacedFootprint.Footprint.Height;
		if (at < r->size)
			fk_h(r->mem + at, n < r->size - at ? n : r->size - at);
	}
	/* a read-back writes what can be told apart */
	if (dst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && ((fk_res*)dst->pResource)->mem)
	{
		fk_res* r = (fk_res*)dst->pResource;
		size_t i;
		for (i = (size_t)dst->PlacedFootprint.Offset; i < r->size; i++)
			r->mem[i] = (unsigned char)(i * 7 + fk_fence);
	}
}

static void STDMETHODCALLTYPE fk_clear_dsv(ID3D12GraphicsCommandList* l, D3D12_CPU_DESCRIPTOR_HANDLE h,
	D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT n, const D3D12_RECT* rects)
{
	fk_hu(0xC1D5ull ^ ((fk_cl*)l)->id); fk_hu(h.ptr); fk_hu(flags); fk_h(&depth, sizeof(depth)); fk_hu(stencil);
	fk_hu(n); fk_hu(rects != NULL);
}
static void STDMETHODCALLTYPE fk_clear_rtv(ID3D12GraphicsCommandList* l, D3D12_CPU_DESCRIPTOR_HANDLE h,
	const FLOAT rgba[4], UINT n, const D3D12_RECT* rects)
{
	fk_hu(0xC127ull ^ ((fk_cl*)l)->id); fk_hu(h.ptr); fk_h(rgba, sizeof(FLOAT) * 4); fk_hu(n); fk_hu(rects != NULL);
}

/* --- the stream buffer's and the heaps' device --- */

static gs_d3d12_stream_buffer fk_sb;
gs_d3d12_stream_buffer* fk_stream(void) { return &fk_sb; }

int gs_d3d12_device_create_upload_buffer(u32 size, void** resource, gs_d3d12_alloc_t* alloc, u8** host_pointer,
	u64* gpu_pointer)
{
	fk_res* r = fk_new_res(size);
	alloc->heap = (void*)(size_t)0x2000;
	*resource     = r;
	*host_pointer = r->mem;
	*gpu_pointer  = r->id << 32;
	return 1;
}
void gs_d3d12_device_release_upload_buffer(void* resource, const gs_d3d12_alloc_t* alloc, u32 size, int defer)
{
	(void)alloc; (void)size; (void)defer;
	ID3D12Resource_Release((ID3D12Resource*)resource);
}
u64 gs_d3d12_device_current_fence(void) { return fk_fence; }
u64 gs_d3d12_device_completed_fence(void) { return fk_completed; }
void gs_d3d12_device_wait_fence(u64 counter) { fk_wait(counter); }

int gs_d3d12_device_create_descriptor_heap(void* device, u32 type, u32 num_descriptors, int shader_visible,
	void** heap, gs_d3d12_cpu_handle* base_cpu, gs_d3d12_gpu_handle* base_gpu, u32* increment)
{
	(void)device; (void)num_descriptors;
	*heap         = (void*)(size_t)(0x100 + type);
	base_cpu->ptr = (size_t)0x100000 * (type + 1);
	if (shader_visible)
		base_gpu->ptr = 0x8000000000ull * (type + 1);
	*increment = 32;
	return 1;
}
void gs_d3d12_device_release_descriptor_heap(void* heap) { (void)heap; }
void gs_d3d12_device_copy_sampler(void* device, gs_d3d12_cpu_handle dst, gs_d3d12_cpu_handle src)
{
	(void)device; (void)dst; (void)src;
}
void gs_d3d12_device_copy_sampler_group(void* device, gs_d3d12_cpu_handle dst, const gs_d3d12_descriptor_handle* src,
	u32 count)
{
	(void)device; (void)dst; (void)src; (void)count;
}

void fk_init(void)
{
	fk_dev_vtbl.CreateShaderResourceView  = fk_srv;
	fk_dev_vtbl.CreateRenderTargetView    = fk_rtv;
	fk_dev_vtbl.CreateDepthStencilView    = fk_dsv;
	fk_dev_vtbl.CreateUnorderedAccessView = fk_uav;
	fk_dev.lpVtbl                         = &fk_dev_vtbl;
	fk_cl_vtbl.ResourceBarrier            = fk_barrier;
	fk_cl_vtbl.CopyTextureRegion          = fk_copy_region;
	fk_cl_vtbl.ClearDepthStencilView      = fk_clear_dsv;
	fk_cl_vtbl.ClearRenderTargetView      = fk_clear_rtv;
	fk_list.lpVtbl                        = &fk_cl_vtbl;
	fk_init_list.lpVtbl                   = &fk_cl_vtbl;
	/* SRV (shader-visible), RTV, DSV: small, so a full heap is had */
	gs_d3d12_descriptor_heap_manager_init(&fk_heaps[0]);
	gs_d3d12_descriptor_heap_manager_init(&fk_heaps[1]);
	gs_d3d12_descriptor_heap_manager_init(&fk_heaps[2]);
	gs_d3d12_descriptor_heap_manager_create(&fk_heaps[0], NULL, 0, 40, 1);
	gs_d3d12_descriptor_heap_manager_create(&fk_heaps[1], NULL, 2, 12, 0);
	gs_d3d12_descriptor_heap_manager_create(&fk_heaps[2], NULL, 3, 6, 0);
	gs_d3d12_stream_buffer_init(&fk_sb);
	gs_d3d12_stream_buffer_create(&fk_sb, 512 * 1024);
}

void fk_shutdown(void)
{
	fk_execute(1);
	gs_d3d12_stream_buffer_destroy(&fk_sb, 0);
}
