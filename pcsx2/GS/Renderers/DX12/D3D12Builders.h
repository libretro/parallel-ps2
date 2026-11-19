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
#include <windows.h>

#include <d3d12.h>

/* Descriptions of root signatures and pipelines filled in a call at a
 * time; what makes the objects from them is the device's. Pointers in a description point into its builder. */

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
#endif

#endif
