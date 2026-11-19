/* A d3d12.dll of the harness's, found beside it before the system's: the
 * device loads d3d12.dll for D3D12SerializeRootSignature, and this one
 * answers with a blob of what it was asked to serialize, the same every
 * run and everywhere. The rest is never called. */
#define COBJMACROS
#define CINTERFACE

#include <stdlib.h>
#include <string.h>

#include <d3d12.h>

typedef struct blob
{
	const ID3D10BlobVtbl* lpVtbl;
	LONG refs;
	unsigned char* data;
	SIZE_T size;
} blob;

static HRESULT STDMETHODCALLTYPE b_qi(ID3D10Blob* This, REFIID riid, void** out)
{
	(void)This;
	(void)riid;
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE b_addref(ID3D10Blob* This)
{
	return (ULONG)++((blob*)This)->refs;
}

static ULONG STDMETHODCALLTYPE b_release(ID3D10Blob* This)
{
	blob* b = (blob*)This;
	if (--b->refs == 0)
	{
		free(b->data);
		free(b);
		return 0;
	}
	return (ULONG)b->refs;
}

static void* STDMETHODCALLTYPE b_ptr(ID3D10Blob* This)
{
	return ((blob*)This)->data;
}

static SIZE_T STDMETHODCALLTYPE b_size(ID3D10Blob* This)
{
	return ((blob*)This)->size;
}

static ID3D10BlobVtbl s_vt;

/* (u32s appended) */
static void put(blob* b, unsigned v)
{
	b->data = (unsigned char*)realloc(b->data, b->size + 4);
	memcpy(b->data + b->size, &v, 4);
	b->size += 4;
}

__declspec(dllexport) HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc,
	D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** out, ID3DBlob** error)
{
	blob* b = (blob*)calloc(1, sizeof(*b));
	UINT i, j;

	s_vt.QueryInterface   = b_qi;
	s_vt.AddRef           = b_addref;
	s_vt.Release          = b_release;
	s_vt.GetBufferPointer = b_ptr;
	s_vt.GetBufferSize    = b_size;
	b->lpVtbl             = &s_vt;
	b->refs               = 1;

	put(b, (unsigned)version);
	put(b, desc->NumParameters);
	put(b, (unsigned)desc->Flags);
	for (i = 0; i < desc->NumParameters; i++)
	{
		const D3D12_ROOT_PARAMETER* p = &desc->pParameters[i];
		put(b, (unsigned)p->ParameterType);
		put(b, (unsigned)p->ShaderVisibility);
		switch (p->ParameterType)
		{
			case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
				put(b, p->DescriptorTable.NumDescriptorRanges);
				for (j = 0; j < p->DescriptorTable.NumDescriptorRanges; j++)
				{
					const D3D12_DESCRIPTOR_RANGE* r = &p->DescriptorTable.pDescriptorRanges[j];
					put(b, (unsigned)r->RangeType);
					put(b, r->NumDescriptors);
					put(b, r->BaseShaderRegister);
					put(b, r->RegisterSpace);
					put(b, r->OffsetInDescriptorsFromTableStart);
				}
				break;
			case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
				put(b, p->Constants.ShaderRegister);
				put(b, p->Constants.RegisterSpace);
				put(b, p->Constants.Num32BitValues);
				break;
			default:
				put(b, p->Descriptor.ShaderRegister);
				put(b, p->Descriptor.RegisterSpace);
				break;
		}
	}
	put(b, desc->NumStaticSamplers);
	*out = (ID3DBlob*)b;
	if (error)
		*error = NULL;
	return S_OK;
}

__declspec(dllexport) HRESULT WINAPI D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid,
	void** out)
{
	(void)adapter;
	(void)level;
	(void)riid;
	*out = NULL;
	return E_NOTIMPL;
}

__declspec(dllexport) HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void** out)
{
	(void)riid;
	*out = NULL;
	return E_NOINTERFACE;
}
