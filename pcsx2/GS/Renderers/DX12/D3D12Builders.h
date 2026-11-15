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

#ifndef GS_D3D12_BUILDERS_H
#define GS_D3D12_BUILDERS_H

#include "common/Pcsx2Defs.h"
#ifdef __cplusplus
#include "common/RedtapeWindows.h"
#else
#include <windows.h>
#endif

#include <d3d12.h>

/* Descriptions of root signatures and pipelines filled in a call at a
 * time; what makes the objects from them is the device's (the C++ views
 * below). Pointers in a description point into its builder. */

#define GS_D3D12_RS_MAX_PARAMETERS 16
#define GS_D3D12_RS_MAX_RANGES 16
#define GS_D3D12_GP_MAX_VERTEX_ATTRIBUTES 16

typedef struct gs_d3d12_rs_builder
{
	D3D12_ROOT_SIGNATURE_DESC desc;
	D3D12_ROOT_PARAMETER params[GS_D3D12_RS_MAX_PARAMETERS];
	D3D12_DESCRIPTOR_RANGE ranges[GS_D3D12_RS_MAX_RANGES];
	u32 num_ranges;
} gs_d3d12_rs_builder;

typedef struct gs_d3d12_gp_builder
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	D3D12_INPUT_ELEMENT_DESC input_elements[GS_D3D12_GP_MAX_VERTEX_ATTRIBUTES];
} gs_d3d12_gp_builder;

typedef struct gs_d3d12_cp_builder
{
	D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
} gs_d3d12_cp_builder;

#ifdef __cplusplus
extern "C" {
#endif

void gs_d3d12_rs_clear(gs_d3d12_rs_builder* b);
void gs_d3d12_rs_set_input_assembler_flag(gs_d3d12_rs_builder* b);
/* each returns the parameter's index */
u32 gs_d3d12_rs_add_32bit_constants(gs_d3d12_rs_builder* b, u32 shader_reg, u32 num_values,
	D3D12_SHADER_VISIBILITY visibility);
u32 gs_d3d12_rs_add_cbv(gs_d3d12_rs_builder* b, u32 shader_reg, D3D12_SHADER_VISIBILITY visibility);
u32 gs_d3d12_rs_add_srv(gs_d3d12_rs_builder* b, u32 shader_reg, D3D12_SHADER_VISIBILITY visibility);
u32 gs_d3d12_rs_add_descriptor_table(gs_d3d12_rs_builder* b, D3D12_DESCRIPTOR_RANGE_TYPE rt, u32 start_shader_reg,
	u32 num_shader_regs, D3D12_SHADER_VISIBILITY visibility);

void gs_d3d12_gp_clear(gs_d3d12_gp_builder* b);
void gs_d3d12_gp_set_root_signature(gs_d3d12_gp_builder* b, ID3D12RootSignature* rs);
void gs_d3d12_gp_set_vertex_shader(gs_d3d12_gp_builder* b, const void* data, u32 data_size);
void gs_d3d12_gp_set_geometry_shader(gs_d3d12_gp_builder* b, const void* data, u32 data_size);
void gs_d3d12_gp_set_pixel_shader(gs_d3d12_gp_builder* b, const void* data, u32 data_size);
void gs_d3d12_gp_add_vertex_attribute(gs_d3d12_gp_builder* b, const char* semantic_name, u32 semantic_index,
	DXGI_FORMAT format, u32 buffer, u32 offset);
void gs_d3d12_gp_set_topology_type(gs_d3d12_gp_builder* b, D3D12_PRIMITIVE_TOPOLOGY_TYPE type);
void gs_d3d12_gp_set_rasterization(gs_d3d12_gp_builder* b, D3D12_FILL_MODE fill, D3D12_CULL_MODE cull, int front_ccw);
void gs_d3d12_gp_set_multisamples(gs_d3d12_gp_builder* b, u32 multisamples);
void gs_d3d12_gp_set_depth(gs_d3d12_gp_builder* b, int test, int write, D3D12_COMPARISON_FUNC func);
void gs_d3d12_gp_set_stencil(gs_d3d12_gp_builder* b, int test, u8 read_mask, u8 write_mask,
	const D3D12_DEPTH_STENCILOP_DESC* front, const D3D12_DEPTH_STENCILOP_DESC* back);
void gs_d3d12_gp_set_no_stencil(gs_d3d12_gp_builder* b);
void gs_d3d12_gp_set_blend(gs_d3d12_gp_builder* b, u32 rt, int enable, D3D12_BLEND src, D3D12_BLEND dst,
	D3D12_BLEND_OP op, D3D12_BLEND alpha_src, D3D12_BLEND alpha_dst, D3D12_BLEND_OP alpha_op, u8 write_mask);
void gs_d3d12_gp_set_color_write_mask(gs_d3d12_gp_builder* b, u32 rt, u8 write_mask);
void gs_d3d12_gp_set_no_blending(gs_d3d12_gp_builder* b);
void gs_d3d12_gp_clear_render_targets(gs_d3d12_gp_builder* b);
void gs_d3d12_gp_set_render_target(gs_d3d12_gp_builder* b, u32 rt, DXGI_FORMAT format);
void gs_d3d12_gp_set_depth_stencil_format(gs_d3d12_gp_builder* b, DXGI_FORMAT format);

void gs_d3d12_cp_clear(gs_d3d12_cp_builder* b);
void gs_d3d12_cp_set_root_signature(gs_d3d12_cp_builder* b, ID3D12RootSignature* rs);
void gs_d3d12_cp_set_shader(gs_d3d12_cp_builder* b, const void* data, u32 data_size);

#ifdef __cplusplus
}

