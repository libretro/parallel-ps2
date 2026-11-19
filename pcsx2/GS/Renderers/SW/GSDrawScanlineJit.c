/*
 * A scanline's pixels for the software renderer: depth test, texture
 * sampling (with or without mipmaps), alpha test, colour, fog, the
 * destination alpha test, blending and the writes, specialised for one
 * selector. What GSDrawScanlineCodeGenerator generated through xbyak,
 * instruction for instruction (tests/swjit holds the bytes).
 *
 * Called as void (*)(int pixels, int left, int top, const GSVertexSW*
 * scan, GSScanlineLocalData* local).
 */

#include "GSJitX86.h"
#include "GSSwJit.h"

/* (_MM_SHUFFLE) */
#define SHUF(a, b, c, d) (((a) << 6) | ((b) << 4) | ((c) << 2) | (d))

/* The local and global data's offsets of a layout */
typedef struct swd_layout
{
	s32 skip, d_z0, d_z1, d_s, d_t, d_q, d_rb, d_ga, d_f;
	s32 step_z, step_stq, step_c, step_f;
	s32 p_z, p_f, c_rb, c_ga, tgrid, tgrid_rcp, zgrid_grid, zgrid_rcp;
	s32 t_z0, t_zs, t_zd, t_vf, t_cov, t_lod_i, t_lod_f, t_uv0, t_uv1, t_uvmm0, t_uvmm1, t_trb, t_tga, gd;
	s32 g_fm, g_zm, g_frb, g_fga, g_mxl, g_l, g_c3, g_c2, g_c1, g_c0, g_lod_i, g_lod_f;
} swd_layout;

static const swd_layout swd_layout8 = {
	GS_SL8_SKIP, GS_SL8_D_Z0, GS_SL8_D_Z1, GS_SL8_D_S, GS_SL8_D_T, GS_SL8_D_Q, GS_SL8_D_RB, GS_SL8_D_GA, GS_SL8_D_F,
	GS_SL8_D8_P_Z, GS_SL8_D8_STQ, GS_SL8_D8_C, GS_SL8_D8_P_F,
	GS_SL8_P_Z, GS_SL8_P_F, GS_SL8_C_RB, GS_SL8_C_GA, GS_SL8_TGRID, GS_SL8_TGRID_RCP, GS_SL8_ZGRID_GRID, GS_SL8_ZGRID_RCP,
	GS_SL8_TEMP_Z0, GS_SL8_TEMP_ZS, GS_SL8_TEMP_ZD, GS_SL8_TEMP_VF, GS_SL8_TEMP_COV, GS_SL8_TEMP_LOD_I, GS_SL8_TEMP_LOD_F,
	GS_SL8_TEMP_UV0, GS_SL8_TEMP_UV1, GS_SL8_TEMP_UVMM0, GS_SL8_TEMP_UVMM1, GS_SL8_TEMP_TRB, GS_SL8_TEMP_TGA, GS_SL8_GD,
	GS_SLG8_FM, GS_SLG8_ZM, GS_SLG8_FRB, GS_SLG8_FGA, GS_SLG8_MXL, GS_SLG8_L, GS_SLG8_LOD_C3, GS_SLG8_LOD_C2,
	GS_SLG8_LOD_C1, GS_SLG8_LOD_C0, GS_SLG8_LOD_I, GS_SLG8_LOD_F
};

static const swd_layout swd_layout4 = {
	GS_SL4_SKIP, GS_SL4_D_Z0, GS_SL4_D_Z1, GS_SL4_D_S, GS_SL4_D_T, GS_SL4_D_Q, GS_SL4_D_RB, GS_SL4_D_GA, GS_SL4_D_F,
	GS_SL4_D4_Z, GS_SL4_D4_STQ, GS_SL4_D4_C, GS_SL4_D4_F,
	GS_SL4_P_Z, GS_SL4_P_F, GS_SL4_C_RB, GS_SL4_C_GA, GS_SL4_TGRID, GS_SL4_TGRID_RCP, GS_SL4_ZGRID_GRID, GS_SL4_ZGRID_RCP,
	GS_SL4_TEMP_Z0, GS_SL4_TEMP_ZS, GS_SL4_TEMP_ZD, GS_SL4_TEMP_VF, GS_SL4_TEMP_COV, GS_SL4_TEMP_LOD_I, GS_SL4_TEMP_LOD_F,
	GS_SL4_TEMP_UV0, GS_SL4_TEMP_UV1, GS_SL4_TEMP_UVMM0, GS_SL4_TEMP_UVMM1, GS_SL4_TEMP_TRB, GS_SL4_TEMP_TGA, GS_SL4_GD,
	GS_SLG4_FM, GS_SLG4_ZM, GS_SLG4_FRB, GS_SLG4_FGA, GS_SLG4_MXL, GS_SLG4_L, GS_SLG4_LOD_C3, GS_SLG4_LOD_C2,
	GS_SLG4_LOD_C1, GS_SLG4_LOD_C0, GS_SLG4_LOD_I, GS_SLG4_LOD_F
};

/* The labels */
enum
{
	L_LOOP,
	L_STEP,
	L_EXIT
};

/* Vector registers by role */
enum
{
	R_FD   = 2,
	R_FM   = 3,
	R_ZM   = 4,
	R_RB   = 5,
	R_GA   = 6,
	R_Z    = 8,
	R_F    = 9,
	R_S    = 10,
	R_T    = 11,
	R_Q    = 12,
	R_F_RB = 13,
	R_F_GA = 14,
	R_TEST = 15
};

typedef struct swd
{
	gsj j;
	const swd_layout* lo;
	/* the selector */
	unsigned fpsm, zpsm, ztst, atst, afail, iip, tfx, tcc, fst, ltf, tlu, fge, date, abe, aba, abb, abc, abd, pabe, aa1;
	unsigned fwrite, rfb, zwrite, ztest, zoverflow, zclamp, wms, wmt, datm, colclamp, fba, dthe, prim, edge, tw, lcm,
		mmin, notest, zequal, hires, tw_hi, fb, zb;
	int ymm, win64, use_lod;
	int vecints, vecsize;
	/* the general registers */
	int a0, a1, a2, a3, t0, t1, t2, t3, local, gd, vm, clut, tex;
	s32 top; /* where top is kept for dithering, from rsp */
	const u8* consts;
	const void* zbias;
} swd;

#define J (&s->j)

/* The vector register of the layout's width, and its xmm */
static gsj_op V(const swd* s, int n)
{
	return s->ymm ? gsj_y(n) : gsj_x(n);
}
#define X(n) gsj_x(n)
#define Q(n) gsj_r64(n)
#define D(n) gsj_r32(n)

static gsj_op loc(const swd* s, s32 off)
{
	return gsj_ptr(s->local, off);
}

static gsj_op glb(const swd* s, s32 off)
{
	return gsj_ptr(s->gd, off);
}

/* On AVX the three-operand form; otherwise dst = src1 first */
typedef void (*swd_op2)(gsj*, gsj_op, gsj_op);
typedef void (*swd_op3)(gsj*, gsj_op, gsj_op, gsj_op);
typedef void (*swd_sh2)(gsj*, gsj_op, unsigned);
typedef void (*swd_sh3)(gsj*, gsj_op, gsj_op, unsigned);

static void three(swd* s, swd_op3 v, swd_op2 o, gsj_op dst, gsj_op src1, gsj_op src2)
{
	if (s->j.avx)
		v(J, dst, src1, src2);
	else
	{
		gsj_movdqa(J, dst, src1);
		o(J, dst, src2);
	}
}

static void three_i(swd* s, swd_sh3 v, swd_sh2 o, gsj_op dst, gsj_op src, unsigned imm)
{
	if (s->j.avx)
		v(J, dst, src, imm);
	else
	{
		gsj_movdqa(J, dst, src);
		o(J, dst, imm);
	}
}
#define THREE(op, d, a, b) three(s, gsj_v##op, gsj_##op, d, a, b)
#define THREE_I(op, d, a, imm) three_i(s, gsj_v##op##_i, gsj_##op##_i, d, a, imm)

/* With ymm, broadcast mem into tmp and op it; otherwise op mem */
static void bcast_op(swd* s, swd_op2 bcast, swd_op2 op, gsj_op dst, gsj_op tmp, gsj_op mem)
{
	if (s->ymm)
	{
		bcast(J, tmp, mem);
		op(J, dst, tmp);
	}
	else
		op(J, dst, mem);
}

/* --- helpers ---------------------------------------------------------- */

static void broadcastf128(swd* s, gsj_op reg, gsj_op mem)
{
	if (s->ymm)
		gsj_vbroadcastf128(J, reg, mem);
	else
		gsj_movaps(J, reg, mem);
}

static void broadcasti128(swd* s, gsj_op reg, gsj_op mem)
{
	if (s->ymm)
		gsj_vbroadcasti128(J, reg, mem);
	else
		gsj_movdqa(J, reg, mem);
}

static void pbroadcastq_local(swd* s, gsj_op reg, gsj_op mem)
{
	if (s->ymm)
		gsj_vpbroadcastq(J, reg, mem);
	else
		gsj_movdqa(J, reg, mem);
}

static void pbroadcastd_local(swd* s, gsj_op reg, gsj_op mem)
{
	if (s->ymm)
		gsj_vpbroadcastd(J, reg, mem);
	else
		gsj_movdqa(J, reg, mem);
}

static void pbroadcastw_local(swd* s, gsj_op reg, gsj_op mem)
{
	if (s->ymm)
		gsj_vpbroadcastw(J, reg, mem);
	else
		gsj_movdqa(J, reg, mem);
}

static void broadcastsd(swd* s, gsj_op reg, gsj_op mem)
{
	if (s->ymm)
		gsj_vbroadcastsd(J, reg, mem);
	else
		gsj_movddup(J, reg, mem);
}

/* eax across vec */
static void broadcast_eax(swd* s, int vec)
{
	gsj_movd(J, X(vec), D(GSJ_RAX));
	if (s->ymm)
		gsj_vpbroadcastd(J, V(s, vec), X(vec));
	else
		gsj_pshufd(J, V(s, vec), V(s, vec), SHUF(0, 0, 0, 0));
}

static void modulate16(swd* s, gsj_op a, gsj_op f, unsigned shift)
{
	gsj_psllw_i(J, a, shift + 1);
	gsj_pmulhw(J, a, f);
}

static void lerp16(swd* s, gsj_op a, gsj_op b, gsj_op f, unsigned shift)
{
	gsj_psubw(J, a, b);
	modulate16(s, a, f, shift);
	gsj_paddw(J, a, b);
}

static void lerp16_4(swd* s, gsj_op a, gsj_op b, gsj_op f)
{
	gsj_psubw(J, a, b);
	gsj_pmullw(J, a, f);
	gsj_psraw_i(J, a, 4);
	gsj_paddw(J, a, b);
}

static void mix16(swd* s, gsj_op a, gsj_op b)
{
	gsj_pblendw(J, a, b, 0xaa);
}

static void clamp16(swd* s, gsj_op a, gsj_op temp)
{
	gsj_packuswb(J, a, a);
	if (!s->ymm)
		gsj_pmovzxbw(J, a, a);
	else
	{
		gsj_pxor(J, temp, temp);
		gsj_punpcklbw(J, a, temp);
	}
}

/* To step when every pixel failed */
static void alltrue(swd* s, gsj_op test)
{
	gsj_pmovmskb(J, D(GSJ_RAX), test);
	gsj_cmp_i(J, D(GSJ_RAX), s->ymm ? 0xffffffffu : 0xffffu);
	gsj_je(J, L_STEP, 1);
}

