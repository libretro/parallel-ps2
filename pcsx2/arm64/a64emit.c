/*
 * AArch64 emitter (see a64emit.h). The macro expansions follow vixl's
 * MacroAssembler step for step, so the words match what it emitted.
 */

#include <string.h>

#include "a64emit.h"

/* Register properties */

static unsigned rk(a64r r) { return A64_KIND(r); }
static int same(a64r a, a64r b) { return a.v == b.v; }
static u32 rc(a64r r) { return (u32)A64_CODE(r); }
static int is_gpr(a64r r) { unsigned k = rk(r); return k >= A64K_W && k <= A64K_SP; }
static int is_v(a64r r) { return rk(r) >= A64K_B; }
static int is_x(a64r r) { return rk(r) == A64K_X || rk(r) == A64K_SP; }
static int is_sp(a64r r) { return rk(r) == A64K_SP || rk(r) == A64K_WSP; }
static int is_zr(a64r r) { return (rk(r) == A64K_W || rk(r) == A64K_X) && rc(r) == 31; }
static unsigned rbits(a64r r) { return is_x(r) ? 64u : 32u; }
static u32 sf(a64r r) { return is_x(r) ? 0x80000000u : 0u; }
static a64r zr_for(a64r r) { return is_x(r) ? A64_XZR : A64_WZR; }
static a64r same_size(a64r like, unsigned code) { return is_x(like) ? A64_X(code) : A64_W(code); }

/* Vector register sizes: total bits, lane bits */
static const u8 s_vbits[] = { 0, 0, 0, 0, 0, 8, 16, 32, 64, 128, 64, 128, 64, 128, 64, 128, 128 };
static const u8 s_lbits[] = { 0, 0, 0, 0, 0, 8, 16, 32, 64, 128, 8, 8, 16, 16, 32, 32, 64 };

static unsigned vbits(a64r r) { return s_vbits[rk(r)]; }
static unsigned lbits(a64r r) { return s_lbits[rk(r)]; }
static int vscalar(a64r r) { return rk(r) >= A64K_B && rk(r) <= A64K_Q; }
static int visq(a64r r) { return vbits(r) == 128; }
static u32 lsize_log2(a64r r)
{
	switch (lbits(r))
	{
		case 8: return 0;
		case 16: return 1;
		case 32: return 2;
		case 64: return 3;
		default: return 4;
	}
}

#define NEON_Q      0x40000000u
#define NEON_SCALAR 0x10000000u

/* Q and size of a vector arrangement */
static u32 vformat(a64r r) { return (visq(r) ? NEON_Q : 0u) | (lsize_log2(r) << 22); }
/* size of a scalar */
static u32 sformat(a64r r) { return lsize_log2(r) << 22; }
/* Q and sz for FP: S or D lanes */
static u32 fpformat(a64r r)
{
	u32 f = (lbits(r) == 64) ? 0x00400000u : 0u;
	if (!vscalar(r) && visq(r))
		f |= NEON_Q;
	return f;
}
static u32 fptype(a64r r) { return (lbits(r) == 64) ? 0x00400000u : (lbits(r) == 16 ? 0x00C00000u : 0u); }

/* Buffer */

void a64_init(a64_emit* e, void* buf, size_t cap)
{
	e->base     = (u8*)buf;
	e->cap      = cap;
	e->pos      = 0;
	e->scratch  = (1u << 16) | (1u << 17);
	e->vscratch = 1u << 31;
	e->error    = 0;
}

void a64_word(a64_emit* e, u32 insn)
{
	if (e->pos + 4 > e->cap)
	{
		e->error = 1;
		return;
	}
	memcpy(e->base + e->pos, &insn, 4);
	e->pos += 4;
}

static u32 peek(const a64_emit* e, size_t at)
{
	u32 w;
	memcpy(&w, e->base + at, 4);
	return w;
}

static void poke(a64_emit* e, size_t at, u32 w) { memcpy(e->base + at, &w, 4); }

#define emit a64_word

/* Immediates */

int a64_is_int(s64 v, unsigned bits)
{
	s64 lim = (s64)1 << (bits - 1);
	return v >= -lim && v < lim;
}
static int is_uint(s64 v, unsigned bits) { return v >= 0 && (u64)v < ((u64)1 << bits); }

int a64_is_addsub_imm(s64 imm)
{
	return is_uint(imm, 12) || (is_uint(imm >> 12, 12) && (imm & 0xfff) == 0);
}

static int clz64(u64 v)
{
	int n = 0;
	if (!v)
		return 64;
	while (!(v & ((u64)1 << 63)))
	{
		v <<= 1;
		n++;
	}
	return n;
}
static int clzw(u64 v, unsigned width) { return width == 32 ? clz64(v & 0xffffffffu) - 32 : clz64(v); }
static int ctzw(u64 v, unsigned width)
{
	int n = 0;
	if (width == 32)
		v &= 0xffffffffu;
	if (!v)
		return (int)width;
	while (!(v & 1))
	{
		v >>= 1;
		n++;
	}
	return n;
}
static u64 lowest_bit(u64 v) { return v & (0 - v); }

/* vixl's IsImmLogical: n, imms and immr for value at width bits */
static int logical_imm(u64 value, unsigned width, unsigned* n, unsigned* imm_s, unsigned* imm_r)
{
	static const u64 mult[] = {
		0x0000000000000001ull, 0x0000000100000001ull, 0x0001000100010001ull,
		0x0101010101010101ull, 0x1111111111111111ull, 0x5555555555555555ull
	};
	int negate = 0;
	u64 a, b, c, mask, candidate;
	int d, clz_a, clz_b, out_n, s, r;
	unsigned bits;

	if (value & 1)
	{
		negate = 1;
		value  = ~value;
	}
	if (width <= 32)
	{
		for (bits = width; bits <= 32; bits *= 2)
		{
			u64 m = ((u64)1 << bits) - 1;
			value <<= bits;
			value |= (value >> bits) & m;
		}
	}
	a = lowest_bit(value);
	b = lowest_bit(value + a);
	c = lowest_bit(value + a - b);
	if (c != 0)
	{
		clz_a = clz64(a);
		d     = clz_a - clz64(c);
		mask  = ((u64)1 << d) - 1;
		out_n = 0;
	}
	else
	{
		if (a == 0)
			return 0;
		clz_a = clz64(a);
		d     = 64;
		mask  = ~(u64)0;
		out_n = 1;
	}
	if (d <= 0 || (d & (d - 1)))
		return 0;
	if (((b - a) & ~mask) != 0)
		return 0;
	candidate = (b - a) * mult[clz64((u64)d) - 57];
	if (value != candidate)
		return 0;
	clz_b = (b == 0) ? -1 : clz64(b);
	s     = clz_a - clz_b;
	if (negate)
	{
		s = d - s;
		r = (clz_b + 1) & (d - 1);
	}
	else
		r = (clz_a + 1) & (d - 1);
	*n     = (unsigned)out_n;
	*imm_s = (unsigned)(((2 * -d) | (s - 1)) & 0x3f);
	*imm_r = (unsigned)r;
	return 1;
}

int a64_is_logical_imm(u64 imm, unsigned width)
{
	unsigned n, s, r;
	return logical_imm(imm, width, &n, &s, &r);
}

static int count_clear_halfwords(u64 imm, unsigned reg_size)
{
	int count = 0;
	unsigned i;
	for (i = 0; i < reg_size / 16; i++)
		if (((imm >> (16 * i)) & 0xffff) == 0)
			count++;
	return count;
}
static int is_movz(u64 imm, unsigned reg_size) { return count_clear_halfwords(imm, reg_size) >= (int)(reg_size / 16) - 1; }

/* Scratch registers */

static unsigned acquire_code(u32* list)
{
	unsigned i;
	for (i = 0; i < 32; i++)
		if (*list & (1u << i))
		{
			*list &= ~(1u << i);
			return i;
		}
	return 31;
}

a64r a64_acquire_x(a64_emit* e)
{
	if (!e->scratch)
		e->error = 1;
	return A64_X(acquire_code(&e->scratch));
}
a64r a64_acquire_w(a64_emit* e)
{
	if (!e->scratch)
		e->error = 1;
	return A64_W(acquire_code(&e->scratch));
}
static a64r acquire_like(a64_emit* e, a64r like) { return is_x(like) ? a64_acquire_x(e) : a64_acquire_w(e); }
void a64_release(a64_emit* e, a64r r)
{
	if (is_v(r))
		e->vscratch |= 1u << rc(r);
	else if (!is_zr(r) && !is_sp(r))
		e->scratch |= 1u << rc(r);
}
static void scratch_include(a64_emit* e, a64r r)
{
	if (!is_zr(r) && !is_sp(r))
		e->scratch |= 1u << rc(r);
}
static void scratch_exclude(a64_emit* e, a64r r)
{
	if (!is_sp(r))
		e->scratch &= ~(1u << rc(r));
}

/* Encoders */

static void move_wide(a64_emit* e, a64r rd, u64 imm, int shift, u32 op)
{
	if (!is_x(rd))
		imm &= 0xffffffffu;
	if (shift >= 0)
		shift /= 16;
	else
	{
		shift = 0;
		if ((imm & 0xffffffffffff0000ull) == 0)
			;
		else if ((imm & 0xffffffff0000ffffull) == 0)
		{
			imm >>= 16;
			shift = 1;
		}
		else if ((imm & 0xffff0000ffffffffull) == 0)
		{
			imm >>= 32;
			shift = 2;
		}
		else if ((imm & 0x0000ffffffffffffull) == 0)
		{
			imm >>= 48;
			shift = 3;
		}
	}
	emit(e, sf(rd) | 0x12800000u | op | ((u32)shift << 21) | (((u32)imm & 0xffff) << 5) | rc(rd));
}
#define MOVN 0x00000000u
#define MOVZ 0x40000000u
#define MOVK 0x60000000u

