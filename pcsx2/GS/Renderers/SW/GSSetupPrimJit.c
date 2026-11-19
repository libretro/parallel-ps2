/*
 * The setup of a primitive's steps for the software renderer's scanline
 * code: from the vertex's step (dscan) and the shift tables, the per-pixel
 * and per-step depth, fog, texture and colour steps the scanline code
 * reads from the local data. What GSSetupPrimCodeGenerator generated
 * through xbyak, instruction for instruction (tests/swjit holds the bytes).
 *
 * Called as void (*)(const GSVertexSW* vertex, const u16* index,
 * const GSVertexSW* dscan, GSScanlineLocalData* local).
 */

#include "GSJitX86.h"
#include "GSSwJit.h"

/* (_MM_SHUFFLE) */
#define SHUF(a, b, c, d) (((a) << 6) | ((b) << 4) | ((c) << 2) | (d))

typedef struct gs_swj_sp
{
	gsj j;
	u64 key;
	int ymm;   /* the eight-pixel layout, in ymm registers */
	int many_regs;
	int vecsize;
	int dsize;
	struct { unsigned z, f, t, c; } en;
	/* registers: the arguments and a temporary */
	int vertex, index, dscan, local, t1;
	/* the shift tables, and where m_shift_128b is from m_shift_256b */
	const void* shift_128b;
	const void* shift_256b;
	s32 shift128_from_256;
} gs_swj_sp;

/* xym(n): the vector register of the layout's width */
static gsj_op xym(const gs_swj_sp* g, int n)
{
	return g->ymm ? gsj_y(n) : gsj_x(n);
}

/* [local + offset] */
static gsj_op loc(const gs_swj_sp* g, s32 off)
{
	return gsj_ptr(g->local, off);
}

/* d[i].field of the local data */
static gsj_op loc_di(const gs_swj_sp* g, int i, s32 field)
{
	return gsj_ptr(g->local, field + (g->ymm ? GS_SL8_SKIP : GS_SL4_SKIP) * i);
}

/* m_shift_256b[i][lane] and m_shift_128b[i], from rax at m_shift_256b */
static gsj_op shift256(int i, int lane)
{
	return gsj_ptr(GSJ_RAX, i * 32 + lane * 4);
}

static gsj_op shift128(const gs_swj_sp* g, int i)
{
	return gsj_ptr(GSJ_RAX, g->shift128_from_256 + i * 16);
}

static gsj_op dscan(const gs_swj_sp* g, s32 off)
{
	return gsj_ptr(g->dscan, off);
}

/* On AVX the three-operand form; otherwise dst = src1 first */
#define THREE(op, dst, src1, src2) \
	do \
	{ \
		if (g->j.avx) \
			gsj_v##op(&g->j, dst, src1, src2); \
		else \
		{ \
			gsj_movdqa(&g->j, dst, src1); \
			gsj_##op(&g->j, dst, src2); \
		} \
	} while (0)
#define THREE_I(op, dst, src1, src2, imm) \
	do \
	{ \
		if (g->j.avx) \
			gsj_v##op(&g->j, dst, src1, src2, imm); \
		else \
		{ \
			gsj_movdqa(&g->j, dst, src1); \
			gsj_##op(&g->j, dst, src2, imm); \
		} \
	} while (0)

/* 128 bits of floats from memory across the register */
static void broadcastf128(gs_swj_sp* g, gsj_op reg, gsj_op mem)
{
	if (g->ymm)
		gsj_vbroadcastf128(&g->j, reg, mem);
	else
		gsj_movaps(&g->j, reg, mem);
}

/* A float from memory across the register */
static void broadcastss(gs_swj_sp* g, gsj_op reg, gsj_op mem)
{
	if (g->j.avx)
		gsj_vbroadcastss(&g->j, reg, mem);
	else
	{
		gsj_movss(&g->j, reg, mem);
		gsj_shufps(&g->j, reg, reg, SHUF(0, 0, 0, 0));
	}
}

/* rax = &vertex[index[n]] */
static void vertex_n(gs_swj_sp* g, int n)
{
	gsj_movzx(&g->j, gsj_r32(GSJ_RAX), gsj_mem(16, g->index, -1, 1, 2 * n));
	gsj_shl(&g->j, gsj_r32(GSJ_RAX), 6);
	gsj_add(&g->j, gsj_r64(GSJ_RAX), gsj_r64(g->vertex));
}

