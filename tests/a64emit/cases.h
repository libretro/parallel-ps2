/*
 * The emitter's cases: every call the recompilers make, over its operand
 * forms (register kinds, zero and stack registers, immediates that fit
 * one instruction and ones that take several, offsets each addressing
 * mode takes and ones it does not, branches resolved and linked, bound
 * out of range). gen_case fills case i of an op from a fixed seed; the
 * hash of the words over an op's cases is held to vixl's.
 */

#ifndef A64EMIT_CASES_H
#define A64EMIT_CASES_H

#include <string.h>

#include "arm64/a64emit.h"

#define OPS(X) \
	X(MOV) X(MOV_IMM) X(MVN) X(MOVZ) X(MOVN) X(MOVK) \
	X(ADD) X(ADD_IMM) X(ADD_SHIFT) X(ADD_EXT) X(ADDS) X(ADDS_IMM) \
	X(SUB) X(SUB_IMM) X(SUB_SHIFT) X(SUBS) X(SUBS_IMM) \
	X(CMP) X(CMP_IMM) X(CMP_SHIFT) X(CMN) X(CMN_IMM) X(NEG) \
	X(AND) X(AND_IMM) X(AND_SHIFT) X(ANDS) X(ANDS_IMM) X(ORR) X(ORR_IMM) X(ORR_SHIFT) \
	X(EOR) X(EOR_IMM) X(EOR_SHIFT) X(BIC) X(BIC_IMM) X(TST) X(TST_IMM) \
	X(LSL) X(LSR) X(ASR) X(LSL_IMM) X(LSR_IMM) X(ASR_IMM) X(UBFX) X(BFI) \
	X(SXTB) X(SXTH) X(SXTW) X(UXTB) X(UXTH) \
	X(MUL) X(MSUB) X(SMULL) X(UMULL) X(SDIV) X(UDIV) X(CLS) X(REV64) \
	X(CSEL) X(CSINC) X(CSINV) X(CSET) \
	X(BR) X(BLR) X(RET) X(ADRP) X(MSR_FPCR) \
	X(B_DISP) X(BL_DISP) X(B_COND_DISP) X(CBZ_DISP) X(CBNZ_DISP) \
	X(LDR) X(STR) X(LDRB) X(LDRSB) X(STRB) X(LDRH) X(LDRSH) X(STRH) X(LDP) X(STP) X(ST1_LANE) \
	X(FMOV) X(FADD) X(FSUB) X(FMUL) X(FMUL_ELEM) X(FDIV) X(FMIN) X(FMAX) X(FMINNM) X(FMAXNM) \
	X(FADDP) X(FADDP_SCALAR) X(FSQRT) X(FCMEQ) X(FCMEQ_ZERO) X(FCMGE) X(FCVTZS) X(SCVTF) \
	X(MOVI) X(MOVI_SHIFT) X(MVNI) X(BIC_VIMM) X(ORR_VIMM) \
	X(DUP) X(DUP_ELEM) X(INS) X(INS_ELEM) X(UMOV) X(EXT) X(TBL) X(TBL2) \
	X(ZIP1) X(ZIP2) X(UZP1) X(SHL) X(USHR) X(SSHR) X(SHRN) X(XTN) X(SSHLL) X(USHLL) \
	X(ADDV) X(UMINV) X(UMAXV) X(VADD) X(VSUB) X(USHL) X(SMIN) X(SMAX) X(UMIN) \
	X(SQADD) X(SQSUB) X(UQADD) X(UQSUB) X(CMEQ) X(CMGT) X(SQABS) X(ABS) \
	X(CMEQ_ZERO) X(CMGT_ZERO) X(CMGE_ZERO) X(CMLE_ZERO) X(CMLT_ZERO) \
	X(VREV64) X(VMOV) X(VMVN) X(VAND) X(VORR) X(VEOR) X(VBIC) X(BSL) X(BIT) X(BIF) X(VUMULL) \
	X(LABELS)

#define OP_ENUM(n) OP_##n,
enum { OPS(OP_ENUM) OP_COUNT };
#undef OP_ENUM

#define OP_NAME(n) #n,
static const char* const s_op_names[] = { OPS(OP_NAME) "" };
#undef OP_NAME

#define CASES_PER_OP 400

typedef struct tcase
{
	int op;
	int narrow;   /* scratch as the recompilers' shared emitter: x16 only, no V */
	a64r r[4];
	s64 imm;
	u64 uimm;
	unsigned u[3];
	a64_mem m;
} tcase;

static u32 s_rng;
static int s_narrow; /* x16 is the only scratch register: keep it out of operands */
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static unsigned rn_(unsigned n) { return rnd() % n; }
static u64 rnd64(void)
{
	u64 hi = rnd();
	return (hi << 32) | rnd();
}

