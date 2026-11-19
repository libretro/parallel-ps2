/* The software renderer's C emitter (GS/Renderers/SW/GSJitX86.c): every
 * instruction the generators use, for SSE4.1, AVX and AVX2, over random
 * registers and every addressing form (no base, rsp/r12 and rbp/r13 bases,
 * an index, scales, displacements of none, eight and 32 bits), then the
 * labels and jumps, the alignment and RIP-relative operands, hashed
 * against EMIT in build.sh: the bytes xbyak made for the same operands
 * when it was the oracle.
 *
 * Every draw from the generator is its own statement: the sequence, and
 * with it the hash, does not depend on a compiler's order of evaluation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GS/Renderers/SW/GSJitX86.h"

static unsigned long long s_rng = 0x9E3779B97F4A7C15ull;
static unsigned rnd(unsigned n)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 7;
	s_rng ^= s_rng << 17;
	return (unsigned)(s_rng % n);
}

static unsigned long long s_hash = 1469598103934665603ull;
static void hash_bytes(const u8* p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		s_hash = (s_hash ^ p[i]) * 1099511628211ull;
}

static int s_checked;

/* The expected hash, from hex (C89 has no strtoull) */
static unsigned long long hex64(const char* s)
{
	unsigned long long v = 0;
	for (; *s; s++)
		v = v * 16 + (unsigned)(*s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10);
	return v;
}

/* --- random operands ------------------------------------------------- */

static s32 rdisp(void)
{
	static const s32 d[] = {0, 0, 0, 4, -4, 127, -128, 128, -129, 0x40, 0x1000, -0x1000, 0x7FFFFFF0, (s32)0x80000000u};
	return d[rnd(sizeof(d) / sizeof(d[0]))];
}

static gsj_op rmem(unsigned bits)
{
	int base  = rnd(6) == 0 ? -1 : (int)rnd(16);
	int index = rnd(3) == 0 ? (int)rnd(16) : -1;
	s32 disp;
	unsigned scale;
	if (index == 4)
		index = -1;
	if (base < 0 && index < 0 && rnd(2))
		base = (int)rnd(16);
	/* (the order GCC drew them in as arguments) */
	disp  = rdisp();
	scale = 1u << rnd(4);
	return gsj_mem(bits, base, index, scale, disp);
}

static int gather_index(int a, int b)
{
	int i;
	do
		i = (int)rnd(16);
	while (i == a || i == b);
	return i;
}

static gsj_op rvec(int ymm) { return ymm ? gsj_y((int)rnd(16)) : gsj_x((int)rnd(16)); }
static gsj_op rvec_or_mem(int ymm, unsigned bits) { return rnd(3) ? rvec(ymm) : rmem(bits); }

/* (64 bytes aligned: the alignment tests are of the address) */
static union { u8 b[4096]; double align_[8]; } s_cbuf_u;
static u8* s_cbuf;

/* One instruction (or sequence) emitted alone, and hashed */
#define ONE(isa_, ccall) \
	do \
	{ \
		gsj jj_; \
		gsj* const j = &jj_; \
		gsj_init(j, s_cbuf, 4096, (isa_) >= 1, (isa_) >= 2); \
		ccall; \
		hash_bytes(s_cbuf, gsj_size(j)); \
		s_checked++; \
	} while (0)