static void depth_xmm(gs_swj_sp* g)
{
	gsj* j = &g->j;
	const int steps = GS_SEL_NOTEST(g->key) ? 1 : 4;
	int i;
	if (!g->en.z && !g->en.f)
		return;
	if (GS_SEL_PRIM(g->key) != GS_SWJ_SPRITE_CLASS)
	{
		if (g->en.f)
		{
			/* GSVector4 df = t.wwww(); local.d4.f = GSVector4i(df * 4.0f).xxzzlh(); */
			broadcastss(g, xym(g, 1), dscan(g, GS_VSW_T + 12));
			THREE(mulps, gsj_x(2), gsj_x(1), gsj_x(3));
			gsj_cvttps2dq(j, gsj_x(2), gsj_x(2));
			gsj_pshuflw(j, gsj_x(2), gsj_x(2), SHUF(2, 2, 0, 0));
			gsj_pshufhw(j, gsj_x(2), gsj_x(2), SHUF(2, 2, 0, 0));
			gsj_movdqa(j, loc(g, GS_SL4_D4_F), gsj_x(2));
			for (i = 0; i < steps; i++)
			{
				/* local.d[i].f = GSVector4i(df * m_shift[i]).xxzzlh(); */
				THREE(mulps, gsj_x(2), gsj_x(1), xym(g, 4 + i));
				gsj_cvttps2dq(j, gsj_x(2), gsj_x(2));
				gsj_pshuflw(j, gsj_x(2), gsj_x(2), SHUF(2, 2, 0, 0));
				gsj_pshufhw(j, gsj_x(2), gsj_x(2), SHUF(2, 2, 0, 0));
				gsj_movdqa(j, loc_di(g, i, GS_SL4_D_F), gsj_x(2));
			}
		}
		if (g->en.z)
		{
			/* dz = broadcast64(&dscan.p.z); local.d4.z = dz.mul64(f32to64(shift)); */
			gsj_movddup(j, gsj_x(0), dscan(g, GS_VSW_P + 8));
			gsj_cvtps2pd(j, gsj_x(1), gsj_x(3));
			gsj_mulpd(j, gsj_x(1), gsj_x(0));
			gsj_movaps(j, loc(g, GS_SL4_D4_Z), gsj_x(1));
			for (i = 0; i < steps; i++)
			{
				gsj_cvtps2pd(j, gsj_x(1), gsj_ptr(GSJ_RAX, (i + 1) * g->vecsize));
				gsj_mulpd(j, gsj_x(1), gsj_x(0));
				gsj_movaps(j, loc_di(g, i, GS_SL4_D_Z0), gsj_x(1));
				gsj_cvtps2pd(j, gsj_x(1), gsj_ptr(GSJ_RAX, (i + 1) * g->vecsize + 8));
				gsj_mulpd(j, gsj_x(1), gsj_x(0));
				gsj_movaps(j, loc_di(g, i, GS_SL4_D_Z1), gsj_x(1));
			}
		}
	}
	else
	{
		/* GSVector4 p = vertex[index[1]].p; */
		vertex_n(g, 1);
		if (g->en.f)
		{
			/* local.p.f = GSVector4i(p).zzzzh().zzzz(); */
			gsj_movaps(j, gsj_x(0), gsj_ptr(GSJ_RAX, GS_VSW_P));
			gsj_cvttps2dq(j, gsj_x(1), gsj_x(0));
			gsj_pshufhw(j, gsj_x(1), gsj_x(1), SHUF(2, 2, 2, 2));
			gsj_pshufd(j, gsj_x(1), gsj_x(1), SHUF(2, 2, 2, 2));
			gsj_movdqa(j, loc(g, GS_SL4_P_F), gsj_x(1));
		}
		if (g->en.z)
		{
			/* (u32 z is bypassed in t.w) */
			gsj_movdqa(j, gsj_x(0), gsj_ptr(GSJ_RAX, GS_VSW_T));
			gsj_pshufd(j, gsj_x(0), gsj_x(0), SHUF(3, 3, 3, 3));
			gsj_movdqa(j, loc(g, GS_SL4_P_Z), gsj_x(0));
		}
	}
}

