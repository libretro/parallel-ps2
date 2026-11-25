/*
 * The AArch64 code the recompilers emit, in C: the instructions the EE,
 * IOP, VU and VIF recompilers use, encoded straight into a code buffer.
 *
 * The macro forms (a64_mov_imm, a64_add_imm, a64_and_imm, a64_ldr with an
 * offset no instruction takes, ...) expand the way vixl's MacroAssembler
 * did, instruction for instruction: tests/a64emit holds them, by hash, to
 * the words vixl made. Scratch registers come from the emitter's lists
 * (x16 and x17, v31 by default), lowest first.
 *
 * A register is an a64r: its number, its kind (W, X, SP, a scalar B..Q or
 * a vector arrangement) and a tag. Calls taking registers pick the general
 * or the NEON/FP form from the kinds they are given: a64_add with V4S
 * registers is a vector ADD, with X registers an integer one.
 *
 * Labels are a64_label, bound once; unresolved branches to a label are
 * chained through their immediate fields and patched at a64_bind. Nothing
 * here allocates. An emitter is one thread's.
 */

#ifndef A64_EMIT_H
#define A64_EMIT_H

#include <stddef.h>

#include <retro_inline.h>

#include "common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A register; a struct, so a register and an integer never mix */
typedef struct a64r
{
	u32 v;
} a64r;

/* Register kinds */
enum
{
	A64K_NONE = 0,
	A64K_W,
	A64K_X,
	A64K_WSP,
	A64K_SP,
	A64K_B,
	A64K_H,
	A64K_S,
	A64K_D,
	A64K_Q,
	A64K_8B,
	A64K_16B,
	A64K_4H,
	A64K_8H,
	A64K_2S,
	A64K_4S,
	A64K_2D
};

#define A64_TAG 0xA6400000u

static INLINE a64r a64_reg(unsigned kind, unsigned code)
{
	a64r r;
	r.v = A64_TAG | ((u32)kind << 8) | ((u32)code & 31u);
	return r;
}
static INLINE a64r a64_noreg(void)
{
	a64r r;
	r.v = 0;
	return r;
}
#define A64_CODE(r)  ((unsigned)((r).v & 31u))
#define A64_KIND(r)  ((unsigned)(((r).v >> 8) & 0xffu))
#define A64_ISREG(r) (((r).v & 0xffff0000u) == A64_TAG)
static INLINE int a64_same(a64r a, a64r b) { return a.v == b.v; }
/* The same register seen as another kind; SP stays SP */
static INLINE a64r a64_as(a64r r, unsigned kind) { return a64_reg(kind, A64_CODE(r)); }
static INLINE a64r a64_asw(a64r r)
{
	unsigned k = A64_KIND(r);
	return a64_reg((k == A64K_SP || k == A64K_WSP) ? A64K_WSP : A64K_W, A64_CODE(r));
}
static INLINE a64r a64_asx(a64r r)
{
	unsigned k = A64_KIND(r);
	return a64_reg((k == A64K_SP || k == A64K_WSP) ? A64K_SP : A64K_X, A64_CODE(r));
}

static INLINE int a64_is_x(a64r r) { return A64_KIND(r) == A64K_X || A64_KIND(r) == A64K_SP; }
/* A vector register of bits (64 or 128; 8..128 with one lane) in lanes */
static INLINE a64r a64_vreg(unsigned code, unsigned bits, unsigned lanes)
{
	unsigned k;
	if (lanes <= 1)
		k = bits == 8 ? A64K_B : bits == 16 ? A64K_H : bits == 32 ? A64K_S : bits == 64 ? A64K_D : A64K_Q;
	else if (bits == 64)
		k = lanes == 8 ? A64K_8B : lanes == 4 ? A64K_4H : lanes == 2 ? A64K_2S : A64K_D;
	else
		k = lanes == 16 ? A64K_16B : lanes == 8 ? A64K_8H : lanes == 4 ? A64K_4S : A64K_2D;
	return a64_reg(k, code);
}
/* Bytes a load or store of r moves */
static INLINE unsigned a64_size_bytes(a64r r)
{
	switch (A64_KIND(r))
	{
		case A64K_W: case A64K_WSP: case A64K_S: return 4;
		case A64K_B: return 1;
		case A64K_H: return 2;
		case A64K_Q: case A64K_16B: case A64K_8H: case A64K_4S: case A64K_2D: return 16;
		default: return 8;
	}
}

