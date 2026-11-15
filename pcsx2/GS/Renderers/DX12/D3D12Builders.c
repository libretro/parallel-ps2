/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

#include <string.h>

#include "D3D12Builders.h"

/* --- root signatures --- */

void gs_d3d12_rs_clear(gs_d3d12_rs_builder* b)
{
	memset(b, 0, sizeof(*b));
	b->desc.pParameters = b->params;
}

void gs_d3d12_rs_set_input_assembler_flag(gs_d3d12_rs_builder* b)
{
	b->desc.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
}

u32 gs_d3d12_rs_add_32bit_constants(gs_d3d12_rs_builder* b, u32 shader_reg, u32 num_values,
	D3D12_SHADER_VISIBILITY visibility)
{
	const u32 index        = b->desc.NumParameters++;
	D3D12_ROOT_PARAMETER* p = &b->params[index];

	p->ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	p->ShaderVisibility         = visibility;
	p->Constants.ShaderRegister = shader_reg;
	p->Constants.RegisterSpace  = 0;
	p->Constants.Num32BitValues = num_values;
	return index;
}

static u32 gs_d3d12_rs_add_descriptor(gs_d3d12_rs_builder* b, D3D12_ROOT_PARAMETER_TYPE type, u32 shader_reg,
	D3D12_SHADER_VISIBILITY visibility)
{
	const u32 index        = b->desc.NumParameters++;
	D3D12_ROOT_PARAMETER* p = &b->params[index];

	p->ParameterType             = type;
	p->ShaderVisibility          = visibility;
	p->Descriptor.ShaderRegister = shader_reg;
	p->Descriptor.RegisterSpace  = 0;
	return index;
}

u32 gs_d3d12_rs_add_cbv(gs_d3d12_rs_builder* b, u32 shader_reg, D3D12_SHADER_VISIBILITY visibility)
{
	return gs_d3d12_rs_add_descriptor(b, D3D12_ROOT_PARAMETER_TYPE_CBV, shader_reg, visibility);
}

u32 gs_d3d12_rs_add_srv(gs_d3d12_rs_builder* b, u32 shader_reg, D3D12_SHADER_VISIBILITY visibility)
{
	return gs_d3d12_rs_add_descriptor(b, D3D12_ROOT_PARAMETER_TYPE_SRV, shader_reg, visibility);
}

u32 gs_d3d12_rs_add_descriptor_table(gs_d3d12_rs_builder* b, D3D12_DESCRIPTOR_RANGE_TYPE rt, u32 start_shader_reg,
	u32 num_shader_regs, D3D12_SHADER_VISIBILITY visibility)
{
	const u32 index          = b->desc.NumParameters++;
	D3D12_ROOT_PARAMETER* p   = &b->params[index];
	D3D12_DESCRIPTOR_RANGE* r = &b->ranges[b->num_ranges++];

	r->RangeType                         = rt;
	r->NumDescriptors                    = num_shader_regs;
	r->BaseShaderRegister                = start_shader_reg;
	r->RegisterSpace                     = 0;
	r->OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	p->ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	p->DescriptorTable.pDescriptorRanges   = r;
	p->DescriptorTable.NumDescriptorRanges = 1;
	p->ShaderVisibility                    = visibility;
	return index;
}

/* --- graphics pipelines --- */

void gs_d3d12_gp_clear(gs_d3d12_gp_builder* b)
{
	memset(b, 0, sizeof(*b));
	b->desc.NodeMask         = 1;
	b->desc.SampleMask       = 0xFFFFFFFFu;
	b->desc.SampleDesc.Count = 1;
}

void gs_d3d12_gp_set_root_signature(gs_d3d12_gp_builder* b, ID3D12RootSignature* rs)
{
	b->desc.pRootSignature = rs;
}

void gs_d3d12_gp_set_vertex_shader(gs_d3d12_gp_builder* b, const void* data, u32 data_size)
{
	b->desc.VS.pShaderBytecode = data;
	b->desc.VS.BytecodeLength  = data_size;
}

void gs_d3d12_gp_set_geometry_shader(gs_d3d12_gp_builder* b, const void* data, u32 data_size)
{
	b->desc.GS.pShaderBytecode = data;
	b->desc.GS.BytecodeLength  = data_size;
}

void gs_d3d12_gp_set_pixel_shader(gs_d3d12_gp_builder* b, const void* data, u32 data_size)
{
	b->desc.PS.pShaderBytecode = data;
	b->desc.PS.BytecodeLength  = data_size;
}

void gs_d3d12_gp_add_vertex_attribute(gs_d3d12_gp_builder* b, const char* semantic_name, u32 semantic_index,
	DXGI_FORMAT format, u32 buffer, u32 offset)
{
	D3D12_INPUT_ELEMENT_DESC* e = &b->input_elements[b->desc.InputLayout.NumElements];

	e->SemanticIndex        = semantic_index;
	e->SemanticName         = semantic_name;
	e->Format               = format;
	e->AlignedByteOffset    = offset;
	e->InputSlot            = buffer;
	e->InputSlotClass       = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
	e->InstanceDataStepRate = 0;

	b->desc.InputLayout.pInputElementDescs = b->input_elements;
	b->desc.InputLayout.NumElements++;
}

