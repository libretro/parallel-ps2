/* The software renderer's C emitter (GS/Renderers/SW/GSJitX86.c) against
 * xbyak through the generators' own wrapper (GSNewCodeGenerator), for
 * SSE4.1, AVX and AVX2: every instruction the generators use, over random
 * registers and every addressing form (no base, rsp/r12 and rbp/r13 bases,
 * an index, scales, displacements of none, eight and 32 bits), must come
 * out the same bytes. Then the labels and jumps, and the alignment.
 *
 * The C emitter alone also hashes what it emitted, against EXPECTED in
 * build.sh: the oracle when xbyak is gone. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>

static unsigned s_cpu;
extern "C" uint64_t cpu_features_get(void) { return s_cpu; }
static void quiet(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
extern "C" { retro_log_printf_t log_cb = quiet; }

#include "GS/Renderers/SW/GSNewCodeGenerator.h"
#include "GS/Renderers/SW/GSJitX86.h"

using namespace Xbyak;

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

static int s_fails, s_checked;

/* One operand in both worlds */
struct Op
{
	gsj_op c;
	int kind;
	int bits;
};

static const Reg64 R64[16] = {Reg64(0), Reg64(1), Reg64(2), Reg64(3), Reg64(4), Reg64(5), Reg64(6), Reg64(7),
	Reg64(8), Reg64(9), Reg64(10), Reg64(11), Reg64(12), Reg64(13), Reg64(14), Reg64(15)};

static Address xaddr(const gsj_op& m)
{
	RegExp e;
	const bool has_base = m.reg >= 0, has_index = m.index >= 0;
	if (has_base && has_index)
	{
		if (m.index_kind == GSJ_XMM)
			e = R64[m.reg] + Xmm(m.index) * m.scale + m.disp;
		else if (m.index_kind == GSJ_YMM)
			e = R64[m.reg] + Ymm(m.index) * m.scale + m.disp;
		else
			e = R64[m.reg] + R64[m.index] * m.scale + m.disp;
	}
	else if (has_base)
		e = R64[m.reg] + m.disp;
	else if (has_index)
		e = R64[m.index] * m.scale + m.disp;
	else
		e = RegExp((size_t)(u32)m.disp);
	return AddressFrame(m.bits)[e];
}