void a64_movz(a64_emit* e, a64r rd, u64 imm, int shift) { move_wide(e, rd, imm, shift, MOVZ); }
void a64_movn(a64_emit* e, a64r rd, u64 imm, int shift) { move_wide(e, rd, imm, shift, MOVN); }
void a64_movk(a64_emit* e, a64r rd, u64 imm, int shift) { move_wide(e, rd, imm, shift, MOVK); }

/* Logical ops (opc in 29..30), NOT (N bit) */
#define LOG_AND  0x00000000u
#define LOG_ORR  0x20000000u
#define LOG_EOR  0x40000000u
#define LOG_ANDS 0x60000000u
#define LOG_NOT  0x00200000u

static void logical_imm_enc(a64_emit* e, a64r rd, a64r rn, unsigned n, unsigned s, unsigned r, u32 op)
{
	emit(e, sf(rd) | 0x12000000u | op | ((u32)n << 22) | ((u32)r << 16) | ((u32)s << 10) | (rc(rn) << 5) | rc(rd));
}

static void logical_shift_enc(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned shift, unsigned amount, u32 op)
{
	emit(e, sf(rd) | 0x0A000000u | op | ((u32)shift << 22) | (rc(rm) << 16) | ((u32)amount << 10) | (rc(rn) << 5) | rc(rd));
}

/* Add/sub (op: sub bit 30, S bit 29) */
#define AS_ADD 0x00000000u
#define AS_SUB 0x40000000u
#define AS_S   0x20000000u

static void addsub_imm_enc(a64_emit* e, a64r rd, a64r rn, s64 imm, u32 op)
{
	u32 enc = is_uint(imm, 12) ? ((u32)imm << 10) : ((((u32)(imm >> 12)) << 10) | 0x00400000u);
	emit(e, sf(rd) | 0x11000000u | op | enc | (rc(rn) << 5) | rc(rd));
}
static void addsub_ext_enc(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned ext, unsigned amount, u32 op)
{
	emit(e, sf(rd) | 0x0B200000u | op | (rc(rm) << 16) | ((u32)ext << 13) | ((u32)amount << 10) | (rc(rn) << 5) | rc(rd));
}
static void addsub_shift_enc(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned shift, unsigned amount, u32 op)
{
	if (is_sp(rn) || is_sp(rd))
		addsub_ext_enc(e, rd, rn, rm, is_x(rm) ? A64_UXTX : A64_UXTW, amount, op);
	else
		emit(e, sf(rd) | 0x0B000000u | op | ((u32)shift << 22) | (rc(rm) << 16) | ((u32)amount << 10) | (rc(rn) << 5) | rc(rd));
}

static void bitfield(a64_emit* e, a64r rd, a64r rn, unsigned immr, unsigned imms, u32 op)
{
	u32 n = is_x(rd) ? 0x00400000u : 0u;
	emit(e, sf(rd) | op | n | ((u32)immr << 16) | ((u32)imms << 10) | (rc(rn) << 5) | rc(rd));
}
#define SBFM 0x13000000u
#define BFM  0x33000000u
#define UBFM 0x53000000u

static void lsl_enc(a64_emit* e, a64r rd, a64r rn, unsigned shift)
{
	unsigned size = rbits(rd);
	bitfield(e, rd, rn, (size - shift) % size, size - shift - 1, UBFM);
}
static void lsr_enc(a64_emit* e, a64r rd, a64r rn, unsigned shift) { bitfield(e, rd, rn, shift, rbits(rd) - 1, UBFM); }
static void asr_enc(a64_emit* e, a64r rd, a64r rn, unsigned shift) { bitfield(e, rd, rn, shift, rbits(rd) - 1, SBFM); }
static void ror_enc(a64_emit* e, a64r rd, a64r rn, unsigned shift)
{
	u32 n = is_x(rd) ? 0x00400000u : 0u;
	emit(e, sf(rd) | 0x13800000u | n | (rc(rn) << 16) | ((u32)shift << 10) | (rc(rn) << 5) | rc(rd));
}

static void emit_shift(a64_emit* e, a64r rd, a64r rn, unsigned shift, unsigned amount)
{
	switch (shift)
	{
		case A64_LSL: lsl_enc(e, rd, rn, amount); break;
		case A64_LSR: lsr_enc(e, rd, rn, amount); break;
		case A64_ASR: asr_enc(e, rd, rn, amount); break;
		default: ror_enc(e, rd, rn, amount); break;
	}
}

static void emit_extend_shift(a64_emit* e, a64r rd, a64r rn, unsigned ext, unsigned left_shift)
{
	unsigned reg_size       = rbits(rd);
	a64r rn_                = same_size(rd, rc(rn));
	unsigned high_bit       = (8u << (ext & 3)) - 1;
	unsigned non_shift_bits = (reg_size - left_shift) & (reg_size - 1);
	if (non_shift_bits > high_bit || non_shift_bits == 0)
	{
		switch (ext)
		{
			case A64_UXTB: case A64_UXTH: case A64_UXTW:
				bitfield(e, rd, rn_, non_shift_bits, high_bit, UBFM);
				break;
			case A64_SXTB: case A64_SXTH: case A64_SXTW:
				bitfield(e, rd, rn_, non_shift_bits, high_bit, SBFM);
				break;
			default:
				lsl_enc(e, rd, rn_, left_shift);
				break;
		}
	}
	else
		lsl_enc(e, rd, rn_, left_shift);
}

/* Register moves (vixl mov: add #0 with sp, orr otherwise) */
static void mov_enc(a64_emit* e, a64r rd, a64r rn)
{
	if (is_sp(rd) || is_sp(rn))
		addsub_imm_enc(e, rd, rn, 0, AS_ADD);
	else
		logical_shift_enc(e, rd, zr_for(rd), rn, A64_LSL, 0, LOG_ORR);
}

/* Immediate moves */

static int one_instr_mov(a64_emit* e, a64r rd, u64 imm)
{
	unsigned n, s, r;
	unsigned size = rbits(rd);
	if (is_movz(imm, size) && !is_sp(rd))
	{
		move_wide(e, rd, imm, -1, MOVZ);
		return 1;
	}
	if (is_movz(~imm, size) && !is_sp(rd))
	{
		move_wide(e, rd, is_x(rd) ? ~imm : (~imm & 0xffffffffu), -1, MOVN);
		return 1;
	}
	if (logical_imm(imm, size, &n, &s, &r))
	{
		logical_imm_enc(e, rd, zr_for(rd), n, s, r, LOG_ORR);
		return 1;
	}
	return 0;
}

static int one_instr_possible(a64r rd, u64 imm)
{
	unsigned size = rbits(rd);
	if ((is_movz(imm, size) || is_movz(~imm, size)) && !is_sp(rd))
		return 1;
	return a64_is_logical_imm(imm, size);
}

void a64_mov_imm(a64_emit* e, a64r rd, u64 imm)
{
	unsigned size = rbits(rd);
	u64 ignored   = 0;
	int invert    = 0;
	int first     = 1;
	a64r temp     = rd;
	u32 saved     = e->scratch;
	unsigned i;

	if (one_instr_mov(e, rd, imm))
		return;
	if (count_clear_halfwords(~imm, size) > count_clear_halfwords(imm, size))
	{
		ignored = 0xffff;
		invert  = 1;
	}
	if (is_sp(rd))
		temp = acquire_like(e, rd);
	for (i = 0; i < size / 16; i++)
	{
		u64 imm16 = (imm >> (16 * i)) & 0xffff;
		if (imm16 == ignored)
			continue;
		if (first)
		{
			if (invert)
				move_wide(e, temp, ~imm16 & 0xffff, (int)(16 * i), MOVN);
			else
				move_wide(e, temp, imm16, (int)(16 * i), MOVZ);
			first = 0;
		}
		else
			move_wide(e, temp, imm16, (int)(16 * i), MOVK);
	}
	if (is_sp(rd))
		mov_enc(e, rd, temp);
	e->scratch = saved;
}

/* vixl's MoveImmediateForShiftedOp: dst gets imm, or imm pre-shifted for
 * the operation to undo; returns the operand's shift and amount. */
enum { PRESHIFT_NONE, PRESHIFT_SP, PRESHIFT_ANY };

static void mov_for_shifted_op(a64_emit* e, a64r dst, u64 imm, int mode, unsigned* shift, unsigned* amount)
{
	unsigned size = rbits(dst);
	int shift_low, shift_high;
	u64 imm_low, imm_high;

	*shift  = A64_LSL;
	*amount = 0;
	if (one_instr_mov(e, dst, imm))
		return;
	shift_low = ctzw(imm, size);
	if (mode == PRESHIFT_SP && shift_low > 4)
		shift_low = 4;
	imm_low    = (u64)((s64)imm >> shift_low);
	shift_high = clzw(imm, size);
	imm_high   = (imm << shift_high) | (shift_high ? (((u64)1 << shift_high) - 1) : 0);
	if (mode != PRESHIFT_NONE && one_instr_possible(dst, imm_low))
	{
		one_instr_mov(e, dst, imm_low);
		*amount = (unsigned)shift_low;
	}
	else if (mode == PRESHIFT_ANY && one_instr_possible(dst, imm_high))
	{
		one_instr_mov(e, dst, imm_high);
		*shift  = A64_LSR;
		*amount = (unsigned)shift_high;
	}
	else
		a64_mov_imm(e, dst, imm);
}

