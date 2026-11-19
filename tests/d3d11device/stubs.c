/* The stub D3D11 driver's log, its objects and the methods that answer
 * more than S_OK (hook_<family>_<method>, which gen_stubs.py's vtables
 * hand over to), D3DCompile, and a worker thread run in step: the device
 * is all that runs, and it runs the same each time. */
#define COBJMACROS
#define CINTERFACE

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi.h>

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

void log_word(u64 v)
{
	int i;
	for (i = 0; i < 8; i++)
	{
		g_hash ^= (v >> (i * 8)) & 0xFF;
		g_hash *= 1099511628211ULL;
	}
	g_words++;
	if (s_dump || getenv("D3D11DEV_DUMP"))
	{
		if (!s_dump)
			s_dump = fopen(getenv("D3D11DEV_DUMP"), "w");
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

/* (D3D11DEV_SEQ: each name and the hash before it, kept in memory and
 * written at the end, so the run is not changed by writing as it goes) */
#define SEQ_MAX 400000
static const char* s_seq_name[SEQ_MAX];
static u64 s_seq_hash[SEQ_MAX];
static unsigned s_seq;

void log_close(void)
{
	const char* path = getenv("D3D11DEV_SEQ");
	if (path)
	{
		FILE* f = fopen(path, "w");
		unsigned i;
		for (i = 0; f && i < s_seq; i++)
			fprintf(f, "%s %016llx\n", s_seq_name[i], (unsigned long long)s_seq_hash[i]);
		if (f)
			fclose(f);
	}
	if (s_dump)
		fclose(s_dump);
	s_dump = NULL;
}

void log_name(const char* s)
{
	if (s_seq < SEQ_MAX)
	{
		s_seq_name[s_seq] = s;
		s_seq_hash[s_seq++] = g_hash;
	}
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
		/* (a call on what was released) */
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
	unsigned by_kind[K_SAMPLER + 1];
	stub_obj* o;
	memset(by_kind, 0, sizeof(by_kind));
	for (o = s_objs; o; o = o->next)
		if (o->refs > 0 && o->kind != K_DEVICE && o->kind != K_CONTEXT && o->kind != K_DXGI_DEVICE
			&& o->kind != K_ADAPTER)
		{
			n++;
			by_kind[o->kind]++;
		}
	if (getenv("D3D11DEV_VERBOSE"))
	{
		unsigned k;
		for (k = 0; k <= K_SAMPLER; k++)
			if (by_kind[k])
				printf("  kind %u: %u left\n", k, by_kind[k]);
	}
	return n;
}

HRESULT hook_obj_QueryInterface(stub_obj* o, REFIID riid, void** out)
{
	stub_obj* r = NULL;
	stub_call(o, "QueryInterface");
	log_bytes(riid, sizeof(GUID));
	if (o->kind == K_DEVICE && (IsEqualGUID(riid, &IID_ID3D11Device1) || IsEqualGUID(riid, &IID_ID3D11Device)))
		r = o;
	else if (o->kind == K_DEVICE && IsEqualGUID(riid, &IID_IDXGIDevice))
	{
		if (!o->res)
			o->res = (stub_obj*)stub_new(K_DXGI_DEVICE);
		else
			o->res->refs++;
		*out = o->res;
		log_word(o->res->id);
		return S_OK;
	}
	else if (o->kind == K_CONTEXT && g_cfg.context1 && IsEqualGUID(riid, &IID_ID3D11DeviceContext1))
		r = o;
	if (!r)
	{
		*out = NULL;
		log_word(0);
		return E_NOINTERFACE;
	}
	r->refs++;
	*out = r;
	log_word(r->id);
	return S_OK;
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

/* ==== the device ==== */

HRESULT hook_dev_CreateBuffer(stub_obj* d, const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* init,
	ID3D11Buffer** out)
{
	stub_obj* b;
	stub_call(d, "CreateBuffer");
	log_bytes(desc, sizeof(*desc));
	if (init)
		log_bytes(init->pSysMem, desc->ByteWidth);
	if (!out)
		return S_FALSE;
	b       = (stub_obj*)stub_new(K_BUFFER);
	b->size = desc->ByteWidth;
	b->bind = desc->BindFlags;
	b->mem  = (u8*)calloc(1, b->size);
	if (init)
		memcpy(b->mem, init->pSysMem, b->size);
	*out = (ID3D11Buffer*)b;
	log_word(b->id);
	return S_OK;
}

HRESULT hook_dev_CreateTexture2D(stub_obj* d, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init,
	ID3D11Texture2D** out)
{
	stub_obj* t;
	stub_call(d, "CreateTexture2D");
	log_bytes(desc, sizeof(*desc));
	log_word(init != NULL);
	t = (stub_obj*)stub_new(K_TEXTURE2D);
	if (desc->CPUAccessFlags)
	{
		t->row_pitch = desc->Width * 16;
		t->size      = (size_t)t->row_pitch * desc->Height;
		t->mem       = (u8*)calloc(1, t->size);
	}
	*out = (ID3D11Texture2D*)t;
	log_word(t->id);
	return S_OK;
}

static HRESULT view(stub_obj* d, const char* name, unsigned kind, ID3D11Resource* res, const void* desc,
	size_t desc_size, void** out)
{
	stub_obj* v;
	stub_call(d, name);
	log_word(stub_id(res));
	if (desc)
		log_bytes(desc, desc_size);
	else
		log_word(0);
	if (!out)
		return S_FALSE;
	v      = (stub_obj*)stub_new(kind);
	v->res = (stub_obj*)res;
	*out   = v;
	log_word(v->id);
	return S_OK;
}

HRESULT hook_dev_CreateShaderResourceView(stub_obj* d, ID3D11Resource* res, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
	ID3D11ShaderResourceView** out)
{
	return view(d, "CreateShaderResourceView", K_SRV, res, desc, sizeof(*desc), (void**)out);
}

HRESULT hook_dev_CreateInputLayout(stub_obj* d, const D3D11_INPUT_ELEMENT_DESC* descs, UINT n, const void* code,
	SIZE_T len, ID3D11InputLayout** out)
{
	UINT i;
	stub_call(d, "CreateInputLayout");
	log_word(n);
	for (i = 0; i < n; i++)
	{
		log_string(descs[i].SemanticName);
		log_word(descs[i].SemanticIndex);
		log_word(descs[i].Format);
		log_word(descs[i].InputSlot);
		log_word(descs[i].AlignedByteOffset);
		log_word(descs[i].InputSlotClass);
		log_word(descs[i].InstanceDataStepRate);
	}
	log_bytes(code, len);
	*out = (ID3D11InputLayout*)stub_new(K_INPUT_LAYOUT);
	log_word(stub_id(*out));
	return S_OK;
}

HRESULT hook_dev_CheckFormatSupport(stub_obj* d, DXGI_FORMAT format, UINT* support)
{
	stub_call(d, "CheckFormatSupport");
	log_word(format);
	/* (BC7 missing on the feature level 10 devices) */
	*support = (format == DXGI_FORMAT_BC7_UNORM && g_cfg.feature_level < D3D_FEATURE_LEVEL_11_0)
		? 0 : D3D11_FORMAT_SUPPORT_TEXTURE2D;
	return S_OK;
}

HRESULT hook_dev_CheckFeatureSupport(stub_obj* d, D3D11_FEATURE feature, void* data, UINT size)
{
	stub_call(d, "CheckFeatureSupport");
	log_word(feature);
	log_word(size);
	memset(data, 0, size);
	if (feature == D3D11_FEATURE_D3D11_OPTIONS2)
		((D3D11_FEATURE_DATA_D3D11_OPTIONS2*)data)->TypedUAVLoadAdditionalFormats = g_cfg.typed_uav_loads;
	return S_OK;
}

D3D_FEATURE_LEVEL hook_dev_GetFeatureLevel(stub_obj* d)
{
	stub_call(d, "GetFeatureLevel");
	return (D3D_FEATURE_LEVEL)g_cfg.feature_level;
}

UINT hook_dev_GetCreationFlags(stub_obj* d)
{
	stub_call(d, "GetCreationFlags");
	return g_cfg.single_threaded ? D3D11_CREATE_DEVICE_SINGLETHREADED : 0;
}

/* ==== DXGI ==== */

HRESULT hook_dxgidev_GetParent(stub_obj* o, REFIID riid, void** out)
{
	stub_call(o, "GetParent");
	log_bytes(riid, sizeof(GUID));
	if (!o->res)
		o->res = (stub_obj*)stub_new(K_ADAPTER);
	else
		o->res->refs++;
	*out = o->res;
	log_word(o->res->id);
	return S_OK;
}

HRESULT hook_adapter_GetDesc1(stub_obj* o, DXGI_ADAPTER_DESC1* desc)
{
	stub_call(o, "GetDesc1");
	memset(desc, 0, sizeof(*desc));
	desc->VendorId = g_cfg.vendor;
	return S_OK;
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

/* ==== the context ==== */

static struct
{
	stub_obj* vb;
	UINT stride;
	UINT offset;
	stub_obj* ib;
	UINT ib_offset;
	stub_obj* layout;
	stub_obj* vs_srv;
} s_ia;

void hook_ctx_IASetVertexBuffers(stub_obj* c, UINT start, UINT n, ID3D11Buffer* const* vbs, const UINT* strides,
	const UINT* offsets)
{
	stub_call(c, "IASetVertexBuffers");
	log_word(start);
	stub_log_objs((void* const*)vbs, n);
	log_bytes(strides, n * sizeof(UINT));
	log_bytes(offsets, n * sizeof(UINT));
	if (start == 0 && n)
	{
		s_ia.vb     = (stub_obj*)vbs[0];
		s_ia.stride = strides[0];
		s_ia.offset = offsets[0];
	}
}

void hook_ctx_IASetIndexBuffer(stub_obj* c, ID3D11Buffer* ib, DXGI_FORMAT format, UINT offset)
{
	stub_call(c, "IASetIndexBuffer");
	log_word(stub_id(ib));
	log_word(format);
	log_word(offset);
	s_ia.ib        = (stub_obj*)ib;
	s_ia.ib_offset = offset;
}

void hook_ctx_IASetInputLayout(stub_obj* c, ID3D11InputLayout* il)
{
	stub_call(c, "IASetInputLayout");
	log_word(stub_id(il));
	s_ia.layout = (stub_obj*)il;
}

void hook_ctx_VSSetShaderResources(stub_obj* c, UINT start, UINT n, ID3D11ShaderResourceView* const* srvs)
{
	stub_call(c, "VSSetShaderResources");
	log_word(start);
	stub_log_objs((void* const*)srvs, n);
	if (start == 0 && n)
		s_ia.vs_srv = (stub_obj*)srvs[0];
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

void hook_ctx_Draw(stub_obj* c, UINT count, UINT start)
{
	stub_call(c, "Draw");
	log_word(count);
	log_word(start);
	if (s_ia.layout && s_ia.vb)
		log_region(s_ia.vb, s_ia.offset + (size_t)start * s_ia.stride, (size_t)count * s_ia.stride);
}

/* The indices, and the vertices they reach: through the input layout, or
 * where it is none from the expansion buffer the vertex shader reads (two
 * indices a vertex at most) */
void hook_ctx_DrawIndexed(stub_obj* c, UINT count, UINT start, INT base)
{
	u32 max = 0;
	UINT i;
	stub_call(c, "DrawIndexed");
	log_word(count);
	log_word(start);
	log_word((u64)(s64)base);
	if (!s_ia.ib || !s_ia.ib->mem)
		return;
	log_region(s_ia.ib, s_ia.ib_offset + (size_t)start * 2, (size_t)count * 2);
	for (i = 0; i < count; i++)
	{
		const size_t at = s_ia.ib_offset + ((size_t)start + i) * 2;
		u16 v;
		if (at + 2 > s_ia.ib->size)
			break;
		memcpy(&v, s_ia.ib->mem + at, 2);
		if (v > max)
			max = v;
	}
	if (s_ia.layout && s_ia.vb)
		log_region(s_ia.vb, s_ia.offset + (size_t)base * s_ia.stride, ((size_t)max + 1) * s_ia.stride);
	else if (s_ia.vs_srv && s_ia.vs_srv->res)
		log_region(s_ia.vs_srv->res, (size_t)base * 32, ((size_t)(max >> 1) + 2) * 32);
}

HRESULT hook_ctx_Map(stub_obj* c, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags,
	D3D11_MAPPED_SUBRESOURCE* m)
{
	stub_obj* r = (stub_obj*)res;
	stub_call(c, "Map");
	log_word(stub_id(res));
	log_word(sub);
	log_word(type);
	log_word(flags);
	if (!r->mem)
		return E_FAIL;
	m->pData      = r->mem;
	m->RowPitch   = r->row_pitch;
	m->DepthPitch = 0;
	return S_OK;
}

void hook_ctx_UpdateSubresource(stub_obj* c, ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* data,
	UINT row_pitch, UINT depth_pitch)
{
	stub_obj* r = (stub_obj*)res;
	stub_call(c, "UpdateSubresource");
	log_word(stub_id(res));
	log_word(sub);
	if (box)
		log_bytes(box, sizeof(*box));
	else
		log_word(0);
	log_word(row_pitch);
	log_word(depth_pitch);
	if (r->kind == K_BUFFER)
	{
		const size_t n = box ? box->right - box->left : r->size;
		log_bytes(data, n);
		if (r->mem && (box ? box->left : 0) + n <= r->size)
			memcpy(r->mem + (box ? box->left : 0), data, n);
	}
}

/* ==== the worker thread, in step ====
 *
 * Started, it runs only when told there is work, every second time, on
 * the thread that told it, until it would wait for more: then it is back
 * where it was told. What it makes is ready by the same draw every run. */

struct sthread
{
	int unused;
};

static void (*s_thread_fn)(void*);
static void* s_thread_data;
static jmp_buf s_parked;
static int s_running;

sthread_t* sthread_create(void (*thread_func)(void*), void* userdata)
{
	static struct sthread t;
	s_thread_fn   = thread_func;
	s_thread_data = userdata;
	return &t;
}

void sthread_join(sthread_t* thread)
{
	(void)thread;
	s_thread_fn = NULL;
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
	static unsigned notifies;
	(void)ec;
	/* (a step behind one time in two: a draw then finds its shader queued
	 * and not made) */
	if (!s_thread_fn || s_running || (++notifies & 1))
		return;
	s_running = 1;
	if (!setjmp(s_parked))
		s_thread_fn(s_thread_data);
	s_running = 0;
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
	longjmp(s_parked, 1);
}