static void blend(swd* s, gsj_op a, gsj_op b, gsj_op mask)
{
	gsj_pand(J, b, mask);
	gsj_pandn(J, mask, a);
	if (s->j.avx)
		gsj_vpor(J, a, b, mask);
	else
	{
		gsj_por(J, b, mask);
		gsj_movdqa(J, a, b);
	}
}

static void blend8r(swd* s, gsj_op b, gsj_op a)
{
	if (s->j.avx)
		gsj_vpblendvb(J, b, a, b, V(s, 0));
	else
	{
		gsj_pblendvb(J, a, b);
		gsj_movdqa(J, b, a);
	}
}

/* l = src & 0xff, h = src >> 8 & 0xff, per word */
static void split16_2x8(swd* s, gsj_op l, gsj_op h, gsj_op src)
{
	if (s->j.avx)
	{
		if (src.reg == h.reg)
		{
			gsj_vpsllw_i(J, l, src, 8);
			gsj_psrlw_i(J, h, 8);
		}
		else if (src.reg == l.reg)
		{
			gsj_vpsrlw_i(J, h, src, 8);
			gsj_psllw_i(J, l, 8);
		}
		else
		{
			gsj_vpsllw_i(J, l, src, 8);
			gsj_vpsrlw_i(J, h, src, 8);
		}
		gsj_psrlw_i(J, l, 8);
	}
	else
	{
		if (src.reg == h.reg)
			gsj_movdqa(J, l, src);
		else if (src.reg == l.reg)
			gsj_movdqa(J, h, src);
		else
		{
			gsj_movdqa(J, l, src);
			gsj_movdqa(J, h, src);
		}
		gsj_psllw_i(J, l, 8);
		gsj_psrlw_i(J, l, 8);
		gsj_psrlw_i(J, h, 8);
	}
}

/* [vm + addr * 2 + off] */
static gsj_op vm_at(const swd* s, int addr, s32 off, unsigned bits)
{
	return gsj_mem(bits, s->vm, addr, 2, off);
}

/* --- the parts ---------------------------------------------------------- */

static void init(swd* s)
{
	const int ymm = s->ymm;
	if (!s->notest)
	{
		/* skip = left & 3; left -= skip; steps = pixels + skip - 4 */
		gsj_mov(J, D(GSJ_RBX), D(s->a1));
		gsj_and_i(J, D(s->a1), (u32)(s->vecints - 1));
		gsj_sub(J, D(GSJ_RBX), D(s->a1));
		gsj_lea(J, D(s->a0), gsj_mem(0, s->a0, s->a1, 1, -s->vecints));
		/* test = m_test[skip] | m_test[7 + (steps & (steps >> 31))] */
		gsj_mov(J, D(GSJ_RAX), D(s->a0));
		gsj_sar(J, D(GSJ_RAX), 31);
		gsj_and(J, D(GSJ_RAX), D(s->a0));
		if (!ymm)
			gsj_shl(J, D(GSJ_RAX), 4);
		gsj_cdqe(J);
		if (!ymm)
		{
			gsj_lea(J, Q(s->t1), gsj_rip(J, s->consts + GS_SLC_TEST_128B));
			gsj_shl(J, D(s->a1), 4);
			gsj_movdqa(J, V(s, R_TEST), gsj_mem(0, s->a1, s->t1, 1, 0));
			gsj_por(J, V(s, R_TEST), gsj_mem(0, GSJ_RAX, s->t1, 1, 7 * 16));
		}
		else
		{
			gsj_lea(J, Q(s->t1), gsj_rip(J, s->consts + GS_SLC_TEST_256B));
			gsj_pmovsxbd(J, V(s, R_TEST), gsj_mem(0, s->t1, s->a1, 8, 0));
			gsj_pmovsxbd(J, V(s, 0), gsj_mem(0, s->t1, GSJ_RAX, 8, 15 * 8));
			gsj_por(J, V(s, R_TEST), V(s, 0));
			gsj_shl(J, D(s->a1), 5);
		}
	}
	else
	{
		gsj_mov(J, D(GSJ_RBX), D(s->a1));
		gsj_xor(J, D(s->a1), D(s->a1));
		gsj_lea(J, D(s->a0), gsj_ptr(s->a0, -s->vecints));
	}

	/* fza_base = &gd->fzbr[top]; fza_offset = &gd->fzbc[left >> 2] */
	gsj_mov(J, Q(GSJ_RAX), glb(s, GS_SLG_FZBR));
	gsj_lea(J, Q(s->t1), gsj_mem(0, GSJ_RAX, s->a2, 8, 0));
	gsj_mov(J, Q(GSJ_RAX), glb(s, GS_SLG_FZBC));
	gsj_lea(J, Q(s->t0), gsj_mem(0, GSJ_RAX, GSJ_RBX, 2, 0));

	if ((s->prim != GS_SWJ_SPRITE_CLASS && ((s->fwrite && s->fge) || s->zb))
		|| (s->fb && (s->edge || s->tfx != GS_SWJ_TFX_NONE || s->iip)))
	{
		/* a1 = &local.d[skip] (a1 was skip << 4) */
		gsj_lea(J, Q(GSJ_RAX), loc(s, 0));
		gsj_lea(J, Q(s->a1), gsj_mem(0, GSJ_RAX, s->a1, 8, 0));
	}

	if (s->prim != GS_SWJ_SPRITE_CLASS)
	{
		if ((s->fwrite && s->fge) || s->zb)
		{
			if (s->fwrite && s->fge)
			{
				/* f = GSVector4i(v.t).zzzzh().zzzz().add16(d[skip].f) */
				const gsj_op f = V(s, R_F);
				if (ymm)
					gsj_vbroadcastss(J, f, gsj_ptr(s->a3, GS_VSW_T + 12));
				else
					gsj_movss(J, f, gsj_ptr(s->a3, GS_VSW_T + 12));
				gsj_cvttps2dq(J, f, f);
				gsj_punpcklwd(J, f, f);
				gsj_pshufd(J, f, f, SHUF(0, 0, 0, 0));
				gsj_paddw(J, f, gsj_ptr(s->a1, s->lo->d_f));
			}

			if (s->zb && s->zequal)
			{
				gsj_cvttsd2si(J, Q(GSJ_RAX), gsj_ptr(s->a3, GS_VSW_P + 8));
				gsj_movd(J, X(R_Z), D(GSJ_RAX));
				if (s->j.avx2)
					gsj_vpbroadcastd(J, V(s, R_Z), X(R_Z));
				else
					gsj_pshufd(J, V(s, R_Z), V(s, R_Z), SHUF(0, 0, 0, 0));
			}
			else if (s->zb)
			{
				/* z0 = v.p.zzzz() + d[skip].z0; z1 likewise */
				if (s->prim != GS_SWJ_POINT_CLASS && !s->edge)
				{
					/* v.p.z snapped to the primitive's grid */
					gsj_movsd(J, X(1), gsj_ptr(s->a3, GS_VSW_P + 8));
					gsj_mulsd(J, X(1), loc(s, s->lo->zgrid_rcp));
					gsj_roundsd(J, X(1), X(1), 8);
					gsj_mulsd(J, X(1), loc(s, s->lo->zgrid_grid));
					if (ymm)
						gsj_vbroadcastsd(J, gsj_y(1), X(1));
					else
						gsj_unpcklpd(J, X(1), X(1));
				}
				else
					broadcastsd(s, V(s, 1), gsj_ptr(s->a3, GS_VSW_P + 8));
				gsj_movaps(J, V(s, 7), gsj_ptr(s->a1, s->lo->d_z0));
				gsj_addpd(J, V(s, 7), V(s, 1));
				gsj_movaps(J, loc(s, s->lo->t_z0), V(s, 7));
				gsj_movaps(J, V(s, R_Z), gsj_ptr(s->a1, s->lo->d_z1));
				gsj_addpd(J, V(s, R_Z), V(s, 1));
			}
		}
	}
	else if (s->fwrite && s->fge)
		pbroadcastw_local(s, V(s, R_F), loc(s, s->lo->p_f));

	if (s->fb)
	{
		if (s->edge)
		{
			/* temp.cov = broadcast16(scan.p).srl16<9>() */
			if (s->j.avx2)
				gsj_vpbroadcastw(J, V(s, 3), gsj_ptr(s->a3, GS_VSW_P));
			else
			{
				gsj_movd(J, X(3), gsj_ptr(s->a3, GS_VSW_P));
				gsj_punpcklwd(J, X(3), X(3));
				gsj_pshufd(J, X(3), X(3), SHUF(0, 0, 0, 0));
			}
			gsj_psrlw_i(J, V(s, 3), 9);
			gsj_movdqa(J, loc(s, s->lo->t_cov), V(s, 3));
		}

		if (s->tfx != GS_SWJ_TFX_NONE)
		{
			const gsj_op vt = V(s, 4);
			const gsj_op ss = V(s, R_S);
			const gsj_op tt = V(s, R_T);
			if (!s->fst && s->prim != GS_SWJ_POINT_CLASS && !s->edge)
			{
				/* v.t snapped to the primitive's grid */
				gsj_movaps(J, X(4), gsj_ptr(s->a3, GS_VSW_T));
				gsj_mulps(J, X(4), loc(s, s->lo->tgrid_rcp));
				gsj_roundps(J, X(4), X(4), 8);
				gsj_mulps(J, X(4), loc(s, s->lo->tgrid));
				if (ymm)
					gsj_vinsertf128(J, gsj_y(4), gsj_y(4), X(4), 1);
			}
			else
				broadcastf128(s, vt, gsj_ptr(s->a3, GS_VSW_T));

			if (s->fst)
			{
				/* s = vti.xxxx() + d[skip].s; t = vti.yyyy() (+ d[skip].t) */
				gsj_cvttps2dq(J, V(s, 6), vt);
				gsj_pshufd(J, ss, V(s, 6), SHUF(0, 0, 0, 0));
				gsj_pshufd(J, tt, V(s, 6), SHUF(1, 1, 1, 1));
				gsj_paddd(J, ss, gsj_ptr(s->a1, s->lo->d_s));
				if (s->prim != GS_SWJ_SPRITE_CLASS || s->mmin)
					gsj_paddd(J, tt, gsj_ptr(s->a1, s->lo->d_t));
				else if (s->ltf)
				{
					const gsj_op vf = V(s, 5);
					gsj_pshuflw(J, vf, tt, SHUF(2, 2, 0, 0));
					gsj_pshufhw(J, vf, vf, SHUF(2, 2, 0, 0));
					gsj_psrlw_i(J, vf, 12);
					gsj_movdqa(J, loc(s, s->lo->t_vf), vf);
				}
			}
			else
			{
				const gsj_op qq = V(s, R_Q);
				if (s->j.avx)
				{
					gsj_vshufps(J, ss, vt, vt, SHUF(0, 0, 0, 0));
					gsj_vshufps(J, tt, vt, vt, SHUF(1, 1, 1, 1));
					gsj_vshufps(J, qq, vt, vt, SHUF(2, 2, 2, 2));
				}
				else
				{
					gsj_movaps(J, ss, vt);
					gsj_movaps(J, tt, vt);
					gsj_movaps(J, qq, vt);
					gsj_shufps(J, ss, ss, SHUF(0, 0, 0, 0));
					gsj_shufps(J, tt, tt, SHUF(1, 1, 1, 1));
					gsj_shufps(J, qq, qq, SHUF(2, 2, 2, 2));
				}
				gsj_addps(J, ss, gsj_ptr(s->a1, s->lo->d_s));
				gsj_addps(J, tt, gsj_ptr(s->a1, s->lo->d_t));
				gsj_addps(J, qq, gsj_ptr(s->a1, s->lo->d_q));
			}
		}

		if (!(s->tfx == GS_SWJ_TFX_DECAL && s->tcc))
		{
			const gsj_op f_rb = V(s, R_F_RB);
			const gsj_op f_ga = V(s, R_F_GA);
			if (s->iip)
			{
				/* vc = GSVector4i(v.c); vc = vc.upl16(vc.zwxy()) */
				if (!ymm)
					gsj_cvttps2dq(J, V(s, 6), gsj_ptr(s->a3, GS_VSW_C));
				else
				{
					gsj_vbroadcastf128(J, gsj_y(6), gsj_ptr(s->a3, GS_VSW_C));
					gsj_cvttps2dq(J, gsj_y(6), gsj_y(6));
				}
				gsj_pshufd(J, V(s, 5), V(s, 6), SHUF(1, 0, 3, 2));
				gsj_punpcklwd(J, V(s, 6), V(s, 5));
				/* rb = vc.xxxx().add16(d[skip].rb); ga = vc.zzzz()... */
				gsj_pshufd(J, f_rb, V(s, 6), SHUF(0, 0, 0, 0));
				gsj_pshufd(J, f_ga, V(s, 6), SHUF(2, 2, 2, 2));
				gsj_paddw(J, f_rb, gsj_ptr(s->a1, s->lo->d_rb));
				gsj_paddw(J, f_ga, gsj_ptr(s->a1, s->lo->d_ga));
			}
			else
			{
				gsj_movdqa(J, f_rb, loc(s, s->lo->c_rb));
				gsj_movdqa(J, f_ga, loc(s, s->lo->c_ga));
			}
			gsj_movdqa(J, V(s, R_RB), f_rb);
			gsj_movdqa(J, V(s, R_GA), f_ga);
		}
	}

	/* (top kept for dithering: a2 is the SysV fzm's edx) */
	if (s->fwrite && s->fpsm == 2 && s->dthe)
		gsj_mov(J, gsj_ptr(GSJ_RSP, s->top), Q(s->a2));

	gsj_mov(J, Q(s->vm), glb(s, GS_SLG_VM));
	if (s->fb && s->tfx != GS_SWJ_TFX_NONE)
	{
		if (s->use_lod)
			gsj_lea(J, Q(s->tex), glb(s, GS_SLG_TEX));
		else
			gsj_mov(J, Q(s->tex), glb(s, GS_SLG_TEX));
	}
}