void a64_mov(a64_emit* e, a64r rd, a64r rn)
{
	if (is_v(rd))
	{
		/* orr vd.16b, vn.16b, vn.16b (8b for D) */
		u32 q = visq(rd) ? NEON_Q : 0u;
		emit(e, q | 0x0EA01C00u | (rc(rn) << 16) | (rc(rn) << 5) | rc(rd));
		return;
	}
	if (!same(rd, rn) || !is_x(rd))
		mov_enc(e, rd, rn);
}

void a64_mvn(a64_emit* e, a64r rd, a64r rn)
{
	if (is_v(rd))
	{
		emit(e, (visq(rd) ? NEON_Q : 0u) | 0x2E205800u | (rc(rn) << 5) | rc(rd));
		return;
	}
	logical_shift_enc(e, rd, zr_for(rd), rn, A64_LSL, 0, LOG_ORR | LOG_NOT);
}

/* Logical macro */

static void logical_macro_imm(a64_emit* e, a64r rd, a64r rn, u64 imm, u32 op)
{
	u32 saved = e->scratch;
	unsigned n, s, r;

	scratch_include(e, rd);
	scratch_exclude(e, rn);
	if (op & LOG_NOT)
	{
		op &= ~LOG_NOT;
		imm = ~imm;
	}
	if (!is_x(rd))
		imm &= 0xffffffffu;
	if (imm == 0 && op != LOG_ANDS)
	{
		if (op == LOG_AND)
			a64_mov_imm(e, rd, 0);
		else
			a64_mov(e, rd, rn);
		e->scratch = saved;
		return;
	}
	if (op != LOG_ANDS && ((is_x(rd) && imm == ~(u64)0) || (!is_x(rd) && imm == 0xffffffffu)))
	{
		if (op == LOG_AND)
			a64_mov(e, rd, rn);
		else if (op == LOG_ORR)
			a64_mov_imm(e, rd, imm);
		else
			a64_mvn(e, rd, rn);
		e->scratch = saved;
		return;
	}
	if (logical_imm(imm, rbits(rd), &n, &s, &r))
		logical_imm_enc(e, rd, rn, n, s, r, op);
	else
	{
		a64r temp = acquire_like(e, rn);
		unsigned shift, amount;
		mov_for_shifted_op(e, temp, imm, is_sp(rn) ? PRESHIFT_NONE : PRESHIFT_ANY, &shift, &amount);
		if (is_sp(rd))
		{
			logical_shift_enc(e, temp, rn, temp, shift, amount, op);
			a64_mov(e, rd, temp);
		}
		else
			logical_shift_enc(e, rd, rn, temp, shift, amount, op);
	}
	e->scratch = saved;
}

static void neon_logical(a64_emit* e, a64r vd, a64r vn, a64r vm, u32 op)
{
	emit(e, (visq(vd) ? NEON_Q : 0u) | op | (rc(vm) << 16) | (rc(vn) << 5) | rc(vd));
}

void a64_and(a64_emit* e, a64r rd, a64r rn, a64r rm)
{
	if (is_v(rd))
		neon_logical(e, rd, rn, rm, 0x0E201C00u);
	else
		logical_shift_enc(e, rd, rn, rm, A64_LSL, 0, LOG_AND);
}
void a64_and_imm(a64_emit* e, a64r rd, a64r rn, u64 imm) { logical_macro_imm(e, rd, rn, imm, LOG_AND); }
void a64_and_shift(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned shift, unsigned amount) { logical_shift_enc(e, rd, rn, rm, shift, amount, LOG_AND); }
void a64_ands(a64_emit* e, a64r rd, a64r rn, a64r rm) { logical_shift_enc(e, rd, rn, rm, A64_LSL, 0, LOG_ANDS); }
void a64_ands_imm(a64_emit* e, a64r rd, a64r rn, u64 imm) { logical_macro_imm(e, rd, rn, imm, LOG_ANDS); }
void a64_orr(a64_emit* e, a64r rd, a64r rn, a64r rm)
{
	if (is_v(rd))
		neon_logical(e, rd, rn, rm, 0x0EA01C00u);
	else
		logical_shift_enc(e, rd, rn, rm, A64_LSL, 0, LOG_ORR);
}
void a64_orr_imm(a64_emit* e, a64r rd, a64r rn, u64 imm) { logical_macro_imm(e, rd, rn, imm, LOG_ORR); }
void a64_orr_shift(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned shift, unsigned amount) { logical_shift_enc(e, rd, rn, rm, shift, amount, LOG_ORR); }
void a64_eor(a64_emit* e, a64r rd, a64r rn, a64r rm)
{
	if (is_v(rd))
		neon_logical(e, rd, rn, rm, 0x2E201C00u);
	else
		logical_shift_enc(e, rd, rn, rm, A64_LSL, 0, LOG_EOR);
}
void a64_eor_imm(a64_emit* e, a64r rd, a64r rn, u64 imm) { logical_macro_imm(e, rd, rn, imm, LOG_EOR); }
void a64_eor_shift(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned shift, unsigned amount) { logical_shift_enc(e, rd, rn, rm, shift, amount, LOG_EOR); }
void a64_bic(a64_emit* e, a64r rd, a64r rn, a64r rm)
{
	if (is_v(rd))
		neon_logical(e, rd, rn, rm, 0x0E601C00u);
	else
		logical_shift_enc(e, rd, rn, rm, A64_LSL, 0, LOG_AND | LOG_NOT);
}
void a64_bic_imm(a64_emit* e, a64r rd, a64r rn, u64 imm) { logical_macro_imm(e, rd, rn, imm, LOG_AND | LOG_NOT); }
void a64_tst(a64_emit* e, a64r rn, a64r rm) { a64_ands(e, zr_for(rn), rn, rm); }
void a64_tst_imm(a64_emit* e, a64r rn, u64 imm) { a64_ands_imm(e, zr_for(rn), rn, imm); }

/* Add/sub macro */

enum { OPND_IMM, OPND_SHIFT, OPND_EXT };

typedef struct opnd
{
	int kind;
	s64 imm;
	a64r reg;
	unsigned shift; /* or extend */
	unsigned amount;
} opnd;

static void addsub(a64_emit* e, a64r rd, a64r rn, const opnd* o, u32 op)
{
	if (o->kind == OPND_IMM)
		addsub_imm_enc(e, rd, rn, o->imm, op);
	else if (o->kind == OPND_SHIFT)
		addsub_shift_enc(e, rd, rn, o->reg, o->shift, o->amount, op);
	else
		addsub_ext_enc(e, rd, rn, o->reg, o->shift, o->amount, op);
}

static void addsub_macro(a64_emit* e, a64r rd, a64r rn, const opnd* o, u32 op)
{
	int zero = (o->kind == OPND_IMM) ? (o->imm == 0) : (o->kind == OPND_SHIFT && is_zr(o->reg));
	if (zero && same(rd, rn) && is_x(rd) && !(op & AS_S))
		return;
	if ((o->kind == OPND_IMM && !a64_is_addsub_imm(o->imm)) || (is_zr(rn) && o->kind != OPND_SHIFT) ||
		(o->kind == OPND_SHIFT && o->shift == A64_ROR))
	{
		u32 saved = e->scratch;
		a64r temp;
		opnd t;
		scratch_include(e, rd);
		scratch_exclude(e, rn);
		temp     = acquire_like(e, rn);
		t.kind   = OPND_SHIFT;
		t.reg    = temp;
		t.imm    = 0;
		t.shift  = A64_LSL;
		t.amount = 0;
		if (o->kind == OPND_IMM)
		{
			int mode = PRESHIFT_ANY;
			if (is_sp(rd))
				mode = (op & AS_S) ? PRESHIFT_NONE : PRESHIFT_SP;
			else if (is_sp(rn))
				mode = PRESHIFT_SP;
			mov_for_shifted_op(e, temp, (u64)o->imm, mode, &t.shift, &t.amount);
		}
		else if (o->kind == OPND_SHIFT && o->amount != 0)
			emit_shift(e, temp, o->reg, o->shift, o->amount);
		else if (o->kind == OPND_EXT)
			emit_extend_shift(e, temp, o->reg, o->shift, o->amount);
		else
			a64_mov(e, temp, o->reg);
		addsub(e, rd, rn, &t, op);
		e->scratch = saved;
	}
	else
		addsub(e, rd, rn, o, op);
}

static int can_negate(s64 imm) { return imm != (s64)((u64)1 << 63); }

static void add_imm_flags(a64_emit* e, a64r rd, a64r rn, s64 imm, u32 op)
{
	opnd o;
	o.kind   = OPND_IMM;
	o.reg    = a64_noreg();
	o.shift  = 0;
	o.amount = 0;
	if (imm < 0 && can_negate(imm) && a64_is_addsub_imm(-imm))
	{
		o.imm = -imm;
		addsub_macro(e, rd, rn, &o, op ^ AS_SUB);
	}
	else
	{
		o.imm = imm;
		addsub_macro(e, rd, rn, &o, op);
	}
}

