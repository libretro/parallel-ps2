/* The stub D3D12 driver's log, its objects and the methods that answer
 * more than S_OK (hook_<family>_<method>, which gen_stubs.py's vtables
 * hand over to), D3DCompile, CreateDXGIFactory2, and threads run as fibers
 * in step: the device is all that runs, and it runs the same each time. */
#define COBJMACROS
#define CINTERFACE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>

#include <rthreads/retro_eventcount.h>
#include <rthreads/rthreads.h>

#include "stubs.h"

u64 g_hash  = 1469598103934665603ULL;
u64 g_words = 0;
stub_config g_cfg;
int g_dead_calls;
static FILE* s_dump;
static u32 s_next_id;
static stub_obj* s_objs;
#define MAX_OBJS 400000
static stub_obj* s_by_id[MAX_OBJS];

void log_word(u64 v)
{
	int i;
	for (i = 0; i < 8; i++)
	{
		g_hash ^= (v >> (i * 8)) & 0xFF;
		g_hash *= 1099511628211ULL;
	}
	g_words++;
	if (s_dump || getenv("D3D12DEV_DUMP"))
	{
		if (!s_dump)
			s_dump = fopen(getenv("D3D12DEV_DUMP"), "w");
		if (s_dump)
			fprintf(s_dump, "%llx\n", (unsigned long long)v);
	}
}

void log_float(float f)
{
	u32 v;
	memcpy(&v, &f, sizeof(v));
	log_word(v);
}

void log_bytes(const void* p, size_t n)
{
	const unsigned char* b = (const unsigned char*)p;
	size_t i;
	log_word(n);
	for (i = 0; i < n; i += 8)
	{
		u64 v = 0;
		size_t k;
		for (k = 0; k < 8 && i + k < n; k++)
			v |= (u64)b[i + k] << (k * 8);
		log_word(v);
	}
}

void log_string(const char* s)
{
	log_bytes(s, strlen(s));
}

void log_close(void)
{
	if (s_dump)
		fclose(s_dump);
	s_dump = NULL;
}

void log_name(const char* s)
{
	if (s_dump)
		fprintf(s_dump, "%s\n", s);
	log_string(s);
}

/* ==== the objects ==== */

void* stub_new(unsigned kind)
{
	stub_obj* o = (stub_obj*)calloc(1, sizeof(*o));
	o->lpVtbl   = stub_vtbl(kind);
	o->refs     = 1;
	o->id       = ++s_next_id;
	o->kind     = kind;
	o->next     = s_objs;
	s_objs      = o;
	if (o->id < MAX_OBJS)
		s_by_id[o->id] = o;
	return o;
}

u64 stub_id(const void* obj)
{
	return obj ? ((const stub_obj*)obj)->id : 0;
}

void stub_call(stub_obj* o, const char* name)
{
	if (o->refs <= 0)
	{
		g_dead_calls++;
		log_name("DEAD");
	}
	log_name(name);
	log_word(o->id);
}

void stub_log_objs(void* const* objs, unsigned n)
{
	unsigned i;
	log_word(n);
	if (!objs)
	{
		log_word(0);
		return;
	}
	for (i = 0; i < n; i++)
		log_word(stub_id(objs[i]));
}

/* Objects the device still holds: everything but the frontend's */
unsigned stub_live_objects(void)
{
	unsigned n = 0;
	unsigned by_kind[K_COUNT];
	stub_obj* o;
	memset(by_kind, 0, sizeof(by_kind));
	for (o = s_objs; o; o = o->next)
		if (o->refs > 0 && o->kind != K_DEVICE && o->kind != K_QUEUE)
		{
			n++;
			by_kind[o->kind]++;
		}
	if (getenv("D3D12DEV_VERBOSE"))
	{
		unsigned k;
		for (k = 0; k < K_COUNT; k++)
			if (by_kind[k])
				printf("  kind %u: %u left\n", k, by_kind[k]);
	}
	return n;
}

