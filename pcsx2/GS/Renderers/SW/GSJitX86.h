/*
 * The x86-64 code the software renderer's generators emit, in C: the
 * instructions they use (SSE4.1, AVX, AVX2 and the general ones around
 * them), encoded straight into the function's room in the code reserve.
 *
 * The encodings are xbyak's, byte for byte (tests/swjit holds them, by
 * hash, to the bytes xbyak made): the same prefixes, ModRM, SIB and
 * displacement choices, imm8 where an immediate fits, the 2-byte VEX form
 * wherever it can be had, and forward jumps short unless asked for near. A generator written against this
 * emits what it emitted through xbyak.
 *
 * Instructions with an SSE and an AVX form take the AVX one when the
 * emitter was made for AVX (gsj_init's avx): gsj_paddd(j, x, op) is
 * paddd x, op or vpaddd x, x, op. The three-operand gsj_v* are AVX only.
 *
 * Labels are numbers below GSJ_LABELS, bound once; GSJ_ANON is the next
 * gsj_bind_anon after the jump (xbyak's "@f" and "@@"). Nothing here
 * allocates. Not thread safe: an emitter is one generator's.
 */

#ifndef GS_JIT_X86_H
#define GS_JIT_X86_H

#include <stddef.h>

#include <retro_inline.h>

#include "common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Operand kinds */
enum
{
	GSJ_NONE = 0,
	GSJ_R8,
	GSJ_R16,
	GSJ_R32,
	GSJ_R64,
	GSJ_XMM,
	GSJ_YMM,
	GSJ_MEM
};

/* A register (kind and number) or a memory operand: base and index
 * registers (-1: none; an xmm or ymm index makes it a gather's), scale,
 * displacement, and its size in bits (0: unsized, as xbyak's ptr). */
typedef struct gsj_op
{
	u8 kind;
	s8 reg;   /* register; for memory, the base */
	s8 index;
	u8 scale;
	u8 index_kind; /* GSJ_R64, or GSJ_XMM/GSJ_YMM for a gather */
	u16 bits;
	s32 disp;
} gsj_op;

static INLINE gsj_op gsj_reg(unsigned kind, int n)
{
	gsj_op o;
	o.kind       = (u8)kind;
	o.reg        = (s8)n;
	o.index      = -1;
	o.scale      = 1;
	o.index_kind = 0;
	o.bits       = (u16)(kind == GSJ_R8 ? 8 : kind == GSJ_R16 ? 16 : kind == GSJ_R32 ? 32 : kind == GSJ_R64 ? 64
		: kind == GSJ_XMM ? 128 : 256);
	o.disp       = 0;
	return o;
}
#define gsj_r8(n)  gsj_reg(GSJ_R8, (n))
#define gsj_r16(n) gsj_reg(GSJ_R16, (n))
#define gsj_r32(n) gsj_reg(GSJ_R32, (n))
#define gsj_r64(n) gsj_reg(GSJ_R64, (n))
#define gsj_x(n)   gsj_reg(GSJ_XMM, (n))
#define gsj_y(n)   gsj_reg(GSJ_YMM, (n))

/* [base + index * scale + disp] of bits (0, 8, 16, 32, 64, 128, 256);
 * base or index -1 for none */
static INLINE gsj_op gsj_mem(unsigned bits, int base, int index, unsigned scale, s32 disp)
{
	gsj_op o;
	o.kind       = GSJ_MEM;
	o.reg        = (s8)base;
	o.index      = (s8)index;
	o.scale      = (u8)scale;
	o.index_kind = GSJ_R64;
	o.bits       = (u16)bits;
	o.disp       = disp;
	return o;
}
/* [base + disp], unsized */
#define gsj_ptr(base, disp) gsj_mem(0, (base), -1, 1, (disp))
/* [rip + disp32] at target: the base GSJ_RIP, the displacement target's
 * distance from the emitter's buffer (resolved as it is encoded) */
