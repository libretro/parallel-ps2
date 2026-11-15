/* The D3D12 builders (GS/Renderers/DX12/D3D12Builders.c): every call at
 * random, the descriptions they fill hashed after each against what the
 * C++ builders filled (the hash below was taken from them, scenario.h).
 * Windows only, as d3d12.h is: built with MinGW, run under wine. */

#include <stdint.h>
#include <stdio.h>

#include "GS/Renderers/DX12/D3D12Builders.h"

#define GP gs_d3d12_gp_builder
#define RS gs_d3d12_rs_builder
#define CP gs_d3d12_cp_builder
#define GP_DESCP(b) (&(b)->desc)
#define GP_CLEAR(b) gs_d3d12_gp_clear(b)
#define GP_ROOT(b, r) gs_d3d12_gp_set_root_signature(b, r)
#define GP_VS(b, d, n) gs_d3d12_gp_set_vertex_shader(b, d, n)
#define GP_GS(b, d, n) gs_d3d12_gp_set_geometry_shader(b, d, n)
#define GP_PS(b, d, n) gs_d3d12_gp_set_pixel_shader(b, d, n)
#define GP_ATTR(b, n, i, f, s, o) gs_d3d12_gp_add_vertex_attribute(b, n, i, f, s, o)
#define GP_TOPO(b, t) gs_d3d12_gp_set_topology_type(b, t)
#define GP_RASTER(b, f, c, ccw) gs_d3d12_gp_set_rasterization(b, f, c, ccw)
#define GP_MSAA(b, n) gs_d3d12_gp_set_multisamples(b, n)
#define GP_NOCULL(b) gs_d3d12_gp_set_rasterization(b, D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, 0)
#define GP_DEPTH(b, t, w, f) gs_d3d12_gp_set_depth(b, t, w, f)
#define GP_STENCIL(b, t, r, w, f, k) gs_d3d12_gp_set_stencil(b, t, r, w, f, k)
#define GP_NODEPTH(b) gs_d3d12_gp_set_depth(b, 0, 0, D3D12_COMPARISON_FUNC_ALWAYS)
#define GP_NOSTENCIL(b) gs_d3d12_gp_set_no_stencil(b)
#define GP_BLEND(b, rt, e, s, d, o, as, ad, ao, m) gs_d3d12_gp_set_blend(b, rt, e, s, d, o, as, ad, ao, m)
#define GP_WRITEMASK(b, rt, m) gs_d3d12_gp_set_color_write_mask(b, rt, m)
#define GP_NOBLEND(b) gs_d3d12_gp_set_no_blending(b)
#define GP_CLEAR_RTS(b) gs_d3d12_gp_clear_render_targets(b)
#define GP_RT(b, rt, f) gs_d3d12_gp_set_render_target(b, rt, f)
#define GP_CLEAR_DS(b) gs_d3d12_gp_set_depth_stencil_format(b, DXGI_FORMAT_UNKNOWN)
#define GP_DS(b, f) gs_d3d12_gp_set_depth_stencil_format(b, f)
#define RS_DESCP(b) (&(b)->desc)
#define RS_RANGES(b) (b)->num_ranges
#define RS_CLEAR(b) gs_d3d12_rs_clear(b)
#define RS_IA(b) gs_d3d12_rs_set_input_assembler_flag(b)
#define RS_CONST(b, r, n, v) gs_d3d12_rs_add_32bit_constants(b, r, n, v)
#define RS_CBV(b, r, v) gs_d3d12_rs_add_cbv(b, r, v)
#define RS_SRV(b, r, v) gs_d3d12_rs_add_srv(b, r, v)
#define RS_TABLE(b, t, s, n, v) gs_d3d12_rs_add_descriptor_table(b, t, s, n, v)
#define CP_DESCP(b) (&(b)->desc)
#define CP_CLEAR(b) gs_d3d12_cp_clear(b)
#define CP_ROOT(b, r) gs_d3d12_cp_set_root_signature(b, r)
#define CP_SHADER(b, d, n) gs_d3d12_cp_set_shader(b, d, n)

#include "scenario.h"

/* the C++ builders' hash */
#define EXPECTED 0x08a26b69u

static GP s_gp;
static RS s_rs;
static CP s_cp;

int main(int argc, char** argv)
{
	u32 h;
	gs_d3d12_gp_clear(&s_gp);
	gs_d3d12_rs_clear(&s_rs);
	gs_d3d12_cp_clear(&s_cp);
	h = run_scenario(&s_gp, &s_rs, &s_cp);
	printf("d3d12builders: %08x\n", (unsigned)h);
	if (argc > 1 && argv[1][0] == '-')
		return 0;
	if (h != EXPECTED)
	{
		printf("FAIL: the descriptions are not what the C++ builders filled\n");
		return 1;
	}
	printf("d3d12builders: ok\n");
	return 0;
}