#include "common/RedtapeWilCom.h"

class D3D12ShaderCache;

/* The C++ device's builders: the C ones, and what makes their objects */
namespace D3D12
{
	class RootSignatureBuilder
	{
	public:
		enum : u32
		{
			MAX_PARAMETERS        = GS_D3D12_RS_MAX_PARAMETERS,
			MAX_DESCRIPTOR_RANGES = GS_D3D12_RS_MAX_RANGES
		};

		RootSignatureBuilder() { Clear(); }
		RootSignatureBuilder(const RootSignatureBuilder&) = delete;
		RootSignatureBuilder& operator=(const RootSignatureBuilder&) = delete;

		void Clear() { gs_d3d12_rs_clear(&m_b); }

		wil::com_ptr_nothrow<ID3D12RootSignature> Create(bool clear = true);

		void SetInputAssemblerFlag() { gs_d3d12_rs_set_input_assembler_flag(&m_b); }

		u32 Add32BitConstants(u32 shader_reg, u32 num_values, D3D12_SHADER_VISIBILITY visibility)
		{
			return gs_d3d12_rs_add_32bit_constants(&m_b, shader_reg, num_values, visibility);
		}
		u32 AddCBVParameter(u32 shader_reg, D3D12_SHADER_VISIBILITY visibility)
		{
			return gs_d3d12_rs_add_cbv(&m_b, shader_reg, visibility);
		}
		u32 AddSRVParameter(u32 shader_reg, D3D12_SHADER_VISIBILITY visibility)
		{
			return gs_d3d12_rs_add_srv(&m_b, shader_reg, visibility);
		}
		u32 AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE rt, u32 start_shader_reg, u32 num_shader_regs,
			D3D12_SHADER_VISIBILITY visibility)
		{
			return gs_d3d12_rs_add_descriptor_table(&m_b, rt, start_shader_reg, num_shader_regs, visibility);
		}