#define GSJ_RIP (-2)
/* The same operand with another size (xbyak's dword[...] and so on) */
static INLINE gsj_op gsj_sized(gsj_op o, unsigned bits)
{
	o.bits = (u16)bits;
	return o;
}
/* A gather's [base + vindex * scale + disp], the index a vector register */
static INLINE gsj_op gsj_vsib(int base, gsj_op vindex, unsigned scale, s32 disp)
{
	gsj_op o   = gsj_mem(0, base, vindex.reg, scale, disp);
	o.index_kind = vindex.kind;
	return o;
}

/* General register numbers */
enum
{
	GSJ_RAX, GSJ_RCX, GSJ_RDX, GSJ_RBX, GSJ_RSP, GSJ_RBP, GSJ_RSI, GSJ_RDI,
	GSJ_R8N, GSJ_R9, GSJ_R10, GSJ_R11, GSJ_R12, GSJ_R13, GSJ_R14, GSJ_R15
};

/* Condition codes the generators jump on */
enum
{
	GSJ_CC_E  = 0x4,
	GSJ_CC_LE = 0xE,
	GSJ_CC_ALWAYS = 0x10
};

#define GSJ_LABELS 8
#define GSJ_FIXUPS 32
/* (a jump to the next gsj_bind_anon) */
#define GSJ_ANON GSJ_LABELS

typedef struct gsj
{
	u8* buf;
	u8* p;
	u8* end;
	int avx, avx2;
	int full;  /* the room ran out: what was emitted is cut short */
	int error; /* a short jump that did not reach its label */
	/* bound offset, or -1: the labels, then two for the anonymous ones,
	 * taken in turn (xbyak names them "@f" and "@b" alternately, and a
	 * jump that did not reach waits under its own) */
	s32 label[GSJ_LABELS + 2];
	unsigned anon; /* gsj_bind_anon calls so far */
	unsigned tail; /* immediate bytes after the operand being encoded */
	struct
	{
		s32 end; /* the offset after the jump's displacement */
		u8 size; /* 1 or 4 bytes */
		u8 label;
	} fix[GSJ_FIXUPS];
	unsigned fixes;
} gsj;

/* An emitter writing into room bytes at buf, for SSE4.1 (avx 0), AVX, or
 * AVX2 (avx2 1 as well) */
void gsj_init(gsj* j, void* buf, size_t room, int avx, int avx2);
/* [rip + ...] addressing target (within 2 GiB of the buffer) */
static INLINE gsj_op gsj_rip(const gsj* j, const void* target)
{
	return gsj_mem(0, GSJ_RIP, -1, 1, (s32)((size_t)target - (size_t)j->buf));
}
/* What was emitted: the room itself when it ran out */
size_t gsj_size(const gsj* j);

void gsj_db(gsj* j, unsigned byte);
void gsj_align(gsj* j, unsigned to);
void gsj_bind(gsj* j, unsigned label);
void gsj_bind_anon(gsj* j);
/* A conditional jump (GSJ_CC_*, or GSJ_CC_ALWAYS for jmp) to a label:
 * short, or near when asked, while the label is ahead; behind, short when
 * it reaches unless near is asked */
void gsj_jcc(gsj* j, unsigned cc, unsigned label, int near_);
/* A jump to an address: jmp short when it reaches, a conditional one near */
void gsj_jcc_abs(gsj* j, unsigned cc, const void* addr);
#define gsj_je(j, l, n)  gsj_jcc((j), GSJ_CC_E, (l), (n))
#define gsj_jle(j, l, n) gsj_jcc((j), GSJ_CC_LE, (l), (n))
#define gsj_jmp(j, l, n) gsj_jcc((j), GSJ_CC_ALWAYS, (l), (n))