static void step(swd* s)
{
	gsj_sub_i(J, D(s->a0), (u32)s->vecints);
	gsj_add_i(J, Q(s->t0), (u32)(s->vecsize / 2));

	if (s->prim != GS_SWJ_SPRITE_CLASS)
	{
		if (s->zb && !s->zequal)
		{
			broadcastsd(s, V(s, 7), loc(s, s->lo->step_z));
			gsj_addpd(J, V(s, R_Z), V(s, 7));
			gsj_addpd(J, V(s, 7), loc(s, s->lo->t_z0));
			gsj_movaps(J, loc(s, s->lo->t_z0), V(s, 7));
		}
		if (s->fwrite && s->fge)
			bcast_op(s, gsj_vpbroadcastw, gsj_paddw, V(s, R_F), V(s, 0), loc(s, s->lo->step_f));
	}

	if (s->fb)
	{
		if (s->tfx != GS_SWJ_TFX_NONE)
		{
			if (s->fst)
			{
				broadcasti128(s, V(s, 0), loc(s, s->lo->step_stq));
				gsj_pshufd(J, V(s, 1), V(s, 0), SHUF(0, 0, 0, 0));
				gsj_paddd(J, V(s, R_S), V(s, 1));
				if (s->prim != GS_SWJ_SPRITE_CLASS || s->mmin)
				{
					gsj_pshufd(J, V(s, 1), V(s, 0), SHUF(1, 1, 1, 1));
					gsj_paddd(J, V(s, R_T), V(s, 1));
				}
			}
			else
			{
				const gsj_op ss = V(s, 2), tt = V(s, 3), qq = V(s, 1);
				if (s->j.avx)
				{
					broadcastf128(s, qq, loc(s, s->lo->step_stq));
					gsj_vshufps(J, ss, qq, qq, SHUF(0, 0, 0, 0));
					gsj_vshufps(J, tt, qq, qq, SHUF(1, 1, 1, 1));
					gsj_vshufps(J, qq, qq, qq, SHUF(2, 2, 2, 2));
				}
				else
				{
					gsj_movaps(J, qq, loc(s, s->lo->step_stq));
					gsj_movaps(J, ss, qq);
					gsj_movaps(J, tt, qq);
					gsj_shufps(J, ss, ss, SHUF(0, 0, 0, 0));
					gsj_shufps(J, tt, tt, SHUF(1, 1, 1, 1));
					gsj_shufps(J, qq, qq, SHUF(2, 2, 2, 2));
				}
				gsj_addps(J, V(s, R_S), ss);
				gsj_addps(J, V(s, R_T), tt);
				gsj_addps(J, V(s, R_Q), qq);
			}
		}

		if (!(s->tfx == GS_SWJ_TFX_DECAL && s->tcc))
		{
			if (s->iip)
			{
				const gsj_op c = V(s, 0);
				pbroadcastq_local(s, c, loc(s, s->lo->step_c));
				gsj_pshufd(J, V(s, R_RB), c, SHUF(0, 0, 0, 0));
				gsj_pshufd(J, V(s, R_GA), c, SHUF(1, 1, 1, 1));
				gsj_paddw(J, V(s, R_F_RB), V(s, R_RB));
				gsj_paddw(J, V(s, R_F_GA), V(s, R_GA));
				gsj_pxor(J, c, c);
				gsj_pmaxsw(J, V(s, R_F_RB), c);
				gsj_pmaxsw(J, V(s, R_F_GA), c);
			}
			gsj_movdqa(J, V(s, R_RB), V(s, R_F_RB));
			gsj_movdqa(J, V(s, R_GA), V(s, R_F_GA));
		}
	}

	if (!s->notest)
	{
		/* test = m_test[7 + (steps & (steps >> 31))] */
		gsj_mov(J, D(GSJ_RAX), D(s->a0));
		gsj_sar(J, D(GSJ_RAX), 31);
		gsj_and(J, D(GSJ_RAX), D(s->a0));
		if (!s->ymm)
		{
			gsj_shl(J, D(GSJ_RAX), 4);
			gsj_cdqe(J);
			gsj_lea(J, Q(s->t2), gsj_rip(J, s->consts + GS_SLC_TEST_128B + 7 * 16));
			gsj_movdqa(J, V(s, R_TEST), gsj_mem(0, GSJ_RAX, s->t2, 1, 0));
		}
		else
		{
			gsj_cdqe(J);
			gsj_lea(J, Q(s->t2), gsj_rip(J, s->consts + GS_SLC_TEST_256B + 15 * 8));
			gsj_pmovsxbd(J, V(s, R_TEST), gsj_mem(0, s->t2, GSJ_RAX, 8, 0));
		}
	}
}

/* dst = vm16[addr] (8 pixels, with tmp, for ymm) */
static void read_pixel(swd* s, gsj_op dst, gsj_op tmp, int addr)
{
	if (!s->ymm)
	{
		gsj_movq(J, dst, vm_at(s, addr, 0, 64));
		gsj_movhps(J, dst, vm_at(s, addr, 16, 64));
	}
	else
	{
		gsj_movq(J, X(dst.reg), vm_at(s, addr, 0, 64));
		gsj_movhps(J, X(dst.reg), vm_at(s, addr, 16, 64));
		gsj_movq(J, X(tmp.reg), vm_at(s, addr, 32, 64));
		gsj_movhps(J, X(tmp.reg), vm_at(s, addr, 48, 64));
		gsj_vinserti128(J, dst, dst, X(tmp.reg), 1);
	}
}

static void test_z(swd* s, gsj_op temp1, gsj_op temp2)
{
	if (!s->zb)
		return;

	/* za = fza_base.y + fza_offset->y */
	gsj_mov(J, D(s->t2), gsj_mem(32, s->t1, -1, 1, 4));
	gsj_add(J, D(s->t2), gsj_mem(32, s->t0, -1, 1, 4));
	gsj_and_i(J, D(s->t2), (s->hires ? 4 * GS_SWJ_HALF_VM_SIZE : GS_SWJ_HALF_VM_SIZE) - 1);

	if (s->prim != GS_SWJ_SPRITE_CLASS)
	{
		if (s->zequal)
			gsj_movdqa(J, V(s, 0), V(s, R_Z));
		else if (s->zoverflow)
		{
			/* zs = (z + 0xc1e00000000fffff).f64toi32() + 0x80000000 */
			broadcastsd(s, temp1, gsj_rip(J, s->zbias));
			gsj_addpd(J, V(s, 7), temp1);
			gsj_addpd(J, temp1, V(s, R_Z));
			gsj_cvtpd2dq(J, X(0), V(s, 7));
			gsj_cvtpd2dq(J, X(temp1.reg), temp1);
			if (s->ymm)
				gsj_vinserti128(J, V(s, 0), V(s, 0), X(temp1.reg), 1);
			else
				gsj_punpcklqdq(J, V(s, 0), temp1);
			gsj_pcmpeqd(J, temp1, temp1);
			gsj_pslld_i(J, temp1, 31);
			gsj_paddd(J, V(s, 0), temp1);
		}
		else
		{
			gsj_cvttpd2dq(J, X(0), V(s, 7));
			gsj_cvttpd2dq(J, X(temp1.reg), V(s, R_Z));
			if (s->ymm)
				gsj_vinserti128(J, V(s, 0), V(s, 0), X(temp1.reg), 1);
			else
				gsj_punpcklqdq(J, V(s, 0), temp1);
		}

		if (s->zclamp)
		{
			gsj_pcmpeqd(J, temp1, temp1);
			gsj_psrld_i(J, temp1, (s->zpsm & 3) * 8);
			gsj_pminud(J, V(s, 0), temp1);
		}

		if (s->zwrite)
			gsj_movdqa(J, loc(s, s->lo->t_zs), V(s, 0));
	}
	else
		pbroadcastd_local(s, V(s, 0), loc(s, s->lo->p_z));

	if (s->ztest)
	{
		read_pixel(s, temp2, temp1, s->t2);
		if (s->zwrite && s->zpsm < 2)
			gsj_movdqa(J, loc(s, s->lo->t_zd), temp2);

		/* zd &= 0xffffffff >> zpsm * 8 */
		if (s->zpsm)
		{
			gsj_pslld_i(J, temp2, s->zpsm * 8);
			gsj_psrld_i(J, temp2, s->zpsm * 8);
		}

		if (s->zpsm == 0)
		{
			/* compared signed, less 0x80000000 */
			gsj_pcmpeqd(J, temp1, temp1);
			gsj_pslld_i(J, temp1, 31);
			gsj_psubd(J, V(s, 0), temp1);
			gsj_psubd(J, temp2, temp1);
		}

		switch (s->ztst)
		{
			case GS_SWJ_ZTST_GEQUAL:
				gsj_pcmpgtd(J, temp2, V(s, 0));
				gsj_por(J, V(s, R_TEST), temp2);
				break;
			case GS_SWJ_ZTST_GREATER:
				gsj_pcmpgtd(J, V(s, 0), temp2);
				gsj_pcmpeqd(J, temp1, temp1);
				gsj_pxor(J, V(s, 0), temp1);
				gsj_por(J, V(s, R_TEST), V(s, 0));
				break;
		}

		alltrue(s, V(s, R_TEST));
	}
}