	private:
		gs_d3d12_rs_builder m_b;
	};

	class GraphicsPipelineBuilder
	{
	public:
		enum : u32
		{
			MAX_VERTEX_ATTRIBUTES = GS_D3D12_GP_MAX_VERTEX_ATTRIBUTES,
		};

		GraphicsPipelineBuilder() { Clear(); }
		GraphicsPipelineBuilder(const GraphicsPipelineBuilder&) = delete;
		GraphicsPipelineBuilder& operator=(const GraphicsPipelineBuilder&) = delete;

		/* (its input layout points into the builder: valid while it is) */
		__fi const D3D12_GRAPHICS_PIPELINE_STATE_DESC& GetDesc() const { return m_b.desc; }

		void Clear() { gs_d3d12_gp_clear(&m_b); }

		wil::com_ptr_nothrow<ID3D12PipelineState> Create(ID3D12Device* device, bool clear = true);
		wil::com_ptr_nothrow<ID3D12PipelineState> Create(ID3D12Device* device, D3D12ShaderCache& cache, bool clear = true);

		void SetRootSignature(ID3D12RootSignature* rs) { gs_d3d12_gp_set_root_signature(&m_b, rs); }

		void SetVertexShader(const void* data, u32 data_size) { gs_d3d12_gp_set_vertex_shader(&m_b, data, data_size); }
		void SetGeometryShader(const void* data, u32 data_size) { gs_d3d12_gp_set_geometry_shader(&m_b, data, data_size); }
		void SetPixelShader(const void* data, u32 data_size) { gs_d3d12_gp_set_pixel_shader(&m_b, data, data_size); }

		void SetVertexShader(const ID3DBlob* blob) { SetVertexShader(BlobData(blob), BlobSize(blob)); }
		void SetGeometryShader(const ID3DBlob* blob) { SetGeometryShader(BlobData(blob), BlobSize(blob)); }
		void SetPixelShader(const ID3DBlob* blob) { SetPixelShader(BlobData(blob), BlobSize(blob)); }

		void AddVertexAttribute(const char* semantic_name, u32 semantic_index, DXGI_FORMAT format, u32 buffer, u32 offset)
		{
			gs_d3d12_gp_add_vertex_attribute(&m_b, semantic_name, semantic_index, format, buffer, offset);
		}

		void SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE type) { gs_d3d12_gp_set_topology_type(&m_b, type); }

		void SetRasterizationState(D3D12_FILL_MODE polygon_mode, D3D12_CULL_MODE cull_mode, bool front_face_ccw)
		{
			gs_d3d12_gp_set_rasterization(&m_b, polygon_mode, cull_mode, front_face_ccw);
		}

		void SetMultisamples(u32 multisamples) { gs_d3d12_gp_set_multisamples(&m_b, multisamples); }

		void SetNoCullRasterizationState() { SetRasterizationState(D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, false); }

		void SetDepthState(bool depth_test, bool depth_write, D3D12_COMPARISON_FUNC compare_op)
		{
			gs_d3d12_gp_set_depth(&m_b, depth_test, depth_write, compare_op);
		}
		void SetStencilState(bool stencil_test, u8 read_mask, u8 write_mask, const D3D12_DEPTH_STENCILOP_DESC& front,
			const D3D12_DEPTH_STENCILOP_DESC& back)
		{
			gs_d3d12_gp_set_stencil(&m_b, stencil_test, read_mask, write_mask, &front, &back);
		}

		void SetNoDepthTestState() { SetDepthState(false, false, D3D12_COMPARISON_FUNC_ALWAYS); }
		void SetNoStencilState() { gs_d3d12_gp_set_no_stencil(&m_b); }

		void SetBlendState(u32 rt, bool blend_enable, D3D12_BLEND src_factor, D3D12_BLEND dst_factor, D3D12_BLEND_OP op,
			D3D12_BLEND alpha_src_factor, D3D12_BLEND alpha_dst_factor, D3D12_BLEND_OP alpha_op,
			u8 write_mask = D3D12_COLOR_WRITE_ENABLE_ALL)
		{
			gs_d3d12_gp_set_blend(&m_b, rt, blend_enable, src_factor, dst_factor, op, alpha_src_factor, alpha_dst_factor,
				alpha_op, write_mask);
		}
		void SetColorWriteMask(u32 rt, u8 write_mask = D3D12_COLOR_WRITE_ENABLE_ALL)
		{
			gs_d3d12_gp_set_color_write_mask(&m_b, rt, write_mask);
		}

		void SetNoBlendingState() { gs_d3d12_gp_set_no_blending(&m_b); }

		void ClearRenderTargets() { gs_d3d12_gp_clear_render_targets(&m_b); }
		void SetRenderTarget(u32 rt, DXGI_FORMAT format) { gs_d3d12_gp_set_render_target(&m_b, rt, format); }

		void ClearDepthStencilFormat() { gs_d3d12_gp_set_depth_stencil_format(&m_b, DXGI_FORMAT_UNKNOWN); }
		void SetDepthStencilFormat(DXGI_FORMAT format) { gs_d3d12_gp_set_depth_stencil_format(&m_b, format); }

	private:
		static const void* BlobData(const ID3DBlob* blob) { return const_cast<ID3DBlob*>(blob)->GetBufferPointer(); }
		static u32 BlobSize(const ID3DBlob* blob)
		{
			return static_cast<u32>(const_cast<ID3DBlob*>(blob)->GetBufferSize());
		}

		gs_d3d12_gp_builder m_b;
	};

	class ComputePipelineBuilder
	{
	public:
		ComputePipelineBuilder() { Clear(); }
		ComputePipelineBuilder(const ComputePipelineBuilder&) = delete;
		ComputePipelineBuilder& operator=(const ComputePipelineBuilder&) = delete;

		void Clear() { gs_d3d12_cp_clear(&m_b); }

		wil::com_ptr_nothrow<ID3D12PipelineState> Create(ID3D12Device* device, bool clear = true);
		wil::com_ptr_nothrow<ID3D12PipelineState> Create(ID3D12Device* device, D3D12ShaderCache& cache, bool clear = true);

		void SetRootSignature(ID3D12RootSignature* rs) { gs_d3d12_cp_set_root_signature(&m_b, rs); }
		void SetShader(const void* data, u32 data_size) { gs_d3d12_cp_set_shader(&m_b, data, data_size); }

	private:
		gs_d3d12_cp_builder m_b;
	};
} /* namespace D3D12 */
#endif

#endif