#define A64_W(n)    a64_reg(A64K_W, (n))
#define A64_X(n)    a64_reg(A64K_X, (n))
#define A64_WZR     a64_reg(A64K_W, 31)
#define A64_XZR     a64_reg(A64K_X, 31)
#define A64_WSP     a64_reg(A64K_WSP, 31)
#define A64_SP      a64_reg(A64K_SP, 31)
#define A64_LR      a64_reg(A64K_X, 30)
#define A64_B(n)    a64_reg(A64K_B, (n))
#define A64_H(n)    a64_reg(A64K_H, (n))
#define A64_S(n)    a64_reg(A64K_S, (n))
#define A64_D(n)    a64_reg(A64K_D, (n))
#define A64_Q(n)    a64_reg(A64K_Q, (n))
#define A64_V8B(n)  a64_reg(A64K_8B, (n))
#define A64_V16B(n) a64_reg(A64K_16B, (n))
#define A64_V4H(n)  a64_reg(A64K_4H, (n))
#define A64_V8H(n)  a64_reg(A64K_8H, (n))
#define A64_V2S(n)  a64_reg(A64K_2S, (n))
#define A64_V4S(n)  a64_reg(A64K_4S, (n))
#define A64_V1D(n)  a64_reg(A64K_D, (n))
#define A64_V2D(n)  a64_reg(A64K_2D, (n))

