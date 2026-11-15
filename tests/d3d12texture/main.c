/* The D3D12 textures (GS/Renderers/DX12/GSTexture12.c) against a device
 * and D3D12 that hash every command, view, resource and byte read
 * (fake.c): the scenario gives what the C++ GSTexture12 gave (the hash on
 * the command line). Windows only, as d3d12.h is: built with MinGW, run
 * under wine. */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GS/Renderers/DX12/GSTexture12.h"
#include "fake.h"

static void RETRO_CALLCONV quiet(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
retro_log_printf_t log_cb = quiet;

/* the device, as GSDevice12 is to the textures */
ID3D12Device* gs_d3d12_device_device(void) { return (ID3D12Device*)fk_device(); }
int gs_d3d12_device_create_placed_resource(const D3D12_RESOURCE_DESC* desc, D3D12_HEAP_TYPE heap_type,
	D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear_value, ID3D12Resource** resource,
	gs_d3d12_alloc_t* alloc)
{
	return fk_create_placed(desc, (unsigned)heap_type, (unsigned)state, clear_value, (void**)resource, alloc);
}
void gs_d3d12_device_defer_resource(const gs_d3d12_alloc_t* alloc, ID3D12Resource* resource)
{
	fk_defer_resource(alloc, resource);
}
gs_d3d12_descriptor_heap_manager* gs_d3d12_device_descriptor_heap(u32 kind) { return fk_heap(kind); }
void gs_d3d12_device_defer_descriptor(u32 kind, gs_d3d12_descriptor_handle* handle)
{
	if (handle->index == GS_D3D12_DESCRIPTOR_INVALID)
		return;
	fk_defer_descriptor(kind, handle->index);
	gs_d3d12_descriptor_handle_clear(handle);
}
void gs_d3d12_device_unbind_texture(gs_texture_d3d12* t) { fk_hu(0x0B1Dull ^ fk_resource_id(t->resource)); }
ID3D12GraphicsCommandList* gs_d3d12_device_command_list(void) { return (ID3D12GraphicsCommandList*)fk_command_list(); }
ID3D12GraphicsCommandList* gs_d3d12_device_init_command_list(void)
{
	return (ID3D12GraphicsCommandList*)fk_init_command_list();
}
void gs_d3d12_device_end_render_pass(void) { fk_end_render_pass(); }
void gs_d3d12_device_execute(int wait) { fk_execute(wait); }
void gs_d3d12_device_execute_for_readback(void) { fk_execute_for_readback(); }
struct gs_d3d12_stream_buffer* gs_d3d12_device_texture_stream_buffer(void) { return fk_stream(); }
void gs_d3d12_device_report_recurring(const char* what, HRESULT hr) { (void)what; fk_report((long)hr); }
void gs_d3d12_device_render_mipmap(gs_texture_d3d12* t, u32 dst_level, u32 dst_width, u32 dst_height, u32 src_level,
	u32 src_width, u32 src_height)
{
	fk_render_mipmap(t->resource, dst_level, dst_width, dst_height, src_level, src_width, src_height);
}

typedef gs_texture_d3d12* TEX;
typedef gs_download_texture* DTEX;

static int t_update(TEX t, const int* rc, const void* data, int pitch, int layer)
{
	gs_texture_rect r;
	r.left = rc[0]; r.top = rc[1]; r.right = rc[2]; r.bottom = rc[3];
	return t->base.m_ops->update(&t->base, &r, data, pitch, layer);
}
static int t_map(TEX t, const int* rc, int layer, unsigned char** bits, int* pitch)
{
	gs_texture_rect r;
	gs_texture_map m;
	int ok;
	if (rc)
	{
		r.left = rc[0]; r.top = rc[1]; r.right = rc[2]; r.bottom = rc[3];
	}
	ok     = t->base.m_ops->map(&t->base, &m, rc ? &r : NULL, layer);
	*bits  = m.bits;
	*pitch = m.pitch;
	return ok;
}
static void d_copy(DTEX d, const int* drc, TEX t, const int* src, u32 level, int use_transfer_pitch)
{
	gs_texture_rect a, b;
	a.left = drc[0]; a.top = drc[1]; a.right = drc[2]; a.bottom = drc[3];
	b.left = src[0]; b.top = src[1]; b.right = src[2]; b.bottom = src[3];
	d->m_ops->copy_from_texture(d, &a, &t->base, &b, level, use_transfer_pitch);
}
static int d_map(DTEX d, const int* rc)
{
	gs_texture_rect r;
	r.left = rc[0]; r.top = rc[1]; r.right = rc[2]; r.bottom = rc[3];
	return d->m_ops->map(d, &r);
}

#define T_CREATE(ty, f, w, h, l, dx, srv, rtv, dsv, uav) gs_texture_d3d12_create(ty, f, w, h, l, (DXGI_FORMAT)(dx), \
	(DXGI_FORMAT)(srv), (DXGI_FORMAT)(rtv), (DXGI_FORMAT)(dsv), (DXGI_FORMAT)(uav))