/* (a GPU address: the resource's id, and the offset in it) */
#define VA_OF(o) ((u64)(o)->id << 32)
static stub_obj* va_resource(u64 va, size_t* offset)
{
	const u64 id = va >> 32;
	*offset      = (size_t)(va & 0xFFFFFFFFu);
	return (id && id < MAX_OBJS) ? s_by_id[id] : NULL;
}

/* n bytes of b from offset, as far as it goes */
static void log_region(const stub_obj* b, size_t offset, size_t n)
{
	if (!b || !b->mem || offset >= b->size)
	{
		log_word(0);
		return;
	}
	if (n > b->size - offset)
		n = b->size - offset;
	log_bytes(b->mem + offset, n);
}

HRESULT hook_obj_QueryInterface(stub_obj* o, REFIID riid, void** out)
{
	stub_call(o, "QueryInterface");
	log_bytes(riid, sizeof(GUID));
	/* (no debug layer, and nothing else asked for) */
	*out = NULL;
	return E_NOINTERFACE;
}

ULONG hook_obj_AddRef(stub_obj* o)
{
	if (o->refs <= 0)
	{
		g_dead_calls++;
		log_name("DEAD AddRef");
	}
	return (ULONG)++o->refs;
}

ULONG hook_obj_Release(stub_obj* o)
{
	if (o->refs <= 0)
	{
		g_dead_calls++;
		log_name("DEAD Release");
		return 0;
	}
	if (--o->refs == 0)
	{
		free(o->mem);
		o->mem = NULL;
	}
	return (ULONG)o->refs;
}

static HRESULT new_out(void** out, unsigned kind)
{
	stub_obj* o = (stub_obj*)stub_new(kind);
	*out        = o;
	log_word(o->id);
	return S_OK;
}

/* ==== the device ==== */

HRESULT hook_dev_CheckFeatureSupport(stub_obj* d, D3D12_FEATURE feature, void* data, UINT size)
{
	stub_call(d, "CheckFeatureSupport");
	log_word(feature);
	log_word(size);
	if (feature == D3D12_FEATURE_FORMAT_SUPPORT)
	{
		D3D12_FEATURE_DATA_FORMAT_SUPPORT* f = (D3D12_FEATURE_DATA_FORMAT_SUPPORT*)data;
		log_word(f->Format);
		/* (BC7 missing on the tier 1 device) */
		f->Support1 = (f->Format == DXGI_FORMAT_BC7_UNORM && g_cfg.heap_tier < 2)
			? D3D12_FORMAT_SUPPORT1_NONE
			: (D3D12_FORMAT_SUPPORT1)(D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE);
		f->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
		return S_OK;
	}
	memset(data, 0, size);
	if (feature == D3D12_FEATURE_D3D12_OPTIONS)
		((D3D12_FEATURE_DATA_D3D12_OPTIONS*)data)->ResourceHeapTier =
			g_cfg.heap_tier >= 2 ? D3D12_RESOURCE_HEAP_TIER_2 : D3D12_RESOURCE_HEAP_TIER_1;
	return S_OK;
}