#ifdef __cplusplus
}
static inline bool operator==(a64r a, a64r b) { return a.v == b.v; }
static inline bool operator!=(a64r a, a64r b) { return a.v != b.v; }
extern "C" {
#endif

/* Conditions */
enum
{
	A64_EQ = 0, A64_NE, A64_HS, A64_LO, A64_MI, A64_PL, A64_VS, A64_VC,
	A64_HI, A64_LS, A64_GE, A64_LT, A64_GT, A64_LE, A64_AL, A64_NV
};
#define A64_CS A64_HS
#define A64_CC A64_LO
#define A64_INVERT(c) ((c) ^ 1)

/* Shifts and extends */
enum { A64_LSL = 0, A64_LSR, A64_ASR, A64_ROR, A64_MSL };
enum { A64_UXTB = 0, A64_UXTH, A64_UXTW, A64_UXTX, A64_SXTB, A64_SXTH, A64_SXTW, A64_SXTX, A64_NOEXT };

/* Memory operand: base plus offset (A64_OFFSET, A64_PREINDEX,
 * A64_POSTINDEX) or base plus a register (A64_REGOFFSET, with a shift or
 * an extend and its amount). */
enum { A64_OFFSET = 0, A64_PREINDEX, A64_POSTINDEX, A64_REGOFFSET };

typedef struct a64_mem
{
	a64r base;
	a64r index;
	s64 offset;
	u8 mode;
	u8 ext;    /* A64_NOEXT: shifted (LSL) */
	u8 amount;
	u8 pad;
} a64_mem;

static INLINE a64_mem a64_mem_mode(a64r base, s64 offset, unsigned mode)
{
	a64_mem m;
	m.base   = base;
	m.index  = a64_noreg();
	m.offset = offset;
	m.mode   = (u8)mode;
	m.ext    = A64_NOEXT;
	m.amount = 0;
	m.pad    = 0;
	return m;
}
static INLINE a64_mem a64_mem_off(a64r base, s64 offset) { return a64_mem_mode(base, offset, A64_OFFSET); }
static INLINE a64_mem a64_mem_pre(a64r base, s64 offset) { return a64_mem_mode(base, offset, A64_PREINDEX); }
static INLINE a64_mem a64_mem_post(a64r base, s64 offset) { return a64_mem_mode(base, offset, A64_POSTINDEX); }
/* base + index, LSL #amount (amount 0 or the access size) */
static INLINE a64_mem a64_mem_reg(a64r base, a64r index, unsigned amount)
{
	a64_mem m = a64_mem_mode(base, 0, A64_REGOFFSET);
	m.index  = index;
	m.amount = (u8)amount;
	return m;
}
/* base + extend(index) << amount */
static INLINE a64_mem a64_mem_ext(a64r base, a64r index, unsigned ext, unsigned amount)
{
	a64_mem m = a64_mem_mode(base, 0, A64_REGOFFSET);
	m.index  = index;
	m.ext    = (u8)ext;
	m.amount = (u8)amount;
	return m;
}

/* Labels: zeroed (A64_LABEL_INIT, a64_label_init, memset or a C++
 * value-initialization) is unbound. */
typedef struct a64_label
{
	s32 pos;  /* bound byte offset + 1; 0 unbound */
	s32 link; /* last unresolved branch's offset + 1; 0 none */
} a64_label;
#define A64_LABEL_INIT { 0, 0 }
static INLINE void a64_label_init(a64_label* l) { l->pos = 0; l->link = 0; }
static INLINE int a64_label_bound(const a64_label* l) { return l->pos > 0; }
/* A bound label's offset in the buffer */
static INLINE size_t a64_label_offset(const a64_label* l) { return (size_t)(l->pos - 1); }

typedef struct a64_emit
{
	u8* base;
	size_t cap;
	size_t pos;
	u32 scratch;  /* available X scratch registers, by bit */
	u32 vscratch; /* available V scratch registers, by bit */
	int error;    /* ran out of room, or a branch out of range */
} a64_emit;

void a64_init(a64_emit* e, void* buf, size_t cap);
static INLINE size_t a64_offset(const a64_emit* e) { return e->pos; }
static INLINE u8* a64_start(const a64_emit* e) { return e->base; }
static INLINE u8* a64_cursor(const a64_emit* e) { return e->base + e->pos; }
static INLINE size_t a64_capacity(const a64_emit* e) { return e->cap; }
void a64_word(a64_emit* e, u32 insn);

/* Immediate tests, as the encodings take them */
int a64_is_addsub_imm(s64 imm);
int a64_is_logical_imm(u64 imm, unsigned width);
int a64_is_int(s64 v, unsigned bits);

/* Scratch registers (the macro forms take theirs here) */
a64r a64_acquire_x(a64_emit* e);
a64r a64_acquire_w(a64_emit* e);
void a64_release(a64_emit* e, a64r r);

/* Labels and branches */
void a64_bind(a64_emit* e, a64_label* l);
void a64_b(a64_emit* e, a64_label* l);
void a64_b_cond(a64_emit* e, a64_label* l, unsigned cond);
void a64_cbz(a64_emit* e, a64r rt, a64_label* l);
void a64_cbnz(a64_emit* e, a64r rt, a64_label* l);
void a64_tbz(a64_emit* e, a64r rt, unsigned bit, a64_label* l);
void a64_tbnz(a64_emit* e, a64r rt, unsigned bit, a64_label* l);
/* Raw branches: displacements in instructions */
void a64_b_disp(a64_emit* e, s64 disp);
void a64_bl_disp(a64_emit* e, s64 disp);
void a64_b_cond_disp(a64_emit* e, s64 disp, unsigned cond);
void a64_cbz_disp(a64_emit* e, a64r rt, s64 disp);
void a64_cbnz_disp(a64_emit* e, a64r rt, s64 disp);
void a64_br(a64_emit* e, a64r xn);
void a64_blr(a64_emit* e, a64r xn);
void a64_ret(a64_emit* e);
/* adrp rd, pages (displacement in 4 KiB pages) */
void a64_adrp(a64_emit* e, a64r rd, s64 pages);
void a64_msr_fpcr(a64_emit* e, a64r rt);
/* Patch the instruction at code to a B to target (+-128 MiB) */
void a64_patch_b(void* code, const void* target);

/* Moves */
void a64_mov(a64_emit* e, a64r rd, a64r rn);
void a64_mov_imm(a64_emit* e, a64r rd, u64 imm);
void a64_mvn(a64_emit* e, a64r rd, a64r rn);
/* movz/movn/movk: shift -1 picks the halfword */
void a64_movz(a64_emit* e, a64r rd, u64 imm, int shift);
void a64_movn(a64_emit* e, a64r rd, u64 imm, int shift);
void a64_movk(a64_emit* e, a64r rd, u64 imm, int shift);

/* Arithmetic: register, immediate, shifted register, extended register */
void a64_add(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_add_imm(a64_emit* e, a64r rd, a64r rn, s64 imm);
void a64_add_shift(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned shift, unsigned amount);
void a64_add_ext(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned ext, unsigned amount);
void a64_adds(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_adds_imm(a64_emit* e, a64r rd, a64r rn, s64 imm);
void a64_sub(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_sub_imm(a64_emit* e, a64r rd, a64r rn, s64 imm);
void a64_sub_shift(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned shift, unsigned amount);
void a64_subs(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_subs_imm(a64_emit* e, a64r rd, a64r rn, s64 imm);
void a64_cmp(a64_emit* e, a64r rn, a64r rm);
void a64_cmp_imm(a64_emit* e, a64r rn, s64 imm);
void a64_cmp_shift(a64_emit* e, a64r rn, a64r rm, unsigned shift, unsigned amount);
void a64_cmn(a64_emit* e, a64r rn, a64r rm);
void a64_cmn_imm(a64_emit* e, a64r rn, s64 imm);
void a64_neg(a64_emit* e, a64r rd, a64r rm);

/* Logical */
void a64_and(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_and_imm(a64_emit* e, a64r rd, a64r rn, u64 imm);
void a64_and_shift(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned shift, unsigned amount);
void a64_ands(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_ands_imm(a64_emit* e, a64r rd, a64r rn, u64 imm);
void a64_orr(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_orr_imm(a64_emit* e, a64r rd, a64r rn, u64 imm);
void a64_orr_shift(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned shift, unsigned amount);
void a64_eor(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_eor_imm(a64_emit* e, a64r rd, a64r rn, u64 imm);
void a64_eor_shift(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned shift, unsigned amount);
void a64_bic(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_bic_imm(a64_emit* e, a64r rd, a64r rn, u64 imm);
void a64_tst(a64_emit* e, a64r rn, a64r rm);
void a64_tst_imm(a64_emit* e, a64r rn, u64 imm);

/* Shifts, bitfields, extends */
void a64_lsl(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_lsr(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_asr(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_lsl_imm(a64_emit* e, a64r rd, a64r rn, unsigned shift);
void a64_lsr_imm(a64_emit* e, a64r rd, a64r rn, unsigned shift);
void a64_asr_imm(a64_emit* e, a64r rd, a64r rn, unsigned shift);
void a64_ubfx(a64_emit* e, a64r rd, a64r rn, unsigned lsb, unsigned width);
void a64_bfi(a64_emit* e, a64r rd, a64r rn, unsigned lsb, unsigned width);
void a64_sxtb(a64_emit* e, a64r rd, a64r rn);
void a64_sxth(a64_emit* e, a64r rd, a64r rn);
void a64_sxtw(a64_emit* e, a64r rd, a64r rn);
void a64_uxtb(a64_emit* e, a64r rd, a64r rn);
void a64_uxth(a64_emit* e, a64r rd, a64r rn);

/* Multiply, divide, misc */
void a64_mul(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_msub(a64_emit* e, a64r rd, a64r rn, a64r rm, a64r ra);
void a64_smull(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_umull(a64_emit* e, a64r rd, a64r rn, a64r rm); /* X <- W*W, or UMULL/UMULL2 on vectors */
void a64_sdiv(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_udiv(a64_emit* e, a64r rd, a64r rn, a64r rm);
void a64_cls(a64_emit* e, a64r rd, a64r rn);
void a64_rev64(a64_emit* e, a64r rd, a64r rn);
void a64_csel(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned cond);
void a64_csinc(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned cond);
void a64_csinv(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned cond);
void a64_cset(a64_emit* e, a64r rd, unsigned cond);

/* Loads and stores (rt's kind picks the size; the macro forms reach any
 * offset) */
void a64_ldr(a64_emit* e, a64r rt, a64_mem m);
void a64_str(a64_emit* e, a64r rt, a64_mem m);
void a64_ldrb(a64_emit* e, a64r rt, a64_mem m);
void a64_ldrsb(a64_emit* e, a64r rt, a64_mem m);
void a64_strb(a64_emit* e, a64r rt, a64_mem m);
void a64_ldrh(a64_emit* e, a64r rt, a64_mem m);
void a64_ldrsh(a64_emit* e, a64r rt, a64_mem m);
void a64_strh(a64_emit* e, a64r rt, a64_mem m);
void a64_ldp(a64_emit* e, a64r rt, a64r rt2, a64_mem m);
void a64_stp(a64_emit* e, a64r rt, a64r rt2, a64_mem m);
void a64_st1_lane(a64_emit* e, a64r vt, unsigned lane, a64_mem m);

/* FP and NEON (kinds pick scalar or vector) */
void a64_fmov(a64_emit* e, a64r rd, a64r rn);
void a64_fadd(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_fsub(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_fmul(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_fmul_elem(a64_emit* e, a64r vd, a64r vn, a64r vm, unsigned index);
void a64_fdiv(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_fmin(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_fmax(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_fminnm(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_fmaxnm(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_faddp(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_faddp_scalar(a64_emit* e, a64r vd, a64r vn);
void a64_fsqrt(a64_emit* e, a64r vd, a64r vn);
void a64_fcmeq(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_fcmeq_zero(a64_emit* e, a64r vd, a64r vn);
void a64_fcmge(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_fcvtzs(a64_emit* e, a64r rd, a64r vn);
void a64_scvtf(a64_emit* e, a64r vd, a64r rn);

void a64_movi(a64_emit* e, a64r vd, u64 imm);
void a64_movi_shift(a64_emit* e, a64r vd, unsigned imm8, unsigned shift, unsigned amount);
void a64_mvni(a64_emit* e, a64r vd, unsigned imm8, unsigned shift, unsigned amount);
void a64_bic_vimm(a64_emit* e, a64r vd, unsigned imm8, unsigned lshift);
void a64_orr_vimm(a64_emit* e, a64r vd, unsigned imm8, unsigned lshift);
void a64_dup(a64_emit* e, a64r vd, a64r rn);
void a64_dup_elem(a64_emit* e, a64r vd, a64r vn, unsigned index);
void a64_ins(a64_emit* e, a64r vd, unsigned index, a64r rn);
void a64_ins_elem(a64_emit* e, a64r vd, unsigned dindex, a64r vn, unsigned sindex);
void a64_umov(a64_emit* e, a64r rd, a64r vn, unsigned index);
void a64_ext(a64_emit* e, a64r vd, a64r vn, a64r vm, unsigned index);
void a64_tbl(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_tbl2(a64_emit* e, a64r vd, a64r vn, a64r vn2, a64r vm);
void a64_zip1(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_zip2(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_uzp1(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_shl(a64_emit* e, a64r vd, a64r vn, unsigned shift);
void a64_ushr(a64_emit* e, a64r vd, a64r vn, unsigned shift);
void a64_sshr(a64_emit* e, a64r vd, a64r vn, unsigned shift);
void a64_shrn(a64_emit* e, a64r vd, a64r vn, unsigned shift);  /* SHRN2 for a Q vd */
void a64_xtn(a64_emit* e, a64r vd, a64r vn);                   /* XTN2 for a Q vd */
void a64_sshll(a64_emit* e, a64r vd, a64r vn, unsigned shift); /* SSHLL2 for a Q vn */
void a64_ushll(a64_emit* e, a64r vd, a64r vn, unsigned shift); /* UXTL is shift 0 */
void a64_addv(a64_emit* e, a64r vd, a64r vn);
void a64_uminv(a64_emit* e, a64r vd, a64r vn);
void a64_umaxv(a64_emit* e, a64r vd, a64r vn);
void a64_ushl(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_smin(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_smax(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_umin(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_sqadd(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_sqsub(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_uqadd(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_uqsub(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_sqabs(a64_emit* e, a64r vd, a64r vn);
void a64_abs(a64_emit* e, a64r vd, a64r vn);
void a64_cmeq(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_cmgt(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_cmeq_zero(a64_emit* e, a64r vd, a64r vn);
void a64_cmgt_zero(a64_emit* e, a64r vd, a64r vn);
void a64_cmge_zero(a64_emit* e, a64r vd, a64r vn);
void a64_cmle_zero(a64_emit* e, a64r vd, a64r vn);
void a64_cmlt_zero(a64_emit* e, a64r vd, a64r vn);
void a64_bsl(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_bit(a64_emit* e, a64r vd, a64r vn, a64r vm);
void a64_bif(a64_emit* e, a64r vd, a64r vn, a64r vm);

#ifdef __cplusplus
}
#endif

#endif