#define T_FREE(t) (t)->base.m_ops->free(&(t)->base)
#define T_UPDATE(t, rc, d, p, l) t_update(t, rc, d, p, l)
#define T_MAP(t, rc, l, b, p) t_map(t, rc, l, b, p)
#define T_UNMAP(t) (t)->base.m_ops->unmap(&(t)->base)
#define T_MIPMAP(t) (t)->base.m_ops->generate_mipmap(&(t)->base)
#define T_TRANSITION(t, s) gs_texture_d3d12_transition(t, gs_d3d12_device_command_list(), (D3D12_RESOURCE_STATES)(s))
#define T_TRANSITION_LEVEL(t, l, a, b) gs_texture_d3d12_transition_level(t, gs_d3d12_device_command_list(), l, \
	(D3D12_RESOURCE_STATES)(a), (D3D12_RESOURCE_STATES)(b))
#define T_COMMIT_CLEAR(t) gs_texture_d3d12_commit_clear(t, NULL)
#define T_COMMIT_CLEAR_CMD(t) gs_texture_d3d12_commit_clear(t, gs_d3d12_device_command_list())
#define T_SET_CLEAR_COLOR(t, c) ((t)->base.m_state = GS_TEXSTATE_CLEARED, (t)->base.m_clear_value.color = (c))
#define T_SET_CLEAR_DEPTH(t, d) ((t)->base.m_state = GS_TEXSTATE_CLEARED, (t)->base.m_clear_value.depth = (d))
#define T_SET_USE(t, f) ((t)->use_fence_counter = (f))
#define T_SET_NEEDS_MIPS(t, v) ((t)->base.m_needs_mipmaps_generated = (u8)(v))
#define T_DESTROY(t, d) gs_texture_d3d12_destroy(t, (int)(d))
#define T_STATE(t) (t)->base.m_state
#define T_RSTATE(t) (unsigned)(t)->resource_state
#define T_NEEDS_MIPS(t) (t)->base.m_needs_mipmaps_generated
#define T_RES(t) (t)->resource
#define T_SRV(t) (t)->srv_descriptor.index
#define T_WRITE(t) (t)->write_descriptor.index
#define T_UAV(t) (t)->uav_descriptor.index
#define T_W(t) (t)->base.m_size.x
#define T_H(t) (t)->base.m_size.y
#define T_TYPE(t) (t)->base.m_type
#define T_FORMAT(t) (t)->base.m_format
#define D_CREATE(w, h, f) gs_download_texture_d3d12_create(w, h, f)
#define D_FREE(d) (d)->m_ops->free(d)
#define D_COPY(d, drc, t, src, l, tp) d_copy(d, drc, t, src, l, tp)
#define D_MAP(d, rc) d_map(d, rc)
#define D_UNMAP(d) (d)->m_ops->unmap(d)
#define D_FLUSH(d) (d)->m_ops->flush(d)
#define D_PTR(d) (d)->m_map_pointer
#define D_PITCH(d) (d)->m_current_pitch
#define D_FORMAT(d) (d)->m_format
#define D_W(d) (d)->m_width
#define D_H(d) (d)->m_height

#include "scenario.h"

int main(int argc, char** argv)
{
	fk_init();
	scenario();
	fk_shutdown();
	printf("d3d12texture: %016llx\n", fk_hash);
	if (fk_live_resources() != 0)
	{
		printf("FAIL: %d resources never released\n", fk_live_resources());
		return 1;
	}
	if (argc > 1 && strtoull(argv[1], NULL, 16) != fk_hash)
	{
		printf("FAIL: the C++ GSTexture12 gave %s\n", argv[1]);
		return 1;
	}
	return 0;
}
