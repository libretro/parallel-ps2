/* A D3D11 device that hashes the shaders and layouts made from the
 * cache's blobs, a log that hashes every line, and the files: in C's
 * view of the COM interfaces, shared by main.c and the C++ oracle. */
#define COBJMACROS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <d3d11.h>

#include "fake.h"

unsigned long long fk_hash = 1469598103934665603ull;
int fk_fail_vs, fk_fail_layout;
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

typedef struct fk_obj
{
	const void* lpVtbl;
	ULONG refs;
} fk_obj;
static HRESULT STDMETHODCALLTYPE fk_qi(IUnknown* t, REFIID r, void** p) { (void)t; (void)r; *p = NULL; return E_NOINTERFACE; }
static ULONG STDMETHODCALLTYPE fk_addref(IUnknown* t) { return ++((fk_obj*)t)->refs; }
static ULONG STDMETHODCALLTYPE fk_rel(IUnknown* t)
{
	fk_obj* o = (fk_obj*)t;
	if (--o->refs)
		return o->refs;
	fk_objects--;
	free(o);
	return 0;
}
static IUnknownVtbl fk_vtbl = {fk_qi, fk_addref, fk_rel};
static void* fk_new(void)
{
	fk_obj* o = (fk_obj*)calloc(1, sizeof(*o));
	o->lpVtbl = &fk_vtbl;
	o->refs   = 1;
	fk_objects++;
	return o;
}
void fk_release(void* object) { if (object) IUnknown_Release((IUnknown*)object); }
int fk_live(void) { return fk_objects; }

static HRESULT STDMETHODCALLTYPE fk_vs(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* l,
	ID3D11VertexShader** out)
{
	(void)d; (void)l;
	fk_hu(0x05ull); fk_hu(len); fk_h(code, len);
	if (fk_fail_vs)
		return E_FAIL;
	*out = (ID3D11VertexShader*)fk_new();
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE fk_ps(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* l,
	ID3D11PixelShader** out)
{
	(void)d; (void)l;
	fk_hu(0x95ull); fk_hu(len); fk_h(code, len);
	*out = (ID3D11PixelShader*)fk_new();
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE fk_cs(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* l,
	ID3D11ComputeShader** out)
{
	(void)d; (void)l;
	fk_hu(0xC5ull); fk_hu(len); fk_h(code, len);
	*out = (ID3D11ComputeShader*)fk_new();
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE fk_layout(ID3D11Device* d, const D3D11_INPUT_ELEMENT_DESC* e, UINT n, const void* code,
	SIZE_T len, ID3D11InputLayout** out)
{
	UINT i;
	(void)d;
	fk_hu(0x1A70ull); fk_hu(n); fk_hu(len); fk_h(code, len);
	for (i = 0; i < n; i++)
	{
		fk_h(e[i].SemanticName, strlen(e[i].SemanticName));
		fk_hu(e[i].SemanticIndex); fk_hu(e[i].Format); fk_hu(e[i].AlignedByteOffset);
	}
	if (fk_fail_layout)
		return E_FAIL;
	*out = (ID3D11InputLayout*)fk_new();
	return S_OK;
}

static ID3D11DeviceVtbl fk_dev_vtbl;
static struct { ID3D11DeviceVtbl* lpVtbl; } fk_dev;
void* fk_device(void)
{
	fk_dev_vtbl.CreateVertexShader  = fk_vs;
	fk_dev_vtbl.CreatePixelShader   = fk_ps;
	fk_dev_vtbl.CreateComputeShader = fk_cs;
	fk_dev_vtbl.CreateInputLayout   = fk_layout;
	fk_dev.lpVtbl                   = &fk_dev_vtbl;
	return &fk_dev;
}

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
