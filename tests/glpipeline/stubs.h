#ifndef GLPIPELINE_STUBS_H
#define GLPIPELINE_STUBS_H

#include "common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every GL call and log line since stubs_reset, as words: the call's id,
 * then its arguments (floats by their bits) */
#define STUB_LOG_MAX (1 << 20)
extern u32 g_log[STUB_LOG_MAX];
extern u32 g_log_len;
void stubs_reset(void);

/* What a pipeline implementation offers the scenario */
typedef struct pipeline_impl
{
	void (*ds_apply_key)(u8 key);        /* a draw's depth key */
	void (*ds_apply_convert)(int write); /* the convert ones */
	void (*ds_apply_date)(void);         /* the DATE one */
	u32 (*sampler)(u8 key, int max_anisotropy);
	void (*blend)(int enable, u32 sf, u32 df, u32 op, u32 sfa, u32 dfa, int is_constant, u8 constant);
	void (*blend_off)(void); /* OMSetBlendState() */
	void (*hw_blend)(u32 key, int dual_source);
	void (*color_mask)(u8 key);
} pipeline_impl;

extern const pipeline_impl g_impl_c;
extern const pipeline_impl g_impl_ref;

#ifdef __cplusplus
}
#endif

#endif