/* Colour and fog step by whole 4-pixel quads: t4 holds the quad step in
 * each of its lanes. Stores it in the upper four lanes of hi, and its
 * negation in the lower four of lo. Destroys t4's ymm and tmp. */
static void quad_steps(gs_swj_sp* g, int t4, int tmp, gsj_op hi, gsj_op lo)
{
	gsj* j = &g->j;
	gsj_pxor(j, gsj_x(tmp), gsj_x(tmp));
	gsj_vinserti128(j, gsj_y(tmp), gsj_y(tmp), gsj_x(t4), 1);
	gsj_movdqa(j, hi, gsj_y(tmp));
	gsj_vinserti128(j, gsj_y(t4), gsj_y(t4), gsj_x(t4), 1);
	gsj_vpsubd(j, gsj_y(t4), gsj_y(tmp), gsj_y(t4));
	gsj_movdqa(j, lo, gsj_y(t4));
}

/* dst = the offsets of skip's lanes from its first pixel: within the
 * quad by d times the lane's place in it, and a quad step for the lanes
 * of the other quad (see quad_steps). */
static void quad_offset(gs_swj_sp* g, int dst, int d, int skip, gsj_op hi, gsj_op lo)
{
	gsj* j = &g->j;
	gsj_vbroadcastf128(j, gsj_y(dst), shift128(g, 1 + (skip & 3)));
	gsj_vmulps(j, gsj_y(dst), gsj_y(dst), gsj_y(d));
	gsj_cvttps2dq(j, gsj_y(dst), gsj_y(dst));
	gsj_vpaddd(j, gsj_y(dst), gsj_y(dst), skip < 4 ? hi : lo);
}

static void depth_ymm(gs_swj_sp* g)
{
	gsj* j = &g->j;
	const int steps = GS_SEL_NOTEST(g->key) ? 1 : g->dsize;
	int i;
	if (!g->en.z && !g->en.f)
		return;
	if (GS_SEL_PRIM(g->key) != GS_SWJ_SPRITE_CLASS)
	{
		if (g->en.f)
		{
			/* df = broadcast32(&dscan.t.w); f4 = GSVector4i(df * 4.0f); local.d8.p.f = f4 * 2; */
			gsj_vbroadcastss(j, gsj_y(1), dscan(g, GS_VSW_T + 12));
			gsj_vmulps(j, gsj_x(0), gsj_x(1), shift128(g, 0));
			gsj_cvttps2dq(j, gsj_x(0), gsj_x(0));
			gsj_vpaddd(j, gsj_x(2), gsj_x(0), gsj_x(0));
			gsj_movd(j, loc(g, GS_SL8_D8_P_F), gsj_x(2));
			quad_steps(g, 0, 2, loc(g, GS_SL8_TEMP_UV0), loc(g, GS_SL8_TEMP_UV1));
			for (i = 0; i < steps; i++)
			{
				/* local.d[i].f = QuadOffsets(df, i, f4).xxzzlh(); */
				quad_offset(g, 0, 1, i, loc(g, GS_SL8_TEMP_UV0), loc(g, GS_SL8_TEMP_UV1));
				gsj_pshuflw(j, gsj_y(0), gsj_y(0), SHUF(2, 2, 0, 0));
				gsj_pshufhw(j, gsj_y(0), gsj_y(0), SHUF(2, 2, 0, 0));
				gsj_movdqa(j, loc_di(g, i, GS_SL8_D_F), gsj_y(0));
			}
		}
		if (g->en.z)
		{
			/* dz = broadcast64(&dscan.p.z); storel(&local.d8.p.z, dz.mul64(f32to64(shift))); */
			gsj_movsd(j, gsj_x(0), gsj_sized(dscan(g, GS_VSW_P + 8), 64));
			gsj_vcvtss2sd(j, gsj_x(1), gsj_x(3), gsj_x(3));
			gsj_vmulsd(j, gsj_x(1), gsj_x(0), gsj_x(1));
			gsj_movsd(j, gsj_sized(loc(g, GS_SL8_D8_P_Z), 64), gsj_x(1));
			gsj_vbroadcastsd(j, gsj_y(0), dscan(g, GS_VSW_P + 8));
			for (i = 0; i < steps; i++)
			{
				gsj_cvtps2pd(j, gsj_y(1), shift256(i + 1, 0));
				gsj_vmulpd(j, gsj_y(1), gsj_y(1), gsj_y(0));
				gsj_movaps(j, loc_di(g, i, GS_SL8_D_Z0), gsj_y(1));
				gsj_cvtps2pd(j, gsj_y(1), shift256(i + 1, 4));
				gsj_vmulpd(j, gsj_y(1), gsj_y(1), gsj_y(0));
				gsj_movaps(j, loc_di(g, i, GS_SL8_D_Z1), gsj_y(1));
			}
		}
	}
	else
	{
		vertex_n(g, 1);
		if (g->en.f)
		{
			/* local.p.f = GSVector4i(vertex[index[1]].p).extract32<3>(); */
			gsj_movaps(j, gsj_x(0), gsj_ptr(GSJ_RAX, GS_VSW_P));
			gsj_cvttps2dq(j, gsj_x(0), gsj_x(0));
			gsj_pextrd(j, loc(g, GS_SL8_P_F), gsj_x(0), 3);
		}
		if (g->en.z)
		{
			/* local.p.z = vertex[index[1]].t.u32[3]; (u32 z is bypassed in t.w) */
			gsj_mov(j, gsj_r32(g->t1), gsj_ptr(GSJ_RAX, GS_VSW_T + 12));
			gsj_mov(j, loc(g, GS_SL8_P_Z), gsj_r32(g->t1));
		}
	}
}