/* --- general -------------------------------------------------------- */
void gsj_add(gsj* j, gsj_op a, gsj_op b);
void gsj_and(gsj* j, gsj_op a, gsj_op b);
void gsj_cmp(gsj* j, gsj_op a, gsj_op b);
void gsj_or(gsj* j, gsj_op a, gsj_op b);
void gsj_sub(gsj* j, gsj_op a, gsj_op b);
void gsj_xor(gsj* j, gsj_op a, gsj_op b);
void gsj_add_i(gsj* j, gsj_op a, u32 imm);
void gsj_and_i(gsj* j, gsj_op a, u32 imm);
void gsj_cmp_i(gsj* j, gsj_op a, u32 imm);
void gsj_or_i(gsj* j, gsj_op a, u32 imm);
void gsj_sub_i(gsj* j, gsj_op a, u32 imm);
void gsj_xor_i(gsj* j, gsj_op a, u32 imm);
void gsj_lea(gsj* j, gsj_op r, gsj_op m);
void gsj_mov(gsj* j, gsj_op a, gsj_op b);
void gsj_mov_i(gsj* j, gsj_op a, u64 imm);
void gsj_movzx(gsj* j, gsj_op r, gsj_op op);
void gsj_not(gsj* j, gsj_op op);
void gsj_push(gsj* j, gsj_op op);
void gsj_pop(gsj* j, gsj_op op);
void gsj_sar_cl(gsj* j, gsj_op op);
void gsj_shl_cl(gsj* j, gsj_op op);
void gsj_shr_cl(gsj* j, gsj_op op);
void gsj_sar(gsj* j, gsj_op op, unsigned imm);
void gsj_shl(gsj* j, gsj_op op, unsigned imm);
void gsj_shr(gsj* j, gsj_op op, unsigned imm);
void gsj_test(gsj* j, gsj_op op, gsj_op r);
void gsj_test_i(gsj* j, gsj_op op, u32 imm);
void gsj_cdqe(gsj* j);
void gsj_ret(gsj* j);
void gsj_vzeroupper(gsj* j);
void gsj_vzeroall(gsj* j);

/* --- SSE, or AVX with the destination repeated ------------------------
 * (x, op): the SSE form, or the AVX one as (x, x, op); gsj_v*(x, x2, op)
 * the AVX three-operand form. */
#define GSJ_DECLARE_A(name) \
	void gsj_##name(gsj* j, gsj_op x, gsj_op op); \
	void gsj_v##name(gsj* j, gsj_op x, gsj_op x2, gsj_op op);
#define GSJ_DECLARE_AI(name) \
	void gsj_##name(gsj* j, gsj_op x, gsj_op op, unsigned imm); \
	void gsj_v##name(gsj* j, gsj_op x, gsj_op x2, gsj_op op, unsigned imm);
#define GSJ_A_LIST(X) \
	X(addps) X(addpd) X(cvtsd2ss) X(cvtss2sd) X(divps) X(maxps) X(minps) X(mulpd) X(mulps) X(mulsd) X(mulss) \
	X(orps) X(packssdw) X(packusdw) X(packuswb) X(paddd) X(paddusb) X(paddw) X(pand) X(pandn) X(pcmpeqd) \
	X(pcmpeqw) X(pcmpgtd) X(pmaxsw) X(pminsd) X(pminud) X(pminsw) X(pmulhrsw) X(pmulhw) X(pmullw) X(por) \
	X(psrad) X(psrlw) X(psubd) X(psubw) X(punpckhdq) X(punpckhwd) X(punpcklbw) X(punpckldq) X(punpcklqdq) \
	X(punpcklwd) X(pxor) X(subps) X(unpcklps) X(unpcklpd) X(xorps)
#define GSJ_AI_LIST(X) X(pblendw) X(pinsrd) X(roundsd) X(shufps)
GSJ_A_LIST(GSJ_DECLARE_A)
GSJ_AI_LIST(GSJ_DECLARE_AI)

/* (x, op): two operands in either form */
#define GSJ_DECLARE_S(name) void gsj_##name(gsj* j, gsj_op x, gsj_op op);
#define GSJ_DECLARE_SI(name) void gsj_##name(gsj* j, gsj_op x, gsj_op op, unsigned imm);
#define GSJ_S_LIST(X) \
	X(cvtdq2ps) X(cvtpd2dq) X(cvtpd2ps) X(cvttpd2dq) X(cvtps2dq) X(cvtps2pd) X(cvttps2dq) X(movddup) \
	X(pmovsxbd) X(pmovzxbw) X(rcpps)