/* --- texels ------------------------------------------------------------- */

/* rbx = tex[lod's level lod] (+ mip) */
static void load_tex_lod(swd* s, int lod, int mip_offset)
{
	const gsj_op at = s->lcm ? glb(s, s->lo->g_lod_i + 4 * lod) : loc(s, s->lo->t_lod_i + 4 * lod);
	gsj_mov(J, D(GSJ_RBX), at);
	gsj_mov(J, Q(GSJ_RBX), gsj_mem(0, s->tex, GSJ_RBX, 8, mip_offset));
}

/* dst[i] = tex (or clut[tex]) at addr[i] */
static void read_texel_lane(swd* s, int dst, int addr, unsigned i, int tex_in_rbx, int preserve)
{
	const int tex     = tex_in_rbx ? GSJ_RBX : s->tex;
	const gsj_op src = s->tlu ? gsj_mem(0, s->clut, GSJ_RAX, 4, 0) : gsj_mem(0, tex, GSJ_RAX, 4, 0);
	if (i == 0)
		gsj_movd(J, D(GSJ_RAX), X(addr));
	else
		gsj_pextrd(J, D(GSJ_RAX), X(addr), i);
	if (s->tlu)
		gsj_movzx(J, D(GSJ_RAX), gsj_mem(8, tex, GSJ_RAX, 1, 0));
	if (i == 0 && !preserve)
		gsj_movd(J, X(dst), src);
	else
		gsj_pinsrd(J, X(dst), src, i);
}

/* The texels of pixels (1 or 4) address registers: d (the destinations),
 * a (the addresses; d[2], d[3] are a[0], a[1]), tmp */
static void read_texels(swd* s, const int* d, const int* a, int tmp, int pixels, int mip_offset)
{
	int i, j;
	mip_offset *= 8;
	if (!s->ymm)
	{
		static const int preserve[4] = {0, 0, 1, 1};
		if (s->use_lod && !s->lcm)
		{
			for (j = 0; j < 4; j++)
			{
				load_tex_lod(s, j, mip_offset);
				for (i = 0; i < pixels; i++)
					read_texel_lane(s, d[i], a[i], j, 1, preserve[i]);
			}
		}
		else
		{
			int rbx = 0;
			if (s->use_lod && s->lcm)
			{
				load_tex_lod(s, 0, mip_offset);
				rbx = 1;
			}
			for (i = 0; i < pixels; i++)
				for (j = 0; j < 4; j++)
					read_texel_lane(s, d[i], a[i], j, rbx, 0);
		}
	}
	else
	{
		/* the gather's mask: the next destination, or the address
		 * register after the last's */
		int t1[4];
		int rbx = 0;
		t1[0] = d[1];
		t1[1] = d[2];
		t1[2] = d[3];
		t1[3] = a[2];
		if (s->use_lod && s->lcm)
		{
			load_tex_lod(s, 0, mip_offset);
			rbx = 1;
		}
		for (i = 0; i < pixels; i++)
		{
			if (s->use_lod && !s->lcm)
			{
				rbx = 1;
				gsj_vextracti128(J, X(t1[i]), gsj_y(a[i]), 1);
				for (j = 0; j < 4; j++)
				{
					load_tex_lod(s, j, mip_offset);
					read_texel_lane(s, d[i], a[i], j, rbx, 0);
					load_tex_lod(s, j + 4, mip_offset);
					read_texel_lane(s, tmp, t1[i], j, rbx, 0);
				}
				gsj_vinserti128(J, gsj_y(d[i]), gsj_y(d[i]), X(tmp), 1);
			}
			else if (!s->tlu)
			{
				gsj_pcmpeqd(J, gsj_y(t1[i]), gsj_y(t1[i]));
				gsj_vpgatherdd(J, gsj_y(d[i]), gsj_vsib(rbx ? GSJ_RBX : s->tex, gsj_y(a[i]), 4, 0), gsj_y(t1[i]));
			}
			else
			{
				gsj_vextracti128(J, X(t1[i]), gsj_y(a[i]), 1);
				for (j = 0; j < 4; j++)
				{
					read_texel_lane(s, d[i], a[i], j, rbx, 0);
					read_texel_lane(s, tmp, t1[i], j, rbx, 0);
				}
				gsj_vinserti128(J, gsj_y(d[i]), gsj_y(d[i]), X(tmp), 1);
			}
		}
	}
}

/* uv0 in v2 (uv1 in v3 with ltf), uf in v4, vf in v7 or temp.vf: the
 * texel colour to v5 (rb) and v6 (ga) */
static void texel_read(swd* s, int mip_offset)
{
	const unsigned shift = s->tw + (s->tw_hi << 3) + 3;
	gsj_pxor(J, V(s, 0), V(s, 0));
	/* x0 = uv0.upl16(); y0 = uv0.uph16() << tw */
	THREE(punpcklwd, V(s, 5), V(s, 2), V(s, 0));
	gsj_punpckhwd(J, V(s, 2), V(s, 0));
	gsj_pslld_i(J, V(s, 2), shift);

	if (s->ltf)
	{
		static const int d4[4] = {5, 6, 0, 2};
		static const int a4[4] = {0, 2, 1, 3};
		const gsj_op vf = V(s, 7);
		THREE(punpcklwd, V(s, 1), V(s, 3), V(s, 0));
		gsj_punpckhwd(J, V(s, 3), V(s, 0));
		gsj_pslld_i(J, V(s, 3), shift);
		/* addr11, addr01, addr00, addr10 */
		THREE(paddd, V(s, 0), V(s, 3), V(s, 1));
		gsj_paddd(J, V(s, 1), V(s, 2));
		gsj_paddd(J, V(s, 2), V(s, 5));
		gsj_paddd(J, V(s, 3), V(s, 5));

		read_texels(s, d4, a4, 7, 4, mip_offset);

		/* c00 in v6, c01 in v0, c10 in v2, c11 in v5 */
		split16_2x8(s, V(s, 3), V(s, 6), V(s, 6));
		split16_2x8(s, V(s, 0), V(s, 1), V(s, 0));
		lerp16_4(s, V(s, 0), V(s, 3), V(s, 4));
		lerp16_4(s, V(s, 1), V(s, 6), V(s, 4));
		split16_2x8(s, V(s, 2), V(s, 3), V(s, 2));
		split16_2x8(s, V(s, 5), V(s, 6), V(s, 5));
		lerp16_4(s, V(s, 5), V(s, 2), V(s, 4));
		lerp16_4(s, V(s, 6), V(s, 3), V(s, 4));
		if (s->ymm)
			gsj_movdqa(J, vf, loc(s, s->lo->t_vf));
		lerp16_4(s, V(s, 5), V(s, 0), vf);
		lerp16_4(s, V(s, 6), V(s, 1), vf);
	}
	else
	{
		/* (one pixel: the mask after v0 is v1, as ReadTexel1 gave it) */
		static const int d1[4] = {5, 0, 2, 1};
		static const int a1[4] = {2, 1, 0, 0};
		gsj_paddd(J, V(s, 2), V(s, 5));
		read_texels(s, d1, a1, 1, 1, mip_offset);
		split16_2x8(s, V(s, 5), V(s, 6), V(s, 5));
	}
}

/* uv wrapped (wms, wmt): min v1, max v5, mask v0, tmp v6 from the global
 * data; for one register or two */
static void wrap(swd* s, int uv0, int uv1)
{
	const int wms_clamp = ((s->wms + 1) >> 1) & 1;
	const int wmt_clamp = ((s->wmt + 1) >> 1) & 1;
	const int region    = ((s->wms | s->wmt) >> 1) & 1;
	const gsj_op mask = V(s, 0), mn = V(s, 1), mx = V(s, 5), tmp = V(s, 6);
	const gsj_op u0 = V(s, uv0);
	int k;

	if (uv1 < 0)
	{
		if (wms_clamp == wmt_clamp)
		{
			if (wms_clamp)
			{
				if (region)
					bcast_op(s, gsj_vbroadcasti128, gsj_pmaxsw, u0, mn, glb(s, GS_SLG_T_MIN));
				else
				{
					gsj_pxor(J, tmp, tmp);
					gsj_pmaxsw(J, u0, tmp);
				}
				bcast_op(s, gsj_vbroadcasti128, gsj_pminsw, u0, mx, glb(s, GS_SLG_T_MAX));
			}
			else
			{
				bcast_op(s, gsj_vbroadcasti128, gsj_pand, u0, mn, glb(s, GS_SLG_T_MIN));
				if (region)
					bcast_op(s, gsj_vbroadcasti128, gsj_por, u0, mx, glb(s, GS_SLG_T_MAX));
			}
		}
		else
		{
			broadcasti128(s, mn, glb(s, GS_SLG_T_MIN));
			broadcasti128(s, mx, glb(s, GS_SLG_T_MAX));
			broadcasti128(s, mask, glb(s, GS_SLG_T_MASK));
			/* repeat = (t & min) | max; clamp = t.sat_i16(min, max);
			 * clamp.blend8(repeat, mask) */
			THREE(pand, tmp, u0, mn);
			if (region)
				gsj_por(J, tmp, mx);
			gsj_pmaxsw(J, u0, mn);
			gsj_pminsw(J, u0, mx);
			gsj_pblendvb(J, u0, tmp);
		}
		return;
	}

	{
		const gsj_op u1 = V(s, uv1);
		if (wms_clamp == wmt_clamp)
		{
			if (wms_clamp)
			{
				if (region)
				{
					broadcasti128(s, mn, glb(s, GS_SLG_T_MIN));
					gsj_pmaxsw(J, u0, mn);
					gsj_pmaxsw(J, u1, mn);
				}
				else
				{
					gsj_pxor(J, tmp, tmp);
					gsj_pmaxsw(J, u0, tmp);
					gsj_pmaxsw(J, u1, tmp);
				}
				broadcasti128(s, mx, glb(s, GS_SLG_T_MAX));
				gsj_pminsw(J, u0, mx);
				gsj_pminsw(J, u1, mx);
			}
			else
			{
				broadcasti128(s, mn, glb(s, GS_SLG_T_MIN));
				gsj_pand(J, u0, mn);
				gsj_pand(J, u1, mn);
				if (region)
				{
					broadcasti128(s, mx, glb(s, GS_SLG_T_MAX));
					gsj_por(J, u0, mx);
					gsj_por(J, u1, mx);
				}
			}
		}
		else
		{
			broadcasti128(s, mn, glb(s, GS_SLG_T_MIN));
			broadcasti128(s, mx, glb(s, GS_SLG_T_MAX));
			broadcasti128(s, mask, glb(s, GS_SLG_T_MASK));
			for (k = 0; k < 2; k++)
			{
				const gsj_op uv = k ? u1 : u0;
				THREE(pand, tmp, uv, mn);
				if (region)
					gsj_por(J, tmp, mx);
				gsj_pmaxsw(J, uv, mn);
				gsj_pminsw(J, uv, mx);
				gsj_pblendvb(J, uv, tmp);
			}
		}
	}
}

/* With mipmaps: min v5, max v6 already shifted to the level; mask v0,
 * tmp v1 */