/* xbyak operand objects for a gsj_op (kept alive in the slot) */
struct XOp
{
	Reg8 r8;
	Reg16 r16;
	Reg32 r32;
	Reg64 r64;
	Xmm x;
	Ymm y;
	Address a;
	XOp(const gsj_op& o)
		: r8(o.kind == GSJ_R8 ? o.reg : 0)
		, r16(o.kind == GSJ_R16 ? o.reg : 0)
		, r32(o.kind == GSJ_R32 ? o.reg : 0)
		, r64(o.kind == GSJ_R64 ? o.reg : 0)
		, x(o.kind == GSJ_XMM ? o.reg : 0)
		, y(o.kind == GSJ_YMM ? o.reg : 0)
		, a(o.kind == GSJ_MEM ? xaddr(o) : Address(0, false, RegExp()))
	{
	}
};
static const Operand& xop(const XOp& x, const gsj_op& o)
{
	switch (o.kind)
	{
		case GSJ_R8: return x.r8;
		case GSJ_R16: return x.r16;
		case GSJ_R32: return x.r32;
		case GSJ_R64: return x.r64;
		case GSJ_XMM: return x.x;
		case GSJ_YMM: return x.y;
		default: return x.a;
	}
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
	if (index == 4)
		index = -1;
	if (base < 0 && index < 0 && rnd(2))
		base = (int)rnd(16);
	return gsj_mem(bits, base, index, 1u << rnd(4), rdisp());
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

/* --- the comparison -------------------------------------------------- */

alignas(64) static u8 s_xbuf[4096];
alignas(64) static u8 s_cbuf[4096];

struct Pair
{
	CodeGenerator cg;
	GSNewCodeGenerator gen;
	gsj c;
	Pair(int isa)
		: cg(sizeof(s_xbuf), s_xbuf)
		, gen(&cg)
	{
		gsj_init(&c, s_cbuf, sizeof(s_cbuf), isa >= 1, isa >= 2);
	}
};

static void compare(Pair& p, const char* what)
{
	const size_t xs = p.cg.getSize(), cs = gsj_size(&p.c);
	s_checked++;
	hash_bytes(s_cbuf, cs);
	if (xs != cs || memcmp(s_xbuf, s_cbuf, xs))
	{
		static const char* reported[64];
		static unsigned nreported;
		size_t i;
		unsigned k;
		s_fails++;
		for (k = 0; k < nreported && strcmp(reported[k], what); k++)
			;
		if (k == nreported && nreported < 64)
		{
			reported[nreported++] = what;
			printf("  FAIL %s:\n    xbyak:", what);
			for (i = 0; i < xs; i++)
				printf(" %02x", s_xbuf[i]);
			printf("\n    C:    ");
			for (i = 0; i < cs; i++)
				printf(" %02x", s_cbuf[i]);
			printf("\n");
		}
	}
}

static void set_isa(int isa)
{
	s_cpu = RETRO_SIMD_SSE4 | (isa >= 1 ? RETRO_SIMD_AVX : 0) | (isa >= 2 ? RETRO_SIMD_AVX2 : 0);
}

#define ONE(isa, what, xcall, ccall) \
	do \
	{ \
		Pair p_(isa); \
		GSNewCodeGenerator& g = p_.gen; \
		gsj* j = &p_.c; \
		(void)g; (void)j; \
		xcall; \
		ccall; \
		compare(p_, what); \
	} while (0)

#define VX(P, Q) ((Q).kind == GSJ_YMM ? (const Xmm&)(P).y : (P).x)

int main(int argc, char** argv)
{
	const unsigned long long want = argc > 1 ? strtoull(argv[1], NULL, 16) : 0;
	const int reps = 300;
	int isa, n;

	for (isa = 0; isa < 3; isa++)
	{
		const int Y = isa == 2;
		set_isa(isa);

#define A2(name) \
		for (n = 0; n < reps; n++) \
		{ \
			const int ymm = (Y && rnd(2)) ? 1 : 0; \
			gsj_op x = rvec(ymm), x2 = rvec(ymm), o = rvec_or_mem(ymm, 0); \
			XOp X(x), X2(x2), O(o); \
			ONE(isa, #name, g.name(VX(X, x), xop(O, o)), gsj_##name(j, x, o)); \
			if (isa) \
				ONE(isa, "v" #name, g.v##name(VX(X, x), VX(X2, x2), xop(O, o)), gsj_v##name(j, x, x2, o)); \
		}
#define A2S(name) /* scalar: xmm only */ \
		for (n = 0; n < reps; n++) \
		{ \
			gsj_op x = rvec(0), x2 = rvec(0), o = rvec_or_mem(0, 0); \
			XOp X(x), X2(x2), O(o); \
			ONE(isa, #name, g.name(X.x, xop(O, o)), gsj_##name(j, x, o)); \
			if (isa) \
				ONE(isa, "v" #name, g.v##name(X.x, X2.x, xop(O, o)), gsj_v##name(j, x, x2, o)); \
		}
#define AI(name, yok) \
		for (n = 0; n < reps; n++) \
		{ \
			const int ymm = (Y && yok && rnd(2)) ? 1 : 0; \
			const unsigned imm = rnd(256); \
			gsj_op x = rvec(ymm), x2 = rvec(ymm), o = rvec_or_mem(ymm, 0); \
			XOp X(x), X2(x2), O(o); \
			ONE(isa, #name, g.name(VX(X, x), xop(O, o), (u8)imm), gsj_##name(j, x, o, imm)); \
			if (isa) \
				ONE(isa, "v" #name, g.v##name(VX(X, x), VX(X2, x2), xop(O, o), (u8)imm), gsj_v##name(j, x, x2, o, imm)); \
		}
#define S2(name, yok) \
		for (n = 0; n < reps; n++) \
		{ \
			const int ymm = (Y && yok && rnd(2)) ? 1 : 0; \
			gsj_op x = rvec(ymm), o = rvec_or_mem(ymm, 0); \
			XOp X(x), O(o); \
			ONE(isa, #name, g.name(VX(X, x), xop(O, o)), gsj_##name(j, x, o)); \
		}
#define SI(name, yok) \
		for (n = 0; n < reps; n++) \
		{ \
			const int ymm = (Y && yok && rnd(2)) ? 1 : 0; \
			const unsigned imm = rnd(256); \
			gsj_op x = rvec(ymm), o = rvec_or_mem(ymm, 0); \
			XOp X(x), O(o); \
			ONE(isa, #name, g.name(VX(X, x), xop(O, o), (u8)imm), gsj_##name(j, x, o, imm)); \
		}
#define SH(name) \
		for (n = 0; n < reps; n++) \
		{ \
			const int ymm = (Y && rnd(2)) ? 1 : 0; \
			const unsigned imm = rnd(256); \
			gsj_op x = rvec(ymm), x2 = rvec(ymm); \
			XOp X(x), X2(x2); \
			ONE(isa, #name " imm", g.name(VX(X, x), (int)imm), gsj_##name##_i(j, x, imm)); \
			if (isa) \
				ONE(isa, "v" #name " imm", g.v##name(VX(X, x), VX(X2, x2), (u8)imm), gsj_v##name##_i(j, x, x2, imm)); \
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
			XOp X(x), X2(x2), O(o);
			ONE(isa, "psrad", g.psrad(VX(X, x), xop(O, o)), gsj_psrad(j, x, o));
			ONE(isa, "psrlw", g.psrlw(VX(X, x), xop(O, o)), gsj_psrlw(j, x, o));
			if (isa)
			{
				ONE(isa, "vpsrad", g.vpsrad(VX(X, x), VX(X2, x2), xop(O, o)), gsj_vpsrad(j, x, x2, o));
				ONE(isa, "vpsrlw", g.vpsrlw(VX(X, x), VX(X2, x2), xop(O, o)), gsj_vpsrlw(j, x, x2, o));
			}
		}
		AI(pblendw, 1) AI(roundsd, 0) AI(shufps, 1)
		for (n = 0; n < reps; n++)
		{
			const unsigned imm = rnd(4);
			gsj_op x = rvec(0), x2 = rvec(0), o = rnd(2) ? gsj_r32((int)rnd(16)) : rmem(32);
			XOp X(x), X2(x2), O(o);
			ONE(isa, "pinsrd", g.pinsrd(X.x, xop(O, o), (u8)imm), gsj_pinsrd(j, x, o, imm));
			if (isa)
				ONE(isa, "vpinsrd", g.vpinsrd(X.x, X2.x, xop(O, o), (u8)imm), gsj_vpinsrd(j, x, x2, o, imm));
		}
		S2(cvtdq2ps, 1) S2(cvtps2dq, 1) S2(cvttps2dq, 1) S2(movddup, 1) S2(rcpps, 1)
		for (n = 0; n < reps; n++)
		{
			const int ymm = (Y && rnd(2)) ? 1 : 0;
			gsj_op x = rvec(ymm), o = rnd(3) ? rvec(0) : rmem(ymm ? 128 : 64);
			XOp X(x), O(o);
			ONE(isa, "pmovsxbd", g.pmovsxbd(VX(X, x), xop(O, o)), gsj_pmovsxbd(j, x, o));
			ONE(isa, "pmovzxbw", g.pmovzxbw(VX(X, x), xop(O, o)), gsj_pmovzxbw(j, x, o));
			ONE(isa, "cvtps2pd", g.cvtps2pd(VX(X, x), xop(O, o)), gsj_cvtps2pd(j, x, o));
		}
		for (n = 0; n < reps; n++)
		{
			/* (the source's length decides the AVX form's) */
			const int ymm = (Y && rnd(2)) ? 1 : 0;
			gsj_op x = rvec(0), o = rnd(3) ? rvec(ymm) : rmem(ymm ? 256 : 128);
			XOp X(x), O(o);
			ONE(isa, "cvtpd2dq", g.cvtpd2dq(X.x, xop(O, o)), gsj_cvtpd2dq(j, x, o));
			ONE(isa, "cvtpd2ps", g.cvtpd2ps(X.x, xop(O, o)), gsj_cvtpd2ps(j, x, o));
			ONE(isa, "cvttpd2dq", g.cvttpd2dq(X.x, xop(O, o)), gsj_cvttpd2dq(j, x, o));
		}
		SI(pshufd, 1) SI(pshufhw, 1) SI(pshuflw, 1) SI(roundps, 1)
		SH(pslld) SH(psllw) SH(psrad) SH(psraw) SH(psrld) SH(psrldq) SH(psrlw)

		for (n = 0; n < reps; n++)
		{
			const int ymm = (Y && rnd(2)) ? 1 : 0;
			gsj_op x = rvec(ymm), m = rmem(0), o = rvec_or_mem(ymm, 0);
			gsj_op xs = rvec(0), m32 = rmem(32), m64 = rmem(64), r32 = gsj_r32((int)rnd(16)), r64 = gsj_r64((int)rnd(16));
			XOp X(x), M(m), O(o), XS(xs), M32(m32), M64(m64), R32(r32), R64_(r64);
			ONE(isa, "movaps load", g.movaps(VX(X, x), xop(O, o)), gsj_movaps(j, x, o));
			ONE(isa, "movaps store", g.movaps(M.a, VX(X, x)), gsj_movaps(j, m, x));
			ONE(isa, "movdqa load", g.movdqa(VX(X, x), xop(O, o)), gsj_movdqa(j, x, o));
			ONE(isa, "movdqa store", g.movdqa(M.a, VX(X, x)), gsj_movdqa(j, m, x));
			ONE(isa, "movd x m", g.movd(XS.x, M32.a), gsj_movd(j, xs, m32));
			ONE(isa, "movd m x", g.movd(M32.a, XS.x), gsj_movd(j, m32, xs));
			ONE(isa, "movd x r", g.movd(XS.x, R32.r32), gsj_movd(j, xs, r32));
			ONE(isa, "movd r x", g.movd(R32.r32, XS.x), gsj_movd(j, r32, xs));
			ONE(isa, "movq x m", g.movq(XS.x, M64.a), gsj_movq(j, xs, m64));
			ONE(isa, "movq m x", g.movq(M64.a, XS.x), gsj_movq(j, m64, xs));
			ONE(isa, "movsd x m", g.movsd(XS.x, M64.a), gsj_movsd(j, xs, m64));
			ONE(isa, "movsd m x", g.movsd(M64.a, XS.x), gsj_movsd(j, m64, xs));
			ONE(isa, "movss x m", g.movss(XS.x, M32.a), gsj_movss(j, xs, m32));
			ONE(isa, "movss m x", g.movss(M32.a, XS.x), gsj_movss(j, m32, xs));
			ONE(isa, "movhps x m", g.movhps(XS.x, M64.a), gsj_movhps(j, xs, m64));
			ONE(isa, "movhps m x", g.movhps(M64.a, XS.x), gsj_movhps(j, m64, xs));
			ONE(isa, "cvtsd2si", g.cvtsd2si(R64_.r64, (n & 1) ? (const Operand&)XS.x : M64.a),
				gsj_cvtsd2si(j, r64, (n & 1) ? xs : m64));
			ONE(isa, "cvttsd2si", g.cvttsd2si(R64_.r64, (n & 1) ? (const Operand&)XS.x : M64.a),
				gsj_cvttsd2si(j, r64, (n & 1) ? xs : m64));
			ONE(isa, "extractps r", g.extractps(R32.r32, XS.x, (u8)(n & 3)), gsj_extractps(j, r32, xs, (unsigned)(n & 3)));
			ONE(isa, "extractps m", g.extractps(M32.a, XS.x, (u8)(n & 3)), gsj_extractps(j, m32, xs, (unsigned)(n & 3)));
			ONE(isa, "pextrd r", g.pextrd(R32.r32, XS.x, (u8)(n & 3)), gsj_pextrd(j, r32, xs, (unsigned)(n & 3)));
			ONE(isa, "pextrd m", g.pextrd(M32.a, XS.x, (u8)(n & 3)), gsj_pextrd(j, m32, xs, (unsigned)(n & 3)));
			ONE(isa, "pextrw r", g.pextrw(R32.r32, XS.x, (u8)(n & 7)), gsj_pextrw(j, r32, xs, (unsigned)(n & 7)));
			ONE(isa, "pmovmskb", g.pmovmskb(R32.r32, VX(X, x)), gsj_pmovmskb(j, r32, x));
			ONE(isa, "pblendvb", g.pblendvb(VX(X, x), xop(O, o)), gsj_pblendvb(j, x, o));
			if (isa)
			{
				gsj_op x2 = rvec(ymm), x4 = rvec(ymm);
				XOp X2(x2), X4(x4);
				ONE(isa, "vpblendvb", g.vpblendvb(VX(X, x), VX(X2, x2), xop(O, o), VX(X4, x4)),
					gsj_vpblendvb(j, x, x2, o, x4));
				ONE(isa, "vbroadcastss", g.vbroadcastss(VX(X, x), (n & 1) ? (const Operand&)M32.a : (const Operand&)XS.x),
					gsj_vbroadcastss(j, x, (n & 1) ? m32 : xs));
			}
		}

		/* AVX2 and AVX only */
		if (isa == 2)
			for (n = 0; n < reps; n++)
			{
				const unsigned imm = rnd(256);
				gsj_op y = rvec(1), y2 = rvec(1), x = rvec(0), xx = rvec(0), m = rmem(0), m128 = rmem(128);
				while (y2.reg == y.reg)
					y2 = rvec(1);
				while (xx.reg == x.reg)
					xx = rvec(0);
				gsj_op o = rvec_or_mem(1, 0), xo = rnd(2) ? rvec(0) : rmem(128), any = rvec_or_mem(rnd(2), 0);
				/* (destination, index and mask three registers: xbyak emits
				 * nothing for a gather that repeats one) */
				gsj_op vs = gsj_vsib(rnd(8) == 0 ? GSJ_RBP : (int)rnd(16),
					rnd(2) ? gsj_y(gather_index(y.reg, y2.reg)) : gsj_x(gather_index(x.reg, xx.reg)), 1u << rnd(4), rdisp());
				XOp Y_(y), Y2(y2), X(x), XX(xx), M(m), M128(m128), O(o), XO(xo), ANY(any), VS(vs);
				const Xmm& gv = vs.index_kind == GSJ_YMM ? (const Xmm&)Y_.y : X.x;
				const Xmm& gm = vs.index_kind == GSJ_YMM ? (const Xmm&)Y2.y : XX.x;
				ONE(isa, "vbroadcastsd m", g.vbroadcastsd(Y_.y, M.a), gsj_vbroadcastsd(j, y, m));
				ONE(isa, "vbroadcastsd x", g.vbroadcastsd(Y_.y, X.x), gsj_vbroadcastsd(j, y, x));
				ONE(isa, "vbroadcasti128", g.vbroadcasti128(Y_.y, M.a), gsj_vbroadcasti128(j, y, m));
				ONE(isa, "vbroadcastf128", g.vbroadcastf128(Y_.y, M.a), gsj_vbroadcastf128(j, y, m));
				ONE(isa, "vextracti128", g.vextracti128(xop(XO, xo), Y_.y, (u8)(imm & 1)), gsj_vextracti128(j, xo, y, imm & 1));
				ONE(isa, "vinsertf128", g.vinsertf128(Y_.y, Y2.y, xop(XO, xo), (u8)(imm & 1)), gsj_vinsertf128(j, y, y2, xo, imm & 1));
				ONE(isa, "vinserti128", g.vinserti128(Y_.y, Y2.y, xop(XO, xo), (u8)(imm & 1)), gsj_vinserti128(j, y, y2, xo, imm & 1));
				ONE(isa, "vpbroadcastd", g.vpbroadcastd(any.kind == GSJ_MEM ? (const Xmm&)Y_.y : VX(ANY, any), xop(XO, xo)),
					gsj_vpbroadcastd(j, any.kind == GSJ_MEM ? y : any, xo));
				ONE(isa, "vpbroadcastq", g.vpbroadcastq(Y_.y, xop(XO, xo)), gsj_vpbroadcastq(j, y, xo));
				ONE(isa, "vpbroadcastw", g.vpbroadcastw(X.x, xop(XO, xo)), gsj_vpbroadcastw(j, x, xo));
				ONE(isa, "vpermq", g.vpermq(Y_.y, xop(O, o), (u8)imm), gsj_vpermq(j, y, o, imm));
				ONE(isa, "vpgatherdd", g.vpgatherdd(gv, VS.a, gm),
					gsj_vpgatherdd(j, vs.index_kind == GSJ_YMM ? y : x, vs, vs.index_kind == GSJ_YMM ? y2 : xx));
				ONE(isa, "vpsravd", g.vpsravd(Y_.y, Y2.y, xop(O, o)), gsj_vpsravd(j, y, y2, o));
				ONE(isa, "vpsrlvd", g.vpsrlvd(Y_.y, Y2.y, xop(O, o)), gsj_vpsrlvd(j, y, y2, o));
				ONE(isa, "vzeroupper", g.vzeroupper(), gsj_vzeroupper(j));
				ONE(isa, "vzeroall", g.vzeroall(), gsj_vzeroall(j));
				(void)M128;
			}
	}

	/* General instructions: 8, 16, 32 and 64 bits, registers and memory */
	set_isa(0);
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
		XOp A(a), B(b), M(m), MP(mp), R32(r32), R64_(r64), M8(m8), M16(m16), M64(m64);
#define ALU(name, cname) 		ONE(0, #name " r r", g.name(xop(A, a), xop(B, b)), gsj_##cname(j, a, b)); 		ONE(0, #name " r m", g.name(xop(A, a), M.a), gsj_##cname(j, a, m)); 		ONE(0, #name " m r", g.name(M.a, xop(A, a)), gsj_##cname(j, m, a)); 		ONE(0, #name " r i", g.name(xop(A, a), imm), gsj_##cname##_i(j, a, imm)); 		ONE(0, #name " m i", g.name(M.a, imm), gsj_##cname##_i(j, m, imm));
		ALU(add, add) ALU(and_, and) ALU(cmp, cmp) ALU(or_, or) ALU(sub, sub) ALU(xor_, xor)
		ONE(0, "mov r r", g.mov(xop(A, a), xop(B, b)), gsj_mov(j, a, b));
		ONE(0, "mov r m", g.mov(xop(A, a), M.a), gsj_mov(j, a, m));
		ONE(0, "mov m r", g.mov(M.a, xop(A, a)), gsj_mov(j, m, a));
		ONE(0, "mov r i", g.mov(xop(A, a), (size_t)imm64), gsj_mov_i(j, a, imm64));
		ONE(0, "mov m i", g.mov(M.a, (size_t)imm64), gsj_mov_i(j, m, imm64));
		ONE(0, "lea", g.lea(R64_.r64, MP.a), gsj_lea(j, r64, mp));
		ONE(0, "lea32", g.lea(R32.r32, MP.a), gsj_lea(j, r32, mp));
		{
			const gsj_op r8 = gsj_r8((int)rnd(4));
			XOp R8_(r8);
			ONE(0, "movzx r32 r8", g.movzx(R32.r32, R8_.r8), gsj_movzx(j, r32, r8));
		}
		ONE(0, "movzx r32 m8", g.movzx(R32.r32, M8.a), gsj_movzx(j, r32, m8));
		ONE(0, "movzx r64 m16", g.movzx(R64_.r64, M16.a), gsj_movzx(j, r64, m16));
		ONE(0, "not r", g.not_(xop(A, a)), gsj_not(j, a));
		ONE(0, "not m", g.not_(M.a), gsj_not(j, m));
		ONE(0, "push", g.push(R64_.r64), gsj_push(j, r64));
		ONE(0, "pop", g.pop(R64_.r64), gsj_pop(j, r64));
		ONE(0, "push m", g.push(M64.a), gsj_push(j, m64));
		ONE(0, "sar i", g.sar(xop(A, a), sh), gsj_sar(j, a, sh));
		ONE(0, "shl i", g.shl(M.a, sh), gsj_shl(j, m, sh));
		ONE(0, "shr i", g.shr(xop(A, a), sh), gsj_shr(j, a, sh));
		ONE(0, "sar cl", g.sar(xop(A, a), Reg8(1)), gsj_sar_cl(j, a));
		ONE(0, "shl cl", g.shl(M.a, Reg8(1)), gsj_shl_cl(j, m));
		ONE(0, "shr cl", g.shr(xop(A, a), Reg8(1)), gsj_shr_cl(j, a));
		ONE(0, "test r r", g.test(xop(A, a), (const Reg&)xop(B, b)), gsj_test(j, a, b));
		ONE(0, "test m r", g.test(M.a, (const Reg&)xop(A, a)), gsj_test(j, m, a));
		ONE(0, "test r i", g.test(xop(A, a), imm), gsj_test_i(j, a, imm));
		ONE(0, "test m i", g.test(M.a, imm), gsj_test_i(j, m, imm));
	}
	ONE(0, "cdqe", g.cdqe(), gsj_cdqe(j));
	ONE(0, "ret", g.ret(), gsj_ret(j));

	/* Labels: forward short and near, backward short and near, the
	 * anonymous ones, a short that does not reach, alignment, and jumps
	 * to addresses */
	for (n = 0; n < reps; n++)
	{
		const unsigned gap = rnd(4) == 0 ? 200 + rnd(100) : rnd(120);
		const unsigned back = rnd(150), al = 1u << rnd(6), t1 = rnd(400), t2 = 4000 - rnd(3900);
		unsigned i;
		ONE(0, "labels",
			{
				g.L("loop");
				for (i = 0; i < back; i++) g.db(0x90);
				g.je("@f");
				g.jle("exit", CodeGenerator::T_NEAR);
				for (i = 0; i < gap; i++) g.db(0x90);
				g.L("@@");
				g.je("step", CodeGenerator::T_NEAR);
				g.jmp("loop", CodeGenerator::T_NEAR);
				g.align(al);
				g.je("@f");
				g.L("step");
				g.jmp("loop");
				g.L("@@");
				g.L("exit");
				g.jmp((const void*)(s_xbuf + t1));
				g.je((const void*)(s_xbuf + t2));
				g.ret();
			},
			{
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
			});
	}

	printf("swjit emit: %d checked, %d failed, %016llx\n", s_checked, s_fails, s_hash);
	if (want && want != s_hash)
	{
		printf("FAIL: expected %016llx\n", want);
		return 1;
	}
	return s_fails != 0;
}
