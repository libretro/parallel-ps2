/* SPDX-License-Identifier: LGPL-3.0+ */
/* GSHWDrawConfig made C (GS/Renderers/Common/GSHWDrawConfig.h) held to
 * what it was (old.h, GSDevice.h's as it was): every member at the same
 * offset, every bit-field in the same bits, the same sizes; and C's
 * layout of it (cside.c) the same as C++'s. */

#include <stdio.h>
#include <string.h>

#include <new>

#include "common/Pcsx2Defs.h"
#include "GS/Renderers/Common/GSKeyHash.h"
#include "GS/Renderers/Common/GSHWDrawConfig.h"
#include "fields.h"

#include "old.h"

extern "C" {
const unsigned* c_offsets(void);
unsigned c_sizes(int i);
void c_bits(int i, unsigned char* out);
}

/* (the old names of the new types) */
#define OLD_gs_hw_draw_config old::GSHWDrawConfig
#define OLD_gs_hw_vs_cb old::GSHWDrawConfig::VSConstantBuffer
#define OLD_gs_hw_ps_cb old::GSHWDrawConfig::PSConstantBuffer
#define OLD_gs_hw_alpha_pass old::GSHWDrawConfig::AlphaPass
#define OLD_gs_hw_blend_multi_pass old::GSHWDrawConfig::BlendMultiPass

static int s_failed;
static long s_checks;

static void check(bool ok, const char* what)
{
	s_checks++;
	if (!ok && !s_failed)
	{
		printf("FAIL: %s\n", what);
		s_failed = 1;
	}
}

int main()
{
	int i = 0;
	const unsigned* c = c_offsets();

#define CHECK_OFFSET(t, m) \
	check(offsetof(t, m) == offsetof(OLD_##t, m), #t "." #m " moved from where it was"); \
	check(offsetof(t, m) == c[i++], #t "." #m " is elsewhere in C");
	GSHWCONFIG_OFFSETS(CHECK_OFFSET)

	check(sizeof(gs_hw_draw_config) == sizeof(old::GSHWDrawConfig), "the config's size");
	check(alignof(gs_hw_draw_config) == alignof(old::GSHWDrawConfig), "the config's alignment");
	check(sizeof(gs_hw_vs_cb) == sizeof(OLD_gs_hw_vs_cb), "the VS constants' size");
	check(sizeof(gs_hw_ps_cb) == sizeof(OLD_gs_hw_ps_cb), "the PS constants' size");
	check(sizeof(gs_hw_alpha_pass) == sizeof(OLD_gs_hw_alpha_pass), "the alpha pass's size");
	check(sizeof(gs_hw_blend_multi_pass) == sizeof(OLD_gs_hw_blend_multi_pass), "the blend pass's size");
	check(c_sizes(0) == sizeof(gs_hw_draw_config) && c_sizes(1) == sizeof(gs_hw_vs_cb)
		&& c_sizes(2) == sizeof(gs_hw_ps_cb) && c_sizes(3) == sizeof(gs_hw_alpha_pass)
		&& c_sizes(4) == sizeof(gs_hw_blend_multi_pass), "C's sizes");

	/* each bit-field alone, in each */
	{
		int k = 0;
#define CHECK_BITS(path, bits) \
		{ \
			static gs_hw_draw_config n; \
			static old::GSHWDrawConfig o; \
			static unsigned char cb[sizeof(gs_hw_draw_config)]; \
			memset((void*)&n, 0, sizeof(n)); \
			memset((void*)&o, 0, sizeof(o)); \
			n.path = (1u << (bits)) - 1u; \
			o.path = (1u << (bits)) - 1u; \
			c_bits(k++, cb); \
			check(memcmp(&n, &o, sizeof(n)) == 0, #path " has other bits than it had"); \
			check(memcmp(&n, cb, sizeof(n)) == 0, #path " has other bits in C"); \
		}
		GSHWCONFIG_BITS(CHECK_BITS)
	}

	/* datm, an enum each side */
	{
		static gs_hw_draw_config n;
		static old::GSHWDrawConfig o;
		memset((void*)&n, 0, sizeof(n));
		memset((void*)&o, 0, sizeof(o));
		n.datm = SetDATM::DATM1_RTA_CORRECTION;
		o.datm = old::SetDATM::DATM1_RTA_CORRECTION;
		check(memcmp(&n, &o, sizeof(n)) == 0, "datm has other bits than it had");
	}

	/* the selectors' constructors as they were */
	check(gs_hw_color_mask().key == old::GSHWDrawConfig::ColorMaskSelector().key, "a colour mask's default");
	check(gs_hw_color_mask(5).key == old::GSHWDrawConfig::ColorMaskSelector(5).key, "a colour mask of 5");
	check(gs_hw_sampler_selector::Linear().key == old::GSHWDrawConfig::SamplerSelector::Linear().key, "the linear sampler");
	check(gs_hw_depth_selector::NoDepth().key == old::GSHWDrawConfig::DepthStencilSelector::NoDepth().key, "no depth");
	check(gs_hw_blend_state(true, 3, 4, 2, 5, 6, true, 77).key
		== old::GSHWDrawConfig::BlendState(true, 3, 4, 2, 5, 6, true, 77).key, "a blend state");
	for (unsigned k = 0; k < 256; k++)
	{
		const gs_hw_sampler_selector n((u8)k);
		const old::GSHWDrawConfig::SamplerSelector o((u8)k);
		check(n.IsMinFilterLinear() == o.IsMinFilterLinear() && n.IsMagFilterLinear() == o.IsMagFilterLinear()
			&& n.IsMipFilterLinear() == o.IsMipFilterLinear() && n.UseMipmapFiltering() == o.UseMipmapFiltering(),
			"a sampler's filters");
	}

	printf("gshwconfig: %ld checks, %d differed\n", s_checks, s_failed);
	return s_failed;
}