D3D12_RESOURCE_ALLOCATION_INFO* hook_dev_GetResourceAllocationInfo(stub_obj* d, D3D12_RESOURCE_ALLOCATION_INFO* ret,
	UINT mask, UINT count, const D3D12_RESOURCE_DESC* descs)
{
	stub_call(d, "GetResourceAllocationInfo");
	log_word(mask);
	log_bytes(descs, count * sizeof(*descs));
	ret->Alignment   = 65536;
	ret->SizeInBytes = (descs->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		? descs->Width
		: descs->Width * descs->Height * 16 * (descs->MipLevels ? descs->MipLevels : 1);
	ret->SizeInBytes = (ret->SizeInBytes + 65535) & ~(u64)65535;
	return ret;
}

HRESULT hook_dev_CreateHeap(stub_obj* d, const D3D12_HEAP_DESC* desc, REFIID riid, void** out)
{
	stub_call(d, "CreateHeap");
	log_bytes(desc, sizeof(*desc));
	return new_out(out, K_HEAP);
}

HRESULT hook_dev_CreatePlacedResource(stub_obj* d, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
	D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out)
{
	stub_obj* r;
	stub_call(d, "CreatePlacedResource");
	log_word(stub_id(heap));
	log_word(offset);
	log_bytes(desc, sizeof(*desc));
	log_word(state);
	if (clear)
		log_bytes(clear, sizeof(*clear));
	else
		log_word(0);
	r = (stub_obj*)stub_new(K_RESOURCE);
	memcpy(r->desc, desc, sizeof(*desc));
	r->buffer = desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
	if (r->buffer)
	{
		r->size = (size_t)desc->Width;
		r->mem  = (u8*)calloc(1, r->size ? r->size : 1);
	}
	*out = r;
	log_word(r->id);
	return S_OK;
}

HRESULT hook_dev_CreateDescriptorHeap(stub_obj* d, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out)
{
	stub_call(d, "CreateDescriptorHeap");
	log_bytes(desc, sizeof(*desc));
	return new_out(out, K_DHEAP);
}

UINT hook_dev_GetDescriptorHandleIncrementSize(stub_obj* d, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	stub_call(d, "GetDescriptorHandleIncrementSize");
	log_word(type);
	return 32;
}

HRESULT hook_dev_CreateCommandAllocator(stub_obj* d, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** out)
{
	stub_call(d, "CreateCommandAllocator");
	log_word(type);
	return new_out(out, K_ALLOCATOR);
}

HRESULT hook_dev_CreateCommandList(stub_obj* d, UINT mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* a,
	ID3D12PipelineState* pso, REFIID riid, void** out)
{
	stub_call(d, "CreateCommandList");
	log_word(type);
	log_word(stub_id(a));
	log_word(stub_id(pso));
	return new_out(out, K_LIST);
}

HRESULT hook_dev_CreateFence(stub_obj* d, UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID riid, void** out)
{
	HRESULT hr;
	stub_call(d, "CreateFence");
	log_word(initial);
	hr                       = new_out(out, K_FENCE);
	((stub_obj*)*out)->value = initial;
	return hr;
}

HRESULT hook_dev_CreateRootSignature(stub_obj* d, UINT mask, const void* code, SIZE_T len, REFIID riid, void** out)
{
	stub_call(d, "CreateRootSignature");
	log_bytes(code, len);
	return new_out(out, K_ROOTSIG);
}

static void log_shader(const D3D12_SHADER_BYTECODE* s)
{
	if (s->pShaderBytecode)
		log_bytes(s->pShaderBytecode, s->BytecodeLength);
	else
		log_word(0);
}

HRESULT hook_dev_CreateGraphicsPipelineState(stub_obj* d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid,
	void** out)
{
	UINT i;
	stub_call(d, "CreateGraphicsPipelineState");
	log_word(stub_id(desc->pRootSignature));
	log_shader(&desc->VS);
	log_shader(&desc->PS);
	log_shader(&desc->DS);
	log_shader(&desc->HS);
	log_shader(&desc->GS);
	log_bytes(&desc->BlendState, sizeof(desc->BlendState));
	log_word(desc->SampleMask);
	log_bytes(&desc->RasterizerState, sizeof(desc->RasterizerState));
	log_bytes(&desc->DepthStencilState, sizeof(desc->DepthStencilState));
	log_word(desc->InputLayout.NumElements);
	for (i = 0; i < desc->InputLayout.NumElements; i++)
	{
		const D3D12_INPUT_ELEMENT_DESC* e = &desc->InputLayout.pInputElementDescs[i];
		log_string(e->SemanticName);
		log_word(e->SemanticIndex);
		log_word(e->Format);
		log_word(e->InputSlot);
		log_word(e->AlignedByteOffset);
		log_word(e->InputSlotClass);
		log_word(e->InstanceDataStepRate);
	}
	log_word(desc->IBStripCutValue);
	log_word(desc->PrimitiveTopologyType);
	log_word(desc->NumRenderTargets);
	log_bytes(desc->RTVFormats, sizeof(desc->RTVFormats));
	log_word(desc->DSVFormat);
	log_bytes(&desc->SampleDesc, sizeof(desc->SampleDesc));
	log_word(desc->NodeMask);
	log_word(desc->CachedPSO.CachedBlobSizeInBytes);
	log_word(desc->Flags);
	return new_out(out, K_PSO);
}

static void view(stub_obj* d, const char* name, ID3D12Resource* res, const void* desc, size_t size,
	D3D12_CPU_DESCRIPTOR_HANDLE h)
{
	stub_call(d, name);
	log_word(stub_id(res));
	if (desc)
		log_bytes(desc, size);
	else
		log_word(0);
	log_word(h.ptr);
}

void hook_dev_CreateShaderResourceView(stub_obj* d, ID3D12Resource* res, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
	D3D12_CPU_DESCRIPTOR_HANDLE h)
{
	view(d, "CreateShaderResourceView", res, desc, sizeof(*desc), h);
}

void hook_dev_CreateRenderTargetView(stub_obj* d, ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
	D3D12_CPU_DESCRIPTOR_HANDLE h)
{
	view(d, "CreateRenderTargetView", res, desc, sizeof(*desc), h);
}

void hook_dev_CreateDepthStencilView(stub_obj* d, ID3D12Resource* res, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
	D3D12_CPU_DESCRIPTOR_HANDLE h)
{
	view(d, "CreateDepthStencilView", res, desc, sizeof(*desc), h);
}

LUID* hook_dev_GetAdapterLuid(stub_obj* d, LUID* ret)
{
	stub_call(d, "GetAdapterLuid");
	ret->LowPart  = 0x1234;
	ret->HighPart = 0;
	return ret;
}

/* ==== the queue and the fence ==== */

HRESULT hook_queue_Signal(stub_obj* q, ID3D12Fence* fence, UINT64 value)
{
	stub_call(q, "Signal");
	log_word(stub_id(fence));
	log_word(value);
	((stub_obj*)fence)->value = value;
	return S_OK;
}

UINT64 hook_fence_GetCompletedValue(stub_obj* f)
{
	stub_call(f, "GetCompletedValue");
	return f->value;
}

HRESULT hook_fence_SetEventOnCompletion(stub_obj* f, UINT64 value, HANDLE ev)
{
	stub_call(f, "SetEventOnCompletion");
	log_word(value);
	/* (everything submitted has run by the time it is waited for) */
	f->value = value > f->value ? value : f->value;
	SetEvent(ev);
	return S_OK;
}

/* ==== descriptor heaps, resources, pipelines ==== */

D3D12_CPU_DESCRIPTOR_HANDLE* hook_dheap_GetCPUDescriptorHandleForHeapStart(stub_obj* h, D3D12_CPU_DESCRIPTOR_HANDLE* ret)
{
	ret->ptr = (SIZE_T)h->id << 24;
	return ret;
}

D3D12_GPU_DESCRIPTOR_HANDLE* hook_dheap_GetGPUDescriptorHandleForHeapStart(stub_obj* h, D3D12_GPU_DESCRIPTOR_HANDLE* ret)
{
	ret->ptr = ((UINT64)h->id << 24) | (1ull << 62);
	return ret;
}

HRESULT hook_res_Map(stub_obj* r, UINT sub, const D3D12_RANGE* range, void** data)
{
	stub_call(r, "Map");
	log_word(sub);
	if (!r->mem)
		return E_FAIL;
	if (data)
		*data = r->mem;
	return S_OK;
}

D3D12_GPU_VIRTUAL_ADDRESS hook_res_GetGPUVirtualAddress(stub_obj* r)
{
	return VA_OF(r);
}

D3D12_RESOURCE_DESC* hook_res_GetDesc(stub_obj* r, D3D12_RESOURCE_DESC* ret)
{
	memcpy(ret, r->desc, sizeof(*ret));
	return ret;
}

/* ==== the blobs ==== */

void* hook_blob_GetBufferPointer(stub_obj* o)
{
	return o->mem;
}

SIZE_T hook_blob_GetBufferSize(stub_obj* o)
{
	return o->size;
}

static ID3DBlob* new_blob(const void* data, size_t size)
{
	stub_obj* b = (stub_obj*)stub_new(K_BLOB);
	b->size     = size;
	b->mem      = (u8*)calloc(1, size ? size : 1);
	if (data)
		memcpy(b->mem, data, size);
	return (ID3DBlob*)b;
}

HRESULT hook_pso_GetCachedBlob(stub_obj* p, ID3DBlob** out)
{
	const u64 v = p->id;
	stub_call(p, "GetCachedBlob");
	*out = new_blob(&v, sizeof(v));
	return S_OK;
}

HRESULT WINAPI D3DCreateBlob(SIZE_T size, ID3DBlob** out)
{
	log_name("D3DCreateBlob");
	log_word(size);
	*out = new_blob(NULL, size);
	return S_OK;
}

static u64 fnv(u64 h, const void* p, size_t n)
{
	const unsigned char* b = (const unsigned char*)p;
	size_t i;
	for (i = 0; i < n; i++)
	{
		h ^= b[i];
		h *= 1099511628211ULL;
	}
	return h;
}

/* The shader: what it was compiled from, folded into its bytes */
HRESULT WINAPI D3DCompile(const void* data, SIZE_T data_size, const char* filename, const D3D_SHADER_MACRO* defines,
	ID3DInclude* include, const char* entrypoint, const char* target, UINT sflags, UINT eflags, ID3DBlob** shader,
	ID3DBlob** error_messages)
{
	u64 code[2];
	(void)filename;
	(void)include;
	code[0] = fnv(1469598103934665603ULL, data, data_size);
	code[1] = fnv(code[0], entrypoint, strlen(entrypoint));
	log_name("D3DCompile");
	log_word(code[0]);
	for (; defines && defines->Name; defines++)
	{
		log_string(defines->Name);
		log_string(defines->Definition);
		code[1] = fnv(code[1], defines->Name, strlen(defines->Name));
		code[1] = fnv(code[1], defines->Definition, strlen(defines->Definition));
	}
	log_string(entrypoint);
	log_string(target);
	log_word(sflags);
	log_word(eflags);
	code[1] = fnv(code[1], target, strlen(target));
	*shader = new_blob(code, sizeof(code));
	if (error_messages)
		*error_messages = NULL;
	return S_OK;
}

/* ==== DXGI ==== */

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** out)
{
	log_name("CreateDXGIFactory2");
	log_word(flags);
	return new_out(out, K_FACTORY);
}

