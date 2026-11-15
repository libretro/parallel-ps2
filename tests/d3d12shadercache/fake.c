/* A D3D12 device whose pipelines give the driver's blob of them back
 * (and turn a stale one down), a log that hashes every line, and the
 * files: in C's view of the COM interfaces, shared by main.c and the C++
 * oracle. */
#define COBJMACROS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include "fake.h"

unsigned long long fk_hash = 1469598103934665603ull;
int fk_fail_create, fk_fail_cached_blob, fk_driver_changed;
static int fk_objects;

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

void RETRO_CALLCONV fk_log(enum retro_log_level level, const char* fmt, ...)
{
	char line[8192];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fk_hu((unsigned)level);
	fk_h(line, strlen(line));
}

/* What a pipeline is made of, as the fake driver sees it */
static unsigned long long fk_mix(unsigned long long h, const void* p, size_t n)
{
	const unsigned char* c = (const unsigned char*)p;
	size_t i;
	for (i = 0; i < n; i++)
	{
		h ^= c[i];
		h *= 1099511628211ull;
	}
	return h;
}
static unsigned long long fk_graphics_id(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d)
{
	unsigned long long h = 0x6EAull;
	UINT i;
	h = fk_mix(h, d->VS.pShaderBytecode, d->VS.BytecodeLength);
	h = fk_mix(h, d->PS.pShaderBytecode, d->PS.BytecodeLength);
	h = fk_mix(h, &d->NumRenderTargets, sizeof(d->NumRenderTargets));
	h = fk_mix(h, &d->RTVFormats[0], sizeof(d->RTVFormats[0]));
	h = fk_mix(h, &d->BlendState.RenderTarget[0].BlendEnable, sizeof(BOOL));
	for (i = 0; i < d->InputLayout.NumElements; i++)
		h = fk_mix(h, d->InputLayout.pInputElementDescs[i].SemanticName,
			strlen(d->InputLayout.pInputElementDescs[i].SemanticName));
	return h;
}
static unsigned long long fk_compute_id(const D3D12_COMPUTE_PIPELINE_STATE_DESC* d)
{
	return fk_mix(0xC0ull, d->CS.pShaderBytecode, d->CS.BytecodeLength);
}

typedef struct fk_pso
{
	const ID3D12PipelineStateVtbl* lpVtbl;
	ULONG refs;
	unsigned long long id;
} fk_pso;

static HRESULT STDMETHODCALLTYPE fk_qi(IUnknown* t, REFIID r, void** p) { (void)t; (void)r; *p = NULL; return E_NOINTERFACE; }
static ULONG STDMETHODCALLTYPE fk_addref(IUnknown* t) { return ++((fk_pso*)t)->refs; }
static ULONG STDMETHODCALLTYPE fk_rel(IUnknown* t)
{
	fk_pso* o = (fk_pso*)t;
	if (--o->refs)
		return o->refs;
	fk_objects--;
	free(o);
	return 0;
}
/* the driver's blob: its tag and the pipeline's id */
static HRESULT STDMETHODCALLTYPE fk_cached_blob(ID3D12PipelineState* t, ID3DBlob** out)
{
	fk_pso* o = (fk_pso*)t;
	fk_hu(0xB10Bull);
	if (fk_fail_cached_blob)
		return E_FAIL;
	if (FAILED(D3DCreateBlob(16, out)))
		return E_OUTOFMEMORY;
	memcpy(ID3D10Blob_GetBufferPointer(*out), "PSOBLOB!", 8);
	memcpy((char*)ID3D10Blob_GetBufferPointer(*out) + 8, &o->id, 8);
	return S_OK;
}
static ID3D12PipelineStateVtbl fk_pso_vtbl;