static void wrap_lod(swd* s, int uv0, int uv1)
{
	const int wms_clamp = ((s->wms + 1) >> 1) & 1;
	const int wmt_clamp = ((s->wmt + 1) >> 1) & 1;
	const int region    = ((s->wms | s->wmt) >> 1) & 1;
	const gsj_op mask = V(s, 0), tmp = V(s, 1), mn = V(s, 5), mx = V(s, 6);
	const int n = uv1 < 0 ? 1 : 2;
	int k;

	if (wms_clamp == wmt_clamp)
	{
		if (wms_clamp)
		{
			if (region)
			{
				for (k = 0; k < n; k++)
					gsj_pmaxsw(J, V(s, k ? uv1 : uv0), mn);
			}
			else
			{
				gsj_pxor(J, tmp, tmp);
				for (k = 0; k < n; k++)
					gsj_pmaxsw(J, V(s, k ? uv1 : uv0), tmp);
			}
			for (k = 0; k < n; k++)
				gsj_pminsw(J, V(s, k ? uv1 : uv0), mx);
		}
		else
		{
			for (k = 0; k < n; k++)
				gsj_pand(J, V(s, k ? uv1 : uv0), mn);
			if (region)
				for (k = 0; k < n; k++)
					gsj_por(J, V(s, k ? uv1 : uv0), mx);
		}
	}
	else
	{
		broadcasti128(s, mask, glb(s, GS_SLG_T_MASK));
		for (k = 0; k < n; k++)
		{
			const gsj_op uv = V(s, k ? uv1 : uv0);
			THREE(pand, tmp, uv, mn);
			if (region)
				gsj_por(J, tmp, mx);
			gsj_pmaxsw(J, uv, mn);
			gsj_pminsw(J, uv, mx);
			gsj_pblendvb(J, uv, tmp);
		}
	}
}

/* u in v2, v in v3 (16.16): less half a texel and the fractions to v4
 * (uf) and v7 (vf, kept in temp.vf for ymm) */
static void bilinear_fractions(swd* s)
{
	const gsj_op vf = V(s, 7);
	gsj_mov_i(J, D(GSJ_RAX), 0x8000);
	broadcast_eax(s, 4);
	gsj_psubd(J, V(s, 2), V(s, 4));
	gsj_psubd(J, V(s, 3), V(s, 4));
	gsj_pshuflw(J, V(s, 4), V(s, 2), SHUF(2, 2, 0, 0));
	gsj_pshufhw(J, V(s, 4), V(s, 4), SHUF(2, 2, 0, 0));
	gsj_psrlw_i(J, V(s, 4), 12);
	gsj_pshuflw(J, vf, V(s, 3), SHUF(2, 2, 0, 0));
	gsj_pshufhw(J, vf, vf, SHUF(2, 2, 0, 0));
	gsj_psrlw_i(J, vf, 12);
	if (s->ymm)
		gsj_movdqa(J, loc(s, s->lo->t_vf), vf);
}

/* uv0 = u.sra32<16>().ps32(v.sra32<16>()), uv1 = uv0 + 1, wrapped */
static void lod_uv(swd* s)
{
	gsj_psrad_i(J, V(s, 2), 16);
	gsj_psrad_i(J, V(s, 3), 16);
	gsj_packssdw(J, V(s, 2), V(s, 3));
	if (s->ltf)
	{
		gsj_pcmpeqd(J, V(s, 1), V(s, 1));
		gsj_psrlw_i(J, V(s, 1), 15);
		THREE(paddw, V(s, 3), V(s, 2), V(s, 1));
		wrap_lod(s, 2, 3);
	}
	else
		wrap_lod(s, 2, -1);
}

static void sample_texture(swd* s)
{
	if (!s->fb || s->tfx == GS_SWJ_TFX_NONE)
		return;

	if (!s->fst)
	{
		THREE(divps, V(s, 2), V(s, R_S), V(s, R_Q));
		THREE(divps, V(s, 3), V(s, R_T), V(s, R_Q));
		gsj_cvttps2dq(J, V(s, 2), V(s, 2));
		gsj_cvttps2dq(J, V(s, 3), V(s, 3));
		if (s->ltf)
		{
			/* u -= 0x8000; v -= 0x8000 */
			gsj_mov_i(J, D(GSJ_RAX), 0x8000);
			broadcast_eax(s, 1);
			gsj_psubd(J, V(s, 2), V(s, 1));
			gsj_psubd(J, V(s, 3), V(s, 1));
		}
	}
	else
	{
		gsj_movdqa(J, V(s, 2), V(s, R_S));
		gsj_movdqa(J, V(s, 3), V(s, R_T));
	}

	if (s->ltf)
	{
		const gsj_op vf = V(s, 7);
		gsj_pshuflw(J, V(s, 4), V(s, 2), SHUF(2, 2, 0, 0));
		gsj_pshufhw(J, V(s, 4), V(s, 4), SHUF(2, 2, 0, 0));
		gsj_psrlw_i(J, V(s, 4), 12);
		if (s->prim != GS_SWJ_SPRITE_CLASS)
		{
			gsj_pshuflw(J, vf, V(s, 3), SHUF(2, 2, 0, 0));
			gsj_pshufhw(J, vf, vf, SHUF(2, 2, 0, 0));
			gsj_psrlw_i(J, vf, 12);
			if (s->ymm)
				gsj_movdqa(J, loc(s, s->lo->t_vf), vf);
		}
		else if (!s->ymm)
			gsj_movdqa(J, vf, loc(s, s->lo->t_vf));
	}

	gsj_psrad_i(J, V(s, 2), 16);
	gsj_psrad_i(J, V(s, 3), 16);
	gsj_packssdw(J, V(s, 2), V(s, 3));

	if (s->ltf)
	{
		gsj_pcmpeqd(J, V(s, 0), V(s, 0));
		gsj_psrlw_i(J, V(s, 0), 15);
		THREE(paddw, V(s, 3), V(s, 2), V(s, 0));
		wrap(s, 2, 3);
	}
	else
		wrap(s, 2, -1);

	texel_read(s, 0);
}

static void sample_texture_lod(swd* s)
{
	const swd_layout* lo = s->lo;
	if (!s->fb || s->tfx == GS_SWJ_TFX_NONE)
		return;

	gsj_movdqa(J, V(s, 4), V(s, R_Q));
	if (!s->fst)
	{
		THREE(divps, V(s, 2), V(s, R_S), V(s, 4));
		THREE(divps, V(s, 3), V(s, R_T), V(s, 4));
		gsj_cvttps2dq(J, V(s, 2), V(s, 2));
		gsj_cvttps2dq(J, V(s, 3), V(s, 3));
	}
	else
	{
		gsj_movdqa(J, V(s, 2), V(s, R_S));
		gsj_movdqa(J, V(s, 3), V(s, R_T));
	}

	if (!s->lcm)
	{
		/* lod = -log2(Q) * (1 << L) + K: e = exp(q) - 127 */
		gsj_pcmpeqd(J, V(s, 1), V(s, 1));
		gsj_psrld_i(J, V(s, 1), 25);
		THREE_I(pslld, V(s, 0), V(s, 4), 1);
		gsj_psrld_i(J, V(s, 0), 24);
		gsj_psubd(J, V(s, 0), V(s, 1));
		gsj_cvtdq2ps(J, V(s, 0), V(s, 0));
		/* m = mant(q) | 1.0f */
		gsj_pslld_i(J, V(s, 4), 9);
		gsj_psrld_i(J, V(s, 4), 9);
		gsj_orps(J, V(s, 4), gsj_rip(J, s->consts + (s->ymm ? GS_SLC_ONE_256B : GS_SLC_ONE_128B)));
		/* (m * c3 + c2) * (m * m) + (m * c1 + (e * l + c0)) */
		THREE(mulps, V(s, 5), V(s, 4), V(s, 4));
		THREE(mulps, V(s, 1), V(s, 4), glb(s, lo->g_c3));
		gsj_addps(J, V(s, 1), glb(s, lo->g_c2));
		gsj_mulps(J, V(s, 5), V(s, 1));
		gsj_mulps(J, V(s, 0), glb(s, lo->g_l));
		gsj_addps(J, V(s, 0), glb(s, lo->g_c0));
		gsj_mulps(J, V(s, 4), glb(s, lo->g_c1));
		gsj_addps(J, V(s, 4), V(s, 0));
		gsj_addps(J, V(s, 4), V(s, 5));
		/* clamped to [0, mxl], times 0x10000 */
		gsj_xorps(J, V(s, 0), V(s, 0));
		gsj_minps(J, V(s, 4), glb(s, lo->g_mxl));
		gsj_maxps(J, V(s, 4), V(s, 0));
		gsj_cvtps2dq(J, V(s, 4), V(s, 4));
		if (s->mmin == 1) /* round-off */
		{
			gsj_mov_i(J, D(GSJ_RAX), 0x8000);
			broadcast_eax(s, 0);
			gsj_paddd(J, V(s, 4), V(s, 0));
		}
		THREE_I(psrld, V(s, 0), V(s, 4), 16);
		gsj_movdqa(J, loc(s, lo->t_lod_i), V(s, 0));
		if (s->mmin == 2) /* trilinear */
		{
			gsj_pshuflw(J, V(s, 1), V(s, 4), SHUF(2, 2, 0, 0));
			gsj_pshufhw(J, V(s, 1), V(s, 1), SHUF(2, 2, 0, 0));
			gsj_movdqa(J, loc(s, lo->t_lod_f), V(s, 1));
		}

		/* u, v, and the wrap's min and max shifted by (int)lod */
		if (s->j.avx2)
		{
			gsj_vpsravd(J, V(s, 2), V(s, 2), V(s, 0));
			gsj_vpsravd(J, V(s, 3), V(s, 3), V(s, 0));
			gsj_movdqa(J, loc(s, lo->t_uv0), V(s, 2));
			gsj_movdqa(J, loc(s, lo->t_uv1), V(s, 3));
			gsj_pxor(J, V(s, 1), V(s, 1));
			broadcasti128(s, V(s, 4), glb(s, GS_SLG_T_MIN));
			gsj_vpunpcklwd(J, V(s, 5), V(s, 4), V(s, 1));
			gsj_vpunpckhwd(J, V(s, 6), V(s, 4), V(s, 1));
			gsj_vpsrlvd(J, V(s, 5), V(s, 5), V(s, 0));
			gsj_vpsrlvd(J, V(s, 6), V(s, 6), V(s, 0));
			gsj_packusdw(J, V(s, 5), V(s, 6));
			broadcasti128(s, V(s, 4), glb(s, GS_SLG_T_MAX));
			gsj_vpunpcklwd(J, V(s, 6), V(s, 4), V(s, 1));
			gsj_vpunpckhwd(J, V(s, 4), V(s, 4), V(s, 1));
			gsj_vpsrlvd(J, V(s, 6), V(s, 6), V(s, 0));
			gsj_vpsrlvd(J, V(s, 4), V(s, 4), V(s, 0));
			gsj_packusdw(J, V(s, 6), V(s, 4));
			gsj_movdqa(J, loc(s, lo->t_uvmm0), V(s, 5));
			gsj_movdqa(J, loc(s, lo->t_uvmm1), V(s, 6));
		}
		else
		{
			static const int uvr[4] = {2, 5, 3, 6};
			int k;
			gsj_movq(J, X(4), glb(s, GS_SLG_T_MINMAX));
			THREE(punpckhdq, X(6), X(2), X(3));
			gsj_punpckldq(J, X(2), X(3));
			gsj_movdqa(J, X(5), X(2));
			gsj_movdqa(J, X(3), X(6));
			for (k = 0; k < 4; k++)
			{
				gsj_movd(J, X(0), loc(s, lo->t_lod_i + 4 * k));
				gsj_psrad(J, X(uvr[k]), X(0));
				THREE(psrlw, X(1), X(4), X(0));
				gsj_movq(J, loc(s, (k & 1 ? lo->t_uvmm1 : lo->t_uvmm0) + (k & 2 ? 8 : 0)), X(1));
			}
			gsj_punpckldq(J, X(2), X(3));
			gsj_punpckhdq(J, X(5), X(6));
			THREE(punpckhdq, X(3), X(2), X(5));
			gsj_punpckldq(J, X(2), X(5));
			gsj_movdqa(J, loc(s, lo->t_uv0), X(2));
			gsj_movdqa(J, loc(s, lo->t_uv1), X(3));
			gsj_movdqa(J, X(5), loc(s, lo->t_uvmm0));
			gsj_movdqa(J, X(6), loc(s, lo->t_uvmm1));
			if (s->j.avx)
			{
				gsj_vpunpcklwd(J, X(0), X(5), X(6));
				gsj_vpunpckhwd(J, X(1), X(5), X(6));
				gsj_vpunpckldq(J, X(5), X(0), X(1));
				gsj_vpunpckhdq(J, X(6), X(0), X(1));
			}
			else
			{
				gsj_movdqa(J, X(0), X(5));
				gsj_punpcklwd(J, X(5), X(6));
				gsj_punpckhwd(J, X(0), X(6));
				gsj_movdqa(J, X(6), X(5));
				gsj_punpckldq(J, X(5), X(0));
				gsj_punpckhdq(J, X(6), X(0));
			}
			gsj_movdqa(J, loc(s, lo->t_uvmm0), X(5));
			gsj_movdqa(J, loc(s, lo->t_uvmm1), X(6));
		}
	}
	else
	{
		/* lod = K */
		gsj_movd(J, X(0), glb(s, lo->g_lod_i));
		gsj_psrad(J, V(s, 2), X(0));
		gsj_psrad(J, V(s, 3), X(0));
		gsj_movdqa(J, loc(s, lo->t_uv0), V(s, 2));
		gsj_movdqa(J, loc(s, lo->t_uv1), V(s, 3));
		gsj_movdqa(J, V(s, 5), loc(s, lo->t_uvmm0));
		gsj_movdqa(J, V(s, 6), loc(s, lo->t_uvmm1));
	}

	if (s->ltf)
		bilinear_fractions(s);
	lod_uv(s);
	texel_read(s, 0);

	if (s->mmin != 1) /* the next level, and the blend between them */
	{
		gsj_movdqa(J, loc(s, lo->t_trb), V(s, 5));
		gsj_movdqa(J, loc(s, lo->t_tga), V(s, 6));
		gsj_movdqa(J, V(s, 2), loc(s, lo->t_uv0));
		gsj_movdqa(J, V(s, 3), loc(s, lo->t_uv1));
		gsj_psrad_i(J, V(s, 2), 1);
		gsj_psrad_i(J, V(s, 3), 1);
		gsj_movdqa(J, V(s, 5), loc(s, lo->t_uvmm0));
		gsj_movdqa(J, V(s, 6), loc(s, lo->t_uvmm1));
		gsj_psrlw_i(J, V(s, 5), 1);
		gsj_psrlw_i(J, V(s, 6), 1);
		if (s->ltf)
			bilinear_fractions(s);
		lod_uv(s);
		texel_read(s, 1);

		gsj_movdqa(J, V(s, 0), s->lcm ? glb(s, lo->g_lod_f) : loc(s, lo->t_lod_f));
		gsj_psrlw_i(J, V(s, 0), 1);
		gsj_movdqa(J, V(s, 2), loc(s, lo->t_trb));
		gsj_movdqa(J, V(s, 3), loc(s, lo->t_tga));
		lerp16(s, V(s, 5), V(s, 2), V(s, 0), 0);
		lerp16(s, V(s, 6), V(s, 3), V(s, 0), 0);
	}
}