static void addsub_reg(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned kind, unsigned shift, unsigned amount, u32 op)
{
	opnd o;
	o.kind   = (int)kind;
	o.imm    = 0;
	o.reg    = rm;
	o.shift  = shift;
	o.amount = amount;
	addsub_macro(e, rd, rn, &o, op);
}

static void neon3same(a64_emit* e, a64r vd, a64r vn, a64r vm, u32 op)
{
	u32 format;
	if (vscalar(vd))
	{
		op |= NEON_Q | NEON_SCALAR;
		format = sformat(vd);
	}
	else
		format = vformat(vd);
	emit(e, format | op | (rc(vm) << 16) | (rc(vn) << 5) | rc(vd));
}

void a64_add(a64_emit* e, a64r rd, a64r rn, a64r rm)
{
	if (is_v(rd))
		neon3same(e, rd, rn, rm, 0x0E208400u);
	else
		addsub_reg(e, rd, rn, rm, OPND_SHIFT, A64_LSL, 0, AS_ADD);
}
void a64_add_imm(a64_emit* e, a64r rd, a64r rn, s64 imm) { add_imm_flags(e, rd, rn, imm, AS_ADD); }
void a64_add_shift(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned shift, unsigned amount) { addsub_reg(e, rd, rn, rm, OPND_SHIFT, shift, amount, AS_ADD); }
void a64_add_ext(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned ext, unsigned amount) { addsub_reg(e, rd, rn, rm, OPND_EXT, ext, amount, AS_ADD); }
void a64_adds(a64_emit* e, a64r rd, a64r rn, a64r rm) { addsub_reg(e, rd, rn, rm, OPND_SHIFT, A64_LSL, 0, AS_ADD | AS_S); }
void a64_adds_imm(a64_emit* e, a64r rd, a64r rn, s64 imm) { add_imm_flags(e, rd, rn, imm, AS_ADD | AS_S); }
void a64_sub(a64_emit* e, a64r rd, a64r rn, a64r rm)
{
	if (is_v(rd))
		neon3same(e, rd, rn, rm, 0x2E208400u);
	else
		addsub_reg(e, rd, rn, rm, OPND_SHIFT, A64_LSL, 0, AS_SUB);
}
void a64_sub_imm(a64_emit* e, a64r rd, a64r rn, s64 imm) { add_imm_flags(e, rd, rn, imm, AS_SUB); }
void a64_sub_shift(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned shift, unsigned amount) { addsub_reg(e, rd, rn, rm, OPND_SHIFT, shift, amount, AS_SUB); }
void a64_subs(a64_emit* e, a64r rd, a64r rn, a64r rm) { addsub_reg(e, rd, rn, rm, OPND_SHIFT, A64_LSL, 0, AS_SUB | AS_S); }
void a64_subs_imm(a64_emit* e, a64r rd, a64r rn, s64 imm) { add_imm_flags(e, rd, rn, imm, AS_SUB | AS_S); }
void a64_cmp(a64_emit* e, a64r rn, a64r rm) { a64_subs(e, zr_for(rn), rn, rm); }
void a64_cmp_imm(a64_emit* e, a64r rn, s64 imm) { a64_subs_imm(e, zr_for(rn), rn, imm); }
void a64_cmp_shift(a64_emit* e, a64r rn, a64r rm, unsigned shift, unsigned amount) { addsub_reg(e, zr_for(rn), rn, rm, OPND_SHIFT, shift, amount, AS_SUB | AS_S); }
void a64_cmn(a64_emit* e, a64r rn, a64r rm) { a64_adds(e, zr_for(rn), rn, rm); }
void a64_cmn_imm(a64_emit* e, a64r rn, s64 imm) { a64_adds_imm(e, zr_for(rn), rn, imm); }
void a64_neg(a64_emit* e, a64r rd, a64r rm) { addsub_reg(e, rd, zr_for(rd), rm, OPND_SHIFT, A64_LSL, 0, AS_SUB); }

/* Shifts, bitfields */

static void dp2(a64_emit* e, a64r rd, a64r rn, a64r rm, u32 op)
{
	emit(e, sf(rd) | op | (rc(rm) << 16) | (rc(rn) << 5) | rc(rd));
}
void a64_lsl(a64_emit* e, a64r rd, a64r rn, a64r rm) { dp2(e, rd, rn, rm, 0x1AC02000u); }
void a64_lsr(a64_emit* e, a64r rd, a64r rn, a64r rm) { dp2(e, rd, rn, rm, 0x1AC02400u); }
void a64_asr(a64_emit* e, a64r rd, a64r rn, a64r rm) { dp2(e, rd, rn, rm, 0x1AC02800u); }
void a64_udiv(a64_emit* e, a64r rd, a64r rn, a64r rm) { dp2(e, rd, rn, rm, 0x1AC00800u); }
void a64_sdiv(a64_emit* e, a64r rd, a64r rn, a64r rm) { dp2(e, rd, rn, rm, 0x1AC00C00u); }
void a64_lsl_imm(a64_emit* e, a64r rd, a64r rn, unsigned shift) { lsl_enc(e, rd, rn, shift); }
void a64_lsr_imm(a64_emit* e, a64r rd, a64r rn, unsigned shift) { lsr_enc(e, rd, rn, shift); }
void a64_asr_imm(a64_emit* e, a64r rd, a64r rn, unsigned shift) { asr_enc(e, rd, rn, shift); }
void a64_ubfx(a64_emit* e, a64r rd, a64r rn, unsigned lsb, unsigned width) { bitfield(e, rd, rn, lsb, lsb + width - 1, UBFM); }
void a64_bfi(a64_emit* e, a64r rd, a64r rn, unsigned lsb, unsigned width)
{
	unsigned size = rbits(rd);
	bitfield(e, rd, rn, (size - lsb) & (size - 1), width - 1, BFM);
}
void a64_sxtb(a64_emit* e, a64r rd, a64r rn) { bitfield(e, rd, rn, 0, 7, SBFM); }
void a64_sxth(a64_emit* e, a64r rd, a64r rn) { bitfield(e, rd, rn, 0, 15, SBFM); }
void a64_sxtw(a64_emit* e, a64r rd, a64r rn) { bitfield(e, rd, rn, 0, 31, SBFM); }
void a64_uxtb(a64_emit* e, a64r rd, a64r rn) { bitfield(e, rd, rn, 0, 7, UBFM); }
void a64_uxth(a64_emit* e, a64r rd, a64r rn) { bitfield(e, rd, rn, 0, 15, UBFM); }

/* Multiply, misc */

static void dp3(a64_emit* e, a64r rd, a64r rn, a64r rm, a64r ra, u32 op)
{
	emit(e, op | (rc(rm) << 16) | (rc(ra) << 10) | (rc(rn) << 5) | rc(rd));
}
void a64_mul(a64_emit* e, a64r rd, a64r rn, a64r rm) { dp3(e, rd, rn, rm, A64_XZR, sf(rd) | 0x1B000000u); }
void a64_msub(a64_emit* e, a64r rd, a64r rn, a64r rm, a64r ra) { dp3(e, rd, rn, rm, ra, sf(rd) | 0x1B008000u); }
void a64_smull(a64_emit* e, a64r rd, a64r rn, a64r rm) { dp3(e, rd, rn, rm, A64_XZR, 0x9B200000u); }
void a64_umull(a64_emit* e, a64r rd, a64r rn, a64r rm)
{
	if (is_v(rd))
		emit(e, vformat(rn) | 0x2E20C000u | (rc(rm) << 16) | (rc(rn) << 5) | rc(rd));
	else
		dp3(e, rd, rn, rm, A64_XZR, 0x9BA00000u);
}
void a64_cls(a64_emit* e, a64r rd, a64r rn) { emit(e, sf(rd) | 0x5AC01400u | (rc(rn) << 5) | rc(rd)); }

static void neon2misc(a64_emit* e, a64r vd, a64r vn, u32 op)
{
	u32 format;
	if (vscalar(vd))
	{
		op |= NEON_Q | NEON_SCALAR;
		format = sformat(vd);
	}
	else
		format = vformat(vd);
	emit(e, format | op | (rc(vn) << 5) | rc(vd));
}

void a64_rev64(a64_emit* e, a64r rd, a64r rn)
{
	if (is_v(rd))
		neon2misc(e, rd, rn, 0x0E200800u);
	else
		emit(e, 0xDAC00C00u | (rc(rn) << 5) | rc(rd));
}

static void condsel(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned cond, u32 op)
{
	emit(e, sf(rd) | op | (rc(rm) << 16) | ((u32)cond << 12) | (rc(rn) << 5) | rc(rd));
}
/* vixl's CselHelper: a zero register is an immediate 0 */
void a64_csel(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned cond)
{
	if (is_zr(rn) && is_zr(rm))
		a64_mov_imm(e, rd, 0);
	else if (is_zr(rm))
		condsel(e, rd, rn, zr_for(rd), cond, 0x1A800000u);
	else if (is_zr(rn))
		condsel(e, rd, rm, zr_for(rd), A64_INVERT(cond), 0x1A800000u);
	else if (same(rn, rm))
		a64_mov(e, rd, rn);
	else
		condsel(e, rd, rn, rm, cond, 0x1A800000u);
}
void a64_csinc(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned cond) { condsel(e, rd, rn, rm, cond, 0x1A800400u); }
void a64_csinv(a64_emit* e, a64r rd, a64r rn, a64r rm, unsigned cond) { condsel(e, rd, rn, rm, cond, 0x5A800000u); }
void a64_cset(a64_emit* e, a64r rd, unsigned cond) { a64_csinc(e, rd, zr_for(rd), zr_for(rd), A64_INVERT(cond)); }