static void texture(gs_swj_sp* g)
{
	gsj* j = &g->j;
	const int fst   = GS_SEL_FST(g->key) != 0;
	const int steps = GS_SEL_NOTEST(g->key) ? 1 : g->dsize;
	const s32 stq   = g->ymm ? GS_SL8_D8_STQ : GS_SL4_D4_STQ;
	const s32 d_s   = g->ymm ? GS_SL8_D_S : GS_SL4_D_S;
	const s32 d_t   = g->ymm ? GS_SL8_D_T : GS_SL4_D_T;
	const s32 d_q   = g->ymm ? GS_SL8_D_Q : GS_SL4_D_Q;
	int i, k;
	if (!g->en.t)
		return;

	/* GSVector4 t = dscan.t; */
	broadcastf128(g, xym(g, 0), dscan(g, GS_VSW_T));
	THREE(mulps, gsj_x(1), gsj_x(0), gsj_x(3));
	if (fst)
	{
		/* local.d4.stq = GSVector4i(t * 4.0f); */
		gsj_cvttps2dq(j, gsj_x(1), gsj_x(1));
		gsj_movdqa(j, loc(g, stq), gsj_x(1));
	}
	else
	{
		/* local.d4.stq = t * 4.0f; */
		gsj_movaps(j, loc(g, stq), gsj_x(1));
	}

	for (k = 0; k < (fst ? 2 : 3); k++)
	{
		/* ds, dt, dq = t.xxxx(), t.yyyy(), t.zzzz() */
		THREE_I(shufps, xym(g, 1), xym(g, 0), xym(g, 0), SHUF(k, k, k, k));
		for (i = 0; i < steps; i++)
		{
			/* GSVector4 v = ds/dt * m_shift[i]; */
			if (i < 4 || g->many_regs)
				THREE(mulps, xym(g, 2), xym(g, 4 + i), xym(g, 1));
			else
				gsj_vmulps(j, gsj_y(2), gsj_y(1), shift256(i + 1, 0));
			if (fst)
			{
				/* local.d[i].s/t = GSVector4i(v); */
				gsj_cvttps2dq(j, xym(g, 2), xym(g, 2));
				if (k == 0)
					gsj_movdqa(j, loc_di(g, i, d_s), xym(g, 2));
				else if (k == 1)
					gsj_movdqa(j, loc_di(g, i, d_t), xym(g, 2));
			}
			else
			{
				/* local.d[i].s/t/q = v; */
				gsj_movaps(j, loc_di(g, i, k == 0 ? d_s : k == 1 ? d_t : d_q), xym(g, 2));
			}
		}
	}
}