/* --- colour ------------------------------------------------------------- */

static void alpha_tfx(swd* s)
{
	const gsj_op ga = V(s, R_GA), f_ga = V(s, R_F_GA), tmpga = V(s, 1);
	if (!s->fb)
		return;

	switch (s->tfx)
	{
		case GS_SWJ_TFX_MODULATE:
			/* gat = gat.modulate16<1>(ga).clamp8(); if (!tcc) mix16(ga >> 7) */
			modulate16(s, ga, f_ga, 1);
			clamp16(s, ga, V(s, 0));
			if (!s->tcc)
			{
				THREE_I(psrlw, tmpga, f_ga, 7);
				mix16(s, ga, tmpga);
			}
			break;
		case GS_SWJ_TFX_DECAL:
		case GS_SWJ_TFX_HIGHLIGHT2:
			if (!s->tcc)
			{
				THREE_I(psrlw, tmpga, f_ga, 7);
				mix16(s, ga, tmpga);
			}
			break;
		case GS_SWJ_TFX_HIGHLIGHT:
			THREE_I(psrlw, tmpga, f_ga, 7);
			if (s->tcc)
				gsj_paddusb(J, tmpga, ga);
			mix16(s, ga, tmpga);
			break;
		case GS_SWJ_TFX_NONE:
			if (s->iip)
				THREE_I(psrlw, ga, f_ga, 7);
			break;
	}

	if (s->aa1)
	{
		/* anti-aliasing after tfx, before the tests: alpha = cov */
		if (!s->abe)
		{
			if (s->edge)
				gsj_movdqa(J, V(s, 0), loc(s, s->lo->t_cov));
			else
			{
				gsj_pcmpeqd(J, V(s, 0), V(s, 0));
				gsj_psllw_i(J, V(s, 0), 15);
				gsj_psrlw_i(J, V(s, 0), 8);
			}
			mix16(s, ga, V(s, 0));
		}
		else
		{
			/* a = a == 0x80 ? cov : a */
			gsj_pcmpeqd(J, V(s, 0), V(s, 0));
			gsj_psllw_i(J, V(s, 0), 15);
			gsj_psrlw_i(J, V(s, 0), 8);
			if (s->edge)
				gsj_movdqa(J, V(s, 1), loc(s, s->lo->t_cov));
			else
				gsj_movdqa(J, V(s, 1), V(s, 0));
			gsj_pcmpeqw(J, V(s, 0), ga);
			gsj_psrld_i(J, V(s, 0), 16);
			gsj_pslld_i(J, V(s, 0), 16);
			gsj_pblendvb(J, ga, V(s, 1));
		}
	}
}

static void read_mask(swd* s)
{
	if (s->fwrite)
		pbroadcastd_local(s, V(s, R_FM), glb(s, s->lo->g_fm));
	if (s->zwrite)
		pbroadcastd_local(s, V(s, R_ZM), glb(s, s->lo->g_zm));
}

static void test_alpha(swd* s)
{
	const gsj_op ga = V(s, R_GA), aref = glb(s, GS_SLG_AREF);
	switch (s->atst)
	{
		case GS_SWJ_ATST_NEVER:
			gsj_pcmpeqd(J, V(s, 1), V(s, 1));
			break;
		case GS_SWJ_ATST_ALWAYS:
			return;
		case GS_SWJ_ATST_LESS:
		case GS_SWJ_ATST_LEQUAL:
			/* t = (ga >> 16) > aref */
			THREE_I(psrld, V(s, 1), ga, 16);
			bcast_op(s, gsj_vbroadcasti128, gsj_pcmpgtd, V(s, 1), V(s, 0), aref);
			break;
		case GS_SWJ_ATST_EQUAL:
			/* t = (ga >> 16) != aref */
			THREE_I(psrld, V(s, 1), ga, 16);
			bcast_op(s, gsj_vbroadcasti128, gsj_pcmpeqd, V(s, 1), V(s, 0), aref);
			gsj_pcmpeqd(J, V(s, 0), V(s, 0));
			gsj_pxor(J, V(s, 1), V(s, 0));
			break;
		case GS_SWJ_ATST_GEQUAL:
		case GS_SWJ_ATST_GREATER:
			/* t = (ga >> 16) < aref */
			THREE_I(psrld, V(s, 0), ga, 16);
			broadcasti128(s, V(s, 1), aref);
			gsj_pcmpgtd(J, V(s, 1), V(s, 0));
			break;
		case GS_SWJ_ATST_NOTEQUAL:
			/* t = (ga >> 16) == aref */
			THREE_I(psrld, V(s, 1), ga, 16);
			bcast_op(s, gsj_vbroadcasti128, gsj_pcmpeqd, V(s, 1), V(s, 0), aref);
			break;
	}

	switch (s->afail)
	{
		case GS_SWJ_AFAIL_KEEP:
			gsj_por(J, V(s, R_TEST), V(s, 1));
			alltrue(s, V(s, R_TEST));
			break;
		case GS_SWJ_AFAIL_FB_ONLY:
			gsj_por(J, V(s, R_ZM), V(s, 1));
			break;
		case GS_SWJ_AFAIL_ZB_ONLY:
			gsj_por(J, V(s, R_FM), V(s, 1));
			break;
		case GS_SWJ_AFAIL_RGB_ONLY:
			gsj_por(J, V(s, R_ZM), V(s, 1));
			gsj_psrld_i(J, V(s, 1), 24);
			gsj_pslld_i(J, V(s, 1), 24);
			gsj_por(J, V(s, R_FM), V(s, 1));
			break;
	}
}

static void color_tfx(swd* s)
{
	const gsj_op rb = V(s, R_RB), ga = V(s, R_GA), f_ga = V(s, R_F_GA), tmpga = V(s, 2);
	if (!s->fwrite)
		return;

	switch (s->tfx)
	{
		case GS_SWJ_TFX_MODULATE:
			modulate16(s, rb, V(s, R_F_RB), 1);
			clamp16(s, rb, V(s, 0));
			break;
		case GS_SWJ_TFX_HIGHLIGHT:
		case GS_SWJ_TFX_HIGHLIGHT2:
			/* gat = gat.modulate16<1>(ga).add16(af).clamp8().mix16(gat);
			 * rbt = rbt.modulate16<1>(rb).add16(af).clamp8() */
			gsj_movdqa(J, V(s, 1), ga);
			modulate16(s, ga, f_ga, 1);
			gsj_pshuflw(J, tmpga, f_ga, SHUF(3, 3, 1, 1));
			gsj_pshufhw(J, tmpga, tmpga, SHUF(3, 3, 1, 1));
			gsj_psrlw_i(J, tmpga, 7);
			gsj_paddw(J, ga, tmpga);
			clamp16(s, ga, V(s, 0));
			mix16(s, ga, V(s, 1));
			modulate16(s, rb, V(s, R_F_RB), 1);
			gsj_paddw(J, rb, tmpga);
			clamp16(s, rb, V(s, 0));
			break;
		case GS_SWJ_TFX_NONE:
			if (s->iip)
				THREE_I(psrlw, rb, V(s, R_F_RB), 7);
			break;
	}
}

static void fog(swd* s)
{
	if (!s->fwrite || !s->fge)
		return;
	/* rb = gd->frb.lerp16<0>(rb, f); ga = gd->fga.lerp16<0>(ga, f).mix16(ga) */
	gsj_movdqa(J, V(s, 1), V(s, R_GA));
	pbroadcastd_local(s, V(s, 0), glb(s, s->lo->g_frb));
	lerp16(s, V(s, R_RB), V(s, 0), V(s, R_F), 0);
	pbroadcastd_local(s, V(s, 0), glb(s, s->lo->g_fga));
	lerp16(s, V(s, R_GA), V(s, 0), V(s, R_F), 0);
	mix16(s, V(s, R_GA), V(s, 1));
}

