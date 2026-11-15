/* What the D3D.c suite fakes: the log, which it hashes, and adapters of
 * a given vendor, in C's view of IDXGIAdapter1. Shared by main.c and the
 * C++ oracle. */
#define COBJMACROS
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <windows.h>
#include <dxgi.h>

#include <libretro.h>

unsigned long long fk_hash = 1469598103934665603ull;

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

/* every line logged, its level and text */
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

typedef struct fk_adapter_obj
{
	IDXGIAdapter1Vtbl* lpVtbl;
	unsigned vendor;
	int ok;
} fk_adapter_obj;

static HRESULT STDMETHODCALLTYPE fk_desc1(IDXGIAdapter1* a, DXGI_ADAPTER_DESC1* d)
{
	const fk_adapter_obj* o = (const fk_adapter_obj*)a;
	if (!o->ok)
		return E_FAIL;
	memset(d, 0, sizeof(*d));
	d->VendorId = o->vendor;
	return S_OK;
}

static IDXGIAdapter1Vtbl fk_adapter_vtbl;
static fk_adapter_obj fk_adapters[16];
static unsigned fk_nadapters;

void* fk_adapter(unsigned vendor, int ok)
{
	fk_adapter_obj* o = &fk_adapters[fk_nadapters++ % 16];
	fk_adapter_vtbl.GetDesc1 = fk_desc1;
	o->lpVtbl = &fk_adapter_vtbl;
	o->vendor = vendor;
	o->ok     = ok;
	return o;
}