static void color_ymm(gs_swj_sp* g)
{
	gsj* j = &g->j;
	const int steps = GS_SEL_NOTEST(g->key) ? 1 : g->dsize;
	/* Scratch for the quad steps: temporaries the scanline code makes
	 * anew each scanline. Not temp.uv_minmax: with a constant LOD that
	 * holds the mip level's clamp from BeginDraw for the whole draw, and
	 * a texel fetch clamped by a colour step reads past its texture. */
	const s32 hi[4] = {GS_SL8_TEMP_UV0, GS_SL8_TEMP_TRB, GS_SL8_TEMP_LOD_I, GS_SL8_TEMP_ZS};
	const s32 lo[4] = {GS_SL8_TEMP_UV1, GS_SL8_TEMP_TGA, GS_SL8_TEMP_LOD_F, GS_SL8_TEMP_ZD};
	const int mask16 = g->many_regs ? 12 : GS_SEL_NOTEST(g->key) ? 6 : 8;
	int i, k;

	/* GSVector4 c = dscan.c; */
	broadcastf128(g, gsj_y(0), dscan(g, GS_VSW_C));

	/* mask16 = 0xFFFF in each lane */
	gsj_pcmpeqd(j, gsj_y(mask16), gsj_y(mask16));
	gsj_psrld_i(j, gsj_y(mask16), 16);

	/* c4 = GSVector4i(dscan.c * 4.0f); local.d8.c = ((c4 + c4) & mask16).xzyw().pu32(); */
	gsj_vmulps(j, gsj_x(1), gsj_x(0), shift128(g, 0));
	gsj_cvttps2dq(j, gsj_x(1), gsj_x(1));
	gsj_vpaddd(j, gsj_x(2), gsj_x(1), gsj_x(1));
	gsj_pshufd(j, gsj_x(2), gsj_x(2), SHUF(3, 1, 2, 0));
	gsj_pand(j, gsj_x(2), gsj_x(mask16));
	gsj_packusdw(j, gsj_x(2), gsj_x(2));
	gsj_movq(j, gsj_sized(loc(g, GS_SL8_D8_C), 64), gsj_x(2));

	for (k = 0; k < 4; k++)
	{
		gsj_pshufd(j, gsj_x(2), gsj_x(1), SHUF(k, k, k, k));
		quad_steps(g, 2, 3, loc(g, hi[k]), loc(g, lo[k]));
	}

	for (k = 0; k < 2; k++)
	{
		/* r and b, then g and a */
		gsj_vshufps(j, gsj_y(2), gsj_y(0), gsj_y(0), SHUF(k, k, k, k));
		gsj_vshufps(j, gsj_y(3), gsj_y(0), gsj_y(0), SHUF(k + 2, k + 2, k + 2, k + 2));
		for (i = 0; i < steps; i++)
		{
			/* x = (QuadOffsets(dx, i, c4.x) & mask16).pu32(); */
			quad_offset(g, 4, 2, i, loc(g, hi[k]), loc(g, lo[k]));
			quad_offset(g, 1, 3, i, loc(g, hi[k + 2]), loc(g, lo[k + 2]));
			gsj_pand(j, gsj_y(4), gsj_y(mask16));
			gsj_packusdw(j, gsj_y(4), gsj_y(4));
			gsj_pand(j, gsj_y(1), gsj_y(mask16));
			gsj_packusdw(j, gsj_y(1), gsj_y(1));
			/* local.d[i].rb = r.upl16(b); local.d[i].ga = g.upl16(a); */
			gsj_punpcklwd(j, gsj_y(4), gsj_y(1));
			gsj_movdqa(j, loc_di(g, i, k == 0 ? GS_SL8_D_RB : GS_SL8_D_GA), gsj_y(4));
		}
	}
}

