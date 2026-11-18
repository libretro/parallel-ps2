/* GLPipeline.c against the C++ it replaced (reference.cpp): a long random
 * run of depth and stencil states, samplers of every key, blends (raw and
 * a draw's, with and without dual source blending) and colour masks makes
 * the same GL calls and log lines, in the same order with the same
 * arguments, from the same cached state. */

#include <stdio.h>
#include <string.h>

#include "glad.h"
#include "GS/Renderers/OpenGL/GLState.h"
#include "stubs.h"

#define STEPS 150000

static u32 s_rng;

static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

/* A few of each so that the cache both hits and misses */
static u32 gl_factor(void)
{
	static const u32 f[] = {GL_ONE, GL_ZERO, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_CONSTANT_COLOR};
	return f[rnd() % 6];
}

static u32 gl_op(void)
{
	static const u32 o[] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT};
	return o[rnd() % 3];
}

static u32 hw_blend_key(void)
{
	/* enable, constant_enable, op (of three), four factors, the constant */
	u32 k  = rnd();
	u32 op = (rnd() % 3) << 2;
	return (k & ~0xFCu) | op;
}

static void scenario(const pipeline_impl* p)
{
	u32 i;
	int key;

	s_rng = 0x9E3779B9u;
	stubs_reset();
	gs_gl_state_clear();
	GLAD_GL_ARB_texture_filter_anisotropic = 0;
	GLAD_GL_EXT_texture_filter_anisotropic = 0;

	/* every sampler, under each anisotropy and extension */
	for (key = 0; key < 256; key++)
	{
		static const int aniso[] = {0, 1, 2, 16};
		int a, e;
		for (e = 0; e < 3; e++)
		{
			GLAD_GL_ARB_texture_filter_anisotropic = e == 1;
			GLAD_GL_EXT_texture_filter_anisotropic = e != 0;
			for (a = 0; a < 4; a++)
				p->sampler((u8)key, aniso[a]);
		}
	}

	for (i = 0; i < STEPS; i++)
	{
		switch (rnd() % 9)
		{
			case 0:
			case 1: p->ds_apply_key((u8)(rnd() & 31)); break;
			case 2:
				if (rnd() & 1)
					p->ds_apply_convert((int)(rnd() & 1));
				else
					p->ds_apply_date();
				break;
			case 3:
			{
				/* (drawn in order: argument order is the compiler's) */
				u32 a[8];
				int j;
				a[0] = rnd() % 4 != 0;
				for (j = 1; j < 6; j++)
					a[j] = j == 3 ? gl_op() : gl_factor();
				a[6] = rnd() & 1;
				a[7] = rnd() % 4 * 64;
				p->blend((int)a[0], a[1], a[2], a[3], a[4], a[5], (int)a[6], (u8)a[7]);
				break;
			}
			case 4: p->blend_off(); break;
			case 5:
			case 6:
			{
				u32 k = hw_blend_key();
				p->hw_blend(k, (int)(rnd() % 3 == 0));
				break;
			}
			case 7: p->color_mask((u8)(rnd() & 0xF)); break;
			default: p->color_mask(0xF); break;
		}
	}
}

static u32 s_ref[STUB_LOG_MAX];

int main(void)
{
	u32 ref_len, i;
	int failed = 0;

	scenario(&g_impl_ref);
	ref_len = g_log_len;
	memcpy(s_ref, g_log, sizeof(u32) * (ref_len < STUB_LOG_MAX ? ref_len : STUB_LOG_MAX));
	scenario(&g_impl_c);

	if (ref_len > STUB_LOG_MAX)
	{
		printf("FAIL: the log ran past its room (%u words)\n", (unsigned)ref_len);
		return 1;
	}
	if (g_log_len != ref_len)
	{
		printf("FAIL: %u words of GL calls, the C++ made %u\n", (unsigned)g_log_len, (unsigned)ref_len);
		failed = 1;
	}
	for (i = 0; i < ref_len && i < g_log_len; i++)
	{
		if (g_log[i] != s_ref[i])
		{
			printf("FAIL: word %u is %08x, the C++ had %08x\n", (unsigned)i, (unsigned)g_log[i], (unsigned)s_ref[i]);
			failed = 1;
			break;
		}
	}
	if (ref_len < 100000)
	{
		printf("FAIL: the scenario made only %u words\n", (unsigned)ref_len);
		failed = 1;
	}
	printf("glpipeline: %u words of GL calls %s\n", (unsigned)ref_len, failed ? "DIFFER" : "the same");
	return failed;
}
