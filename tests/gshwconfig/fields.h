/* SPDX-License-Identifier: LGPL-3.0+ */
/* The config's members by offset, and its bit-fields by path and width */
#ifndef GSHWCONFIG_FIELDS_H
#define GSHWCONFIG_FIELDS_H

#define GSHWCONFIG_OFFSETS(X) \
	X(gs_hw_draw_config, rt) X(gs_hw_draw_config, ds) X(gs_hw_draw_config, tex) X(gs_hw_draw_config, pal) \
	X(gs_hw_draw_config, verts) X(gs_hw_draw_config, indices) X(gs_hw_draw_config, nverts) \
	X(gs_hw_draw_config, nindices) X(gs_hw_draw_config, indices_per_prim) X(gs_hw_draw_config, drawlist) \
	X(gs_hw_draw_config, drawlist_count) X(gs_hw_draw_config, scissor) X(gs_hw_draw_config, drawarea) \
	X(gs_hw_draw_config, topology) X(gs_hw_draw_config, ps) X(gs_hw_draw_config, vs) X(gs_hw_draw_config, blend) \
	X(gs_hw_draw_config, sampler) X(gs_hw_draw_config, colormask) X(gs_hw_draw_config, depth) \
	X(gs_hw_draw_config, require_one_barrier) X(gs_hw_draw_config, require_full_barrier) \
	X(gs_hw_draw_config, destination_alpha) X(gs_hw_draw_config, alpha_second_pass) \
	X(gs_hw_draw_config, blend_multi_pass) X(gs_hw_draw_config, cb_vs) X(gs_hw_draw_config, cb_ps) \
	X(gs_hw_draw_config, colclip_mode) X(gs_hw_draw_config, colclip_frame) \
	X(gs_hw_draw_config, colclip_update_area) \
	X(gs_hw_vs_cb, vertex_scale) X(gs_hw_vs_cb, vertex_offset) X(gs_hw_vs_cb, texture_scale) \
	X(gs_hw_vs_cb, texture_offset) X(gs_hw_vs_cb, point_size) X(gs_hw_vs_cb, max_depth) \
	X(gs_hw_ps_cb, FogColor_AREF) X(gs_hw_ps_cb, WH) X(gs_hw_ps_cb, TA_MaxDepth_Af) X(gs_hw_ps_cb, FbMask) \
	X(gs_hw_ps_cb, HalfTexel) X(gs_hw_ps_cb, MinMax) X(gs_hw_ps_cb, LODParams) X(gs_hw_ps_cb, STRange) \
	X(gs_hw_ps_cb, ChannelShuffle) X(gs_hw_ps_cb, TCOffsetHack) X(gs_hw_ps_cb, STScale) \
	X(gs_hw_ps_cb, ChannelOffset) X(gs_hw_ps_cb, DitherMatrix) X(gs_hw_ps_cb, ScaleFactor) \
	X(gs_hw_alpha_pass, ps) X(gs_hw_alpha_pass, enable) X(gs_hw_alpha_pass, colormask) \
	X(gs_hw_alpha_pass, depth) X(gs_hw_alpha_pass, ps_aref) \
	X(gs_hw_blend_multi_pass, blend) X(gs_hw_blend_multi_pass, blend_hw) X(gs_hw_blend_multi_pass, dither) \
	X(gs_hw_blend_multi_pass, enable)

/* (datm is an enum in C++: set through its own path there) */
#define GSHWCONFIG_BITS(X) \
	X(sampler.tau, 1) X(sampler.tav, 1) X(sampler.biln, 1) X(sampler.triln, 3) X(sampler.aniso, 1) \
	X(sampler.lodclamp, 1) X(depth.ztst, 2) X(depth.zwe, 1) X(depth.date, 1) X(depth.date_one, 1) \
	X(colormask.wr, 1) X(colormask.wg, 1) X(colormask.wb, 1) X(colormask.wa, 1) X(colormask.wrgba, 4) \
	X(blend.enable, 1) X(blend.constant_enable, 1) X(blend.op, 6) X(blend.src_factor, 4) \
	X(blend.dst_factor, 4) X(blend.src_factor_alpha, 4) X(blend.dst_factor_alpha, 4) X(blend.constant, 8) \
	X(line_expand, 1) X(alpha_second_pass.depth.zwe, 1) X(alpha_second_pass.colormask.wa, 1) \
	X(blend_multi_pass.blend.op, 6) X(blend_multi_pass.blend.dst_factor_alpha, 4)

#endif