HRESULT hook_factory_EnumAdapterByLuid(stub_obj* f, LUID luid, REFIID riid, void** out)
{
	stub_call(f, "EnumAdapterByLuid");
	log_bytes(&luid, sizeof(luid));
	return new_out(out, K_ADAPTER);
}

HRESULT hook_adapter_GetDesc(stub_obj* a, DXGI_ADAPTER_DESC* desc)
{
	stub_call(a, "GetDesc");
	memset(desc, 0, sizeof(*desc));
	desc->VendorId = g_cfg.vendor;
	return S_OK;
}

/* ==== the command list ==== */

static struct
{
	D3D12_VERTEX_BUFFER_VIEW vb;
	D3D12_INDEX_BUFFER_VIEW ib;
	u64 vs_srv;
} s_ia;

static void log_resource_barrier(const D3D12_RESOURCE_BARRIER* b)
{
	log_word(b->Type);
	log_word(b->Flags);
	switch (b->Type)
	{
		case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
			log_word(stub_id(b->Transition.pResource));
			log_word(b->Transition.Subresource);
			log_word(b->Transition.StateBefore);
			log_word(b->Transition.StateAfter);
			break;
		case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
			log_word(stub_id(b->Aliasing.pResourceBefore));
			log_word(stub_id(b->Aliasing.pResourceAfter));
			break;
		default:
			log_word(stub_id(b->UAV.pResource));
			break;
	}
}