/* Branches */

enum { BR_UNCOND, BR_COND, BR_COMPARE, BR_TEST };

static int branch_kind(u32 w)
{
	if ((w & 0x7C000000u) == 0x14000000u)
		return BR_UNCOND;
	if ((w & 0xFF000010u) == 0x54000000u)
		return BR_COND;
	if ((w & 0x7E000000u) == 0x34000000u)
		return BR_COMPARE;
	return BR_TEST;
}

static int branch_fits(int kind, s64 disp)
{
	switch (kind)
	{
		case BR_UNCOND: return a64_is_int(disp, 26);
		case BR_TEST: return a64_is_int(disp, 14);
		default: return a64_is_int(disp, 19);
	}
}

static u32 branch_set(u32 w, s64 disp)
{
	switch (branch_kind(w))
	{
		case BR_UNCOND: return (w & ~0x03FFFFFFu) | ((u32)disp & 0x03FFFFFFu);
		case BR_TEST: return (w & ~(0x3FFFu << 5)) | (((u32)disp & 0x3FFFu) << 5);
		default: return (w & ~(0x7FFFFu << 5)) | (((u32)disp & 0x7FFFFu) << 5);
	}
}

static s64 branch_get(u32 w)
{
	switch (branch_kind(w))
	{
		case BR_UNCOND: return (s64)(w & 0x03FFFFFFu);
		case BR_TEST: return (s64)((w >> 5) & 0x3FFFu);
		default: return (s64)((w >> 5) & 0x7FFFFu);
	}
}

/* Emit w branching to l: resolved now if l is bound, else linked (its
 * immediate holds the distance back to the previous link, 0 at the end) */
static void branch_to(a64_emit* e, a64_label* l, u32 w, int kind)
{
	s64 disp;
	if (l->pos > 0)
	{
		disp = ((s64)l->pos - 1 - (s64)e->pos) / 4;
		if (!branch_fits(kind, disp))
			e->error = 1;
		emit(e, branch_set(w, disp));
		return;
	}
	disp = (l->link > 0) ? ((s64)e->pos - ((s64)l->link - 1)) / 4 : 0;
	if (!branch_fits(kind, disp))
		e->error = 1;
	if (e->pos + 4 <= e->cap)
		l->link = (s32)e->pos + 1;
	emit(e, branch_set(w, disp));
}

static int bound_out_of_range(const a64_emit* e, const a64_label* l, int kind)
{
	return l->pos > 0 && !branch_fits(kind, ((s64)l->pos - 1 - (s64)e->pos) / 4);
}

void a64_bind(a64_emit* e, a64_label* l)
{
	s32 at = l->link - 1;
	while (at >= 0)
	{
		u32 w    = peek(e, (size_t)at);
		s64 prev = branch_get(w);
		s64 disp = ((s64)e->pos - (s64)at) / 4;
		if (!branch_fits(branch_kind(w), disp))
			e->error = 1;
		poke(e, (size_t)at, branch_set(w, disp));
		at = prev ? (s32)(at - prev * 4) : -1;
	}
	l->link = 0;
	l->pos  = (s32)e->pos + 1;
}

void a64_b(a64_emit* e, a64_label* l) { branch_to(e, l, 0x14000000u, BR_UNCOND); }

void a64_b_cond(a64_emit* e, a64_label* l, unsigned cond)
{
	if (bound_out_of_range(e, l, BR_COND))
	{
		a64_label done = A64_LABEL_INIT;
		branch_to(e, &done, 0x54000000u | A64_INVERT(cond), BR_COND);
		a64_b(e, l);
		a64_bind(e, &done);
		return;
	}
	branch_to(e, l, 0x54000000u | cond, BR_COND);
}

static void cb(a64_emit* e, a64r rt, a64_label* l, u32 op)
{
	if (bound_out_of_range(e, l, BR_COMPARE))
	{
		a64_label done = A64_LABEL_INIT;
		branch_to(e, &done, sf(rt) | (op ^ 0x01000000u) | rc(rt), BR_COMPARE);
		a64_b(e, l);
		a64_bind(e, &done);
		return;
	}
	branch_to(e, l, sf(rt) | op | rc(rt), BR_COMPARE);
}
void a64_cbz(a64_emit* e, a64r rt, a64_label* l) { cb(e, rt, l, 0x34000000u); }
void a64_cbnz(a64_emit* e, a64r rt, a64_label* l) { cb(e, rt, l, 0x35000000u); }

static void tb(a64_emit* e, a64r rt, unsigned bit, a64_label* l, u32 op)
{
	u32 b = ((u32)(bit & 0x20) << 26) | ((u32)(bit & 0x1f) << 19);
	if (bound_out_of_range(e, l, BR_TEST))
	{
		a64_label done = A64_LABEL_INIT;
		branch_to(e, &done, (op ^ 0x01000000u) | b | rc(rt), BR_TEST);
		a64_b(e, l);
		a64_bind(e, &done);
		return;
	}
	branch_to(e, l, op | b | rc(rt), BR_TEST);
}
void a64_tbz(a64_emit* e, a64r rt, unsigned bit, a64_label* l) { tb(e, rt, bit, l, 0x36000000u); }
void a64_tbnz(a64_emit* e, a64r rt, unsigned bit, a64_label* l) { tb(e, rt, bit, l, 0x37000000u); }

void a64_b_disp(a64_emit* e, s64 disp) { emit(e, 0x14000000u | ((u32)disp & 0x03FFFFFFu)); }
void a64_bl_disp(a64_emit* e, s64 disp) { emit(e, 0x94000000u | ((u32)disp & 0x03FFFFFFu)); }
void a64_b_cond_disp(a64_emit* e, s64 disp, unsigned cond) { emit(e, 0x54000000u | (((u32)disp & 0x7FFFFu) << 5) | cond); }
void a64_cbz_disp(a64_emit* e, a64r rt, s64 disp) { emit(e, sf(rt) | 0x34000000u | (((u32)disp & 0x7FFFFu) << 5) | rc(rt)); }
void a64_cbnz_disp(a64_emit* e, a64r rt, s64 disp) { emit(e, sf(rt) | 0x35000000u | (((u32)disp & 0x7FFFFu) << 5) | rc(rt)); }
void a64_br(a64_emit* e, a64r xn) { emit(e, 0xD61F0000u | (rc(xn) << 5)); }
void a64_blr(a64_emit* e, a64r xn) { emit(e, 0xD63F0000u | (rc(xn) << 5)); }
void a64_ret(a64_emit* e) { emit(e, 0xD65F03C0u); }
void a64_adrp(a64_emit* e, a64r rd, s64 pages)
{
	emit(e, 0x90000000u | (((u32)pages & 3u) << 29) | ((((u32)(pages >> 2)) & 0x7FFFFu) << 5) | rc(rd));
}
void a64_msr_fpcr(a64_emit* e, a64r rt) { emit(e, 0xD51B4400u | rc(rt)); }

void a64_patch_b(void* code, const void* target)
{
	s64 disp = ((s64)((const u8*)target - (const u8*)code)) / 4;
	u32 w    = 0x14000000u | ((u32)disp & 0x03FFFFFFu);
	memcpy(code, &w, 4);
}

/* Loads and stores: op is size<<30 | V<<26 | opc<<22 */

#define LS_UNSIGNED 0x39000000u
#define LS_UNSCALED 0x38000000u
#define LS_POST     0x38000400u
#define LS_PRE      0x38000C00u
#define LS_REG      0x38200800u

static unsigned ls_size(u32 op)
{
	if ((op & 0x04000000u) && (op & 0x00800000u))
		return 4;
	return (unsigned)(op >> 30);
}

static int is_ls_scaled(s64 offset, unsigned size)
{
	s64 unit = (s64)1 << size;
	return (offset % unit) == 0 && is_uint(offset / unit, 12);
}

static void ls_enc(a64_emit* e, a64r rt, const a64_mem* m, u32 op)
{
	unsigned size = ls_size(op);
	u32 w         = op | rc(rt) | (rc(m->base) << 5);
	s64 off       = m->offset;
	switch (m->mode)
	{
		case A64_OFFSET:
			if (is_ls_scaled(off, size))
				w |= LS_UNSIGNED | ((u32)(off >> size) << 10);
			else
				w |= LS_UNSCALED | (((u32)off & 0x1FFu) << 12);
			break;
		case A64_PREINDEX: w |= LS_PRE | (((u32)off & 0x1FFu) << 12); break;
		case A64_POSTINDEX: w |= LS_POST | (((u32)off & 0x1FFu) << 12); break;
		default:
			w |= LS_REG | (rc(m->index) << 16) | ((u32)(m->ext == A64_NOEXT ? A64_UXTX : m->ext) << 13) |
				(m->amount ? 0x1000u : 0u);
			break;
	}
	emit(e, w);
}

static void ls_macro(a64_emit* e, a64r rt, a64_mem m, u32 op)
{
	s64 off       = m.offset;
	unsigned size = ls_size(op);
	if (m.mode == A64_OFFSET && !is_ls_scaled(off, size) && !a64_is_int(off, 9))
	{
		u32 saved = e->scratch;
		a64r temp = acquire_like(e, m.base);
		a64_mem r;
		a64_mov_imm(e, temp, (u64)off);
		r = a64_mem_reg(m.base, temp, 0);
		ls_enc(e, rt, &r, op);
		e->scratch = saved;
	}
	else if (m.mode == A64_POSTINDEX && !a64_is_int(off, 9))
	{
		a64_mem r = a64_mem_off(m.base, 0);
		ls_enc(e, rt, &r, op);
		a64_add_imm(e, m.base, m.base, off);
	}
	else if (m.mode == A64_PREINDEX && !a64_is_int(off, 9))
	{
		a64_mem r = a64_mem_off(m.base, 0);
		a64_add_imm(e, m.base, m.base, off);
		ls_enc(e, rt, &r, op);
	}
	else
		ls_enc(e, rt, &m, op);
}

