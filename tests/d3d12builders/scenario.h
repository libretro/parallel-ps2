/* What the D3D12 builder tests do, against the GP_/CP_/RS_ operations
 * the includer defines (the C functions, or the C++ classes when the
 * expected hash was made): every call at random, the description hashed
 * after each, its pointers into the builder taken as offsets and its
 * names as what they say. */

#include <string.h>

static u32 s_h;
static u32 s_pick;

static void mix_bytes(const void* p, size_t n)
{
	const unsigned char* b = (const unsigned char*)p;
	size_t i;
	for (i = 0; i < n; i++)
	{
		s_h ^= b[i];
		s_h *= 16777619u;
	}
}

static void mix(u32 v) { mix_bytes(&v, sizeof(v)); }

static u32 pick(void)
{
	s_pick ^= s_pick << 13;
	s_pick ^= s_pick >> 17;
	s_pick ^= s_pick << 5;
	return s_pick;
}

static const char* const s_names[4] = {"POSITION", "TEXCOORD", "COLOR", "BLENDINDICES"};

static u32 name_index(const char* n)
{
	u32 i;
	if (!n)
		return 99;
	for (i = 0; i < 4; i++)
		if (!strcmp(n, s_names[i]))
			return i;
	return 98;
}

static u64 rel(const void* p, const void* base)
{
	return p ? (u64)((const char*)p - (const char*)base) : 0xFFFFFFFFFFFFFFFFull;
}

static void hash_gp(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, const void* base)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC c = *d;
	u32 i;
	c.InputLayout.pInputElementDescs = (const D3D12_INPUT_ELEMENT_DESC*)(uintptr_t)rel(d->InputLayout.pInputElementDescs, base);
	mix_bytes(&c, sizeof(c));
	for (i = 0; i < d->InputLayout.NumElements; i++)
	{
		D3D12_INPUT_ELEMENT_DESC e = d->InputLayout.pInputElementDescs[i];
		e.SemanticName = (const char*)(uintptr_t)name_index(e.SemanticName);
		mix_bytes(&e, sizeof(e));
	}
}

static void hash_rs(const D3D12_ROOT_SIGNATURE_DESC* d, const void* base)
{
	D3D12_ROOT_SIGNATURE_DESC c = *d;
	u32 i;
	c.pParameters = (const D3D12_ROOT_PARAMETER*)(uintptr_t)rel(d->pParameters, base);
	mix_bytes(&c, sizeof(c));
	for (i = 0; i < d->NumParameters; i++)
	{
		D3D12_ROOT_PARAMETER p = d->pParameters[i];
		if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
		{
			mix_bytes(p.DescriptorTable.pDescriptorRanges, sizeof(D3D12_DESCRIPTOR_RANGE));
			p.DescriptorTable.pDescriptorRanges =
				(const D3D12_DESCRIPTOR_RANGE*)(uintptr_t)rel(p.DescriptorTable.pDescriptorRanges, base);
		}
		mix_bytes(&p, sizeof(p));
	}
}

static void hash_cp(const D3D12_COMPUTE_PIPELINE_STATE_DESC* d)
{
	mix_bytes(d, sizeof(*d));
}

#define FAKE(n) ((void*)(uintptr_t)(0x1000u * (n)))