static void read_frame(swd* s)
{
	if (!s->fb)
		return;
	/* rbx = fa = fza_base.x + fza_offset->x */
	gsj_mov(J, D(GSJ_RBX), gsj_mem(32, s->t1, -1, 1, 0));
	gsj_add(J, D(GSJ_RBX), gsj_mem(32, s->t0, -1, 1, 0));
	gsj_and_i(J, D(GSJ_RBX), (s->hires ? 4 * GS_SWJ_HALF_VM_SIZE : GS_SWJ_HALF_VM_SIZE) - 1);
	if (!s->rfb)
		return;
	read_pixel(s, V(s, R_FD), V(s, 0), GSJ_RBX);
}

static void test_dest_alpha(swd* s)
{
	const gsj_op fd = V(s, R_FD);
	if (!s->date || (s->fpsm != 0 && s->fpsm != 2))
		return;

	/* test |= ((fd [<< 16]) ^ gd->datm).sra32<31>() */
	if (s->datm)
	{
		if (s->fpsm == 2)
		{
			gsj_pxor(J, V(s, 0), V(s, 0));
			THREE_I(pslld, V(s, 1), fd, 16);
			gsj_psrad_i(J, V(s, 1), 31);
			gsj_pcmpeqd(J, V(s, 1), V(s, 0));
		}
		else
		{
			gsj_pcmpeqd(J, V(s, 0), V(s, 0));
			THREE(pxor, V(s, 1), fd, V(s, 0));
			gsj_psrad_i(J, V(s, 1), 31);
		}
	}
	else if (s->fpsm == 2)
	{
		THREE_I(pslld, V(s, 1), fd, 16);
		gsj_psrad_i(J, V(s, 1), 31);
	}
	else
		THREE_I(psrad, V(s, 1), fd, 31);

	gsj_por(J, V(s, R_TEST), V(s, 1));
	alltrue(s, V(s, R_TEST));
}

/* edx = fzm = ~(fm == ~0).ps32(zm == ~0).mask() */
static void write_mask(swd* s)
{
	if (s->notest)
		return;
	if (s->fwrite)
		gsj_por(J, V(s, R_FM), V(s, R_TEST));
	if (s->zwrite)
		gsj_por(J, V(s, R_ZM), V(s, R_TEST));
	gsj_pcmpeqd(J, V(s, 1), V(s, 1));
	if (s->fwrite && s->zwrite)
	{
		THREE(pcmpeqd, V(s, 0), V(s, 1), V(s, R_ZM));
		gsj_pcmpeqd(J, V(s, 1), V(s, R_FM));
		gsj_packssdw(J, V(s, 1), V(s, 0));
	}
	else if (s->fwrite)
	{
		gsj_pcmpeqd(J, V(s, 1), V(s, R_FM));
		gsj_packssdw(J, V(s, 1), V(s, 1));
	}
	else if (s->zwrite)
	{
		gsj_pcmpeqd(J, V(s, 1), V(s, R_ZM));
		gsj_packssdw(J, V(s, 1), V(s, 1));
	}
	gsj_pmovmskb(J, D(GSJ_RDX), V(s, 1));
	gsj_not(J, D(GSJ_RDX));
}

/* One pixel i of src's lane j to vm16[addr], by psm */
static void write_pixel_one(swd* s, int src, int addr, unsigned i, unsigned j, unsigned psm)
{
	static const int offsets[8] = {0, 2, 8, 10, 16, 18, 24, 26};
	const gsj_op dst = vm_at(s, addr, offsets[i] * 2, 0);
	switch (psm)
	{
		case 0:
			if (j == 0)
				gsj_movd(J, dst, X(src));
			else
				gsj_pextrd(J, dst, X(src), j);
			break;
		case 1:
			if (j == 0)
				gsj_movd(J, D(GSJ_RAX), X(src));
			else
				gsj_pextrd(J, D(GSJ_RAX), X(src), j);
			gsj_xor(J, D(GSJ_RAX), dst);
			gsj_and_i(J, D(GSJ_RAX), 0xffffff);
			gsj_xor(J, dst, D(GSJ_RAX));
			break;
		case 2:
			if (j == 0)
				gsj_movd(J, D(GSJ_RAX), X(src));
			else
				gsj_pextrw(J, D(GSJ_RAX), X(src), j * 2);
			gsj_mov(J, dst, gsj_r16(GSJ_RAX));
			break;
	}
}

/* src to vm16[addr] where fzm (dl or dh, or edx for ymm) says, fz the
 * z half */
static void write_pixel(swd* s, int src, int addr, int fz, int fast, unsigned psm)
{
	const gsj_op mask  = s->ymm ? D(GSJ_RDX) : gsj_r8(fz ? 6 : 2); /* edx, dh, dl */
	const unsigned sh  = s->ymm ? fz * 8 : 0;
	unsigned i;

	if (s->notest)
	{
		if (fast)
		{
			gsj_movq(J, vm_at(s, addr, 0, 64), X(src));
			gsj_movhps(J, vm_at(s, addr, 16, 64), X(src));
			if (s->ymm)
			{
				gsj_vextracti128(J, X(src), gsj_y(src), 1);
				gsj_movq(J, vm_at(s, addr, 32, 64), X(src));
				gsj_movhps(J, vm_at(s, addr, 48, 64), X(src));
			}
		}
		else
		{
			for (i = 0; i < 4; i++)
				write_pixel_one(s, src, addr, i, i, psm);
			if (s->ymm)
			{
				gsj_vextracti128(J, X(src), gsj_y(src), 1);
				for (i = 4; i < 8; i++)
					write_pixel_one(s, src, addr, i, i - 4, psm);
			}
		}
		return;
	}

	if (fast)
	{
		/* if (fzm & 0x0f) storel(&vm16[addr + 0]); ... */
		const unsigned n = s->ymm ? 4 : 2;
		for (i = 0; i < n; i++)
		{
			if (i == 2)
				gsj_vextracti128(J, X(src), gsj_y(src), 1);
			gsj_test_i(J, mask, (0x0fu << (4 * (i & 1) + 16 * (i >> 1))) << sh);
			gsj_je(J, GSJ_ANON, 0);
			if (i & 1)
				gsj_movhps(J, vm_at(s, addr, 16 * i, 64), X(src));
			else
				gsj_movq(J, vm_at(s, addr, 16 * i, 64), X(src));
			gsj_bind_anon(J);
		}
	}
	else
	{
		const unsigned n = s->ymm ? 8 : 4;
		for (i = 0; i < n; i++)
		{
			if (i == 4)
				gsj_vextracti128(J, X(src), gsj_y(src), 1);
			gsj_test_i(J, mask, (0x03u << (2 * (i & 3) + 16 * (i >> 2))) << sh);
			gsj_je(J, GSJ_ANON, 0);
			write_pixel_one(s, src, addr, i, i & 3, psm);
			gsj_bind_anon(J);
		}
	}
}

static void write_zbuf(swd* s)
{
	int fast;
	if (!s->zwrite)
		return;
	if (s->prim != GS_SWJ_SPRITE_CLASS)
		gsj_movdqa(J, V(s, 1), loc(s, s->lo->t_zs));
	else
		pbroadcastd_local(s, V(s, 1), loc(s, s->lo->p_z));

	if (s->ztest && s->zpsm < 2)
	{
		/* zs = zs.blend8(zd, zm) */
		if (s->j.avx)
			gsj_vpblendvb(J, V(s, 1), V(s, 1), loc(s, s->lo->t_zd), V(s, R_ZM));
		else
		{
			gsj_movdqa(J, V(s, 0), V(s, R_ZM));
			gsj_movdqa(J, V(s, 7), loc(s, s->lo->t_zd));
			gsj_pblendvb(J, V(s, 1), V(s, 7));
		}
	}

	fast = s->ztest ? s->zpsm < 2 : s->zpsm == 0 && s->notest;
	write_pixel(s, 1, s->t2, 1, fast, s->zpsm);
}

static void alpha_blend(swd* s)
{
	const gsj_op rb = V(s, R_RB), ga = V(s, R_GA), fd = V(s, R_FD);
	const gsj_op dst_rb = V(s, 0), dst_ga = V(s, 1), tmp1 = V(s, R_TEST), tmp2 = V(s, 4);
	if (!s->fwrite)
		return;
	if (s->abe == 0 && s->aa1 == 0)
		return;

	if (((s->aba != s->abb) && (s->aba == 1 || s->abb == 1 || s->abc == 1)) || s->abd == 1)
	{
		switch (s->fpsm)
		{
			case 0:
			case 1:
				/* c[2] = fd & mask; c[3] = (fd >> 8) & mask */
				split16_2x8(s, dst_rb, dst_ga, fd);
				break;
			case 2:
				/* c[2] = ((fd & 0x7c00) << 9) | ((fd & 0x001f) << 3);
				 * c[3] = ((fd & 0x8000) << 8) | ((fd & 0x03e0) >> 2) */
				gsj_pcmpeqd(J, tmp1, tmp1);
				gsj_psrld_i(J, tmp1, 27);
				THREE(pand, dst_rb, fd, tmp1);
				gsj_pslld_i(J, dst_rb, 3);
				gsj_pslld_i(J, tmp1, 10);
				THREE(pand, tmp2, fd, tmp1);
				gsj_pslld_i(J, tmp2, 9);
				gsj_por(J, dst_rb, tmp2);
				gsj_psrld_i(J, tmp1, 5);
				THREE(pand, dst_ga, fd, tmp1);
				gsj_psrld_i(J, dst_ga, 2);
				gsj_psllw_i(J, tmp1, 10);
				THREE(pand, tmp2, fd, tmp1);
				gsj_pslld_i(J, tmp2, 8);
				gsj_por(J, dst_ga, tmp2);
				break;
		}
	}

	if (s->pabe || ((s->aba != s->abb) && (s->abb == 0 || s->abd == 0)))
		gsj_movdqa(J, tmp2, rb);

	if (s->aba != s->abb)
	{
		/* rb = c[aba * 2] - c[abb * 2], times a, plus c[abd * 2] */
		if (s->aba == 1)
			gsj_movdqa(J, rb, dst_rb);
		else if (s->aba == 2)
			gsj_pxor(J, rb, rb);
		if (s->abb == 0)
			gsj_psubw(J, rb, tmp2);
		else if (s->abb == 1)
			gsj_psubw(J, rb, dst_rb);

		if (!(s->fpsm == 1 && s->abc == 1))
		{
			/* a = abc < 2 ? c[abc * 2 + 1].yywwlh().sll16<7>() : gd->afix */
			if (s->abc < 2)
			{
				gsj_pshuflw(J, tmp1, s->abc ? dst_ga : ga, SHUF(3, 3, 1, 1));
				gsj_pshufhw(J, tmp1, tmp1, SHUF(3, 3, 1, 1));
				gsj_psllw_i(J, tmp1, 7);
			}
			else if (s->abc == 2)
				pbroadcastw_local(s, tmp1, glb(s, GS_SLG_AFIX));
			modulate16(s, rb, tmp1, 1);
		}

		if (s->abd == 0)
			gsj_paddw(J, rb, tmp2);
		else if (s->abd == 1)
			gsj_paddw(J, rb, dst_rb);
	}
	else if (s->abd == 1)
		gsj_movdqa(J, rb, dst_rb);
	else if (s->abd == 2)
		gsj_pxor(J, rb, rb);

	if (s->pabe)
	{
		/* mask = (c[1] << 8).sra32<31>(); rb = c[0].blend8(rb, mask) */
		THREE_I(pslld, V(s, 0), ga, 8);
		gsj_psrad_i(J, V(s, 0), 31);
		blend8r(s, rb, tmp2);
	}

	gsj_movdqa(J, tmp2, ga);

	if (s->aba != s->abb)
	{
		if (s->aba == 1)
			gsj_movdqa(J, ga, dst_ga);
		else if (s->aba == 2)
			gsj_pxor(J, ga, ga);
		if (s->abb == 0)
			gsj_psubw(J, ga, tmp2);
		else if (s->abb == 1)
			gsj_psubw(J, ga, dst_ga);
		if (!(s->fpsm == 1 && s->abc == 1))
			modulate16(s, ga, tmp1, 1);
		if (s->abd == 0)
			gsj_paddw(J, ga, tmp2);
		else if (s->abd == 1)
			gsj_paddw(J, ga, dst_ga);
	}
	else if (s->abd == 1)
		gsj_movdqa(J, ga, dst_ga);
	else if (s->abd == 2)
		gsj_pxor(J, ga, ga);

	if (s->pabe)
	{
		/* the source alpha kept: ga = c[1].blend8(ga, mask).mix16(c[1]) */
		gsj_psrld_i(J, V(s, 0), 16);
		blend8r(s, ga, tmp2);
	}
	else if (s->fpsm != 1)
		mix16(s, ga, tmp2);
}