static u32 ls_op_v(a64r rt, int load)
{
	u32 l = load ? 0x00400000u : 0u;
	switch (rk(rt))
	{
		case A64K_B: return 0x04000000u | l;
		case A64K_H: return 0x44000000u | l;
		case A64K_S: return 0x84000000u | l;
		case A64K_D: case A64K_8B: case A64K_4H: case A64K_2S: return 0xC4000000u | l;
		default: return 0x04800000u | l;
	}
}

void a64_ldr(a64_emit* e, a64r rt, a64_mem m) { ls_macro(e, rt, m, is_v(rt) ? ls_op_v(rt, 1) : (is_x(rt) ? 0xC0400000u : 0x80400000u)); }
void a64_str(a64_emit* e, a64r rt, a64_mem m) { ls_macro(e, rt, m, is_v(rt) ? ls_op_v(rt, 0) : (is_x(rt) ? 0xC0000000u : 0x80000000u)); }
void a64_ldrb(a64_emit* e, a64r rt, a64_mem m) { ls_macro(e, rt, m, 0x00400000u); }
void a64_ldrsb(a64_emit* e, a64r rt, a64_mem m) { ls_macro(e, rt, m, is_x(rt) ? 0x00800000u : 0x00C00000u); }
void a64_strb(a64_emit* e, a64r rt, a64_mem m) { ls_macro(e, rt, m, 0x00000000u); }
void a64_ldrh(a64_emit* e, a64r rt, a64_mem m) { ls_macro(e, rt, m, 0x40400000u); }
void a64_ldrsh(a64_emit* e, a64r rt, a64_mem m) { ls_macro(e, rt, m, is_x(rt) ? 0x40800000u : 0x40C00000u); }
void a64_strh(a64_emit* e, a64r rt, a64_mem m) { ls_macro(e, rt, m, 0x40000000u); }

/* Pairs: op is opc<<30 | V<<26 | L<<22 */
static void lsp_enc(a64_emit* e, a64r rt, a64r rt2, const a64_mem* m, u32 op, unsigned size)
{
	u32 mode = (m->mode == A64_OFFSET) ? 0x29000000u : (m->mode == A64_PREINDEX ? 0x29800000u : 0x28800000u);
	emit(e, op | mode | ((((u32)(m->offset >> size)) & 0x7Fu) << 15) | (rc(rt2) << 10) | (rc(m->base) << 5) | rc(rt));
}

static void lsp_macro(a64_emit* e, a64r rt, a64r rt2, a64_mem m, int load)
{
	u32 op;
	unsigned size;
	s64 unit;
	if (is_v(rt))
	{
		switch (vbits(rt))
		{
			case 32: op = 0x04000000u; size = 2; break;
			case 64: op = 0x44000000u; size = 3; break;
			default: op = 0x84000000u; size = 4; break;
		}
	}
	else if (is_x(rt))
	{
		op   = 0x80000000u;
		size = 3;
	}
	else
	{
		op   = 0x00000000u;
		size = 2;
	}
	if (load)
		op |= 0x00400000u;
	unit = (s64)1 << size;
	if ((m.offset % unit) == 0 && a64_is_int(m.offset / unit, 7))
		lsp_enc(e, rt, rt2, &m, op, size);
	else if (m.mode == A64_OFFSET)
	{
		u32 saved = e->scratch;
		a64r temp = acquire_like(e, m.base);
		a64_mem r = a64_mem_off(temp, 0);
		a64_add_imm(e, temp, m.base, m.offset);
		lsp_enc(e, rt, rt2, &r, op, size);
		e->scratch = saved;
	}
	else if (m.mode == A64_POSTINDEX)
	{
		a64_mem r = a64_mem_off(m.base, 0);
		lsp_enc(e, rt, rt2, &r, op, size);
		a64_add_imm(e, m.base, m.base, m.offset);
	}
	else
	{
		a64_mem r = a64_mem_off(m.base, 0);
		a64_add_imm(e, m.base, m.base, m.offset);
		lsp_enc(e, rt, rt2, &r, op, size);
	}
}
void a64_ldp(a64_emit* e, a64r rt, a64r rt2, a64_mem m) { lsp_macro(e, rt, rt2, m, 1); }
void a64_stp(a64_emit* e, a64r rt, a64r rt2, a64_mem m) { lsp_macro(e, rt, rt2, m, 0); }

void a64_st1_lane(a64_emit* e, a64r vt, unsigned lane, a64_mem m)
{
	unsigned lane_size = lbits(vt) / 8;
	u32 w              = 0x0D000000u;
	u32 l;
	lane *= lane_size;
	if (lane_size == 8)
		lane++;
	l = (u32)lane;
	w |= ((l << 10) & 0x00000C00u) | ((l << 10) & 0x00001000u) | ((l << 27) & NEON_Q);
	switch (lane_size)
	{
		case 1: break;
		case 2: w |= 0x00004000u; break;
		case 4: w |= 0x00008000u; break;
		default: w |= 0x00008400u; break;
	}
	w |= rc(m.base) << 5;
	if (m.mode == A64_POSTINDEX)
		w |= 0x00800000u | (m.offset == 0 ? (rc(m.index) << 16) : (0x1Fu << 16));
	emit(e, w | rc(vt));
}

/* FP */

void a64_fmov(a64_emit* e, a64r rd, a64r rn)
{
	if (is_v(rd) && is_v(rn))
		emit(e, fptype(rd) | 0x1E204000u | (rc(rn) << 5) | rc(rd));
	else if (is_v(rd))
		emit(e, (vbits(rd) == 64 ? 0x9E670000u : 0x1E270000u) | (rc(rn) << 5) | rc(rd));
	else
		emit(e, (vbits(rn) == 64 ? 0x9E660000u : 0x1E260000u) | (rc(rn) << 5) | rc(rd));
}

/* scalar: FP data-processing 2-source; vector: three-same FP */
static void fp3(a64_emit* e, a64r vd, a64r vn, a64r vm, u32 sca, u32 vec)
{
	u32 op;
	if (vscalar(vd))
		op = sca ? (fptype(vd) | sca) : (fpformat(vd) | vec | NEON_Q | NEON_SCALAR);
	else
		op = fpformat(vd) | vec;
	emit(e, op | (rc(vm) << 16) | (rc(vn) << 5) | rc(vd));
}

void a64_fadd(a64_emit* e, a64r vd, a64r vn, a64r vm) { fp3(e, vd, vn, vm, 0x1E202800u, 0x0E20D400u); }
void a64_fsub(a64_emit* e, a64r vd, a64r vn, a64r vm) { fp3(e, vd, vn, vm, 0x1E203800u, 0x0EA0D400u); }
void a64_fmul(a64_emit* e, a64r vd, a64r vn, a64r vm) { fp3(e, vd, vn, vm, 0x1E200800u, 0x2E20DC00u); }
void a64_fdiv(a64_emit* e, a64r vd, a64r vn, a64r vm) { fp3(e, vd, vn, vm, 0x1E201800u, 0x2E20FC00u); }
void a64_fmax(a64_emit* e, a64r vd, a64r vn, a64r vm) { fp3(e, vd, vn, vm, 0x1E204800u, 0x0E20F400u); }
void a64_fmin(a64_emit* e, a64r vd, a64r vn, a64r vm) { fp3(e, vd, vn, vm, 0x1E205800u, 0x0EA0F400u); }
void a64_fmaxnm(a64_emit* e, a64r vd, a64r vn, a64r vm) { fp3(e, vd, vn, vm, 0x1E206800u, 0x0E20C400u); }
void a64_fminnm(a64_emit* e, a64r vd, a64r vn, a64r vm) { fp3(e, vd, vn, vm, 0x1E207800u, 0x0EA0C400u); }
void a64_faddp(a64_emit* e, a64r vd, a64r vn, a64r vm) { fp3(e, vd, vn, vm, 0, 0x2E20D400u); }
void a64_fcmeq(a64_emit* e, a64r vd, a64r vn, a64r vm) { fp3(e, vd, vn, vm, 0, 0x0E20E400u); }
void a64_fcmge(a64_emit* e, a64r vd, a64r vn, a64r vm) { fp3(e, vd, vn, vm, 0, 0x2E20E400u); }

void a64_faddp_scalar(a64_emit* e, a64r vd, a64r vn) { emit(e, fpformat(vd) | 0x7E30D800u | (rc(vn) << 5) | rc(vd)); }

void a64_fmul_elem(a64_emit* e, a64r vd, a64r vn, a64r vm, unsigned index)
{
	u32 op = 0x0F809000u;
	u32 hl;
	if (vbits(vm) == 64)
		hl = (u32)(index & 1) << 11;
	else
		hl = (((u32)(index >> 1) & 1) << 11) | (((u32)index & 1) << 21);
	if (vscalar(vd))
		op |= NEON_Q | NEON_SCALAR;
	op |= fpformat(vd);
	emit(e, op | hl | (rc(vm) << 16) | (rc(vn) << 5) | rc(vd));
}