void hook_list_ResourceBarrier(stub_obj* l, UINT n, const D3D12_RESOURCE_BARRIER* barriers)
{
	UINT i;
	stub_call(l, "ResourceBarrier");
	log_word(n);
	for (i = 0; i < n; i++)
		log_resource_barrier(&barriers[i]);
}

static void log_copy_location(const D3D12_TEXTURE_COPY_LOCATION* loc)
{
	log_word(stub_id(loc->pResource));
	log_word(loc->Type);
	if (loc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
		log_word(loc->SubresourceIndex);
	else
		log_bytes(&loc->PlacedFootprint, sizeof(loc->PlacedFootprint));
}

void hook_list_CopyTextureRegion(stub_obj* l, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
	const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box)
{
	stub_call(l, "CopyTextureRegion");
	log_copy_location(dst);
	log_word(x);
	log_word(y);
	log_word(z);
	log_copy_location(src);
	if (box)
		log_bytes(box, sizeof(*box));
	else
		log_word(0);
}

void hook_list_CopyBufferRegion(stub_obj* l, ID3D12Resource* dst, UINT64 dst_offset, ID3D12Resource* src,
	UINT64 src_offset, UINT64 count)
{
	stub_obj* d = (stub_obj*)dst;
	stub_obj* s = (stub_obj*)src;
	stub_call(l, "CopyBufferRegion");
	log_word(stub_id(dst));
	log_word(dst_offset);
	log_word(stub_id(src));
	log_word(src_offset);
	log_word(count);
	/* (done at once: nothing reads it before the list would have run) */
	if (d->mem && s->mem && dst_offset + count <= d->size && src_offset + count <= s->size)
		memcpy(d->mem + dst_offset, s->mem + src_offset, (size_t)count);
}

void hook_list_SetGraphicsRoot32BitConstants(stub_obj* l, UINT index, UINT count, const void* data, UINT offset)
{
	stub_call(l, "SetGraphicsRoot32BitConstants");
	log_word(index);
	log_bytes(data, count * 4u);
	log_word(offset);
}

void hook_list_IASetVertexBuffers(stub_obj* l, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* views)
{
	stub_call(l, "IASetVertexBuffers");
	log_word(start);
	log_bytes(views, n * sizeof(*views));
	if (start == 0 && n)
		s_ia.vb = views[0];
}

void hook_list_IASetIndexBuffer(stub_obj* l, const D3D12_INDEX_BUFFER_VIEW* view)
{
	stub_call(l, "IASetIndexBuffer");
	log_bytes(view, sizeof(*view));
	s_ia.ib = *view;
}

void hook_list_SetGraphicsRootShaderResourceView(stub_obj* l, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va)
{
	stub_call(l, "SetGraphicsRootShaderResourceView");
	log_word(index);
	log_word(va);
	s_ia.vs_srv = va;
}

void hook_list_DrawInstanced(stub_obj* l, UINT count, UINT instances, UINT start, UINT start_instance)
{
	size_t offset;
	const stub_obj* vb = va_resource(s_ia.vb.BufferLocation, &offset);
	stub_call(l, "DrawInstanced");
	log_word(count);
	log_word(instances);
	log_word(start);
	log_word(start_instance);
	log_region(vb, offset + (size_t)start * s_ia.vb.StrideInBytes, (size_t)count * s_ia.vb.StrideInBytes);
}

/* The indices, and the vertices they reach: through the vertex buffer,
 * and from the vertex shader's buffer (two indices a vertex at most) */
void hook_list_DrawIndexedInstanced(stub_obj* l, UINT count, UINT instances, UINT start, INT base, UINT start_instance)
{
	size_t ib_off, vb_off, srv_off;
	const stub_obj* ib  = va_resource(s_ia.ib.BufferLocation, &ib_off);
	const stub_obj* vb  = va_resource(s_ia.vb.BufferLocation, &vb_off);
	const stub_obj* srv = va_resource(s_ia.vs_srv, &srv_off);
	u32 max             = 0;
	UINT i;
	stub_call(l, "DrawIndexedInstanced");
	log_word(count);
	log_word(instances);
	log_word(start);
	log_word((u64)(s64)base);
	log_word(start_instance);
	if (!ib || !ib->mem)
		return;
	log_region(ib, ib_off + (size_t)start * 2, (size_t)count * 2);
	for (i = 0; i < count; i++)
	{
		const size_t at = ib_off + ((size_t)start + i) * 2;
		u16 v;
		if (at + 2 > ib->size)
			break;
		memcpy(&v, ib->mem + at, 2);
		if (v > max)
			max = v;
	}
	log_region(vb, vb_off + (size_t)base * s_ia.vb.StrideInBytes, ((size_t)max + 1) * s_ia.vb.StrideInBytes);
	log_region(srv, srv_off, ((size_t)(max >> 1) + 2) * 32);
}

static void log_begin_access(const D3D12_RENDER_PASS_BEGINNING_ACCESS* a, int depth)
{
	log_word(a->Type);
	if (a->Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
	{
		log_word(a->Clear.ClearValue.Format);
		if (depth)
		{
			log_float(a->Clear.ClearValue.DepthStencil.Depth);
			log_word(a->Clear.ClearValue.DepthStencil.Stencil);
		}
		else
			log_bytes(a->Clear.ClearValue.Color, sizeof(a->Clear.ClearValue.Color));
	}
}

void hook_list_BeginRenderPass(stub_obj* l, UINT n, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts,
	const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS flags)
{
	UINT i;
	stub_call(l, "BeginRenderPass");
	log_word(n);
	for (i = 0; i < n; i++)
	{
		log_word(rts[i].cpuDescriptor.ptr);
		log_begin_access(&rts[i].BeginningAccess, 0);
		log_word(rts[i].EndingAccess.Type);
	}
	if (ds)
	{
		log_word(ds->cpuDescriptor.ptr);
		log_begin_access(&ds->DepthBeginningAccess, 1);
		log_begin_access(&ds->StencilBeginningAccess, 1);
		log_word(ds->DepthEndingAccess.Type);
		log_word(ds->StencilEndingAccess.Type);
	}
	else
		log_word(0);
	log_word(flags);
}

/* ==== threads, as fibers in step ====
 *
 * A thread is a fiber that runs when the drawing thread waits for it or
 * tells it there is work, until it waits itself: then the drawing thread
 * goes on where it was. What it makes is ready at the same point every
 * run. */

struct sthread
{
	void* fiber;
	void (*fn)(void*);
	void* data;
	int finished;
};

static void* s_main_fiber;
static struct sthread* s_current; /* the thread running, or NULL: the drawing thread */
static struct sthread* s_thread;  /* (one at a time) */

static void WINAPI fiber_entry(void* p)
{
	struct sthread* t = (struct sthread*)p;
	t->fn(t->data);
	t->finished = 1;
	s_current   = NULL;
	SwitchToFiber(s_main_fiber);
}

static void run_thread(struct sthread* t)
{
	if (!t || t->finished || s_current)
		return;
	s_current = t;
	SwitchToFiber(t->fiber);
}

sthread_t* sthread_create(void (*thread_func)(void*), void* userdata)
{
	struct sthread* t = (struct sthread*)calloc(1, sizeof(*t));
	if (!s_main_fiber)
		s_main_fiber = ConvertThreadToFiber(NULL);
	t->fn    = thread_func;
	t->data  = userdata;
	t->fiber = CreateFiber(256 * 1024, fiber_entry, t);
	s_thread = t;
	return (sthread_t*)t;
}

void sthread_join(sthread_t* thread)
{
	struct sthread* t = (struct sthread*)thread;
	while (!t->finished)
		run_thread(t);
	DeleteFiber(t->fiber);
	if (s_thread == t)
		s_thread = NULL;
	free(t);
}

bool retro_eventcount_init(retro_eventcount_t* ec)
{
	memset(ec, 0, sizeof(*ec));
	return true;
}

void retro_eventcount_free(retro_eventcount_t* ec)
{
	(void)ec;
}

void retro_eventcount_notify(retro_eventcount_t* ec)
{
	(void)ec;
	if (!s_current)
		run_thread(s_thread);
}

int retro_eventcount_prepare_wait(retro_eventcount_t* ec)
{
	(void)ec;
	return 0;
}

void retro_eventcount_cancel_wait(retro_eventcount_t* ec)
{
	(void)ec;
}

void retro_eventcount_commit_wait(retro_eventcount_t* ec, int key)
{
	(void)ec;
	(void)key;
	if (s_current)
	{
		/* the thread waits: back to the drawing thread */
		s_current = NULL;
		SwitchToFiber(s_main_fiber);
	}
	else
		run_thread(s_thread);
}
