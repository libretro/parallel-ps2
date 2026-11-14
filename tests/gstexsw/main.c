/* The SW texture (GS/Renderers/SW/GSTextureSW.c) and the texture common
 * part it is made of (GSTextureBase.h): textures of every type and format
 * updated, mapped and freed through the ops table answer, pad and hold
 * their texels as the C++ class did (the hash below was taken from it);
 * then the common part as a texture starts, the edges of Update, and the
 * CPU read-back texture. */

#include <stdio.h>
#include <string.h>

#include "GS/Renderers/SW/GSTextureSW.h"

typedef gs_texture* T_TEX;

static int t_update(gs_texture* t, int l, int tp, int r, int b, const void* src, int pitch)
{
	gs_texture_rect rc;
	rc.left   = l;
	rc.top    = tp;
	rc.right  = r;
	rc.bottom = b;
	return t->m_ops->update(t, &rc, src, pitch, 0);
}

static int t_map(gs_texture* t, int use_rect, int x, int y, u8** bits, int* pitch)
{
	gs_texture_map m;
	gs_texture_rect rc;
	int ok;
	rc.left   = x;
	rc.top    = y;
	rc.right  = x + 1;
	rc.bottom = y + 1;
	ok = t->m_ops->map(t, &m, use_rect ? &rc : NULL, 0);
	if (ok)
	{
		*bits  = m.bits;
		*pitch = m.pitch;
	}
	return ok;
}

#define T_CREATE(type, w, h, f)               gs_texture_sw_create(type, w, h, f)
#define T_PITCH(t)                            (((gs_texture_sw*)(t))->pitch)
#define T_DATA(t)                             (((gs_texture_sw*)(t))->data)
#define T_UPDATE(t, l, tp, r, b, s, p)        t_update(t, l, tp, r, b, s, p)
#define T_MAP(t, u, x, y, bits, pitch)        t_map(t, u, x, y, bits, pitch)
#define T_UNMAP(t)                            (t)->m_ops->unmap(t)
#define T_FREE(t)                             (t)->m_ops->free(t)

#include "scenario.h"

/* The C++ GSTextureSW's hash of this scenario */
#define EXPECTED 0x442aabffu

static int s_failed;

static void check(int ok, const char* what)
{
	if (!ok)
	{
		printf("FAIL: %s\n", what);
		s_failed = 1;
	}
}

int main(int argc, char** argv)
{
	const u32 h = run_scenario();
	printf("sw textures %08x\n", (unsigned)h);
	if (argc > 1 && !strcmp(argv[1], "--print"))
		return 0;
	check(h == EXPECTED, "the SW textures behave as the C++ class did");

	{
		gs_texture* t = gs_texture_sw_create(GS_TEXTYPE_RENDER_TARGET, 33, 7, GS_TEX_COLOR);
		gs_texture_sw* sw = (gs_texture_sw*)t;
		check(t != NULL, "made");
		check(t->m_type == GS_TEXTYPE_RENDER_TARGET && t->m_format == GS_TEX_COLOR, "its type and format");
		check(t->m_size.x == 33 && t->m_size.y == 7 && t->m_mipmap_levels == 1, "its size, one level");
		check(t->m_state == GS_TEXSTATE_DIRTY && t->m_needs_mipmaps_generated && t->m_last_frame_used == 0
				&& t->m_clear_value.color == 0, "the common part as a texture starts");
		check(sw->pitch == 160 && ((size_t)sw->data & 31) == 0, "rows of 33 texels padded to 160 bytes, aligned");
		check(t->m_ops->generate_mipmap == NULL, "no mipmaps to generate");
		check(!t_update(t, 0, 0, 34, 1, sw->data, 160), "an update past the right edge is refused");
		check(!t_update(t, -1, 0, 1, 1, sw->data, 160), "and one left of it");
		check(!t_update(t, 0, 0, 0, 1, sw->data, 160), "and an empty one");
		check(t_update(t, 32, 6, 33, 7, sw->data, 160), "the last texel is taken");
		t->m_ops->free(t);
	}
	{
		gs_texture* t = gs_texture_sw_create(GS_TEXTYPE_TEXTURE, 3, 0, GS_TEX_UNORM8);
		check(t != NULL && ((gs_texture_sw*)t)->pitch == 32, "a texture of no rows still has storage");
		t->m_ops->free(t);
	}

	{
		gs_download_texture* d = gs_download_texture_sw_create(33, 5, GS_TEX_COLOR);
		gs_download_texture_sw* sw = (gs_download_texture_sw*)d;
		gs_texture_rect rc;
		size_t i;
		int zero = 1;
		rc.left = rc.top = 0;
		rc.right = 33;
		rc.bottom = 5;
		check(d != NULL && d->m_width == 33 && d->m_height == 5 && d->m_format == GS_TEX_COLOR, "a download texture");
		check(d->m_current_pitch == 160 && sw->buffer_size == 800, "rows padded as the textures'");
		for (i = 0; i < sw->buffer_size; i++)
			zero &= sw->buffer[i] == 0;
		check(zero && d->m_map_pointer == NULL && !d->m_needs_flush, "zeroed, unmapped, nothing to flush");
		d->m_ops->copy_from_texture(d, &rc, NULL, &rc, 0, 1);
		d->m_ops->flush(d);
		check(d->m_ops->map(d, &rc) && d->m_map_pointer == sw->buffer, "mapped: its rows");
		d->m_ops->unmap(d);
		check(d->m_map_pointer == NULL, "unmapped");
		d->m_ops->free(d);
	}

	if (s_failed)
		return 1;
	printf("gstexsw: ok\n");
	return 0;
}