void a64_fsqrt(a64_emit* e, a64r vd, a64r vn)
{
	if (vscalar(vd))
		emit(e, fptype(vd) | 0x1E21C000u | (rc(vn) << 5) | rc(vd));
	else
		emit(e, fpformat(vd) | 0x2EA1F800u | (rc(vn) << 5) | rc(vd));
}

void a64_fcmeq_zero(a64_emit* e, a64r vd, a64r vn)
{
	u32 op = 0x0EA0D800u;
	if (vscalar(vd))
		op |= NEON_Q | NEON_SCALAR;
	emit(e, fpformat(vd) | op | (rc(vn) << 5) | rc(vd));
}

void a64_fcvtzs(a64_emit* e, a64r rd, a64r vn)
{
	if (is_gpr(rd))
		emit(e, sf(rd) | fptype(vn) | 0x1E380000u | (rc(vn) << 5) | rc(rd));
	else
		emit(e, fpformat(rd) | 0x0EA1B800u | (vscalar(rd) ? (NEON_Q | NEON_SCALAR) : 0u) | (rc(vn) << 5) | rc(rd));
}

void a64_scvtf(a64_emit* e, a64r vd, a64r rn)
{
	if (is_gpr(rn))
		emit(e, sf(rn) | fptype(vd) | 0x1E220000u | (rc(rn) << 5) | rc(vd));
	else
		emit(e, fpformat(vd) | 0x0E21D800u | (vscalar(vd) ? (NEON_Q | NEON_SCALAR) : 0u) | (rc(rn) << 5) | rc(vd));
}

/* NEON modified immediates */

#define MODIMM_MOVI 0x0F000400u
#define MODIMM_MVNI 0x2F000400u
#define MODIMM_ORR  0x0F001400u
#define MODIMM_BIC  0x2F001400u

static u32 abcdefgh(unsigned imm8) { return (((u32)imm8 >> 5) & 7u) << 16 | ((u32)imm8 & 0x1fu) << 5; }

static void modimm_lsl(a64_emit* e, a64r vd, unsigned imm8, unsigned left_shift, u32 op)
{
	unsigned c1, c2, c3;
	u32 cmode;
	if (rk(vd) == A64K_8B || rk(vd) == A64K_16B)
	{
		c1 = 1;
		c2 = 1;
		c3 = 1;
	}
	else
	{
		c1 = (left_shift >> 3) & 1;
		c2 = left_shift >> 4;
		c3 = (rk(vd) == A64K_4H || rk(vd) == A64K_8H) ? 1u : 0u;
	}
	cmode = ((u32)c3 << 3) | ((u32)c2 << 2) | ((u32)c1 << 1);
	emit(e, (visq(vd) ? NEON_Q : 0u) | op | abcdefgh(imm8) | (cmode << 12) | rc(vd));
}

static void modimm_msl(a64_emit* e, a64r vd, unsigned imm8, unsigned shift_amount, u32 op)
{
	u32 cmode = 0xcu | ((shift_amount >> 4) & 1u);
	emit(e, (visq(vd) ? NEON_Q : 0u) | op | abcdefgh(imm8) | (cmode << 12) | rc(vd));
}

static void movi_raw(a64_emit* e, a64r vd, u64 imm, unsigned shift, unsigned amount)
{
	if (rk(vd) == A64K_2D || rk(vd) == A64K_D)
	{
		unsigned imm8 = 0;
		int i;
		for (i = 0; i < 8; i++)
			if (((imm >> (i * 8)) & 0xff) == 0xff)
				imm8 |= 1u << i;
		emit(e, (rk(vd) == A64K_2D ? NEON_Q : 0u) | 0x20000000u | MODIMM_MOVI | abcdefgh(imm8) | (0xeu << 12) | rc(vd));
	}
	else if (shift == A64_LSL)
		modimm_lsl(e, vd, (unsigned)imm, amount, MODIMM_MOVI);
	else
		modimm_msl(e, vd, (unsigned)imm, amount, MODIMM_MOVI);
}

void a64_movi_shift(a64_emit* e, a64r vd, unsigned imm8, unsigned shift, unsigned amount)
{
	if (amount != 0 || shift != A64_LSL)
		movi_raw(e, vd, imm8, shift, amount);
	else
		a64_movi(e, vd, imm8);
}

void a64_mvni(a64_emit* e, a64r vd, unsigned imm8, unsigned shift, unsigned amount)
{
	if (shift == A64_LSL)
		modimm_lsl(e, vd, imm8, amount, MODIMM_MVNI);
	else
		modimm_msl(e, vd, imm8, amount, MODIMM_MVNI);
}
void a64_bic_vimm(a64_emit* e, a64r vd, unsigned imm8, unsigned lshift) { modimm_lsl(e, vd, imm8, lshift, MODIMM_BIC); }
void a64_orr_vimm(a64_emit* e, a64r vd, unsigned imm8, unsigned lshift) { modimm_lsl(e, vd, imm8, lshift, MODIMM_ORR); }

static int is64v(a64r vd) { return vbits(vd) == 64; }

static void movi16(a64_emit* e, a64r vd, u64 imm)
{
	unsigned byte1 = (unsigned)(imm & 0xff);
	unsigned byte2 = (unsigned)((imm >> 8) & 0xff);
	if (byte1 == byte2)
		movi_raw(e, a64_as(vd, is64v(vd) ? A64K_8B : A64K_16B), byte1, A64_LSL, 0);
	else if (byte1 == 0)
		movi_raw(e, vd, byte2, A64_LSL, 8);
	else if (byte2 == 0)
		movi_raw(e, vd, byte1, A64_LSL, 0);
	else if (byte1 == 0xff)
		a64_mvni(e, vd, ~byte2 & 0xff, A64_LSL, 8);
	else if (byte2 == 0xff)
		a64_mvni(e, vd, ~byte1 & 0xff, A64_LSL, 0);
	else
	{
		u32 saved = e->scratch;
		a64r temp = a64_acquire_w(e);
		move_wide(e, temp, imm, -1, MOVZ);
		a64_dup(e, vd, temp);
		e->scratch = saved;
	}
}

static void movi32(a64_emit* e, a64r vd, u64 imm)
{
	unsigned bytes[4];
	int i, all0orff = 1;
	for (i = 0; i < 4; i++)
	{
		bytes[i] = (unsigned)((imm >> (i * 8)) & 0xff);
		if (bytes[i] != 0 && bytes[i] != 0xff)
			all0orff = 0;
	}
	if (all0orff)
	{
		movi_raw(e, a64_as(vd, is64v(vd) ? A64K_D : A64K_2D), (imm << 32) | imm, A64_LSL, 0);
		return;
	}
	for (i = 0; i < 4; i++)
		if ((imm & ((u64)0xff << (i * 8))) == imm)
		{
			movi_raw(e, vd, bytes[i], A64_LSL, (unsigned)(i * 8));
			return;
		}
	for (i = 0; i < 4; i++)
	{
		u64 mask = ~((u64)0xff << (i * 8)) & 0xffffffffu;
		if ((imm & mask) == mask)
		{
			a64_mvni(e, vd, ~bytes[i] & 0xff, A64_LSL, (unsigned)(i * 8));
			return;
		}
	}
	if ((imm & 0xff00ffff) == 0x0000ffff)
	{
		movi_raw(e, vd, bytes[2], A64_MSL, 16);
		return;
	}
	if ((imm & 0xffff00ff) == 0x000000ff)
	{
		movi_raw(e, vd, bytes[1], A64_MSL, 8);
		return;
	}
	if ((imm & 0xff00ffff) == 0xff000000)
	{
		a64_mvni(e, vd, ~bytes[2] & 0xff, A64_MSL, 16);
		return;
	}
	if ((imm & 0xffff00ff) == 0xffff0000)
	{
		a64_mvni(e, vd, ~bytes[1] & 0xff, A64_MSL, 8);
		return;
	}
	if (((imm >> 16) & 0xffff) == (imm & 0xffff))
	{
		movi16(e, a64_as(vd, is64v(vd) ? A64K_4H : A64K_8H), imm & 0xffff);
		return;
	}
	{
		u32 saved = e->scratch;
		a64r temp = a64_acquire_w(e);
		a64_mov_imm(e, temp, imm);
		a64_dup(e, vd, temp);
		e->scratch = saved;
	}
}

static void movi64(a64_emit* e, a64r vd, u64 imm)
{
	int i, all0orff = 1;
	for (i = 0; i < 8; i++)
	{
		unsigned b = (unsigned)((imm >> (i * 8)) & 0xff);
		if (b != 0 && b != 0xff)
			all0orff = 0;
	}
	if (all0orff)
	{
		movi_raw(e, vd, imm, A64_LSL, 0);
		return;
	}
	if (((imm >> 32) & 0xffffffffu) == (imm & 0xffffffffu))
	{
		movi32(e, a64_as(vd, is64v(vd) ? A64K_2S : A64K_4S), imm & 0xffffffffu);
		return;
	}
	{
		u32 saved = e->scratch;
		a64r temp = a64_acquire_x(e);
		a64_mov_imm(e, temp, imm);
		if (rk(vd) == A64K_D)
			a64_fmov(e, vd, temp);
		else
			a64_dup(e, a64_as(vd, A64K_2D), temp);
		e->scratch = saved;
	}
}

