/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#ifndef GS_DEVICE11_H
#define GS_DEVICE11_H

/* The Direct3D 11 device (gs_device_new_d3d11): the frontend's device and
 * context, through the table in GSDevice11.c */

#include <boolean.h>

#include "common/Pcsx2Defs.h"
#include "D3D.h"
#include "D3D11ShaderCache.h"
#include "D3DDynamicPS.h"
#include "GSTexture11.h"
#include "GS/Renderers/Common/GSDeviceBase.h"
#include "GS/Renderers/Common/GSFlatMap.h"
#include "GS/Renderers/Common/GSHWDrawConfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GS_D3D11_MAX_TEXTURES 4
#define GS_D3D11_VERTEX_BUFFER_SIZE (32 * 1024 * 1024)
#define GS_D3D11_INDEX_BUFFER_SIZE (16 * 1024 * 1024)

/* A blend state's key: the colour mask, then the blend */
typedef union gs_d3d11_blend_selector
{
	struct
	{
		gs_hw_color_mask colormask;
		u8 pad[3];
		gs_hw_blend_state blend;
	} s;
	u64 key;
} gs_d3d11_blend_selector;

typedef struct gs_d3d11_vertex_shader
{
	ID3D11VertexShader* vs;
	ID3D11InputLayout* il;
	bool built; /* (tried: either may be null) */
} gs_d3d11_vertex_shader;

struct gs_d3d11_ps_worker;

typedef struct gs_device_d3d11
{
	gs_device base;

	ID3D11Device1* dev;
	/* The context the frontend handed over, as the base interface: that
	 * is all this renderer calls through, with one exception below. A
	 * frontend running threaded video gives a hardware core a deferred
	 * context behind a proxy, and the proxy answers for the base
	 * interface only. */
	ID3D11DeviceContext* ctx;
	/* The same context as 11.1, when it has one; null otherwise. Only
	 * DiscardView wants it, and DiscardView is a hint. */
	ID3D11DeviceContext1* ctx1;

	ID3D11Buffer* vb;
	ID3D11Buffer* ib;
	ID3D11Buffer* expand_vb;
	ID3D11Buffer* expand_ib;
	ID3D11ShaderResourceView* expand_vb_srv;

	D3D_FEATURE_LEVEL feature_level;
	u32 vb_pos;            /* bytes */
	u32 ib_pos;            /* indices */
	u32 structured_vb_pos; /* bytes */
	int d3d_texsize;

	struct
	{
		D3D11_PRIMITIVE_TOPOLOGY topology;
		ID3D11InputLayout* layout;
		ID3D11Buffer* index_buffer;
		ID3D11VertexShader* vs;
		ID3D11Buffer* vs_cb;
		ID3D11ShaderResourceView* ps_sr_views[GS_D3D11_MAX_TEXTURES];
		ID3D11PixelShader* ps;
		ID3D11Buffer* ps_cb;
		ID3D11SamplerState* ps_ss[1];
		gs_point viewport;
		gs_recti scissor;
		u32 vb_stride;
		ID3D11DepthStencilState* dss;
		u8 sref;
		ID3D11BlendState* bs;
		u8 bf;
		ID3D11RenderTargetView* rt_view;
		ID3D11DepthStencilView* dsv;
	} state;

	struct
	{
		ID3D11InputLayout* il;
		ID3D11VertexShader* vs;
		ID3D11PixelShader* ps[GS_SHADER_CONVERT_COUNT];
		ID3D11SamplerState* ln;
		ID3D11SamplerState* pt;
		ID3D11DepthStencilState* dss;
		ID3D11DepthStencilState* dss_write;
		ID3D11BlendState* bs[16];
	} convert;

	struct
	{
		ID3D11PixelShader* ps[2];
		ID3D11Buffer* cb;
		ID3D11BlendState* bs;
	} merge;

	struct
	{
		ID3D11PixelShader* ps[GS_SHADER_INTERLACE_COUNT];
		ID3D11Buffer* cb;
	} interlace;

	struct
	{
		ID3D11DepthStencilState* dss;
		ID3D11BlendState* bs;
		ID3D11PixelShader* primid_init_ps[4];
	} date;

	/* The shaders and states by key: the vertex shaders, samplers and
	 * depth states in a slot each, the pixel shaders and blend states in
	 * maps sized as the device is made */
	gs_d3d11_vertex_shader vs[256];
	ID3D11Buffer* vs_cb;
	gs_flat_map_t ps;   /* gs_hw_ps_selector -> ID3D11PixelShader* (null: queued, or failed) */
	ID3D11Buffer* ps_cb;
	ID3D11SamplerState* ps_ss[256];
	ID3D11DepthStencilState* om_dss[32];
	gs_flat_map_t om_bs; /* u64 -> ID3D11BlendState* */

	/* The draws of one frame come in runs that share their state; the
	 * last key of each lookup is kept with what it found, and the lookup
	 * is skipped while the key has not changed. */
	u32 last_vs_key;
	const gs_d3d11_vertex_shader* last_vs;
	gs_hw_ps_selector last_ps_sel;
	bool last_ps_valid;
	ID3D11PixelShader* last_ps;
	u32 last_ps_ss_key;
	ID3D11SamplerState* last_ps_ss;
	u32 last_dss_key;
	ID3D11DepthStencilState* last_dss;
	u64 last_bs_key;
	ID3D11BlendState* last_bs;
	ID3D11RasterizerState* rs;

	gs_hw_vs_cb vs_cb_cache;
	gs_hw_ps_cb ps_cb_cache;

	/* No shader is compiled on the draw path. Every vertex shader and the
	 * dynamic pixel shaders (one per interface, gs_d3d_dynamic_index: they
	 * read the selector from ps_key_cb) are built as the device is
	 * created; a pixel selector not built yet draws with its dynamic one
	 * while the worker builds its own (ps holds a null one meanwhile).
	 * From the worker's start the shader cache is the worker's. */
	ID3D11PixelShader* ps_dynamic[D3D_DYNAMIC_PS_COUNT];
	ID3D11Buffer* ps_key_cb;
	u32 ps_key[4];
	bool ps_key_valid;
	struct gs_d3d11_ps_worker* ps_worker;
	u32 shaders_built_here;  /* on the draw path: none, but without the worker */
	u32 shaders_built_ahead; /* as the device was created */
	u32 ps_objects_here;     /* worker's shaders made into objects here */
	u32 dynamic_draws;
	bool test_dynamic;

	gs_d3d11_shader_cache shader_cache;
	gs_d3d_macros sm; /* the render thread's */
} gs_device_d3d11;

#ifdef __cplusplus
}
#endif

#endif