static void color(gs_swj_sp* g)
{
	gsj* j = &g->j;
	const u64 key = g->key;
	int i;
	if (!g->en.c)
		return;

	if (GS_SEL_IIP(key) && g->ymm)
		color_ymm(g);
	else if (GS_SEL_IIP(key))
	{
		const int steps = GS_SEL_NOTEST(key) ? 1 : 4;
		const gsj_op mask16 = xym(g, GS_SEL_NOTEST(key) ? 6 : 8);

		/* GSVector4 c = dscan.c; */
		broadcastf128(g, xym(g, 0), dscan(g, GS_VSW_C));
		gsj_pcmpeqd(j, mask16, mask16);
		gsj_psrld_i(j, mask16, 16);

		/* local.d4.c = (GSVector4i(dscan.c * step_shift) & mask16).xzyw().pu32(); */
		THREE(mulps, gsj_x(1), gsj_x(0), gsj_x(3));
		gsj_cvttps2dq(j, gsj_x(1), gsj_x(1));
		gsj_pshufd(j, gsj_x(1), gsj_x(1), SHUF(3, 1, 2, 0));
		gsj_pand(j, xym(g, 1), mask16);
		gsj_packusdw(j, gsj_x(1), gsj_x(1));
		gsj_movdqa(j, loc(g, GS_SL4_D4_C), gsj_x(1));

		/* (xym3 is not needed any more) dr = c.xxxx(), db = c.zzzz() */
		THREE_I(shufps, xym(g, 2), xym(g, 0), xym(g, 0), SHUF(0, 0, 0, 0));
		THREE_I(shufps, xym(g, 3), xym(g, 0), xym(g, 0), SHUF(2, 2, 2, 2));
		for (i = 0; i < steps; i++)
		{
			/* r = GSVector4i(dr * m_shift[i]).ps32(); b likewise */
			THREE(mulps, xym(g, 0), xym(g, 4 + i), xym(g, 2));
			gsj_cvttps2dq(j, xym(g, 0), xym(g, 0));
			gsj_pand(j, xym(g, 0), mask16);
			gsj_packusdw(j, xym(g, 0), xym(g, 0));
			THREE(mulps, xym(g, 1), xym(g, 4 + i), xym(g, 3));
			gsj_cvttps2dq(j, xym(g, 1), xym(g, 1));
			gsj_pand(j, xym(g, 1), mask16);
			gsj_packusdw(j, xym(g, 1), xym(g, 1));
			/* local.d[i].rb = r.upl16(b); */
			gsj_punpcklwd(j, xym(g, 0), xym(g, 1));
			gsj_movdqa(j, loc_di(g, i, GS_SL4_D_RB), xym(g, 0));
		}

		/* (not enough registers: c again) dg = c.yyyy(), da = c.wwww() */
		broadcastf128(g, xym(g, 0), dscan(g, GS_VSW_C));
		THREE_I(shufps, xym(g, 2), xym(g, 0), xym(g, 0), SHUF(1, 1, 1, 1));
		THREE_I(shufps, xym(g, 3), xym(g, 0), xym(g, 0), SHUF(3, 3, 3, 3));
		for (i = 0; i < steps; i++)
		{
			/* g = GSVector4i(dg * m_shift[i]).ps32(); a likewise */
			THREE(mulps, xym(g, 0), xym(g, 4 + i), xym(g, 2));
			gsj_cvttps2dq(j, xym(g, 0), xym(g, 0));
			gsj_pand(j, xym(g, 0), mask16);
			gsj_packusdw(j, xym(g, 0), xym(g, 1));
			THREE(mulps, xym(g, 1), xym(g, 4 + i), xym(g, 3));
			gsj_cvttps2dq(j, xym(g, 1), xym(g, 1));
			gsj_pand(j, xym(g, 1), mask16);
			gsj_packusdw(j, xym(g, 1), xym(g, 1));
			/* local.d[i].ga = g.upl16(a); */
			gsj_punpcklwd(j, xym(g, 0), xym(g, 1));
			gsj_movdqa(j, loc_di(g, i, GS_SL4_D_GA), xym(g, 0));
		}
	}
	else
	{
		/* GSVector4i c = GSVector4i(vertex[index[last]].c); */
		const unsigned prim = GS_SEL_PRIM(key);
		const int last      = prim == GS_SWJ_TRIANGLE_CLASS ? 2 : prim == GS_SWJ_POINT_CLASS ? 0 : 1;
		/* (a sprite's last vertex was loaded in the depth already) */
		if (!(prim == GS_SWJ_SPRITE_CLASS && (g->en.z || g->en.f)))
			vertex_n(g, last);
		if (!g->ymm)
			gsj_cvttps2dq(j, gsj_x(0), gsj_ptr(GSJ_RAX, GS_VSW_C));
		else
		{
			gsj_vbroadcasti128(j, gsj_y(0), gsj_ptr(GSJ_RAX, GS_VSW_C));
			gsj_cvttps2dq(j, gsj_y(0), gsj_y(0));
		}
		/* c = c.upl16(c.zwxy()); */
		gsj_pshufd(j, xym(g, 1), xym(g, 0), SHUF(1, 0, 3, 2));
		gsj_punpcklwd(j, xym(g, 0), xym(g, 1));
		/* if (!tme) c = c.srl16<7>(); */
		if (GS_SEL_TFX(key) == GS_SWJ_TFX_NONE)
			gsj_psrlw_i(j, xym(g, 0), 7);
		/* local.c.rb = c.xxxx(); local.c.ga = c.zzzz(); */
		gsj_pshufd(j, xym(g, 1), xym(g, 0), SHUF(0, 0, 0, 0));
		gsj_pshufd(j, xym(g, 2), xym(g, 0), SHUF(2, 2, 2, 2));
		gsj_movdqa(j, loc(g, g->ymm ? GS_SL8_C_RB : GS_SL4_C_RB), xym(g, 1));
		gsj_movdqa(j, loc(g, g->ymm ? GS_SL8_C_GA : GS_SL4_C_GA), xym(g, 2));
	}
}