void a64_movi(a64_emit* e, a64r vd, u64 imm)
{
	switch (rk(vd))
	{
		case A64K_8B: case A64K_16B: movi_raw(e, vd, imm, A64_LSL, 0); break;
		case A64K_4H: case A64K_8H: movi16(e, vd, imm); break;
		case A64K_2S: case A64K_4S: movi32(e, vd, imm); break;
		default: movi64(e, vd, imm); break;
	}
}

/* Copies */

static u32 imm5(unsigned lsize, unsigned index) { return (((u32)index << (lsize + 1)) | (1u << lsize)) << 16; }

void a64_dup(a64_emit* e, a64r vd, a64r rn)
{
	emit(e, (is64v(vd) ? 0u : NEON_Q) | 0x0E000C00u | imm5(lsize_log2(vd), 0) | (rc(rn) << 5) | rc(vd));
}

void a64_dup_elem(a64_emit* e, a64r vd, a64r vn, unsigned index)
{
	u32 q, scalar;
	if (vscalar(vd))
	{
		q      = NEON_Q;
		scalar = NEON_SCALAR;
	}
	else
	{
		q      = is64v(vd) ? 0u : NEON_Q;
		scalar = 0;
	}
	emit(e, q | scalar | 0x0E000400u | imm5(lsize_log2(vn), index) | (rc(vn) << 5) | rc(vd));
}

void a64_ins(a64_emit* e, a64r vd, unsigned index, a64r rn)
{
	emit(e, 0x4E001C00u | imm5(lsize_log2(vd), index) | (rc(rn) << 5) | rc(vd));
}

void a64_ins_elem(a64_emit* e, a64r vd, unsigned dindex, a64r vn, unsigned sindex)
{
	unsigned s = lsize_log2(vd);
	emit(e, 0x6E000400u | imm5(s, dindex) | (((u32)sindex << s) << 11) | (rc(vn) << 5) | rc(vd));
}

void a64_umov(a64_emit* e, a64r rd, a64r vn, unsigned index)
{
	unsigned s = lsize_log2(vn);
	emit(e, (s == 3 ? NEON_Q : 0u) | 0x0E003C00u | imm5(s, index) | (rc(vn) << 5) | rc(rd));
}

void a64_ext(a64_emit* e, a64r vd, a64r vn, a64r vm, unsigned index)
{
	emit(e, vformat(vd) | 0x2E000000u | (rc(vm) << 16) | ((u32)index << 11) | (rc(vn) << 5) | rc(vd));
}

void a64_tbl(a64_emit* e, a64r vd, a64r vn, a64r vm)
{
	emit(e, 0x0E000000u | (visq(vd) ? NEON_Q : 0u) | (rc(vm) << 16) | (rc(vn) << 5) | rc(vd));
}

void a64_tbl2(a64_emit* e, a64r vd, a64r vn, a64r vn2, a64r vm)
{
	(void)vn2;
	emit(e, 0x0E002000u | (visq(vd) ? NEON_Q : 0u) | (rc(vm) << 16) | (rc(vn) << 5) | rc(vd));
}

static void perm(a64_emit* e, a64r vd, a64r vn, a64r vm, u32 op)
{
	emit(e, vformat(vd) | op | (rc(vm) << 16) | (rc(vn) << 5) | rc(vd));
}
void a64_zip1(a64_emit* e, a64r vd, a64r vn, a64r vm) { perm(e, vd, vn, vm, 0x0E003800u); }
void a64_zip2(a64_emit* e, a64r vd, a64r vn, a64r vm) { perm(e, vd, vn, vm, 0x0E007800u); }
void a64_uzp1(a64_emit* e, a64r vd, a64r vn, a64r vm) { perm(e, vd, vn, vm, 0x0E001800u); }

/* Shifts by immediate */

static void shift_imm(a64_emit* e, a64r vd, a64r vn, u32 op, u32 immhb)
{
	u32 q, scalar;
	if (vscalar(vn))
	{
		q      = NEON_Q;
		scalar = NEON_SCALAR;
	}
	else
	{
		q      = is64v(vd) ? 0u : NEON_Q;
		scalar = 0;
	}
	emit(e, q | op | scalar | (immhb << 16) | (rc(vn) << 5) | rc(vd));
}
void a64_shl(a64_emit* e, a64r vd, a64r vn, unsigned shift) { shift_imm(e, vd, vn, 0x0F005400u, lbits(vn) + shift); }
void a64_ushr(a64_emit* e, a64r vd, a64r vn, unsigned shift) { shift_imm(e, vd, vn, 0x2F000400u, 2 * lbits(vn) - shift); }
void a64_sshr(a64_emit* e, a64r vd, a64r vn, unsigned shift) { shift_imm(e, vd, vn, 0x0F000400u, 2 * lbits(vn) - shift); }
void a64_shrn(a64_emit* e, a64r vd, a64r vn, unsigned shift)
{
	u32 q = vscalar(vn) ? (NEON_Q | NEON_SCALAR) : (is64v(vd) ? 0u : NEON_Q);
	emit(e, q | 0x0F008400u | ((2 * lbits(vd) - shift) << 16) | (rc(vn) << 5) | rc(vd));
}
static void shift_long(a64_emit* e, a64r vd, a64r vn, unsigned shift, u32 op)
{
	emit(e, (is64v(vn) ? 0u : NEON_Q) | op | ((lbits(vn) + shift) << 16) | (rc(vn) << 5) | rc(vd));
}
void a64_sshll(a64_emit* e, a64r vd, a64r vn, unsigned shift) { shift_long(e, vd, vn, shift, 0x0F00A400u); }
void a64_ushll(a64_emit* e, a64r vd, a64r vn, unsigned shift) { shift_long(e, vd, vn, shift, 0x2F00A400u); }

void a64_xtn(a64_emit* e, a64r vd, a64r vn)
{
	u32 op = 0x0E212800u, format;
	if (vscalar(vd))
	{
		op |= NEON_Q | NEON_SCALAR;
		format = sformat(vd);
	}
	else
		format = vformat(vd);
	emit(e, format | op | (rc(vn) << 5) | rc(vd));
}

static void across(a64_emit* e, a64r vd, a64r vn, u32 op) { emit(e, vformat(vn) | op | (rc(vn) << 5) | rc(vd)); }
void a64_addv(a64_emit* e, a64r vd, a64r vn) { across(e, vd, vn, 0x0E31B800u); }
void a64_uminv(a64_emit* e, a64r vd, a64r vn) { across(e, vd, vn, 0x2E31A800u); }
void a64_umaxv(a64_emit* e, a64r vd, a64r vn) { across(e, vd, vn, 0x2E30A800u); }

void a64_ushl(a64_emit* e, a64r vd, a64r vn, a64r vm) { neon3same(e, vd, vn, vm, 0x2E204400u); }
void a64_smin(a64_emit* e, a64r vd, a64r vn, a64r vm) { neon3same(e, vd, vn, vm, 0x0E206C00u); }
void a64_smax(a64_emit* e, a64r vd, a64r vn, a64r vm) { neon3same(e, vd, vn, vm, 0x0E206400u); }
void a64_umin(a64_emit* e, a64r vd, a64r vn, a64r vm) { neon3same(e, vd, vn, vm, 0x2E206C00u); }
void a64_sqadd(a64_emit* e, a64r vd, a64r vn, a64r vm) { neon3same(e, vd, vn, vm, 0x0E200C00u); }
void a64_sqsub(a64_emit* e, a64r vd, a64r vn, a64r vm) { neon3same(e, vd, vn, vm, 0x0E202C00u); }
void a64_uqadd(a64_emit* e, a64r vd, a64r vn, a64r vm) { neon3same(e, vd, vn, vm, 0x2E200C00u); }
void a64_uqsub(a64_emit* e, a64r vd, a64r vn, a64r vm) { neon3same(e, vd, vn, vm, 0x2E202C00u); }
void a64_cmeq(a64_emit* e, a64r vd, a64r vn, a64r vm) { neon3same(e, vd, vn, vm, 0x2E208C00u); }
void a64_cmgt(a64_emit* e, a64r vd, a64r vn, a64r vm) { neon3same(e, vd, vn, vm, 0x0E203400u); }
void a64_sqabs(a64_emit* e, a64r vd, a64r vn) { neon2misc(e, vd, vn, 0x0E207800u); }
void a64_abs(a64_emit* e, a64r vd, a64r vn) { neon2misc(e, vd, vn, 0x0E20B800u); }
void a64_cmeq_zero(a64_emit* e, a64r vd, a64r vn) { neon2misc(e, vd, vn, 0x0E209800u); }
void a64_cmgt_zero(a64_emit* e, a64r vd, a64r vn) { neon2misc(e, vd, vn, 0x0E208800u); }
void a64_cmge_zero(a64_emit* e, a64r vd, a64r vn) { neon2misc(e, vd, vn, 0x2E208800u); }
void a64_cmle_zero(a64_emit* e, a64r vd, a64r vn) { neon2misc(e, vd, vn, 0x2E209800u); }
void a64_cmlt_zero(a64_emit* e, a64r vd, a64r vn) { neon2misc(e, vd, vn, 0x0E20A800u); }
void a64_bsl(a64_emit* e, a64r vd, a64r vn, a64r vm) { neon_logical(e, vd, vn, vm, 0x2E601C00u); }
void a64_bit(a64_emit* e, a64r vd, a64r vn, a64r vm) { neon_logical(e, vd, vn, vm, 0x2EA01C00u); }
void a64_bif(a64_emit* e, a64r vd, a64r vn, a64r vm) { neon_logical(e, vd, vn, vm, 0x2EE01C00u); }
