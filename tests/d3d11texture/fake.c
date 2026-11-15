/* The fake D3D11 device and context (fake.h), in C's view of the COM
 * interfaces: built the same for the C textures and the C++ oracle. */
#define COBJMACROS
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <d3d11.h>

#include "fake.h"

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

/* Every object: IUnknown's three, all that is called on them */
typedef struct fk_obj
{
	const void* lpVtbl;
	ULONG refs;
	unsigned long long id;
	unsigned w, h;
	unsigned char* mem; /* a staging texture's */
} fk_obj;

static unsigned long long fk_next = 1;
static int fk_live;

unsigned long long fk_id(const void* object) { return object ? ((const fk_obj*)object)->id : 0; }
int fk_live_objects(void) { return fk_live; }

static HRESULT STDMETHODCALLTYPE fk_qi(IUnknown* t, REFIID r, void** p) { (void)t; (void)r; *p = NULL; return E_NOINTERFACE; }
static ULONG STDMETHODCALLTYPE fk_addref(IUnknown* t) { return ++((fk_obj*)t)->refs; }
static ULONG STDMETHODCALLTYPE fk_release(IUnknown* t)
{
	fk_obj* o = (fk_obj*)t;
	if (--o->refs)
		return o->refs;
	fk_hu(0xDEAD0000ull ^ o->id);
	fk_live--;
	free(o->mem);
	free(o);
	return 0;
}
static IUnknownVtbl fk_obj_vtbl = {fk_qi, fk_addref, fk_release};

static fk_obj* fk_new(unsigned w, unsigned h, int staging)
{
	fk_obj* o = (fk_obj*)calloc(1, sizeof(*o));
	o->lpVtbl = &fk_obj_vtbl;
	o->refs   = 1;
	o->id     = fk_next++;
	o->w      = w;
	o->h      = h;
	if (staging)
		o->mem = (unsigned char*)calloc((size_t)w * h, 16);
	fk_live++;
	return o;
}

/* --- the device --- */

static HRESULT STDMETHODCALLTYPE fk_create_texture(ID3D11Device* d, const D3D11_TEXTURE2D_DESC* desc,
	const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out)
{
	(void)d;
	fk_hu(0x7E8ull); fk_h(desc, sizeof(*desc)); fk_hu(init != NULL);
	if (desc->Width > 4000)
		return E_OUTOFMEMORY;
	*out = (ID3D11Texture2D*)fk_new(desc->Width, desc->Height, desc->Usage == D3D11_USAGE_STAGING);
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE fk_srv(ID3D11Device* d, ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
	ID3D11ShaderResourceView** out)
{
	(void)d;
	fk_hu(0x52Bull ^ fk_id(r)); fk_hu(desc != NULL);
	if (desc)
		fk_h(desc, sizeof(*desc));
	*out = (ID3D11ShaderResourceView*)fk_new(0, 0, 0);
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE fk_rtv(ID3D11Device* d, ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC* desc,
	ID3D11RenderTargetView** out)
{
	(void)d;
	fk_hu(0x27Bull ^ fk_id(r)); fk_hu(desc != NULL);
	if (desc)
		fk_h(desc, sizeof(*desc));
	*out = (ID3D11RenderTargetView*)fk_new(0, 0, 0);
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE fk_dsv(ID3D11Device* d, ID3D11Resource* r, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc,
	ID3D11DepthStencilView** out)
{
	(void)d;
	fk_hu(0xD5Bull ^ fk_id(r)); fk_hu(desc != NULL);
	if (desc)
		fk_h(desc, sizeof(*desc));
	*out = (ID3D11DepthStencilView*)fk_new(0, 0, 0);
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE fk_uav(ID3D11Device* d, ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc,
	ID3D11UnorderedAccessView** out)
{
	(void)d;
	fk_hu(0xAABull ^ fk_id(r)); fk_hu(desc != NULL);
	if (desc)
		fk_h(desc, sizeof(*desc));
	*out = (ID3D11UnorderedAccessView*)fk_new(0, 0, 0);
	return S_OK;
}

static ID3D11DeviceVtbl fk_dev_vtbl;
static struct { ID3D11DeviceVtbl* lpVtbl; } fk_dev;
void* fk_device(void) { return &fk_dev; }

/* --- the context --- */

static void STDMETHODCALLTYPE fk_update(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
	const void* data, UINT pitch, UINT depth_pitch)
{
	(void)c;
	fk_hu(0x0BDull ^ fk_id(r)); fk_hu(sub); fk_hu(box != NULL);
	if (box)
		fk_h(box, sizeof(*box));
	fk_hu(pitch); fk_hu(depth_pitch);
	fk_h(data, 64);
}
static void STDMETHODCALLTYPE fk_mips(ID3D11DeviceContext* c, ID3D11ShaderResourceView* v)
{
	(void)c;
	fk_hu(0x319ull ^ fk_id(v));
}
static void STDMETHODCALLTYPE fk_copy(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dsub, UINT x, UINT y, UINT z,
	ID3D11Resource* src, UINT ssub, const D3D11_BOX* box)
{
	fk_obj* d = (fk_obj*)dst;
	size_t i;
	(void)c;
	fk_hu(0xC0Bull ^ fk_id(dst)); fk_hu(dsub); fk_hu(x); fk_hu(y); fk_hu(z); fk_hu(fk_id(src)); fk_hu(ssub);
	fk_hu(box != NULL);
	if (box)
		fk_h(box, sizeof(*box));
	if (d->mem)
		for (i = 0; i < (size_t)d->w * d->h * 16; i++)
			d->mem[i] = (unsigned char)(i * 7 + d->id + x);
}
static HRESULT STDMETHODCALLTYPE fk_map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags,
	D3D11_MAPPED_SUBRESOURCE* m)
{
	fk_obj* o = (fk_obj*)r;
	(void)c;
	fk_hu(0x3A9ull ^ fk_id(r)); fk_hu(sub); fk_hu(type); fk_hu(flags);
	if (!o->mem)
		return E_FAIL;
	m->pData      = o->mem;
	m->RowPitch   = o->w * 16;
	m->DepthPitch = o->w * o->h * 16;
	return S_OK;
}
static void STDMETHODCALLTYPE fk_unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub)
{
	(void)c;
	fk_hu(0x07A9ull ^ fk_id(r)); fk_hu(sub);
}

static ID3D11DeviceContextVtbl fk_ctx_vtbl;
static struct { ID3D11DeviceContextVtbl* lpVtbl; } fk_ctx;
void* fk_context(void) { return &fk_ctx; }

void fk_init(void)
{
	fk_dev_vtbl.CreateTexture2D           = fk_create_texture;
	fk_dev_vtbl.CreateShaderResourceView  = fk_srv;
	fk_dev_vtbl.CreateRenderTargetView    = fk_rtv;
	fk_dev_vtbl.CreateDepthStencilView    = fk_dsv;
	fk_dev_vtbl.CreateUnorderedAccessView = fk_uav;
	fk_dev.lpVtbl                         = &fk_dev_vtbl;
	fk_ctx_vtbl.UpdateSubresource         = fk_update;
	fk_ctx_vtbl.GenerateMips              = fk_mips;
	fk_ctx_vtbl.CopySubresourceRegion     = fk_copy;
	fk_ctx_vtbl.Map                       = fk_map;
	fk_ctx_vtbl.Unmap                     = fk_unmap;
	fk_ctx.lpVtbl                         = &fk_ctx_vtbl;
}