void gs_d3d12_gp_set_topology_type(gs_d3d12_gp_builder* b, D3D12_PRIMITIVE_TOPOLOGY_TYPE type)
{
	b->desc.PrimitiveTopologyType = type;
}

void gs_d3d12_gp_set_rasterization(gs_d3d12_gp_builder* b, D3D12_FILL_MODE fill, D3D12_CULL_MODE cull, int front_ccw)
{
	b->desc.RasterizerState.FillMode              = fill;
	b->desc.RasterizerState.CullMode              = cull;
	b->desc.RasterizerState.FrontCounterClockwise = front_ccw ? TRUE : FALSE;
}

void gs_d3d12_gp_set_multisamples(gs_d3d12_gp_builder* b, u32 multisamples)
{
	b->desc.RasterizerState.MultisampleEnable = multisamples > 1;
	b->desc.SampleDesc.Count                  = multisamples;
}

void gs_d3d12_gp_set_depth(gs_d3d12_gp_builder* b, int test, int write, D3D12_COMPARISON_FUNC func)
{
	b->desc.DepthStencilState.DepthEnable    = test ? TRUE : FALSE;
	b->desc.DepthStencilState.DepthWriteMask = write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	b->desc.DepthStencilState.DepthFunc      = func;
}

void gs_d3d12_gp_set_stencil(gs_d3d12_gp_builder* b, int test, u8 read_mask, u8 write_mask,
	const D3D12_DEPTH_STENCILOP_DESC* front, const D3D12_DEPTH_STENCILOP_DESC* back)
{
	b->desc.DepthStencilState.StencilEnable    = test ? TRUE : FALSE;
	b->desc.DepthStencilState.StencilReadMask  = read_mask;
	b->desc.DepthStencilState.StencilWriteMask = write_mask;
	b->desc.DepthStencilState.FrontFace        = *front;
	b->desc.DepthStencilState.BackFace         = *back;
}

void gs_d3d12_gp_set_no_stencil(gs_d3d12_gp_builder* b)
{
	D3D12_DEPTH_STENCILOP_DESC empty;
	memset(&empty, 0, sizeof(empty));
	gs_d3d12_gp_set_stencil(b, 0, 0, 0, &empty, &empty);
}

void gs_d3d12_gp_set_blend(gs_d3d12_gp_builder* b, u32 rt, int enable, D3D12_BLEND src, D3D12_BLEND dst,
	D3D12_BLEND_OP op, D3D12_BLEND alpha_src, D3D12_BLEND alpha_dst, D3D12_BLEND_OP alpha_op, u8 write_mask)
{
	D3D12_RENDER_TARGET_BLEND_DESC* d = &b->desc.BlendState.RenderTarget[rt];

	d->BlendEnable           = enable ? TRUE : FALSE;
	d->SrcBlend              = src;
	d->DestBlend             = dst;
	d->BlendOp               = op;
	d->SrcBlendAlpha         = alpha_src;
	d->DestBlendAlpha        = alpha_dst;
	d->BlendOpAlpha          = alpha_op;
	d->RenderTargetWriteMask = write_mask;

	if (rt > 0)
		b->desc.BlendState.IndependentBlendEnable = TRUE;
}

void gs_d3d12_gp_set_color_write_mask(gs_d3d12_gp_builder* b, u32 rt, u8 write_mask)
{
	b->desc.BlendState.RenderTarget[rt].RenderTargetWriteMask = write_mask;
}

void gs_d3d12_gp_set_no_blending(gs_d3d12_gp_builder* b)
{
	gs_d3d12_gp_set_blend(b, 0, 0, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE,
		D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_COLOR_WRITE_ENABLE_ALL);
	b->desc.BlendState.IndependentBlendEnable = FALSE;
}

void gs_d3d12_gp_clear_render_targets(gs_d3d12_gp_builder* b)
{
	u32 i;
	b->desc.NumRenderTargets = 0;
	for (i = 0; i < sizeof(b->desc.RTVFormats) / sizeof(b->desc.RTVFormats[0]); i++)
		b->desc.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
}

void gs_d3d12_gp_set_render_target(gs_d3d12_gp_builder* b, u32 rt, DXGI_FORMAT format)
{
	b->desc.RTVFormats[rt] = format;
	if (rt >= b->desc.NumRenderTargets)
		b->desc.NumRenderTargets = rt + 1;
}

void gs_d3d12_gp_set_depth_stencil_format(gs_d3d12_gp_builder* b, DXGI_FORMAT format)
{
	b->desc.DSVFormat = format;
}

/* --- compute pipelines --- */

void gs_d3d12_cp_clear(gs_d3d12_cp_builder* b)
{
	memset(b, 0, sizeof(*b));
}

void gs_d3d12_cp_set_root_signature(gs_d3d12_cp_builder* b, ID3D12RootSignature* rs)
{
	b->desc.pRootSignature = rs;
}

void gs_d3d12_cp_set_shader(gs_d3d12_cp_builder* b, const void* data, u32 data_size)
{
	b->desc.CS.pShaderBytecode = data;
	b->desc.CS.BytecodeLength  = data_size;
}