/* A general register: x0..x30, with the stack pointer or the zero
 * register now and then where allowed */
static a64r g_reg(int x, int sp_ok, int zr_ok)
{
	for (;;)
	{
		unsigned c = rn_(36);
		if (c >= 32)
			c = 16 + (c & 1); /* lean on the scratch registers */
		if (s_narrow && c == 16)
			continue;
		if (c == 31)
		{
			if (sp_ok && (rnd() & 1))
				return x ? A64_SP : A64_WSP;
			if (zr_ok)
				return x ? A64_XZR : A64_WZR;
			continue;
		}
		return x ? A64_X(c) : A64_W(c);
	}
}

static a64r v_reg(unsigned kind) { return a64_reg(kind, rn_(32)); }

static const unsigned s_vec_all[] = { A64K_8B, A64K_16B, A64K_4H, A64K_8H, A64K_2S, A64K_4S, A64K_2D };
static unsigned pick(const unsigned* list, unsigned n) { return list[rn_(n)]; }
#define PICK(l) pick((l), (unsigned)(sizeof(l) / sizeof((l)[0])))

/* Immediates: small, 16-bit chunks, logical patterns, near-encodable,
 * negatives, anything */
static u64 imm_any(int x)
{
	u64 v, a;
	/* one draw per statement: the order of two in one expression is the
	 * compiler's to choose */
	switch (rn_(11))
	{
		case 10: a = (rnd() & 0xffff) | 1; v = a << (1 + rn_(10)); break;
		case 0: v = rn_(4096); break;
		case 1: v = (u64)rn_(4096) << 12; break;
		case 2: a = rnd() & 0xffff; v = a << (16 * rn_(x ? 4 : 2)); break;
		case 3: a = rnd() & 0xffff; v = ~(a << (16 * rn_(x ? 4 : 2))); break;
		case 4:
		{
			/* a run of ones, rotated and repeated */
			unsigned esize = 2u << rn_(x ? 6 : 5);
			unsigned ones  = 1 + rn_(esize - 1);
			unsigned rot   = rn_(esize);
			u64 elt        = (ones >= 64) ? ~(u64)0 : (((u64)1 << ones) - 1);
			unsigned i;
			if (esize < 64)
				elt = ((elt >> rot) | (elt << (esize - rot))) & (((u64)1 << esize) - 1);
			v = 0;
			for (i = 0; i < 64; i += esize)
				v |= elt << i;
			break;
		}
		case 5: v = (u64)(s64)-(s64)(1 + rn_(5000)); break;
		case 6: a = rn_(16); v = a << rn_(x ? 60 : 28); break;
		case 7: v = rnd64() & 0xffffffffu; break;
		case 8: v = 0; if (rnd() & 1) v = ~(u64)0; break;
		default: v = rnd64(); break;
	}
	if (!x)
	{
		/* a W immediate: 32 bits, or a negative 32-bit value */
		if (v >> 32 && (v >> 31) != 0x1ffffffffull)
			v &= 0xffffffffu;
	}
	return v;
}

static s64 offset_for(unsigned size)
{
	switch (rn_(6))
	{
		case 0: return (s64)rn_(4096) << size;
		case 1: return (s64)rn_(512) - 256;
		case 2: return (s64)rn_(1 << 16) - (1 << 15);
		case 3: return (s64)(rnd64() >> 20) - ((s64)1 << 43);
		case 4:
		{
			s64 a = (s64)rn_(4096) << size;
			return a + 4096 * ((s64)1 << size) * rn_(3);
		}
		default: return (s64)rn_(64) * (1 << size);
	}
}

static a64_mem mem_for(unsigned size, int reg_ok)
{
	a64r base = g_reg(1, 1, 0);
	unsigned mode = rn_(reg_ok ? 4 : 3);
	switch (mode)
	{
		case 0: return a64_mem_off(base, offset_for(size));
		case 1: return a64_mem_pre(base, rn_(4) ? (s64)rn_(512) - 256 : offset_for(size));
		case 2: return a64_mem_post(base, rn_(4) ? (s64)rn_(512) - 256 : offset_for(size));
		default:
		{
			unsigned amt = (rnd() & 1) ? size : 0;
			switch (rn_(4))
			{
				case 0: return a64_mem_reg(base, g_reg(1, 0, 1), amt);
				case 1: return a64_mem_ext(base, g_reg(0, 0, 1), A64_UXTW, amt);
				case 2: return a64_mem_ext(base, g_reg(0, 0, 1), A64_SXTW, amt);
				default: return a64_mem_ext(base, g_reg(1, 0, 1), A64_SXTX, amt);
			}
		}
	}
}

