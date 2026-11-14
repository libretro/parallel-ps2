/* The Vulkan builders (GS/Renderers/Vulkan/VKBuilders.c) against a
 * device that hashes what it is asked to create (fake.h): the scenario
 * (scenario.h) gives the hash the C++ builders gave, which build.sh
 * holds; and what the scenario builds stays inside each builder's
 * arrays. */
#include <stdio.h>
#include <stdlib.h>

#include <libretro.h>
#include "GS/Renderers/Vulkan/VKBuilders.h"

static void quiet(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
retro_log_printf_t log_cb = quiet;

#include "fake.h"

#define GP_DECL struct gs_vk_gp_builder gpb
#define GP_BEGIN gs_vk_gp_init(&gpb)
#define GP_PROV(m) gs_vk_gp_set_provoking_vertex(&gpb, m)
#define GP_LAYOUT(l) gs_vk_gp_set_layout(&gpb, l)
#define GP_VB(b, s) gs_vk_gp_add_vertex_buffer(&gpb, b, s)
#define GP_VA(l, b, f, o) gs_vk_gp_add_vertex_attribute(&gpb, l, b, f, o)
#define GP_TOPO(t) gs_vk_gp_set_topology(&gpb, t)
#define GP_DYN(s) gs_vk_gp_add_dynamic_state(&gpb, s)
#define GP_DYN_VS do { gs_vk_gp_add_dynamic_state(&gpb, VK_DYNAMIC_STATE_VIEWPORT); \
	gs_vk_gp_add_dynamic_state(&gpb, VK_DYNAMIC_STATE_SCISSOR); } while (0)
#define GP_NOCULL gs_vk_gp_set_rasterization(&gpb, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE)
#define GP_RP(r, s) gs_vk_gp_set_render_pass(&gpb, r, s)
#define GP_VS(m, si) gs_vk_gp_set_shader(&gpb, VK_SHADER_STAGE_VERTEX_BIT, m, si)
#define GP_FS(m, si) gs_vk_gp_set_shader(&gpb, VK_SHADER_STAGE_FRAGMENT_BIT, m, si)
#define GP_STENCIL(t, f, b) gs_vk_gp_set_stencil(&gpb, t, f, b)
#define GP_DEPTH(t, w, o) gs_vk_gp_set_depth(&gpb, t, w, o)
#define GP_MASK(a, m) gs_vk_gp_set_write_mask(&gpb, a, m)
#define GP_BLEND(at, e, s, d, o, as, ad, ao, m) gs_vk_gp_set_blend_attachment(&gpb, at, e, s, d, o, as, ad, ao, m)
#define GP_RAST(p, c, f) gs_vk_gp_set_rasterization(&gpb, p, c, f)
#define GP_LINE(m) gs_vk_gp_set_line_rasterization(&gpb, m)
#define GP_FLAGS(f) gs_vk_gp_add_blend_flags(&gpb, f)
#define GP_LIB(p) gs_vk_gp_set_library(&gpb, p)
#define GP_LINKED(l, n) gs_vk_gp_set_linked_libraries(&gpb, l, n)
#define GP_CREATE(d, c, out) (out = gs_vk_gp_create(&gpb, d, c))

#define DSL_DECL struct gs_vk_dsl_builder dslb
#define DSL_BEGIN gs_vk_dsl_init(&dslb)
#define DSL_ADD(b, t, c, s) gs_vk_dsl_add_binding(&dslb, b, t, c, s)
#define DSL_CREATE(d, out) (out = gs_vk_dsl_create(&dslb, d))
#define PL_DECL struct gs_vk_pl_builder plb
#define PL_BEGIN gs_vk_pl_init(&plb)
#define PL_SET(l) gs_vk_pl_add_set(&plb, l)
#define PL_PUSH(s, o, n) gs_vk_pl_add_push_constants(&plb, s, o, n)
#define PL_CREATE(d, out) (out = gs_vk_pl_create(&plb, d))

#define DSU_DECL struct gs_vk_ds_update dsub
#define DSU_BEGIN gs_vk_dsu_init(&dsub)
#define DSU_CIS(s, b, v, sm, l) gs_vk_dsu_add_image(&dsub, s, b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, v, sm, l)
#define DSU_IMG(s, b, v, l) gs_vk_dsu_add_image(&dsub, s, b, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, v, VK_NULL_HANDLE, l)
#define DSU_IA(s, b, v, l) gs_vk_dsu_add_image(&dsub, s, b, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, v, VK_NULL_HANDLE, l)
#define DSU_BUF(s, b, t, buf, o, n) gs_vk_dsu_add_buffer(&dsub, s, b, t, buf, o, n)
#define DSU_UPDATE(d) gs_vk_dsu_update(&dsub, d)

#define FB_DECL struct gs_vk_fb_builder fbb
#define FB_BEGIN(r, w, h) gs_vk_fb_init(&fbb, r, w, h)
#define FB_ADD(v) gs_vk_fb_add_attachment(&fbb, v)
#define FB_CREATE(d, out) (out = gs_vk_fb_create(&fbb, d))

#define CHAIN(h, p) gs_vk_add_to_chain(h, p)

#include "scenario.h"

int main(int argc, char** argv)
{
	unsigned long long want = argc > 1 ? strtoull(argv[1], NULL, 16) : 0;
	fake_install();
	scenario((VkDevice)(size_t)0x77);
	printf("vkbuilders: %016llx\n", vkb_hash);
	if (want && want != vkb_hash)
	{
		printf("FAIL: the C++ builders gave %016llx\n", want);
		return 1;
	}
	return 0;
}