static void write_frame(swd* s)
{
	const gsj_op tmp = V(s, 15);
	int fast;
	if (!s->fwrite)
		return;

	if (s->fpsm == 2 && s->dthe)
	{
		/* rb += dimx[(top & 3) * 2]; ga += dimx[(top & 3) * 2 + 1] */
		gsj_mov(J, D(GSJ_RAX), gsj_ptr(GSJ_RSP, s->top));
		gsj_and_i(J, D(GSJ_RAX), 3);
		gsj_shl(J, D(GSJ_RAX), 5);
		gsj_add(J, Q(GSJ_RAX), glb(s, GS_SLG_DIMX));
		bcast_op(s, gsj_vbroadcasti128, gsj_paddw, V(s, 5), tmp, gsj_ptr(GSJ_RAX, 0));
		bcast_op(s, gsj_vbroadcasti128, gsj_paddw, V(s, 6), tmp, gsj_ptr(GSJ_RAX, 16));
	}

	if (s->colclamp == 0)
	{
		/* c[0] &= 0x00ff00ff; c[1] &= 0x00ff00ff */
		gsj_pcmpeqd(J, tmp, tmp);
		gsj_psrlw_i(J, tmp, 8);
		gsj_pand(J, V(s, 5), tmp);
		gsj_pand(J, V(s, 6), tmp);
	}

	/* fs = c[0].upl16(c[1]).pu16(c[0].uph16(c[1])) */
	THREE(punpckhwd, tmp, V(s, 5), V(s, 6));
	gsj_punpcklwd(J, V(s, 5), V(s, 6));
	gsj_packuswb(J, V(s, 5), tmp);

	if (s->fba && s->fpsm != 1)
	{
		gsj_pcmpeqd(J, tmp, tmp);
		gsj_pslld_i(J, tmp, 31);
		gsj_por(J, V(s, 5), tmp);
	}

	if (s->fpsm == 2)
	{
		/* rb = fs & 0x00f800f8; ga = fs & 0x8000f800;
		 * fs = (ga >> 16) | (rb >> 9) | (ga >> 6) | (rb >> 3) */
		gsj_mov_i(J, D(GSJ_RAX), 0x00f800f8);
		broadcast_eax(s, 0);
		gsj_mov_i(J, D(GSJ_RAX), 0x8000f800);
		broadcast_eax(s, 1);
		gsj_pand(J, V(s, 0), V(s, 5));
		gsj_pand(J, V(s, 1), V(s, 5));
		THREE_I(psrld, V(s, 5), V(s, 0), 9);
		gsj_psrld_i(J, V(s, 0), 3);
		THREE_I(psrld, V(s, 6), V(s, 1), 16);
		gsj_psrld_i(J, V(s, 1), 6);
		gsj_por(J, V(s, 0), V(s, 1));
		gsj_por(J, V(s, 5), V(s, 6));
		gsj_por(J, V(s, 5), V(s, 0));
	}

	if (s->rfb)
		blend(s, V(s, 5), V(s, R_FD), V(s, R_FM));

	fast = s->rfb ? s->fpsm < 2 : s->fpsm == 0 && s->notest;
	write_pixel(s, 5, GSJ_RBX, 0, fast, s->fpsm);
}

/* --- the function ------------------------------------------------------- */

size_t gs_swj_draw_scanline(const gs_swj_target* t, void* code, size_t room)
{
	swd sd;
	swd* const s = &sd;
	const u64 k = t->key;
	int i, tme;

	gsj_init(J, code, room, t->avx, t->avx2);
	s->ymm      = t->ymm;
	s->win64    = t->win64;
	s->lo       = t->ymm ? &swd_layout8 : &swd_layout4;
	s->consts   = (const u8*)t->consts;
	s->zbias    = t->zbias;
	s->vecsize  = t->ymm ? 32 : 16;
	s->vecints  = s->vecsize / 4;

	s->fpsm = GS_SEL_FPSM(k); s->zpsm = GS_SEL_ZPSM(k); s->ztst = GS_SEL_ZTST(k); s->atst = GS_SEL_ATST(k);
	s->afail = GS_SEL_AFAIL(k); s->iip = GS_SEL_IIP(k); s->tfx = GS_SEL_TFX(k); s->tcc = GS_SEL_TCC(k);
	s->fst = GS_SEL_FST(k); s->ltf = GS_SEL_LTF(k); s->tlu = GS_SEL_TLU(k); s->fge = GS_SEL_FGE(k);
	s->date = GS_SEL_DATE(k); s->abe = GS_SEL_ABE(k); s->aba = GS_SEL_ABA(k); s->abb = GS_SEL_ABB(k);
	s->abc = GS_SEL_ABC(k); s->abd = GS_SEL_ABD(k); s->pabe = GS_SEL_PABE(k); s->aa1 = GS_SEL_AA1(k);
	s->fwrite = GS_SEL_FWRITE(k); s->rfb = GS_SEL_RFB(k); s->zwrite = GS_SEL_ZWRITE(k); s->ztest = GS_SEL_ZTEST(k);
	s->zoverflow = GS_SEL_ZOVERFLOW(k); s->zclamp = GS_SEL_ZCLAMP(k); s->wms = GS_SEL_WMS(k); s->wmt = GS_SEL_WMT(k);
	s->datm = GS_SEL_DATM(k); s->colclamp = GS_SEL_COLCLAMP(k); s->fba = GS_SEL_FBA(k); s->dthe = GS_SEL_DTHE(k);
	s->prim = GS_SEL_PRIM(k); s->edge = GS_SEL_EDGE(k); s->tw = GS_SEL_TW(k); s->lcm = GS_SEL_LCM(k);
	s->mmin = GS_SEL_MMIN(k); s->notest = GS_SEL_NOTEST(k); s->zequal = GS_SEL_ZEQUAL(k); s->hires = GS_SEL_HIRES(k);
	s->tw_hi = GS_SEL_TW_HI(k); s->fb = GS_SEL_FB(k); s->zb = GS_SEL_ZB(k);
	s->use_lod = s->mmin != 0;

	if (s->win64)
	{
		s->a0 = GSJ_RCX; s->a1 = GSJ_RDX; s->a2 = GSJ_R8N; s->a3 = GSJ_R9;
		s->t0 = GSJ_RDI; s->t1 = GSJ_RSI; s->t2 = GSJ_R8N; s->t3 = GSJ_R9;
		s->local = GSJ_R10;
		s->top = 0;
	}
	else
	{
		s->a0 = GSJ_RDI; s->a1 = GSJ_RSI; s->a2 = GSJ_RDX; s->a3 = GSJ_RCX;
		s->t0 = GSJ_R10; s->t1 = GSJ_R9; s->t2 = GSJ_RCX; s->t3 = GSJ_RSI;
		s->local = GSJ_R8N;
		s->top = -48; /* below the red zone's saved registers */
	}
	s->gd   = GSJ_R12;
	s->vm   = s->t3;
	s->clut = GSJ_R11;
	s->tex  = GSJ_R13;

	/* the prologue: Win64 saves rbx, rsi, rdi, r12, r13 and xmm6-15 and
	 * reads local (the fifth argument) from the stack; SysV keeps rbx, r12
	 * and r13 in the red zone */
	if (s->win64)
	{
		gsj_push(J, Q(GSJ_RBX));
		gsj_push(J, Q(GSJ_RSI));
		gsj_push(J, Q(GSJ_RDI));
		gsj_push(J, Q(GSJ_R12));
		gsj_push(J, Q(GSJ_R13));
		gsj_sub_i(J, Q(GSJ_RSP), 16 + 16 * 10);
		for (i = 0; i < 10; i++)
			gsj_movdqa(J, gsj_ptr(GSJ_RSP, 16 + 16 * i), X(i + 6));
		/* (32 bytes of shadow space and the 5 pushes and the return) */
		gsj_mov(J, Q(s->local), gsj_ptr(GSJ_RSP, 16 + 16 * 10 + 80));
	}
	else
	{
		gsj_mov(J, gsj_ptr(GSJ_RSP, -8), Q(GSJ_RBX));
		gsj_mov(J, gsj_ptr(GSJ_RSP, -16), Q(GSJ_R12));
		gsj_mov(J, gsj_ptr(GSJ_RSP, -24), Q(GSJ_R13));
	}

	gsj_mov(J, Q(s->gd), loc(s, s->lo->gd));
	if (s->fb && s->tfx != GS_SWJ_TFX_NONE && s->tlu)
		gsj_mov(J, Q(s->clut), glb(s, GS_SLG_CLUT));

	init(s);

	if (!s->edge)
		gsj_align(J, 16);

	gsj_bind(J, L_LOOP);

	tme = s->tfx != GS_SWJ_TFX_NONE;
	test_z(s, V(s, tme ? 5 : 2), V(s, tme ? 6 : 3));
	if (s->use_lod)
		sample_texture_lod(s);
	else
		sample_texture(s);
	alpha_tfx(s);
	read_mask(s);
	test_alpha(s);
	color_tfx(s);
	fog(s);
	read_frame(s);
	test_dest_alpha(s);
	write_mask(s);
	write_zbuf(s);
	alpha_blend(s);
	write_frame(s);

	gsj_bind(J, L_STEP);

	/* if (steps <= 0) break */
	if (!s->edge)
	{
		gsj_test(J, D(s->a0), D(s->a0));
		gsj_jle(J, L_EXIT, 1);
		step(s);
		gsj_jmp(J, L_LOOP, 1);
	}

	gsj_bind(J, L_EXIT);

	if (s->win64)
	{
		for (i = 0; i < 10; i++)
			gsj_movdqa(J, X(i + 6), gsj_ptr(GSJ_RSP, 16 + 16 * i));
		gsj_add_i(J, Q(GSJ_RSP), 16 + 16 * 10);
		gsj_pop(J, Q(GSJ_R13));
		gsj_pop(J, Q(GSJ_R12));
		gsj_pop(J, Q(GSJ_RDI));
		gsj_pop(J, Q(GSJ_RSI));
		gsj_pop(J, Q(GSJ_RBX));
	}
	else
	{
		gsj_mov(J, Q(GSJ_RBX), gsj_ptr(GSJ_RSP, -8));
		gsj_mov(J, Q(GSJ_R12), gsj_ptr(GSJ_RSP, -16));
		gsj_mov(J, Q(GSJ_R13), gsj_ptr(GSJ_RSP, -24));
	}
	if (s->ymm)
		gsj_vzeroupper(J);
	gsj_ret(J);

	return gsj_size(J);
}
