/* The Vulkan textures (GS/Renderers/Vulkan/GSTextureVK.c) against a
 * device that hashes what they ask of it (fake.h), through the scenario
 * of scenario.h: the hash is what the C++ GSTextureVK gave, which
 * build.sh holds. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include "GS/Renderers/Vulkan/GSTextureVK.h"

static void quiet(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
retro_log_printf_t log_cb = quiet;

#include "fake.h"

/* the hooks, as the device answers them */
VkDevice gs_vk_device_handle(void) { return (VkDevice)(size_t)0x77; }
VkFormat gs_vk_device_native_format(unsigned format) { return fk_format(format); }
int gs_vk_device_create_image(const VkImageCreateInfo* ici, VkImage* image, gs_vk_alloc_t* alloc)
{
	return fk_create_image(ici, image, alloc);
}
void gs_vk_device_image_counts(u64* made, u64* destroyed) { *made = fk_made; *destroyed = fk_destroyed; }
void gs_vk_device_count_image_destroyed(void) { fk_destroyed++; HU(0xc0d); }
void gs_vk_device_unbind_texture(gs_texture_vk* t) { (void)t; fk_unbind(); }
void gs_vk_device_defer_framebuffer(VkFramebuffer fb) { fk_defer_fb(fb); }
void gs_vk_device_defer_image_view(VkImageView view) { fk_defer_view(view); }
void gs_vk_device_defer_image(VkImage image, const gs_vk_alloc_t* alloc, u64 bytes) { fk_defer_image(image, alloc, bytes); }
VkCommandBuffer gs_vk_device_command_buffer(void) { return fk_cmd; }
VkCommandBuffer gs_vk_device_init_command_buffer(void) { return fk_init_cmdbuf(); }
void gs_vk_device_end_render_pass(void) { fk_end_render_pass(); }
void gs_vk_device_execute(int wait) { fk_execute(wait); }
VkBuffer gs_vk_device_staging_buffer(u32 size, void** mapped, gs_vk_alloc_t* alloc) { return fk_staging(size, mapped, alloc); }
struct gs_vk_stream_buffer* gs_vk_device_upload_buffer(void) { return &fk_upload; }
u32 gs_vk_device_copy_offset_alignment(void) { return fk_offset_align; }
u32 gs_vk_device_copy_pitch_alignment(void) { return fk_pitch_align; }
int gs_vk_device_stencil_buffer(void) { return fk_stencil; }
VkRenderPass gs_vk_device_load_store_pass(VkFormat color, VkFormat depth, int feedback_loop)
{
	return fk_pass(color, depth, feedback_loop);
}
int gs_vk_device_create_readback_buffer(u32 size, VkBuffer* buffer, gs_vk_alloc_t* alloc, void** mapped)
{
	return fk_readback(size, buffer, alloc, mapped);
}

typedef gs_texture_vk* TEX;
typedef gs_download_texture* DTEX;

static gs_texture_rect sc_rect(const int* r)
{
	gs_texture_rect g;
	g.left = r[0]; g.top = r[1]; g.right = r[2]; g.bottom = r[3];
	return g;
}
static int sc_map(TEX t, const int* rc, int layer, unsigned char** bits, int* pitch)
{
	gs_texture_map m;
	gs_texture_rect r;
	int ok;
	if (rc)
		r = sc_rect(rc);
	ok = t->base.m_ops->map(&t->base, &m, rc ? &r : NULL, layer);
	*bits  = m.bits;
	*pitch = m.pitch;
	return ok;
}
static int sc_update(TEX t, const int* rc, const void* data, int pitch, int layer)
{
	const gs_texture_rect r = sc_rect(rc);
	return t->base.m_ops->update(&t->base, &r, data, pitch, layer);
}
static void sc_copy(DTEX d, const int* drc, TEX t, const int* src, int level, int use_tp)
{
	const gs_texture_rect a = sc_rect(drc);
	const gs_texture_rect b = sc_rect(src);
	d->m_ops->copy_from_texture(d, &a, &t->base, &b, (u32)level, use_tp);
}
static int sc_dmap(DTEX d, const int* rc)
{
	const gs_texture_rect r = sc_rect(rc);
	const int ok = d->m_ops->map(d, &r);
	H(d->m_map_pointer, 64);
	return ok;
}