static HRESULT fk_make(unsigned long long id, const D3D12_CACHED_PIPELINE_STATE* cached, REFIID riid, void** out)
{
	fk_pso* o;
	fk_hu(id);
	fk_hu(cached->CachedBlobSizeInBytes);
	fk_h(cached->pCachedBlob, cached->CachedBlobSizeInBytes);
	fk_h(riid, sizeof(*riid));
	if (fk_fail_create)
		return E_INVALIDARG;
	if (cached->CachedBlobSizeInBytes
		&& (fk_driver_changed || cached->CachedBlobSizeInBytes != 16
			|| memcmp(cached->pCachedBlob, "PSOBLOB!", 8)
			|| memcmp((const char*)cached->pCachedBlob + 8, &id, 8)))
		return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
	o         = (fk_pso*)calloc(1, sizeof(*o));
	o->lpVtbl = &fk_pso_vtbl;
	o->refs   = 1;
	o->id     = id;
	fk_objects++;
	*out = o;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE fk_create_graphics(ID3D12Device* d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
	REFIID riid, void** out)
{
	(void)d;
	fk_hu(0x6Aull);
	return fk_make(fk_graphics_id(desc), &desc->CachedPSO, riid, out);
}
static HRESULT STDMETHODCALLTYPE fk_create_compute(ID3D12Device* d, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
	REFIID riid, void** out)
{
	(void)d;
	fk_hu(0xCAull);
	return fk_make(fk_compute_id(desc), &desc->CachedPSO, riid, out);
}

static ID3D12DeviceVtbl fk_dev_vtbl;
static struct { ID3D12DeviceVtbl* lpVtbl; } fk_dev;
void* fk_device(void)
{
	fk_pso_vtbl.QueryInterface = (HRESULT (STDMETHODCALLTYPE*)(ID3D12PipelineState*, REFIID, void**))fk_qi;
	fk_pso_vtbl.AddRef         = (ULONG (STDMETHODCALLTYPE*)(ID3D12PipelineState*))fk_addref;
	fk_pso_vtbl.Release        = (ULONG (STDMETHODCALLTYPE*)(ID3D12PipelineState*))fk_rel;
	fk_pso_vtbl.GetCachedBlob  = fk_cached_blob;
	fk_dev_vtbl.CreateGraphicsPipelineState = fk_create_graphics;
	fk_dev_vtbl.CreateComputePipelineState  = fk_create_compute;
	fk_dev.lpVtbl = &fk_dev_vtbl;
	return &fk_dev;
}
void fk_release(void* object) { if (object) IUnknown_Release((IUnknown*)object); }
int fk_live(void) { return fk_objects; }

void fk_hash_file(const char* path)
{
	unsigned char buf[4096];
	size_t n, total = 0;
	FILE* f = fopen(path, "rb");
	fk_hu(f != NULL);
	if (!f)
		return;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
	{
		fk_h(buf, n);
		total += n;
	}
	fclose(f);
	fk_hu(total);
}

void fk_truncate(const char* path, long size)
{
	unsigned char* buf = (unsigned char*)malloc((size_t)size);
	FILE* f            = fopen(path, "rb");
	size_t n           = f ? fread(buf, 1, (size_t)size, f) : 0;
	if (f)
		fclose(f);
	f = fopen(path, "wb");
	if (f)
	{
		fwrite(buf, 1, n, f);
		fclose(f);
	}
	free(buf);
}

void fk_poke(const char* path, long at, unsigned value)
{
	FILE* f = fopen(path, "r+b");
	if (!f)
		return;
	fseek(f, at, SEEK_SET);
	fwrite(&value, sizeof(value), 1, f);
	fclose(f);
}

/* An index with entry 0's key again, at entry 1's blob: the first stands */
void fk_dup_key(const char* path)
{
	unsigned char e[2][64];
	FILE* f = fopen(path, "r+b");
	if (!f)
		return;
	fseek(f, 4, SEEK_SET);
	if (fread(e, 64, 2, f) == 2)
	{
		memcpy(e[0] + 56, e[1] + 56, 8);
		fseek(f, 0, SEEK_END);
		fwrite(e[0], 64, 1, f);
	}
	fclose(f);
}