#define GSJ_SI_LIST(X) X(pshufd) X(pshufhw) X(pshuflw) X(roundps)
GSJ_S_LIST(GSJ_DECLARE_S)
GSJ_SI_LIST(GSJ_DECLARE_SI)

/* Shifts by an immediate: (x, imm), or the AVX (x, x2, imm) */
#define GSJ_DECLARE_SHIFT(name) \
	void gsj_##name##_i(gsj* j, gsj_op x, unsigned imm); \
	void gsj_v##name##_i(gsj* j, gsj_op x, gsj_op x2, unsigned imm);
#define GSJ_SHIFT_LIST(X) X(pslld) X(psllw) X(psrad) X(psraw) X(psrld) X(psrldq) X(psrlw)
GSJ_SHIFT_LIST(GSJ_DECLARE_SHIFT)

/* Moves: register or memory to register, and register to memory */
void gsj_movaps(gsj* j, gsj_op a, gsj_op b);
void gsj_movdqa(gsj* j, gsj_op a, gsj_op b);
void gsj_movd(gsj* j, gsj_op a, gsj_op b);
void gsj_movq(gsj* j, gsj_op a, gsj_op b);
void gsj_movsd(gsj* j, gsj_op a, gsj_op b);
void gsj_movss(gsj* j, gsj_op a, gsj_op b);
void gsj_movhps(gsj* j, gsj_op a, gsj_op b);
void gsj_cvtsd2si(gsj* j, gsj_op r, gsj_op op);
void gsj_cvttsd2si(gsj* j, gsj_op r, gsj_op op);
void gsj_extractps(gsj* j, gsj_op op, gsj_op x, unsigned imm);
void gsj_pextrd(gsj* j, gsj_op op, gsj_op x, unsigned imm);
void gsj_pextrw(gsj* j, gsj_op op, gsj_op x, unsigned imm);
void gsj_pmovmskb(gsj* j, gsj_op r, gsj_op x);
/* pblendvb x, op, xmm0; the AVX one as (x, x, op, xmm0) */
void gsj_pblendvb(gsj* j, gsj_op x, gsj_op op);
void gsj_vpblendvb(gsj* j, gsj_op x, gsj_op x2, gsj_op op, gsj_op x4);

/* --- AVX and AVX2 only ----------------------------------------------- */
void gsj_vbroadcastss(gsj* j, gsj_op x, gsj_op op);
void gsj_vbroadcastsd(gsj* j, gsj_op y, gsj_op op);
void gsj_vbroadcasti128(gsj* j, gsj_op y, gsj_op m);
void gsj_vbroadcastf128(gsj* j, gsj_op y, gsj_op m);
void gsj_vextracti128(gsj* j, gsj_op op, gsj_op y, unsigned imm);
void gsj_vinsertf128(gsj* j, gsj_op y1, gsj_op y2, gsj_op op, unsigned imm);
void gsj_vinserti128(gsj* j, gsj_op y1, gsj_op y2, gsj_op op, unsigned imm);
void gsj_vpbroadcastd(gsj* j, gsj_op x, gsj_op op);
void gsj_vpbroadcastq(gsj* j, gsj_op x, gsj_op op);
void gsj_vpbroadcastw(gsj* j, gsj_op x, gsj_op op);
void gsj_vpermq(gsj* j, gsj_op y, gsj_op op, unsigned imm);
void gsj_vpgatherdd(gsj* j, gsj_op x1, gsj_op m, gsj_op x2);
void gsj_vpsravd(gsj* j, gsj_op x1, gsj_op x2, gsj_op op);
void gsj_vpsrlvd(gsj* j, gsj_op x1, gsj_op x2, gsj_op op);

#ifdef __cplusplus
}
#endif

#endif