#define T_CREATE(ty, f, w, h, l) gs_texture_vk_create(ty, f, (int)(w), (int)(h), (int)(l))
#define T_FREE(t) (t)->base.m_ops->free(&(t)->base)
#define T_LAYOUT(t) (t)->layout
#define T_STATE(t) (t)->base.m_state
#define T_NEEDS_MIPS(t) (t)->base.m_needs_mipmaps_generated
#define T_IMAGE(t) (t)->image
#define T_VIEW(t) (t)->view
#define T_W(t) (t)->base.m_size.x
#define T_H(t) (t)->base.m_size.y
#define T_TYPE(t) (t)->base.m_type
#define T_SET_USE(t, f) ((t)->use_fence_counter = (f))
#define T_UPDATE(t, rc, d, p, l) sc_update(t, rc, d, p, l)
#define T_MAP(t, rc, l, b, p) sc_map(t, rc, l, b, p)
#define T_UNMAP(t) (t)->base.m_ops->unmap(&(t)->base)
#define T_MIPMAP(t) (t)->base.m_ops->generate_mipmap(&(t)->base)
#define T_TRANSITION(t, l) gs_texture_vk_transition(t, gs_vk_device_command_buffer(), l)
#define T_TRANSITION_CMD(t, l) gs_texture_vk_transition(t, fk_cmd, l)
#define T_LEVELS(t, s, n, a, b) gs_texture_vk_transition_levels(t, fk_cmd, (int)(s), (int)(n), a, b)
#define T_SET_CLEAR_COLOR(t, c) ((t)->base.m_state = GS_TEXSTATE_CLEARED, (t)->base.m_clear_value.color = (c))
#define T_SET_CLEAR_DEPTH(t, d) ((t)->base.m_state = GS_TEXSTATE_CLEARED, (t)->base.m_clear_value.depth = (d))
#define T_SET_NEEDS_MIPS(t, v) ((t)->base.m_needs_mipmaps_generated = (u8)(v))
#define T_SET_STATE(t, s) ((t)->base.m_state = (u8)(s))
#define T_COMMIT_CLEAR(t) gs_texture_vk_commit_clear(t, VK_NULL_HANDLE)
#define T_COMMIT_CLEAR_CMD(t) gs_texture_vk_commit_clear(t, fk_cmd)
#define T_FB(t, d, l) gs_texture_vk_framebuffer(t, d, (int)(l))
#define T_DESTROY(t, d) gs_texture_vk_destroy(t, (int)(d))
#define D_CREATE(w, h, f) gs_download_texture_vk_create(w, h, f)
#define D_FREE(d) (d)->m_ops->free(d)
#define D_W(d) (d)->m_width
#define D_H(d) (d)->m_height
#define D_PITCH(d) (d)->m_current_pitch
#define D_COPY(d, drc, t, src, l, tp) sc_copy(d, drc, t, src, l, tp)
#define D_FLUSH(d) (d)->m_ops->flush(d)
#define D_MAP(d, rc) sc_dmap(d, rc)
#define D_UNMAP(d) (d)->m_ops->unmap(d)

#include "scenario.h"

int main(int argc, char** argv)
{
	unsigned long long want = argc > 1 ? strtoull(argv[1], NULL, 16) : 0;
	fk_install();
	gs_vk_stream_buffer_init(&fk_upload);
	if (!gs_vk_stream_buffer_create(&fk_upload, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 512 * 1024))
		return 2;
	scenario();
	printf("vktexture: %016llx\n", fk_hash);
	if (want && want != fk_hash)
	{
		printf("FAIL: the C++ GSTextureVK gave %016llx\n", want);
		return 1;
	}
	return 0;
}