int main(int argc, char** argv)
{
	const unsigned long long want = argc > 1 ? hex64(argv[1]) : 0;
	const int reps = 300;
	int isa, n;
	s_cbuf = (u8*)(((size_t)s_cbuf_u.b + 63) & ~(size_t)63);

	for (isa = 0; isa < 3; isa++)
	{
		const int Y = isa == 2;

#define A2(name) \
		for (n = 0; n < reps; n++) \
		{ \
			const int ymm = (Y && rnd(2)) ? 1 : 0; \
			gsj_op x = rvec(ymm), x2 = rvec(ymm), o = rvec_or_mem(ymm, 0); \
			ONE(isa, gsj_##name(j, x, o)); \
			if (isa) \
				ONE(isa, gsj_v##name(j, x, x2, o)); \
		}
#define A2S(name) /* scalar: xmm only */ \
		for (n = 0; n < reps; n++) \
		{ \
			gsj_op x = rvec(0), x2 = rvec(0), o = rvec_or_mem(0, 0); \
			ONE(isa, gsj_##name(j, x, o)); \
			if (isa) \
				ONE(isa, gsj_v##name(j, x, x2, o)); \
		}
#define AI(name, yok) \
		for (n = 0; n < reps; n++) \
		{ \
			const int ymm = (Y && yok && rnd(2)) ? 1 : 0; \
			const unsigned imm = rnd(256); \
			gsj_op x = rvec(ymm), x2 = rvec(ymm), o = rvec_or_mem(ymm, 0); \
			ONE(isa, gsj_##name(j, x, o, imm)); \
			if (isa) \
				ONE(isa, gsj_v##name(j, x, x2, o, imm)); \
		}
#define S2(name, yok) \
		for (n = 0; n < reps; n++) \
		{ \
			const int ymm = (Y && yok && rnd(2)) ? 1 : 0; \
			gsj_op x = rvec(ymm), o = rvec_or_mem(ymm, 0); \
			ONE(isa, gsj_##name(j, x, o)); \
		}
#define SI(name, yok) \
		for (n = 0; n < reps; n++) \
		{ \
			const int ymm = (Y && yok && rnd(2)) ? 1 : 0; \
			const unsigned imm = rnd(256); \
			gsj_op x = rvec(ymm), o = rvec_or_mem(ymm, 0); \
			ONE(isa, gsj_##name(j, x, o, imm)); \
		}
#define SH(name) \
		for (n = 0; n < reps; n++) \
		{ \
			const int ymm = (Y && rnd(2)) ? 1 : 0; \
			const unsigned imm = rnd(256); \
			gsj_op x = rvec(ymm), x2 = rvec(ymm); \
			ONE(isa, gsj_##name##_i(j, x, imm)); \
			if (isa) \
				ONE(isa, gsj_v##name##_i(j, x, x2, imm)); \
		}

		A2(addps) A2(addpd) A2S(cvtsd2ss) A2S(cvtss2sd) A2(divps) A2(maxps) A2(minps) A2(mulpd) A2(mulps)
		A2S(mulsd) A2S(mulss) A2(orps) A2(packssdw) A2(packusdw) A2(packuswb) A2(paddd) A2(paddusb) A2(paddw)
		A2(pand) A2(pandn) A2(pcmpeqd) A2(pcmpeqw) A2(pcmpgtd) A2(pmaxsw) A2(pminsd) A2(pminud) A2(pminsw)
		A2(pmulhrsw) A2(pmulhw) A2(pmullw) A2(por) A2(psubd) A2(psubw) A2(punpckhdq) A2(punpckhwd)
		A2(punpcklbw) A2(punpckldq) A2(punpcklqdq) A2(punpcklwd) A2(pxor) A2(subps) A2(unpcklps) A2(unpcklpd)
		A2(xorps)
		/* (shift counts in an xmm or memory: the count is 128 bits) */
		for (n = 0; n < reps; n++)
		{
			const int ymm = (Y && rnd(2)) ? 1 : 0;
			gsj_op x = rvec(ymm), x2 = rvec(ymm), o = rnd(3) ? rvec(0) : rmem(0);
			ONE(isa, gsj_psrad(j, x, o));
			ONE(isa, gsj_psrlw(j, x, o));
			if (isa)
			{
				ONE(isa, gsj_vpsrad(j, x, x2, o));
				ONE(isa, gsj_vpsrlw(j, x, x2, o));
			}
		}
		AI(pblendw, 1) AI(roundsd, 0) AI(shufps, 1)
		for (n = 0; n < reps; n++)
		{
			const unsigned imm = rnd(4);
			gsj_op x = rvec(0), x2 = rvec(0), o = rnd(2) ? gsj_r32((int)rnd(16)) : rmem(32);
			ONE(isa, gsj_pinsrd(j, x, o, imm));
			if (isa)
				ONE(isa, gsj_vpinsrd(j, x, x2, o, imm));
		}
		S2(cvtdq2ps, 1) S2(cvtps2dq, 1) S2(cvttps2dq, 1) S2(movddup, 1) S2(rcpps, 1)
		for (n = 0; n < reps; n++)
		{
			const int ymm = (Y && rnd(2)) ? 1 : 0;
			gsj_op x = rvec(ymm), o = rnd(3) ? rvec(0) : rmem(ymm ? 128 : 64);
			ONE(isa, gsj_pmovsxbd(j, x, o));
			ONE(isa, gsj_pmovzxbw(j, x, o));
			ONE(isa, gsj_cvtps2pd(j, x, o));
		}
		for (n = 0; n < reps; n++)
		{
			/* (the source's length decides the AVX form's) */
			const int ymm = (Y && rnd(2)) ? 1 : 0;
			gsj_op x = rvec(0), o = rnd(3) ? rvec(ymm) : rmem(ymm ? 256 : 128);
			ONE(isa, gsj_cvtpd2dq(j, x, o));
			ONE(isa, gsj_cvtpd2ps(j, x, o));
			ONE(isa, gsj_cvttpd2dq(j, x, o));
		}
		SI(pshufd, 1) SI(pshufhw, 1) SI(pshuflw, 1) SI(roundps, 1)
		SH(pslld) SH(psllw) SH(psrad) SH(psraw) SH(psrld) SH(psrldq) SH(psrlw)

		for (n = 0; n < reps; n++)
		{
			const int ymm = (Y && rnd(2)) ? 1 : 0;
			gsj_op x = rvec(ymm), m = rmem(0), o = rvec_or_mem(ymm, 0);
			gsj_op xs = rvec(0), m32 = rmem(32), m64 = rmem(64), r32 = gsj_r32((int)rnd(16)), r64 = gsj_r64((int)rnd(16));
			ONE(isa, gsj_movaps(j, x, o));
			ONE(isa, gsj_movaps(j, m, x));
			ONE(isa, gsj_movdqa(j, x, o));
			ONE(isa, gsj_movdqa(j, m, x));
			ONE(isa, gsj_movd(j, xs, m32));
			ONE(isa, gsj_movd(j, m32, xs));
			ONE(isa, gsj_movd(j, xs, r32));
			ONE(isa, gsj_movd(j, r32, xs));
			ONE(isa, gsj_movq(j, xs, m64));
			ONE(isa, gsj_movq(j, m64, xs));
			ONE(isa, gsj_movsd(j, xs, m64));
			ONE(isa, gsj_movsd(j, m64, xs));
			ONE(isa, gsj_movss(j, xs, m32));
			ONE(isa, gsj_movss(j, m32, xs));
			ONE(isa, gsj_movhps(j, xs, m64));
			ONE(isa, gsj_movhps(j, m64, xs));
			ONE(isa, gsj_cvtsd2si(j, r64, (n & 1) ? xs : m64));
			ONE(isa, gsj_cvttsd2si(j, r64, (n & 1) ? xs : m64));
			ONE(isa, gsj_extractps(j, r32, xs, (unsigned)(n & 3)));
			ONE(isa, gsj_extractps(j, m32, xs, (unsigned)(n & 3)));
			ONE(isa, gsj_pextrd(j, r32, xs, (unsigned)(n & 3)));
			ONE(isa, gsj_pextrd(j, m32, xs, (unsigned)(n & 3)));
			ONE(isa, gsj_pextrw(j, r32, xs, (unsigned)(n & 7)));
			ONE(isa, gsj_pmovmskb(j, r32, x));
			ONE(isa, gsj_pblendvb(j, x, o));
			if (isa)
			{
				gsj_op x2 = rvec(ymm), x4 = rvec(ymm);
				ONE(isa, gsj_vpblendvb(j, x, x2, o, x4));
				ONE(isa, gsj_vbroadcastss(j, x, (n & 1) ? m32 : xs));
			}
		}

		/* AVX2 and AVX only */
		if (isa == 2)
			for (n = 0; n < reps; n++)
			{
				const unsigned imm = rnd(256);
				gsj_op y = rvec(1), y2 = rvec(1), x = rvec(0), xx = rvec(0), m = rmem(0), m128 = rmem(128);
				gsj_op o, xo, any, vs, vi;
				int vbase;
				unsigned vscale;
				s32 vdisp;
				while (y2.reg == y.reg)
					y2 = rvec(1);
				while (xx.reg == x.reg)
					xx = rvec(0);
				o   = rvec_or_mem(1, 0);
				xo  = rnd(2) ? rvec(0) : rmem(128);
				any = rvec_or_mem(rnd(2), 0);
				/* (destination, index and mask three registers) */
				vdisp  = rdisp();
				vscale = 1u << rnd(4);
				vi     = rnd(2) ? gsj_y(gather_index(y.reg, y2.reg)) : gsj_x(gather_index(x.reg, xx.reg));
				vbase  = rnd(8) == 0 ? GSJ_RBP : (int)rnd(16);
				vs = gsj_vsib(vbase, vi, vscale, vdisp);
				(void)m128;
				ONE(isa, gsj_vbroadcastsd(j, y, m));
				ONE(isa, gsj_vbroadcastsd(j, y, x));
				ONE(isa, gsj_vbroadcasti128(j, y, m));
				ONE(isa, gsj_vbroadcastf128(j, y, m));
				ONE(isa, gsj_vextracti128(j, xo, y, imm & 1));
				ONE(isa, gsj_vinsertf128(j, y, y2, xo, imm & 1));
				ONE(isa, gsj_vinserti128(j, y, y2, xo, imm & 1));
				ONE(isa, gsj_vpbroadcastd(j, any.kind == GSJ_MEM ? y : any, xo));
				ONE(isa, gsj_vpbroadcastq(j, y, xo));
				ONE(isa, gsj_vpbroadcastw(j, x, xo));
				ONE(isa, gsj_vpermq(j, y, o, imm));
				ONE(isa, gsj_vpgatherdd(j, vs.index_kind == GSJ_YMM ? y : x, vs, vs.index_kind == GSJ_YMM ? y2 : xx));
				ONE(isa, gsj_vpsravd(j, y, y2, o));
				ONE(isa, gsj_vpsrlvd(j, y, y2, o));
				ONE(isa, gsj_vzeroupper(j));
				ONE(isa, gsj_vzeroall(j));
			}
	}

	/* General instructions: 8, 16, 32 and 64 bits, registers and memory */
	for (n = 0; n < reps * 8; n++)
	{
		static const unsigned kinds[] = {GSJ_R8, GSJ_R16, GSJ_R32, GSJ_R64};
		static const u32 imms[] = {0, 1, 0x7F, 0x80, 0xFFFFFF80u, 0xFFFFFF7Fu, 0x7FFF, 0x8000, 0xFFFF8000u, 0x12345678u, 0xFFFFFFFFu};
		const unsigned k = kinds[rnd(4)], bits = k == GSJ_R8 ? 8 : k == GSJ_R16 ? 16 : k == GSJ_R32 ? 32 : 64;
		/* (8-bit registers 0 to 3, as the generators use: 4 to 7 are ah to bh) */
		const int lim = k == GSJ_R8 ? 4 : 16;
		gsj_op a = gsj_reg(k, (int)rnd(lim)), b = gsj_reg(k, (int)rnd(lim)), m = rmem(bits), mp = rmem(0);
		gsj_op r32 = gsj_r32((int)rnd(16)), r64 = gsj_r64((int)rnd(16)), m8 = rmem(8), m16 = rmem(16), m64 = rmem(64);
		const u32 imm = rnd(4) ? imms[rnd(sizeof(imms) / sizeof(imms[0]))] : (u32)rnd(0x7FFFFFFF);
		const u64 imm64 = rnd(3) == 0 ? ((u64)imm << 32 | rnd(65536)) : rnd(2) ? (u64)(s64)(s32)imm : imm;
		const unsigned sh = rnd(3) == 0 ? 1 : rnd(32);
		gsj_op r8;
#define ALU(cname) \
		ONE(0, gsj_##cname(j, a, b)); \
		ONE(0, gsj_##cname(j, a, m)); \
		ONE(0, gsj_##cname(j, m, a)); \
		ONE(0, gsj_##cname##_i(j, a, imm)); \
		ONE(0, gsj_##cname##_i(j, m, imm));
		ALU(add) ALU(and) ALU(cmp) ALU(or) ALU(sub) ALU(xor)
		ONE(0, gsj_mov(j, a, b));
		ONE(0, gsj_mov(j, a, m));
		ONE(0, gsj_mov(j, m, a));
		ONE(0, gsj_mov_i(j, a, imm64));
		ONE(0, gsj_mov_i(j, m, imm64));
		ONE(0, gsj_lea(j, r64, mp));
		ONE(0, gsj_lea(j, r32, mp));
		r8 = gsj_r8((int)rnd(4));
		ONE(0, gsj_movzx(j, r32, r8));
		ONE(0, gsj_movzx(j, r32, m8));
		ONE(0, gsj_movzx(j, r64, m16));
		ONE(0, gsj_not(j, a));
		ONE(0, gsj_not(j, m));
		ONE(0, gsj_push(j, r64));
		ONE(0, gsj_pop(j, r64));
		ONE(0, gsj_push(j, m64));
		ONE(0, gsj_sar(j, a, sh));
		ONE(0, gsj_shl(j, m, sh));
		ONE(0, gsj_shr(j, a, sh));
		ONE(0, gsj_sar_cl(j, a));
		ONE(0, gsj_shl_cl(j, m));
		ONE(0, gsj_shr_cl(j, a));
		ONE(0, gsj_test(j, a, b));
		ONE(0, gsj_test(j, m, a));
		ONE(0, gsj_test_i(j, a, imm));
		ONE(0, gsj_test_i(j, m, imm));
	}
	ONE(0, gsj_cdqe(j));
	ONE(0, gsj_ret(j));

	/* Labels: forward short and near, backward short and near, the
	 * anonymous ones, a short that does not reach, alignment, and jumps
	 * to addresses */
	for (n = 0; n < reps; n++)
	{
		const unsigned gap = rnd(4) == 0 ? 200 + rnd(100) : rnd(120);
		const unsigned back = rnd(150), al = 1u << rnd(6), t1 = rnd(400), t2 = 4000 - rnd(3900);
		unsigned i;
		{
			gsj jj_;
			gsj* const j = &jj_;
			gsj_init(j, s_cbuf, 4096, 0, 0);
			gsj_bind(j, 0);
			for (i = 0; i < back; i++) gsj_db(j, 0x90);
			gsj_je(j, GSJ_ANON, 0);
			gsj_jle(j, 1, 1);
			for (i = 0; i < gap; i++) gsj_db(j, 0x90);
			gsj_bind_anon(j);
			gsj_je(j, 2, 1);
			gsj_jmp(j, 0, 1);
			gsj_align(j, al);
			gsj_je(j, GSJ_ANON, 0);
			gsj_bind(j, 2);
			gsj_jmp(j, 0, 0);
			gsj_bind_anon(j);
			gsj_bind(j, 1);
			gsj_jcc_abs(j, GSJ_CC_ALWAYS, s_cbuf + t1);
			gsj_jcc_abs(j, GSJ_CC_E, s_cbuf + t2);
			gsj_ret(j);
			hash_bytes(s_cbuf, gsj_size(j));
			s_checked++;
		}
	}

	/* [rip + target]: the displacement from the instruction's end, past
	 * any immediate (the target the same distance from each buffer) */
	for (isa = 0; isa < 3; isa++)
	{
		for (n = 0; n < 200; n++)
		{
			static const s32 dist[] = {0, 64, -64, 4096, -100000, 0x10000000, -0x10000000};
			s32 d;
			int r, x;
			u32 imm;
			d   = dist[rnd(sizeof(dist) / sizeof(dist[0]))];
			d  += (s32)rnd(64);
			r   = (int)rnd(16);
			x   = (int)rnd(16);
			imm = rnd(2) ? rnd(100) : 0x12345;
#define CA (gsj_rip(j, s_cbuf + d))
			ONE(isa, gsj_lea(j, gsj_r64(r), CA));
			ONE(isa, gsj_mov(j, gsj_r64(r), CA));
			ONE(isa, gsj_add(j, gsj_r64(r), CA));
			ONE(isa, gsj_movdqa(j, gsj_x(x), CA));
			ONE(isa, gsj_orps(j, gsj_x(x), CA));
			ONE(isa, gsj_pshufd(j, gsj_x(x), CA, imm & 0xFF));
			ONE(isa, gsj_add_i(j, gsj_sized(CA, 32), imm));
			ONE(isa, gsj_mov_i(j, gsj_sized(CA, 32), imm));
			ONE(isa, gsj_test_i(j, gsj_sized(CA, 32), imm));
			ONE(isa, gsj_shl(j, gsj_sized(CA, 32), imm & 31));
			if (isa == 2)
			{
				ONE(isa, gsj_vbroadcastsd(j, gsj_y(x), CA));
				ONE(isa, gsj_vpblendvb(j, gsj_y(x), gsj_y(r), CA, gsj_y(0)));
			}
#undef CA
		}
	}

	printf("swjit emit: %d emitted, %016llx\n", s_checked, s_hash);
	if (want && want != s_hash)
	{
		printf("FAIL: expected %016llx\n", want);
		return 1;
	}
	return 0;
}