size_t gs_swj_setup_prim(const gs_swj_target* t, void* code, size_t room)
{
	gs_swj_sp sp;
	gs_swj_sp* g = &sp;
	gsj* j       = &sp.j;
	const u64 key = t->key;
	const unsigned prim = GS_SEL_PRIM(key);
	int needs_shift, saved = 0, i;

	gsj_init(j, code, room, t->avx, t->ymm);
	g->key     = key;
	g->ymm     = t->ymm;
	g->vecsize = t->ymm ? 32 : 16;
	g->dsize   = t->ymm ? 8 : 4;
	g->shift_128b = t->shift_128b;
	g->shift_256b = t->shift_256b;
	g->shift128_from_256 = (s32)((const u8*)t->shift_128b - (const u8*)t->shift_256b);
	if (t->win64)
	{
		g->vertex = GSJ_RCX;
		g->index  = GSJ_RDX;
		g->dscan  = GSJ_R8N;
		g->local  = GSJ_R9;
		g->t1     = GSJ_R10;
	}
	else
	{
		g->vertex = GSJ_RDI;
		g->index  = GSJ_RSI;
		g->dscan  = GSJ_RDX;
		g->local  = GSJ_RCX;
		g->t1     = GSJ_R8N;
	}
	g->en.z = GS_SEL_ZB(key) ? 1 : 0;
	g->en.f = GS_SEL_FB(key) && GS_SEL_FGE(key) ? 1 : 0;
	g->en.t = GS_SEL_FB(key) && GS_SEL_TFX(key) != GS_SWJ_TFX_NONE ? 1 : 0;
	g->en.c = GS_SEL_FB(key) && !(GS_SEL_TFX(key) == GS_SWJ_TFX_DECAL && GS_SEL_TCC(key)) ? 1 : 0;

	needs_shift  = ((g->en.z || g->en.f) && prim != GS_SWJ_SPRITE_CLASS) || g->en.t || (g->en.c && GS_SEL_IIP(key));
	g->many_regs = g->ymm && !GS_SEL_NOTEST(key) && needs_shift;

	/* (xmm6 and up are the callee's to keep on Windows) */
	if (t->win64)
	{
		saved = g->many_regs ? 7 : GS_SEL_NOTEST(key) ? 1 : 3;
		gsj_sub_i(j, gsj_r64(GSJ_RSP), (u32)(8 + 16 * saved));
		for (i = 0; i < saved; i++)
			gsj_movdqa(j, gsj_ptr(GSJ_RSP, i * 16), gsj_x(i + 6));
	}

	if (needs_shift)
	{
		gsj_mov_i(j, gsj_r64(GSJ_RAX), (u64)(size_t)(g->ymm ? t->shift_256b : t->shift_128b));
		for (i = 0; i < (GS_SEL_NOTEST(key) ? 2 : g->many_regs ? 9 : 5); i++)
			gsj_movaps(j, xym(g, 3 + i), gsj_ptr(GSJ_RAX, i * g->vecsize));
	}

	if (g->ymm)
		depth_ymm(g);
	else
		depth_xmm(g);
	texture(g);
	color(g);

	if (t->win64)
	{
		for (i = 0; i < saved; i++)
			gsj_movdqa(j, gsj_x(i + 6), gsj_ptr(GSJ_RSP, i * 16));
		gsj_add_i(j, gsj_r64(GSJ_RSP), (u32)(8 + 16 * saved));
	}
	if (g->ymm)
		gsj_vzeroupper(j);
	gsj_ret(j);
	return gsj_size(j);
}