static unsigned cond14(void) { return rn_(14); }

static void gen_gpr3(tcase* c, int sp_d, int sp_n, int zr_m)
{
	int x  = (int)(rnd() & 1);
	c->r[0] = g_reg(x, sp_d, 0);
	c->r[1] = g_reg(x, sp_n, 0);
	c->r[2] = g_reg(x, 0, zr_m);
	if (rn_(6) == 0)
		c->r[1] = c->r[0];
}

static void gen_case(int op, unsigned i, tcase* c)
{
	int x;
	unsigned k;
	memset(c, 0, sizeof(*c));
	s_rng   = 0x9E3779B9u ^ ((u32)op * 2654435761u) ^ (i * 40503u);
	if (!s_rng)
		s_rng = 1;
	rnd();
	c->op     = op;
	c->narrow = (int)(i & 1);
	s_narrow  = c->narrow;
	x         = (int)(rnd() & 1);
	switch (op)
	{
		case OP_MOV:
			c->r[0] = g_reg(x, 1, 0);
			c->r[1] = (rn_(4) == 0) ? c->r[0] : g_reg(x, 1, 0);
			break;
		case OP_MOV_IMM:
			c->r[0] = g_reg(x, rn_(8) == 0, 0);
			c->uimm = imm_any(x);
			break;
		case OP_MVN: case OP_NEG: case OP_CLS:
			c->r[0] = g_reg(x, 0, 0);
			c->r[1] = g_reg(x, 0, op == OP_MVN || op == OP_NEG);
			break;
		case OP_MOVZ: case OP_MOVN: case OP_MOVK:
			c->r[0] = g_reg(x, 0, 0);
			c->u[0] = rn_(3) ? (rn_(x ? 4 : 2) * 16) : (unsigned)-1;
			if (c->u[0] == (unsigned)-1)
			{
				c->uimm = rnd() & 0xffff;
				c->uimm <<= 16 * rn_(x ? 4 : 2);
			}
			else
				c->uimm = rnd() & 0xffff;
			break;
		case OP_ADD: case OP_SUB:
			gen_gpr3(c, 1, 1, 1);
			break;
		case OP_ADDS: case OP_SUBS:
			gen_gpr3(c, 0, 1, 1);
			break;
		case OP_ADD_IMM: case OP_SUB_IMM: case OP_ADDS_IMM: case OP_SUBS_IMM:
			c->r[0] = g_reg(x, op == OP_ADD_IMM || op == OP_SUB_IMM, 0);
			c->r[1] = g_reg(x, 1, rn_(10) == 0);
			if (rn_(6) == 0)
				c->r[1] = c->r[0];
			c->imm = (s64)imm_any(x);
			if (rn_(3) == 0)
				c->imm = (s64)rn_(8192) - 4096;
			/* the stack pointer limits the pre-shift */
			if (rn_(4) == 0)
			{
				if ((op == OP_ADD_IMM || op == OP_SUB_IMM) && (rnd() & 1))
					c->r[0] = x ? A64_SP : A64_WSP;
				else
					c->r[1] = x ? A64_SP : A64_WSP;
				c->uimm = (rnd() & 0xffff) | 1;
				c->imm  = (s64)(c->uimm << (1 + rn_(10)));
				c->uimm = 0;
			}
			break;
		case OP_ADD_SHIFT: case OP_SUB_SHIFT:
			gen_gpr3(c, 0, 0, 1);
			c->u[0] = rn_(4);
			c->u[1] = rn_(x ? 64 : 32);
			break;
		case OP_CMP_SHIFT:
			c->r[0] = g_reg(x, 0, 0);
			c->r[1] = g_reg(x, 0, 1);
			c->u[0] = rn_(3);
			c->u[1] = rn_(x ? 64 : 32);
			break;
		case OP_ADD_EXT:
			c->r[0] = g_reg(x, 1, 0);
			c->r[1] = g_reg(x, 1, 0);
			c->u[0] = rn_(8);
			c->u[1] = rn_(5);
			c->r[2] = g_reg(x && (c->u[0] & 3) == 3, 0, 1);
			if (!x && (c->u[0] & 3) == 3)
				c->u[0] &= ~1u;
			break;
		case OP_CMP: case OP_CMN:
			c->r[0] = g_reg(x, 1, 0);
			c->r[1] = g_reg(x, 0, 1);
			break;
		case OP_CMP_IMM: case OP_CMN_IMM:
			c->r[0] = g_reg(x, 1, 0);
			c->imm  = (s64)imm_any(x);
			if (rn_(3) == 0)
				c->imm = (s64)rn_(8192) - 4096;
			break;
		case OP_AND: case OP_ORR: case OP_EOR: case OP_BIC: case OP_ANDS:
			gen_gpr3(c, 0, 0, 1);
			break;
		case OP_AND_IMM: case OP_ORR_IMM: case OP_EOR_IMM: case OP_BIC_IMM: case OP_ANDS_IMM:
			c->r[0] = g_reg(x, op != OP_ANDS_IMM, 0);
			c->r[1] = g_reg(x, 0, rn_(10) == 0);
			if (rn_(6) == 0 && A64_CODE(c->r[0]) != 31)
				c->r[1] = c->r[0];
			c->uimm = imm_any(x);
			break;
		case OP_AND_SHIFT: case OP_ORR_SHIFT: case OP_EOR_SHIFT:
			gen_gpr3(c, 0, 0, 1);
			c->u[0] = rn_(4);
			c->u[1] = rn_(x ? 64 : 32);
			break;
		case OP_TST:
			c->r[0] = g_reg(x, 0, 0);
			c->r[1] = g_reg(x, 0, 1);
			break;
		case OP_TST_IMM:
			c->r[0] = g_reg(x, 0, 0);
			c->uimm = imm_any(x);
			break;
		case OP_LSL: case OP_LSR: case OP_ASR: case OP_MUL: case OP_SDIV: case OP_UDIV:
			gen_gpr3(c, 0, 0, 0);
			break;
		case OP_MSUB:
			gen_gpr3(c, 0, 0, 0);
			c->r[3] = g_reg(x, 0, 0);
			break;
		case OP_LSL_IMM: case OP_LSR_IMM: case OP_ASR_IMM:
			c->r[0] = g_reg(x, 0, 0);
			c->r[1] = g_reg(x, 0, 0);
			c->u[0] = rn_(x ? 64 : 32);
			break;
		case OP_UBFX: case OP_BFI:
			c->r[0] = g_reg(x, 0, 0);
			c->r[1] = g_reg(x, 0, 0);
			k       = x ? 64 : 32;
			c->u[0] = rn_(k);
			c->u[1] = 1 + rn_(k - c->u[0]);
			break;
		case OP_SXTB: case OP_SXTH: case OP_SXTW:
			c->r[0] = g_reg(op == OP_SXTW ? 1 : x, 0, 0);
			c->r[1] = g_reg((int)(rnd() & 1), 0, 0);
			break;
		case OP_UXTB: case OP_UXTH:
			c->r[0] = g_reg(0, 0, 0);
			c->r[1] = g_reg(0, 0, 0);
			break;
		case OP_SMULL: case OP_UMULL:
			c->r[0] = g_reg(1, 0, 0);
			c->r[1] = g_reg(0, 0, 0);
			c->r[2] = g_reg(0, 0, 0);
			break;
		case OP_REV64:
			c->r[0] = g_reg(1, 0, 0);
			c->r[1] = g_reg(1, 0, 0);
			break;
		case OP_CSEL: case OP_CSINC: case OP_CSINV:
			c->r[0] = g_reg(x, 0, 0);
			c->r[1] = g_reg(x, 0, op != OP_CSEL || rn_(4) == 0);
			c->r[2] = g_reg(x, 0, op != OP_CSEL || rn_(4) == 0);
			if (rn_(8) == 0)
				c->r[2] = c->r[1];
			if (op == OP_CSEL && rn_(8) == 0)
				c->r[1] = x ? A64_XZR : A64_WZR;
			c->u[0] = cond14();
			break;
		case OP_CSET:
			c->r[0] = g_reg(x, 0, 0);
			c->u[0] = cond14();
			break;
		case OP_BR: case OP_BLR:
			c->r[0] = g_reg(1, 0, 0);
			break;
		case OP_RET:
			break;
		case OP_ADRP:
			c->r[0] = g_reg(1, 0, 0);
			c->imm  = (s64)rn_(1u << 21) - (1 << 20);
			break;
		case OP_MSR_FPCR:
			c->r[0] = g_reg(1, 0, 0);
			break;
		case OP_B_DISP: case OP_BL_DISP:
			c->imm = (s64)rn_(1u << 26) - (1 << 25);
			break;
		case OP_B_COND_DISP:
			c->imm  = (s64)rn_(1u << 19) - (1 << 18);
			c->u[0] = cond14();
			break;
		case OP_CBZ_DISP: case OP_CBNZ_DISP:
			c->r[0] = g_reg(x, 0, 0);
			c->imm  = (s64)rn_(1u << 19) - (1 << 18);
			break;
		case OP_LDR: case OP_STR:
		{
			static const unsigned kinds[] = { A64K_W, A64K_X, A64K_B, A64K_H, A64K_S, A64K_D, A64K_Q };
			static const unsigned sizes[] = { 2, 3, 0, 1, 2, 3, 4 };
			k       = rn_(7);
			c->r[0] = (kinds[k] == A64K_W || kinds[k] == A64K_X) ? g_reg(kinds[k] == A64K_X, 0, op == OP_STR) : v_reg(kinds[k]);
			c->m    = mem_for(sizes[k], 1);
			break;
		}
		case OP_LDRB: case OP_STRB: case OP_LDRSB:
			c->r[0] = g_reg(op == OP_LDRSB ? x : 0, 0, op == OP_STRB);
			c->m    = mem_for(0, 1);
			break;
		case OP_LDRH: case OP_STRH: case OP_LDRSH:
			c->r[0] = g_reg(op == OP_LDRSH ? x : 0, 0, op == OP_STRH);
			c->m    = mem_for(1, 1);
			break;
		case OP_LDP: case OP_STP:
		{
			static const unsigned kinds[] = { A64K_W, A64K_X, A64K_S, A64K_D, A64K_Q };
			static const unsigned sizes[] = { 2, 3, 2, 3, 4 };
			k = rn_(5);
			if (k < 2)
			{
				c->r[0] = g_reg(k == 1, 0, op == OP_STP);
				do
					c->r[1] = g_reg(k == 1, 0, op == OP_STP);
				while (op == OP_LDP && a64_same(c->r[1], c->r[0]));
			}
			else
			{
				c->r[0] = v_reg(kinds[k]);
				do
					c->r[1] = v_reg(kinds[k]);
				while (op == OP_LDP && a64_same(c->r[1], c->r[0]));
			}
			c->m = mem_for(sizes[k], 0);
			if (rn_(2))
				c->m.offset = ((s64)rn_(128) - 64) * ((s64)1 << sizes[k]);
			break;
		}
		case OP_ST1_LANE:
		{
			static const unsigned kinds[] = { A64K_16B, A64K_8H, A64K_4S, A64K_2D };
			static const unsigned lanes[] = { 16, 8, 4, 2 };
			k       = rn_(4);
			c->r[0] = v_reg(kinds[k]);
			c->u[0] = rn_(lanes[k]);
			switch (rn_(3))
			{
				case 0: c->m = a64_mem_off(g_reg(1, 1, 0), 0); break;
				case 1: c->m = a64_mem_post(g_reg(1, 1, 0), (s64)(16 / lanes[k])); break;
				default:
					c->m       = a64_mem_post(g_reg(1, 1, 0), 0);
					c->m.index = g_reg(1, 0, 0);
					break;
			}
			break;
		}
		case OP_FMOV:
			switch (rn_(3))
			{
				case 0: c->r[0] = v_reg(x ? A64K_D : A64K_S); c->r[1] = g_reg(x, 0, 1); break;
				case 1: c->r[0] = g_reg(x, 0, 0); c->r[1] = v_reg(x ? A64K_D : A64K_S); break;
				default: c->r[0] = v_reg(x ? A64K_D : A64K_S); c->r[1] = v_reg(x ? A64K_D : A64K_S); break;
			}
			break;
		case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: case OP_FMIN: case OP_FMAX:
		case OP_FMINNM: case OP_FMAXNM: case OP_FCMEQ: case OP_FCMGE: case OP_FADDP:
		{
			static const unsigned kinds[] = { A64K_S, A64K_D, A64K_2S, A64K_4S, A64K_2D };
			k = (op == OP_FADDP) ? kinds[2 + rn_(3)] : PICK(kinds);
			c->r[0] = v_reg(k);
			c->r[1] = v_reg(k);
			c->r[2] = v_reg(k);
			break;
		}
		case OP_FSQRT: case OP_FCMEQ_ZERO:
		{
			static const unsigned kinds[] = { A64K_S, A64K_D, A64K_2S, A64K_4S, A64K_2D };
			k       = PICK(kinds);
			c->r[0] = v_reg(k);
			c->r[1] = v_reg(k);
			break;
		}
		case OP_FMUL_ELEM:
		{
			static const unsigned kinds[] = { A64K_S, A64K_2S, A64K_4S, A64K_D, A64K_2D };
			k       = PICK(kinds);
			c->r[0] = v_reg(k);
			c->r[1] = v_reg(k);
			c->r[2] = v_reg((k == A64K_D || k == A64K_2D) ? A64K_D : A64K_S);
			c->u[0] = rn_((k == A64K_D || k == A64K_2D) ? 2 : 4);
			break;
		}
		case OP_FADDP_SCALAR:
			c->r[0] = v_reg(x ? A64K_D : A64K_S);
			c->r[1] = v_reg(x ? A64K_2D : A64K_2S);
			break;
		case OP_FCVTZS: case OP_SCVTF:
			if (rnd() & 1)
			{
				unsigned d = rnd() & 1;
				c->r[op == OP_SCVTF ? 1 : 0] = g_reg(x, 0, op == OP_SCVTF);
				c->r[op == OP_SCVTF ? 0 : 1] = v_reg(d ? A64K_D : A64K_S);
			}
			else
			{
				static const unsigned kinds[] = { A64K_2S, A64K_4S, A64K_2D };
				k       = PICK(kinds);
				c->r[0] = v_reg(k);
				c->r[1] = v_reg(k);
			}
			break;
		case OP_MOVI:
		{
			static const unsigned kinds[] = { A64K_8B, A64K_16B, A64K_4H, A64K_8H, A64K_2S, A64K_4S, A64K_2D, A64K_D };
			k       = PICK(kinds);
			c->r[0] = v_reg(k);
			switch (k)
			{
				case A64K_8B: case A64K_16B: c->uimm = rnd() & 0xff; break;
				case A64K_4H: case A64K_8H:
				{
					static const u64 pats[] = { 0x0000, 0x00ff, 0xff00, 0xffff, 0x1212, 0x0012, 0x1200, 0xff12, 0x12ff, 0x1234 };
					c->uimm = pats[rn_(10)];
					if (c->uimm == 0x1234)
						c->uimm = rnd() & 0xffff;
					else if (c->uimm & 0x1212)
					{
						u64 hi = (c->uimm & 0x1200) ? (u64)(rnd() & 0xff) << 8 : 0;
						u64 lo = (c->uimm & 0x0012) ? (u64)(rnd() & 0xff) : 0;
						c->uimm = (c->uimm & ~(u64)0x1212) | hi | lo;
					}
					break;
				}
				case A64K_2S: case A64K_4S:
				{
					unsigned b = rnd() & 0xff, s = 8 * rn_(4);
					switch (rn_(9))
					{
						case 0: c->uimm = (u64)b << s; break;
						case 1: c->uimm = ~((u64)b << s) & 0xffffffffu; break;
						case 2: c->uimm = 0x0000ffffu | ((u64)b << 16); break;
						case 3: c->uimm = 0x000000ffu | ((u64)b << 8); break;
						case 4: c->uimm = 0xff000000u | ((u64)b << 16); break;
						case 5: c->uimm = 0xffff0000u | ((u64)b << 8); break;
						case 6: c->uimm = (rnd() & 0xffff) * 0x10001u; break;
						case 7: c->uimm = (rnd() & 1) ? 0xff : 0; c->uimm |= (u64)((rnd() & 1) ? 0xff : 0) << 24; break;
						default: c->uimm = rnd(); break;
					}
					break;
				}
				default:
					switch (rn_(3))
					{
						case 0: c->uimm = 0; { unsigned j; for (j = 0; j < 8; j++) if (rnd() & 1) c->uimm |= (u64)0xff << (8 * j); } break;
						case 1: c->uimm = (u64)rnd(); c->uimm |= c->uimm << 32; break;
						default: c->uimm = rnd64(); break;
					}
					break;
			}
			break;
		}
		case OP_MOVI_SHIFT: case OP_MVNI:
		{
			static const unsigned kinds[] = { A64K_4H, A64K_8H, A64K_2S, A64K_4S };
			k       = PICK(kinds);
			c->r[0] = v_reg(k);
			c->uimm = rnd() & 0xff;
			if (k == A64K_4H || k == A64K_8H)
			{
				c->u[0] = A64_LSL;
				c->u[1] = 8 * rn_(2);
			}
			else if (rnd() & 1)
			{
				c->u[0] = A64_MSL;
				c->u[1] = 8 + 8 * rn_(2);
			}
			else
			{
				c->u[0] = A64_LSL;
				c->u[1] = 8 * rn_(4);
			}
			break;
		}
		case OP_BIC_VIMM: case OP_ORR_VIMM:
		{
			static const unsigned kinds[] = { A64K_4H, A64K_8H, A64K_2S, A64K_4S };
			k       = PICK(kinds);
			c->r[0] = v_reg(k);
			c->uimm = rnd() & 0xff;
			c->u[0] = (k == A64K_4H || k == A64K_8H) ? 8 * rn_(2) : 8 * rn_(4);
			break;
		}
		case OP_DUP:
		{
			static const unsigned kinds[] = { A64K_8B, A64K_16B, A64K_4H, A64K_8H, A64K_2S, A64K_4S, A64K_2D };
			k       = PICK(kinds);
			c->r[0] = v_reg(k);
			c->r[1] = g_reg(k == A64K_2D, 0, 1);
			break;
		}
		case OP_DUP_ELEM:
		{
			static const unsigned dk[] = { A64K_8B, A64K_16B, A64K_4H, A64K_8H, A64K_2S, A64K_4S, A64K_2D, A64K_B, A64K_H, A64K_S, A64K_D };
			static const unsigned nk[] = { A64K_16B, A64K_16B, A64K_8H, A64K_8H, A64K_4S, A64K_4S, A64K_2D, A64K_16B, A64K_8H, A64K_4S, A64K_2D };
			static const unsigned nl[] = { 16, 16, 8, 8, 4, 4, 2, 16, 8, 4, 2 };
			k       = rn_(11);
			c->r[0] = v_reg(dk[k]);
			c->r[1] = v_reg(nk[k]);
			c->u[0] = rn_(nl[k]);
			break;
		}
		case OP_INS: case OP_INS_ELEM: case OP_UMOV:
		{
			static const unsigned kinds[] = { A64K_16B, A64K_8H, A64K_4S, A64K_2D };
			static const unsigned lanes[] = { 16, 8, 4, 2 };
			k       = rn_(4);
			c->r[0] = v_reg(kinds[k]);
			c->u[0] = rn_(lanes[k]);
			c->u[1] = rn_(lanes[k]);
			if (op == OP_INS)
				c->r[1] = g_reg(k == 3, 0, 1);
			else if (op == OP_INS_ELEM)
				c->r[1] = v_reg(kinds[k]);
			else
			{
				c->r[1] = c->r[0];
				c->r[0] = g_reg(k == 3, 0, 0);
			}
			break;
		}
		case OP_EXT:
			k       = (rnd() & 1) ? A64K_16B : A64K_8B;
			c->r[0] = v_reg(k);
			c->r[1] = v_reg(k);
			c->r[2] = v_reg(k);
			c->u[0] = rn_(k == A64K_16B ? 16 : 8);
			break;
		case OP_TBL: case OP_TBL2:
			k       = (rnd() & 1) ? A64K_16B : A64K_8B;
			c->r[0] = v_reg(k);
			c->r[1] = A64_V16B(rn_(op == OP_TBL2 ? 31 : 32));
			c->r[2] = v_reg(k);
			c->r[3] = A64_V16B((A64_CODE(c->r[1]) + 1) & 31);
			break;
		case OP_ZIP1: case OP_ZIP2: case OP_UZP1: case OP_VADD: case OP_VSUB: case OP_USHL:
		case OP_SQADD: case OP_SQSUB: case OP_UQADD: case OP_UQSUB: case OP_CMEQ: case OP_CMGT:
			k = PICK(s_vec_all);
			if ((op == OP_VADD || op == OP_VSUB || op == OP_CMEQ || op == OP_CMGT || op == OP_USHL || op == OP_SQADD) && rn_(6) == 0)
				k = A64K_D;
			c->r[0] = v_reg(k);
			c->r[1] = v_reg(k);
			c->r[2] = v_reg(k);
			break;
		case OP_SMIN: case OP_SMAX: case OP_UMIN:
		{
			static const unsigned kinds[] = { A64K_8B, A64K_16B, A64K_4H, A64K_8H, A64K_2S, A64K_4S };
			k       = PICK(kinds);
			c->r[0] = v_reg(k);
			c->r[1] = v_reg(k);
			c->r[2] = v_reg(k);
			break;
		}
		case OP_SHL: case OP_USHR: case OP_SSHR:
			k = PICK(s_vec_all);
			if (rn_(6) == 0)
				k = A64K_D;
			c->r[0] = v_reg(k);
			c->r[1] = v_reg(k);
			{
				unsigned lb = (k == A64K_8B || k == A64K_16B) ? 8 : (k == A64K_4H || k == A64K_8H) ? 16 : (k == A64K_2S || k == A64K_4S) ? 32 : 64;
				c->u[0] = (op == OP_SHL) ? rn_(lb) : 1 + rn_(lb);
			}
			break;
		case OP_SHRN: case OP_XTN:
		{
			static const unsigned dk[] = { A64K_8B, A64K_4H, A64K_2S, A64K_16B, A64K_8H, A64K_4S };
			static const unsigned nk[] = { A64K_8H, A64K_4S, A64K_2D, A64K_8H, A64K_4S, A64K_2D };
			static const unsigned lb[] = { 8, 16, 32, 8, 16, 32 };
			k       = rn_(6);
			c->r[0] = v_reg(dk[k]);
			c->r[1] = v_reg(nk[k]);
			c->u[0] = 1 + rn_(lb[k]);
			break;
		}
		case OP_SSHLL: case OP_USHLL:
		{
			static const unsigned dk[] = { A64K_8H, A64K_4S, A64K_2D, A64K_8H, A64K_4S, A64K_2D };
			static const unsigned nk[] = { A64K_8B, A64K_4H, A64K_2S, A64K_16B, A64K_8H, A64K_4S };
			static const unsigned lb[] = { 8, 16, 32, 8, 16, 32 };
			k       = rn_(6);
			c->r[0] = v_reg(dk[k]);
			c->r[1] = v_reg(nk[k]);
			c->u[0] = rn_(lb[k]);
			break;
		}
		case OP_ADDV: case OP_UMINV: case OP_UMAXV:
		{
			static const unsigned nk[] = { A64K_8B, A64K_16B, A64K_4H, A64K_8H, A64K_4S };
			static const unsigned dk[] = { A64K_B, A64K_B, A64K_H, A64K_H, A64K_S };
			k       = rn_(5);
			c->r[0] = v_reg(dk[k]);
			c->r[1] = v_reg(nk[k]);
			break;
		}
		case OP_SQABS: case OP_ABS: case OP_CMEQ_ZERO: case OP_CMGT_ZERO: case OP_CMGE_ZERO: case OP_CMLE_ZERO: case OP_CMLT_ZERO:
			k = PICK(s_vec_all);
			if (rn_(6) == 0)
				k = A64K_D;
			c->r[0] = v_reg(k);
			c->r[1] = v_reg(k);
			break;
		case OP_VREV64:
		{
			static const unsigned kinds[] = { A64K_8B, A64K_16B, A64K_4H, A64K_8H, A64K_2S, A64K_4S };
			k       = PICK(kinds);
			c->r[0] = v_reg(k);
			c->r[1] = v_reg(k);
			break;
		}
		case OP_VMOV: case OP_VMVN:
			k       = (rnd() & 1) ? A64K_16B : A64K_8B;
			c->r[0] = v_reg(k);
			c->r[1] = v_reg(k);
			break;
		case OP_VAND: case OP_VORR: case OP_VEOR: case OP_VBIC: case OP_BSL: case OP_BIT: case OP_BIF:
			k       = (rnd() & 1) ? A64K_16B : A64K_8B;
			c->r[0] = v_reg(k);
			c->r[1] = v_reg(k);
			c->r[2] = v_reg(k);
			break;
		case OP_VUMULL:
		{
			static const unsigned dk[] = { A64K_8H, A64K_4S, A64K_2D, A64K_8H, A64K_4S, A64K_2D };
			static const unsigned nk[] = { A64K_8B, A64K_4H, A64K_2S, A64K_16B, A64K_8H, A64K_4S };
			k       = rn_(6);
			c->r[0] = v_reg(dk[k]);
			c->r[1] = v_reg(nk[k]);
			c->r[2] = v_reg(nk[k]);
			break;
		}
		case OP_LABELS:
			/* u[0]: shape, u[1]: filler words, r[0]: register, u[2]: cond/bit */
			c->u[0] = rn_(8);
			c->u[1] = rn_(3) == 0 ? 9000 + rn_(300000) : rn_(40);
			/* backward ones go just out of range too */
			if (rn_(4) == 0)
				c->u[1] = (c->u[0] == 7) ? 8180 + rn_(40) : (c->u[0] == 2 || c->u[0] == 4) ? 262130 + rn_(40) : c->u[1];
			/* forward links stay in range (vixl would have emitted veneers) */
			if (((c->u[0] == 5 || c->u[0] == 6) && c->u[1] > 8000) || (c->u[0] != 2 && c->u[0] != 4 && c->u[0] != 7 && c->u[1] > 200000))
				c->u[1] = (c->u[0] == 5 || c->u[0] == 6) ? 8000 : 200000;
			c->r[0] = g_reg(x, 0, 0);
			c->u[2] = (c->u[0] >= 6) ? rn_(x ? 64 : 32) : cond14();
			break;
	}
}

/* FNV-1a over the words a case emitted, its op, and its length */
static u64 hash_words(u64 h, const u32* w, unsigned n)
{
	unsigned i, b;
	for (i = 0; i < n; i++)
		for (b = 0; b < 4; b++)
		{
			h ^= (w[i] >> (8 * b)) & 0xff;
			h *= 0x100000001b3ull;
		}
	h ^= n;
	h *= 0x100000001b3ull;
	return h;
}

#endif