static void sc_gp(GP* b, u32 rounds)
{
	u32 r;
	for (r = 0; r < rounds; r++)
	{
		const u32 op = pick() % 22;
		const u32 rt = pick() % 8;
		switch (op)
		{
			case 0: GP_CLEAR(b); break;
			case 1: GP_ROOT(b, (ID3D12RootSignature*)FAKE(1 + pick() % 4)); break;
			case 2: GP_VS(b, FAKE(5 + pick() % 4), pick() % 5000); break;
			case 3: GP_GS(b, FAKE(9 + pick() % 4), pick() % 5000); break;
			case 4: GP_PS(b, FAKE(13 + pick() % 4), pick() % 5000); break;
			case 5:
				if (GP_DESCP(b)->InputLayout.NumElements < 16)
					GP_ATTR(b, s_names[pick() % 4], pick() % 3, (DXGI_FORMAT)(pick() % 120), pick() % 4, pick() % 64);
				break;
			case 6: GP_TOPO(b, (D3D12_PRIMITIVE_TOPOLOGY_TYPE)(pick() % 5)); break;
			case 7: GP_RASTER(b, (D3D12_FILL_MODE)(2 + pick() % 2), (D3D12_CULL_MODE)(1 + pick() % 3), pick() & 1); break;
			case 8: GP_MSAA(b, 1u << (pick() % 4)); break;
			case 9: GP_NOCULL(b); break;
			case 10: GP_DEPTH(b, pick() & 1, pick() & 1, (D3D12_COMPARISON_FUNC)(1 + pick() % 8)); break;
			case 11:
			{
				D3D12_DEPTH_STENCILOP_DESC f, k;
				f.StencilFailOp      = (D3D12_STENCIL_OP)(1 + pick() % 8);
				f.StencilDepthFailOp = (D3D12_STENCIL_OP)(1 + pick() % 8);
				f.StencilPassOp      = (D3D12_STENCIL_OP)(1 + pick() % 8);
				f.StencilFunc        = (D3D12_COMPARISON_FUNC)(1 + pick() % 8);
				k                    = f;
				k.StencilPassOp      = (D3D12_STENCIL_OP)(1 + pick() % 8);
				GP_STENCIL(b, pick() & 1, (u8)pick(), (u8)pick(), &f, &k);
				break;
			}
			case 12: GP_NODEPTH(b); break;
			case 13: GP_NOSTENCIL(b); break;
			case 14:
			case 15:
				GP_BLEND(b, rt, pick() & 1, (D3D12_BLEND)(1 + pick() % 19), (D3D12_BLEND)(1 + pick() % 19),
					(D3D12_BLEND_OP)(1 + pick() % 5), (D3D12_BLEND)(1 + pick() % 19), (D3D12_BLEND)(1 + pick() % 19),
					(D3D12_BLEND_OP)(1 + pick() % 5), (u8)(pick() & 15));
				break;
			case 16: GP_WRITEMASK(b, rt, (u8)(pick() & 15)); break;
			case 17: GP_NOBLEND(b); break;
			case 18: GP_CLEAR_RTS(b); break;
			case 19: GP_RT(b, rt, (DXGI_FORMAT)(pick() % 120)); break;
			case 20: GP_CLEAR_DS(b); break;
			default: GP_DS(b, (DXGI_FORMAT)(pick() % 120)); break;
		}
		mix(op);
		hash_gp(GP_DESCP(b), b);
	}
}

static void sc_rs(RS* b, u32 rounds)
{
	u32 r;
	for (r = 0; r < rounds; r++)
	{
		const u32 op = pick() % 9;
		const D3D12_SHADER_VISIBILITY vis = (D3D12_SHADER_VISIBILITY)(pick() % 6);
		const int room = RS_DESCP(b)->NumParameters < 16;
		switch (op)
		{
			case 0: RS_CLEAR(b); break;
			case 1: RS_IA(b); break;
			case 2: if (room) mix(RS_CONST(b, pick() % 8, 1 + pick() % 16, vis)); break;
			case 3: if (room) mix(RS_CBV(b, pick() % 8, vis)); break;
			case 4: if (room) mix(RS_SRV(b, pick() % 8, vis)); break;
			default:
				if (room && RS_RANGES(b) < 16)
					mix(RS_TABLE(b, (D3D12_DESCRIPTOR_RANGE_TYPE)(pick() % 4), pick() % 8, 1 + pick() % 8, vis));
				break;
		}
		mix(op);
		hash_rs(RS_DESCP(b), b);
	}
}

static void sc_cp(CP* b, u32 rounds)
{
	u32 r;
	for (r = 0; r < rounds; r++)
	{
		const u32 op = pick() % 4;
		switch (op)
		{
			case 0: CP_CLEAR(b); break;
			case 1: CP_ROOT(b, (ID3D12RootSignature*)FAKE(20 + pick() % 4)); break;
			default: CP_SHADER(b, FAKE(30 + pick() % 4), pick() % 9000); break;
		}
		mix(op);
		hash_cp(CP_DESCP(b));
	}
}

static u32 run_scenario(GP* gp, RS* rs, CP* cp)
{
	s_h    = 2166136261u;
	s_pick = 0xD3D12B1Du;
	sc_gp(gp, 60000);
	sc_rs(rs, 40000);
	sc_cp(cp, 5000);
	return s_h;
}
