/* The D3D11 textures (GS/Renderers/DX11/GSTexture11.c) against a device
 * and context that hash every texture, view, update, copy and map
 * (fake.c): the scenario gives what the C++ GSTexture11 gave (the hash on
 * the command line). Windows only, as d3d11.h is: built with MinGW, run
 * under wine. */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GS/Renderers/DX11/GSTexture11.h"
#include "fake.h"

static void RETRO_CALLCONV quiet(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
retro_log_printf_t log_cb = quiet;

ID3D11Device* gs_d3d11_device_device(void) { return (ID3D11Device*)fk_device(); }
ID3D11DeviceContext* gs_d3d11_device_context(void) { return (ID3D11DeviceContext*)fk_context(); }
void gs_d3d11_device_commit_clear(gs_texture_d3d11* t) { fk_hu(0xC1EA2ull ^ fk_id(t->texture)); }

typedef gs_texture_d3d11* TEX;
typedef gs_download_texture* DTEX;

static int t_update(TEX t, const int* rc, const void* data, int pitch, int layer)
{
	gs_texture_rect r;
	r.left = rc[0]; r.top = rc[1]; r.right = rc[2]; r.bottom = rc[3];
	return t->base.m_ops->update(&t->base, &r, data, pitch, layer);
}
static int t_map(TEX t, unsigned char** bits, int* pitch)
{
	gs_texture_map m;
	int ok;
	m.bits  = NULL;
	m.pitch = 0;
	ok      = t->base.m_ops->map(&t->base, &m, NULL, 0);
	*bits   = m.bits;
	*pitch  = m.pitch;
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

#define T_CREATE(ty, f, w, h, l) gs_texture_d3d11_create(ty, f, w, h, l)
#define T_FREE(t) (t)->base.m_ops->free(&(t)->base)
#define T_UPDATE(t, rc, d, p, l) t_update(t, rc, d, p, l)
#define T_MAP(t, b, p) t_map(t, b, p)
#define T_UNMAP(t) (t)->base.m_ops->unmap(&(t)->base)
#define T_MIPMAP(t) (t)->base.m_ops->generate_mipmap(&(t)->base)
#define T_SET_NEEDS_MIPS(t, v) ((t)->base.m_needs_mipmaps_generated = (u8)(v))
#define T_SRV(t) gs_texture_d3d11_srv(t)
#define T_RTV(t) gs_texture_d3d11_rtv(t)
#define T_DSV(t) gs_texture_d3d11_dsv(t)
#define T_UAV(t) gs_texture_d3d11_uav(t)
#define T_TEXTURE(t) (t)->texture
#define T_W(t) (t)->base.m_size.x
#define T_H(t) (t)->base.m_size.y
#define T_LEVELS(t) (t)->base.m_mipmap_levels
#define T_NEEDS_MIPS(t) (t)->base.m_needs_mipmaps_generated
#define T_TYPE(t) (t)->base.m_type
#define T_FORMAT(t) (t)->base.m_format
#define D_CREATE(w, h, f) gs_download_texture_d3d11_create(w, h, f)
#define D_FREE(d) (d)->m_ops->free(d)
#define D_COPY(d, drc, t, src, l, tp) d_copy(d, drc, t, src, l, tp)
#define D_MAP(d, rc) d_map(d, rc)
#define D_UNMAP(d) (d)->m_ops->unmap(d)
#define D_FLUSH(d) (d)->m_ops->flush(d)
#define D_PTR(d) (d)->m_map_pointer
#define D_PITCH(d) (d)->m_current_pitch
#define D_W(d) (d)->m_width
#define D_H(d) (d)->m_height

#include "scenario.h"

int main(int argc, char** argv)
{
	fk_init();
	scenario();
	printf("d3d11texture: %016llx\n", fk_hash);
	if (fk_live_objects() != 0)
	{
		printf("FAIL: %d objects never released\n", fk_live_objects());
		return 1;
	}
	if (argc > 1 && strtoull(argv[1], NULL, 16) != fk_hash)
	{
		printf("FAIL: the C++ GSTexture11 gave %s\n", argv[1]);
		return 1;
	}
	return 0;
}
