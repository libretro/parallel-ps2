/*
 * The software renderer's x86-64 emitter (GSJitX86.h). Each encoding is
 * the one xbyak makes for the same instruction and operands; the comments
 * name xbyak's routine where the choice is its own.
 */

#include <string.h>

#include "GSJitX86.h"

/* --- bytes ----------------------------------------------------------- */

/* (past the room nothing is written and the size stays the room, as
 * xbyak's fixed buffer does) */
static void gsj_b(gsj* j, unsigned v)
{
	if (j->p >= j->end)
	{
		j->full = 1;
		return;
	}
	*j->p++ = (u8)v;
}

static void gsj_b32(gsj* j, u32 v)
{
	gsj_b(j, v);
	gsj_b(j, v >> 8);
	gsj_b(j, v >> 16);
	gsj_b(j, v >> 24);
}

static void gsj_bn(gsj* j, u64 v, unsigned bytes)
{
	unsigned i;
	for (i = 0; i < bytes; i++)
		gsj_b(j, (unsigned)(v >> (8 * i)));
}

static s32 gsj_at(const gsj* j)
{
	return (s32)(j->p - j->buf);
}

void gsj_init(gsj* j, void* buf, size_t room, int avx, int avx2)
{
	unsigned i;
	memset(j, 0, sizeof(*j));
	j->buf  = (u8*)buf;
	j->p    = j->buf;
	j->end  = j->buf + room;
	j->avx  = avx || avx2;
	j->avx2 = avx2;
	for (i = 0; i < GSJ_LABELS + 2; i++)
		j->label[i] = -1;
}

size_t gsj_size(const gsj* j)
{
	return (size_t)(j->p - j->buf);
}

void gsj_db(gsj* j, unsigned byte)
{
	gsj_b(j, byte);
}

/* (xbyak's nop: the recommended multi-byte forms, nine bytes at most) */
void gsj_align(gsj* j, unsigned to)
{
	static const u8 nops[9][9] = {
		{0x90},
		{0x66, 0x90},
		{0x0F, 0x1F, 0x00},
		{0x0F, 0x1F, 0x40, 0x00},
		{0x0F, 0x1F, 0x44, 0x00, 0x00},
		{0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
		{0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
		{0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
		{0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
	};
	size_t remain, len, i;
	if (to <= 1 || (to & (to - 1)))
		return;
	remain = (size_t)j->p % to;
	if (!remain)
		return;
	remain = to - remain;
	while (remain > 0)
	{
		len = remain < 9 ? remain : 9;
		for (i = 0; i < len; i++)
			gsj_b(j, nops[len - 1][i]);
		remain -= len;
	}
}

/* --- labels and jumps ------------------------------------------------ */

static int gsj_s8(s32 v)
{
	return v >= -128 && v <= 127;
}

/* (xbyak's define_inner: a short jump that does not reach stops the
 * patching there, its displacement and the rest left as they are) */
static void gsj_resolve(gsj* j, unsigned label)
{
	const s32 at = j->label[label];
	unsigned i = 0;
	while (i < j->fixes)
	{
		s32 disp;
		if (j->fix[i].label != label)
		{
			i++;
			continue;
		}
		disp = at - j->fix[i].end;
		if (j->fix[i].size == 1 && !gsj_s8(disp))
		{
			j->error = 1;
			return;
		}
		if (j->fix[i].end <= (s32)(j->end - j->buf))
		{
			u8* d = j->buf + j->fix[i].end - j->fix[i].size;
			if (j->fix[i].size == 1)
				d[0] = (u8)disp;
			else
			{
				d[0] = (u8)disp;
				d[1] = (u8)(disp >> 8);
				d[2] = (u8)(disp >> 16);
				d[3] = (u8)(disp >> 24);
			}
		}
		j->fix[i] = j->fix[--j->fixes];
	}
}

void gsj_bind(gsj* j, unsigned label)
{
	if (j->label[label] >= 0)
		return;
	j->label[label] = gsj_at(j);
	gsj_resolve(j, label);
}

void gsj_bind_anon(gsj* j)
{
	const unsigned slot = GSJ_LABELS + (j->anon & 1);
	j->label[slot] = gsj_at(j);
	gsj_resolve(j, slot);
	j->label[slot] = -1;
	j->anon++;
}

/* (xbyak's makeJmp: short when it reaches and near is not asked) */
static void gsj_jump_to(gsj* j, unsigned cc, s32 disp, int near_)
{
	if (!near_ && gsj_s8(disp - 2))
	{
		gsj_b(j, cc == GSJ_CC_ALWAYS ? 0xEB : 0x70 | cc);
		gsj_b(j, (unsigned)(disp - 2));
	}
	else if (cc == GSJ_CC_ALWAYS)
	{
		gsj_b(j, 0xE9);
		gsj_b32(j, (u32)(disp - 5));
	}
	else
	{
		gsj_b(j, 0x0F);
		gsj_b(j, 0x80 | cc);
		gsj_b32(j, (u32)(disp - 6));
	}
}

void gsj_jcc(gsj* j, unsigned cc, unsigned label, int near_)
{
	if (label == GSJ_ANON)
		label = GSJ_LABELS + (j->anon & 1);
	else if (j->label[label] >= 0)
	{
		gsj_jump_to(j, cc, j->label[label] - gsj_at(j), near_);
		return;
	}
	if (near_)
	{
		if (cc == GSJ_CC_ALWAYS)
			gsj_b(j, 0xE9);
		else
		{
			gsj_b(j, 0x0F);
			gsj_b(j, 0x80 | cc);
		}
		gsj_b32(j, 0);
	}
	else
	{
		gsj_b(j, cc == GSJ_CC_ALWAYS ? 0xEB : 0x70 | cc);
		gsj_b(j, 0);
	}
	if (j->fixes < GSJ_FIXUPS)
	{
		j->fix[j->fixes].end   = gsj_at(j);
		j->fix[j->fixes].size  = (u8)(near_ ? 4 : 1);
		j->fix[j->fixes].label = (u8)label;
		j->fixes++;
	}
	else
		j->error = 1;
}

/* (xbyak: jmp short when it reaches, a conditional jump always near) */
void gsj_jcc_abs(gsj* j, unsigned cc, const void* addr)
{
	gsj_jump_to(j, cc, (s32)((const u8*)addr - j->p), cc != GSJ_CC_ALWAYS);
}

/* --- prefixes, ModRM and SIB ----------------------------------------- */

/* A memory operand as xbyak encodes it: a general index scaled by one
 * with no base is the base, and one scaled by two with no base is base
 * and index both (RegExp and its optimize) */
static gsj_op gsj_norm(const gsj_op* o)
{
	gsj_op m = *o;
	if (m.kind != GSJ_MEM || m.index < 0 || m.reg >= 0 || m.index_kind != GSJ_R64)
		return m;
	if (m.scale == 1)
	{
		m.reg   = m.index;
		m.index = -1;
	}
	else if (m.scale == 2)
	{
		m.reg   = m.index;
		m.scale = 1;
	}
	return m;
}

/* The REX bits of a memory operand: X from its index, B from its base */
static unsigned gsj_mem_rex(const gsj_op* o)
{
	const gsj_op m = gsj_norm(o);
	unsigned rex   = 0;
	if (m.index >= 8)
		rex |= 2;
	if (m.reg >= 8)
		rex |= 1;
	return rex;
}

/* (xbyak's setSIB) */
static void gsj_modrm_mem(gsj* j, unsigned reg, const gsj_op* o)
{
	const gsj_op mm     = gsj_norm(o);
	const gsj_op* m     = &mm;
	const u32 disp      = (u32)m->disp;
	const int has_base  = m->reg >= 0;
	const int has_index = m->index >= 0;
	const unsigned base = has_base ? (unsigned)m->reg & 7 : 5;
	unsigned mod        = 2;
	int sib;
	if (m->reg == GSJ_RIP)
	{
		/* (xbyak's opAddr: from the end of the instruction) */
		gsj_b(j, ((reg & 7) << 3) | 5);
		gsj_b32(j, (u32)(m->disp - (gsj_at(j) + 4 + (s32)j->tail)));
		return;
	}
	if (!has_base || (base != 5 && disp == 0))
		mod = 0;
	else if (gsj_s8((s32)disp))
		mod = 1;
	sib = has_index || (has_base && base == 4) || (!has_base && !has_index);
	if (sib)
	{
		const unsigned ss = !has_index ? 0 : m->scale == 8 ? 3 : m->scale == 4 ? 2 : m->scale == 2 ? 1 : 0;
		gsj_b(j, (mod << 6) | ((reg & 7) << 3) | 4);
		gsj_b(j, (ss << 6) | ((has_index ? (unsigned)m->index & 7 : 4) << 3) | base);
	}
	else
		gsj_b(j, (mod << 6) | ((reg & 7) << 3) | base);
	if (mod == 1)
		gsj_b(j, disp);
	else if (mod == 2 || !has_base)
		gsj_b32(j, disp);
}

/* ModRM for reg and rm (a register or memory) */
static void gsj_modrm(gsj* j, unsigned reg, const gsj_op* rm)
{
	if (rm->kind == GSJ_MEM)
		gsj_modrm_mem(j, reg, rm);
	else
		gsj_b(j, 0xC0 | ((reg & 7) << 3) | ((unsigned)rm->reg & 7));
}

/* A REX for reg (its number, and whether it is a 64-bit register) and rm,
 * when one is needed */
static void gsj_rex(gsj* j, unsigned reg, int w, const gsj_op* rm)
{
	unsigned rex = (w ? 8 : 0) | (reg >= 8 ? 4 : 0);
	if (rm->kind == GSJ_MEM)
		rex |= gsj_mem_rex(rm);
	else
	{
		if (rm->reg >= 8)
			rex |= 1;
		if (rm->kind == GSJ_R64)
			rex |= 8;
	}
	if (rex)
		gsj_b(j, 0x40 | rex);
}

/* An SSE instruction: prefix, REX, 0F (and 38 or 3A), opcode, ModRM, and
 * an immediate (imm < 0: none). map: 0, 0x38 or 0x3A. */
static void gsj_sse(gsj* j, unsigned pref, unsigned map, unsigned op, unsigned reg, int w, const gsj_op* rm, int imm)
{
	if (pref)
		gsj_b(j, pref);
	gsj_rex(j, reg, w, rm);
	gsj_b(j, 0x0F);
	if (map)
		gsj_b(j, map);
	gsj_b(j, op);
	j->tail = imm >= 0;
	gsj_modrm(j, reg, rm);
	j->tail = 0;
	if (imm >= 0)
		gsj_b(j, (unsigned)imm);
}

/* A VEX instruction (xbyak's vex): pp the legacy prefix it stands for,
 * map 0, 0x38 or 0x3A, vvvv the second source's number (0 for none) */
static void gsj_vex(gsj* j, unsigned pref, unsigned map, int w, int l, unsigned op, unsigned reg, unsigned vvvv,
	const gsj_op* rm, int imm)
{
	const unsigned pp   = pref == 0x66 ? 1 : pref == 0xF3 ? 2 : pref == 0xF2 ? 3 : 0;
	const unsigned mmmm = map == 0x38 ? 2 : map == 0x3A ? 3 : 1;
	const unsigned v    = (((~vvvv) & 15) << 3) | (l ? 4 : 0) | pp;
	const int r         = reg >= 8;
	int x, b;
	if (rm->kind == GSJ_MEM)
	{
		const gsj_op m = gsj_norm(rm);
		x = m.index >= 8;
		b = m.reg >= 8;
	}
	else
	{
		x = 0;
		b = rm->reg >= 8;
	}
	if (!b && !x && !w && mmmm == 1)
	{
		gsj_b(j, 0xC5);
		gsj_b(j, (r ? 0 : 0x80) | v);
	}
	else
	{
		gsj_b(j, 0xC4);
		gsj_b(j, (r ? 0 : 0x80) | (x ? 0 : 0x40) | (b ? 0 : 0x20) | mmmm);
		gsj_b(j, (w ? 0x80 : 0) | v);
	}
	gsj_b(j, op);
	j->tail = imm >= 0;
	gsj_modrm(j, reg, rm);
	j->tail = 0;
	if (imm >= 0)
		gsj_b(j, (unsigned)imm);
}

static int gsj_ymm(const gsj_op* o)
{
	return o->kind == GSJ_YMM;
}

/* --- general instructions -------------------------------------------- */

static unsigned gsj_bits(const gsj_op* o)
{
	return o->bits;
}

/* REX (and 66 for 16 bits) for a general instruction on rm with reg, of
 * size bits (xbyak's rex) */
static void gsj_grex(gsj* j, unsigned reg, unsigned bits, const gsj_op* rm, unsigned rm_bits)
{
	unsigned rex = 0;
	if (bits == 16 || rm_bits == 16)
		gsj_b(j, 0x66);
	if (bits == 64 || rm_bits == 64)
		rex |= 8;
	if (reg >= 8)
		rex |= 4;
	if (rm->kind == GSJ_MEM)
		rex |= gsj_mem_rex(rm);
	else if (rm->reg >= 8)
		rex |= 1;
	if (rex)
		gsj_b(j, 0x40 | rex);
}

/* (xbyak's opRM_RM: op a, b; a register from memory, or into a register
 * or memory from a register) */
static void gsj_alu_rr(gsj* j, unsigned code, gsj_op a, gsj_op b)
{
	if (a.kind != GSJ_MEM && b.kind == GSJ_MEM)
	{
		gsj_grex(j, (unsigned)a.reg, gsj_bits(&a), &b, 0);
		gsj_b(j, code | 2 | (a.kind == GSJ_R8 ? 0 : 1));
		gsj_modrm(j, (unsigned)a.reg, &b);
	}
	else
	{
		gsj_grex(j, (unsigned)b.reg, gsj_bits(&b), &a, a.kind == GSJ_MEM ? 0 : gsj_bits(&a));
		gsj_b(j, code | (b.kind == GSJ_R8 ? 0 : 1));
		gsj_modrm(j, (unsigned)b.reg, &a);
	}
}

static int gsj_disp8(u32 v)
{
	return v >= 0xFFFFFF80u || v <= 0x7F;
}

/* (xbyak's opRM_I) */
static void gsj_alu_i(gsj* j, unsigned code, unsigned ext, gsj_op op, u32 imm)
{
	const unsigned bits = gsj_bits(&op);
	unsigned imm_bits   = gsj_disp8(imm) ? 8 : (imm >= 0xFFFF8000u || imm <= 0x7FFF) ? 16 : 32;
	if (bits == 8)
		imm_bits = 8;
	if (bits < imm_bits)
		return;
	if ((bits == 32 || bits == 64) && imm_bits == 16)
		imm_bits = 32;
	if (op.kind != GSJ_MEM && op.reg == 0 && (bits == imm_bits || (bits == 64 && imm_bits == 32)))
	{
		if (bits == 16)
			gsj_b(j, 0x66);
		if (bits == 64)
			gsj_b(j, 0x48);
		gsj_b(j, code | 4 | (imm_bits == 8 ? 0 : 1));
	}
	else
	{
		const unsigned tmp = imm_bits < (bits < 32 ? bits : 32) ? 2 : 0;
		gsj_grex(j, ext, bits, &op, 0);
		gsj_b(j, (0x80 | tmp) | (bits == 8 ? 0 : 1));
		j->tail = imm_bits / 8;
		gsj_modrm(j, ext, &op);
		j->tail = 0;
	}
	gsj_bn(j, imm, imm_bits / 8);
}

#define GSJ_ALU(name, code, ext) \
	void gsj_##name(gsj* j, gsj_op a, gsj_op b) { gsj_alu_rr(j, code, a, b); } \
	void gsj_##name##_i(gsj* j, gsj_op a, u32 imm) { gsj_alu_i(j, code, ext, a, imm); }
GSJ_ALU(add, 0x00, 0)
GSJ_ALU(or, 0x08, 1)
GSJ_ALU(and, 0x20, 4)
GSJ_ALU(sub, 0x28, 5)
GSJ_ALU(xor, 0x30, 6)
GSJ_ALU(cmp, 0x38, 7)

void gsj_lea(gsj* j, gsj_op r, gsj_op m)
{
	gsj_grex(j, (unsigned)r.reg, gsj_bits(&r), &m, 0);
	gsj_b(j, 0x8D);
	gsj_modrm(j, (unsigned)r.reg, &m);
}

void gsj_mov(gsj* j, gsj_op a, gsj_op b)
{
	gsj_alu_rr(j, 0x88, a, b);
}

/* (xbyak's mov_imm, and C7 into memory) */
void gsj_mov_i(gsj* j, gsj_op a, u64 imm)
{
	if (a.kind != GSJ_MEM)
	{
		unsigned bits      = gsj_bits(&a);
		const unsigned idx = (unsigned)a.reg;
		unsigned code      = 0xB0 | ((bits == 8 ? 0 : 1) << 3);
		if (bits == 64 && (imm >> 32) == 0)
		{
			if (idx >= 8)
				gsj_b(j, 0x41);
			bits = 32;
		}
		else
		{
			if (bits == 16)
				gsj_b(j, 0x66);
			if (bits == 64 || idx >= 8)
				gsj_b(j, 0x40 | (bits == 64 ? 8 : 0) | (idx >= 8 ? 1 : 0));
			if (bits == 64 && (s64)imm == (s64)(s32)imm)
			{
				gsj_b(j, 0xC7);
				code = 0xC0;
				bits = 32;
			}
		}
		gsj_b(j, code | (idx & 7));
		gsj_bn(j, imm, bits / 8);
	}
	else
	{
		unsigned size = a.bits / 8;
		if (size <= 4)
		{
			const s64 s = (s64)imm >> (size * 8);
			if (size == 0 || (s != 0 && s != -1))
				return;
		}
		else
		{
			if ((s64)imm != (s64)(s32)imm)
				return;
			size = 4;
		}
		gsj_grex(j, 0, a.bits, &a, 0);
		gsj_b(j, 0xC6 | (a.bits == 8 ? 0 : 1));
		j->tail = size;
		gsj_modrm(j, 0, &a);
		j->tail = 0;
		gsj_bn(j, imm, size);
	}
}

/* (xbyak's opMovxx: from 8 or 16 bits) */
void gsj_movzx(gsj* j, gsj_op r, gsj_op op)
{
	const unsigned w = op.bits == 16;
	gsj_grex(j, (unsigned)r.reg, gsj_bits(&r), &op, 0);
	gsj_b(j, 0x0F);
	gsj_b(j, 0xB6 | w);
	gsj_modrm(j, (unsigned)r.reg, &op);
}

/* (xbyak's opR_ModM: an opcode extension in reg) */
static void gsj_ext(gsj* j, unsigned code, unsigned ext, gsj_op op)
{
	gsj_grex(j, ext, gsj_bits(&op), &op, 0);
	gsj_b(j, code | (op.bits == 8 ? 0 : 1));
	gsj_modrm(j, ext, &op);
}

void gsj_not(gsj* j, gsj_op op)
{
	gsj_ext(j, 0xF6, 2, op);
}

/* (xbyak's opPushPop: 64-bit registers or memory) */
static void gsj_pushpop(gsj* j, unsigned code, unsigned ext, unsigned alt, gsj_op op)
{
	if (op.kind != GSJ_MEM)
	{
		if (op.reg >= 8)
			gsj_b(j, 0x41);
		gsj_b(j, alt | ((unsigned)op.reg & 7));
		return;
	}
	if (op.bits == 16)
		gsj_b(j, 0x66);
	{
		const unsigned rex = gsj_mem_rex(&op);
		if (rex)
			gsj_b(j, 0x40 | rex);
	}
	gsj_b(j, code);
	gsj_modrm(j, ext, &op);
}

void gsj_push(gsj* j, gsj_op op)
{
	gsj_pushpop(j, 0xFF, 6, 0x50, op);
}

void gsj_pop(gsj* j, gsj_op op)
{
	gsj_pushpop(j, 0x8F, 0, 0x58, op);
}

static void gsj_shift(gsj* j, unsigned ext, gsj_op op, unsigned imm)
{
	j->tail = imm != 1;
	gsj_ext(j, imm == 1 ? 0xD0 : 0xC0, ext, op);
	j->tail = 0;
	if (imm != 1)
		gsj_b(j, imm);
}

void gsj_sar(gsj* j, gsj_op op, unsigned imm) { gsj_shift(j, 7, op, imm); }
void gsj_shl(gsj* j, gsj_op op, unsigned imm) { gsj_shift(j, 4, op, imm); }
void gsj_shr(gsj* j, gsj_op op, unsigned imm) { gsj_shift(j, 5, op, imm); }
void gsj_sar_cl(gsj* j, gsj_op op) { gsj_ext(j, 0xD2, 7, op); }
void gsj_shl_cl(gsj* j, gsj_op op) { gsj_ext(j, 0xD2, 4, op); }
void gsj_shr_cl(gsj* j, gsj_op op) { gsj_ext(j, 0xD2, 5, op); }

void gsj_test(gsj* j, gsj_op op, gsj_op r)
{
	gsj_grex(j, (unsigned)r.reg, gsj_bits(&r), &op, op.kind == GSJ_MEM ? 0 : gsj_bits(&op));
	gsj_b(j, 0x84 | (r.kind == GSJ_R8 ? 0 : 1));
	gsj_modrm(j, (unsigned)r.reg, &op);
}

void gsj_test_i(gsj* j, gsj_op op, u32 imm)
{
	const unsigned size = op.bits / 8 < 4 ? op.bits / 8 : 4;
	if (op.kind != GSJ_MEM && op.reg == 0)
	{
		if (op.bits == 16)
			gsj_b(j, 0x66);
		if (op.bits == 64)
			gsj_b(j, 0x48);
		gsj_b(j, 0xA8 | (op.bits == 8 ? 0 : 1));
	}
	else
	{
		j->tail = size;
		gsj_ext(j, 0xF6, 0, op);
		j->tail = 0;
	}
	gsj_bn(j, imm, size);
}

void gsj_cdqe(gsj* j)
{
	gsj_b(j, 0x48);
	gsj_b(j, 0x98);
}

void gsj_ret(gsj* j)
{
	gsj_b(j, 0xC3);
}

void gsj_vzeroupper(gsj* j)
{
	gsj_b(j, 0xC5);
	gsj_b(j, 0xF8);
	gsj_b(j, 0x77);
}

void gsj_vzeroall(gsj* j)
{
	gsj_b(j, 0xC5);
	gsj_b(j, 0xFC);
	gsj_b(j, 0x77);
}

/* --- SSE and AVX ----------------------------------------------------- */

/* (x, op): SSE, or AVX as (x, x, op) */
#define GSJ_A(name, pref, map, op) \
	void gsj_##name(gsj* j, gsj_op x, gsj_op o) \
	{ \
		if (j->avx) \
			gsj_vex(j, pref, map, 0, gsj_ymm(&x), op, (unsigned)x.reg, (unsigned)x.reg, &o, -1); \
		else \
			gsj_sse(j, pref, map, op, (unsigned)x.reg, 0, &o, -1); \
	} \
	void gsj_v##name(gsj* j, gsj_op x, gsj_op x2, gsj_op o) \
	{ \
		gsj_vex(j, pref, map, 0, gsj_ymm(&x), op, (unsigned)x.reg, (unsigned)x2.reg, &o, -1); \
	}
#define GSJ_AI(name, pref, map, op) \
	void gsj_##name(gsj* j, gsj_op x, gsj_op o, unsigned imm) \
	{ \
		if (j->avx) \
			gsj_vex(j, pref, map, 0, gsj_ymm(&x), op, (unsigned)x.reg, (unsigned)x.reg, &o, (int)(imm & 0xFF)); \
		else \
			gsj_sse(j, pref, map, op, (unsigned)x.reg, 0, &o, (int)(imm & 0xFF)); \
	} \
	void gsj_v##name(gsj* j, gsj_op x, gsj_op x2, gsj_op o, unsigned imm) \
	{ \
		gsj_vex(j, pref, map, 0, gsj_ymm(&x), op, (unsigned)x.reg, (unsigned)x2.reg, &o, (int)(imm & 0xFF)); \
	}
/* (x, op): two operands in either form */
#define GSJ_S(name, pref, map, op) \
	void gsj_##name(gsj* j, gsj_op x, gsj_op o) \
	{ \
		if (j->avx) \
			gsj_vex(j, pref, map, 0, gsj_ymm(&x), op, (unsigned)x.reg, 0, &o, -1); \
		else \
			gsj_sse(j, pref, map, op, (unsigned)x.reg, 0, &o, -1); \
	}
#define GSJ_SI(name, pref, map, op) \
	void gsj_##name(gsj* j, gsj_op x, gsj_op o, unsigned imm) \
	{ \
		if (j->avx) \
			gsj_vex(j, pref, map, 0, gsj_ymm(&x), op, (unsigned)x.reg, 0, &o, (int)(imm & 0xFF)); \
		else \
			gsj_sse(j, pref, map, op, (unsigned)x.reg, 0, &o, (int)(imm & 0xFF)); \
	}
/* (x, op) whose AVX form's length is its source's (xbyak's opCvt) */
#define GSJ_CVT(name, pref, map, op) \
	void gsj_##name(gsj* j, gsj_op x, gsj_op o) \
	{ \
		if (j->avx) \
			gsj_vex(j, pref, map, 0, o.bits == 256, op, (unsigned)x.reg, 0, &o, -1); \
		else \
			gsj_sse(j, pref, map, op, (unsigned)x.reg, 0, &o, -1); \
	}

GSJ_A(addps, 0, 0, 0x58)
GSJ_A(addpd, 0x66, 0, 0x58)
GSJ_A(cvtsd2ss, 0xF2, 0, 0x5A)
GSJ_A(cvtss2sd, 0xF3, 0, 0x5A)
GSJ_A(divps, 0, 0, 0x5E)
GSJ_A(maxps, 0, 0, 0x5F)
GSJ_A(minps, 0, 0, 0x5D)
GSJ_A(mulpd, 0x66, 0, 0x59)
GSJ_A(mulps, 0, 0, 0x59)
GSJ_A(mulsd, 0xF2, 0, 0x59)
GSJ_A(mulss, 0xF3, 0, 0x59)
GSJ_A(orps, 0, 0, 0x56)
GSJ_A(packssdw, 0x66, 0, 0x6B)
GSJ_A(packusdw, 0x66, 0x38, 0x2B)
GSJ_A(packuswb, 0x66, 0, 0x67)
GSJ_A(paddd, 0x66, 0, 0xFE)
GSJ_A(paddusb, 0x66, 0, 0xDC)
GSJ_A(paddw, 0x66, 0, 0xFD)
GSJ_A(pand, 0x66, 0, 0xDB)
GSJ_A(pandn, 0x66, 0, 0xDF)
GSJ_A(pcmpeqd, 0x66, 0, 0x76)
GSJ_A(pcmpeqw, 0x66, 0, 0x75)
GSJ_A(pcmpgtd, 0x66, 0, 0x66)
GSJ_A(pmaxsw, 0x66, 0, 0xEE)
GSJ_A(pminsd, 0x66, 0x38, 0x39)
GSJ_A(pminud, 0x66, 0x38, 0x3B)
GSJ_A(pminsw, 0x66, 0, 0xEA)
GSJ_A(pmulhrsw, 0x66, 0x38, 0x0B)
GSJ_A(pmulhw, 0x66, 0, 0xE5)
GSJ_A(pmullw, 0x66, 0, 0xD5)
GSJ_A(por, 0x66, 0, 0xEB)
GSJ_A(psrad, 0x66, 0, 0xE2)
GSJ_A(psrlw, 0x66, 0, 0xD1)
GSJ_A(psubd, 0x66, 0, 0xFA)
GSJ_A(psubw, 0x66, 0, 0xF9)
GSJ_A(punpckhdq, 0x66, 0, 0x6A)
GSJ_A(punpckhwd, 0x66, 0, 0x69)
GSJ_A(punpcklbw, 0x66, 0, 0x60)
GSJ_A(punpckldq, 0x66, 0, 0x62)
GSJ_A(punpcklqdq, 0x66, 0, 0x6C)
GSJ_A(punpcklwd, 0x66, 0, 0x61)
GSJ_A(pxor, 0x66, 0, 0xEF)
GSJ_A(subps, 0, 0, 0x5C)
GSJ_A(unpcklps, 0, 0, 0x14)
GSJ_A(unpcklpd, 0x66, 0, 0x14)
GSJ_A(xorps, 0, 0, 0x57)
GSJ_AI(pblendw, 0x66, 0x3A, 0x0E)
GSJ_AI(pinsrd, 0x66, 0x3A, 0x22)
GSJ_AI(roundsd, 0x66, 0x3A, 0x0B)
GSJ_AI(shufps, 0, 0, 0xC6)
GSJ_S(cvtdq2ps, 0, 0, 0x5B)
GSJ_CVT(cvtpd2dq, 0xF2, 0, 0xE6)
GSJ_CVT(cvtpd2ps, 0x66, 0, 0x5A)
GSJ_CVT(cvttpd2dq, 0x66, 0, 0xE6)
GSJ_S(cvtps2dq, 0x66, 0, 0x5B)
GSJ_S(cvtps2pd, 0, 0, 0x5A)
GSJ_S(cvttps2dq, 0xF3, 0, 0x5B)
GSJ_S(movddup, 0xF2, 0, 0x12)
GSJ_S(pmovsxbd, 0x66, 0x38, 0x21)
GSJ_S(pmovzxbw, 0x66, 0x38, 0x30)
GSJ_S(rcpps, 0, 0, 0x53)
GSJ_SI(pshufd, 0x66, 0, 0x70)
GSJ_SI(pshufhw, 0xF3, 0, 0x70)
GSJ_SI(pshuflw, 0xF2, 0, 0x70)
GSJ_SI(roundps, 0x66, 0x3A, 0x08)

/* Shifts by an immediate: the extension in reg; for AVX the destination
 * in vvvv and the source in rm */
#define GSJ_SHIFT(name, op, ext) \
	void gsj_##name##_i(gsj* j, gsj_op x, unsigned imm) \
	{ \
		if (j->avx) \
			gsj_vex(j, 0x66, 0, 0, gsj_ymm(&x), op, ext, (unsigned)x.reg, &x, (int)(imm & 0xFF)); \
		else \
			gsj_sse(j, 0x66, 0, op, ext, 0, &x, (int)(imm & 0xFF)); \
	} \
	void gsj_v##name##_i(gsj* j, gsj_op x, gsj_op x2, unsigned imm) \
	{ \
		gsj_vex(j, 0x66, 0, 0, gsj_ymm(&x), op, ext, (unsigned)x.reg, &x2, (int)(imm & 0xFF)); \
	}
GSJ_SHIFT(pslld, 0x72, 6)
GSJ_SHIFT(psllw, 0x71, 6)
GSJ_SHIFT(psrad, 0x72, 4)
GSJ_SHIFT(psraw, 0x71, 4)
GSJ_SHIFT(psrld, 0x72, 2)
GSJ_SHIFT(psrldq, 0x73, 3)
GSJ_SHIFT(psrlw, 0x71, 2)

/* A load (a a register) or a store (a memory), with the loads' and the
 * stores' opcodes; vvvv none */
static void gsj_move(gsj* j, unsigned pref, unsigned load, unsigned store, gsj_op a, gsj_op b)
{
	const int is_store = a.kind == GSJ_MEM;
	const gsj_op* x    = is_store ? &b : &a;
	const gsj_op* rm   = is_store ? &a : &b;
	const unsigned op  = is_store ? store : load;
	if (j->avx)
		gsj_vex(j, pref, 0, 0, gsj_ymm(x), op, (unsigned)x->reg, 0, rm, -1);
	else
		gsj_sse(j, pref, 0, op, (unsigned)x->reg, 0, rm, -1);
}

void gsj_movaps(gsj* j, gsj_op a, gsj_op b) { gsj_move(j, 0, 0x28, 0x29, a, b); }
void gsj_movdqa(gsj* j, gsj_op a, gsj_op b) { gsj_move(j, 0x66, 0x6F, 0x7F, a, b); }
void gsj_movsd(gsj* j, gsj_op a, gsj_op b) { gsj_move(j, 0xF2, 0x10, 0x11, a, b); }
void gsj_movss(gsj* j, gsj_op a, gsj_op b) { gsj_move(j, 0xF3, 0x10, 0x11, a, b); }

/* movd: into an xmm from a 32-bit register or memory (6E), or out (7E) */
void gsj_movd(gsj* j, gsj_op a, gsj_op b)
{
	const int out      = a.kind != GSJ_XMM;
	const gsj_op* x    = out ? &b : &a;
	const gsj_op* rm   = out ? &a : &b;
	const unsigned op  = out ? 0x7E : 0x6E;
	if (j->avx)
		gsj_vex(j, 0x66, 0, 0, 0, op, (unsigned)x->reg, 0, rm, -1);
	else
		gsj_sse(j, 0x66, 0, op, (unsigned)x->reg, 0, rm, -1);
}

/* movq: a load (F3 0F 7E) or a store (66 0F D6) */
void gsj_movq(gsj* j, gsj_op a, gsj_op b)
{
	if (a.kind == GSJ_MEM)
	{
		if (j->avx)
			gsj_vex(j, 0x66, 0, 0, 0, 0xD6, (unsigned)b.reg, 0, &a, -1);
		else
			gsj_sse(j, 0x66, 0, 0xD6, (unsigned)b.reg, 0, &a, -1);
	}
	else if (j->avx)
		gsj_vex(j, 0xF3, 0, 0, 0, 0x7E, (unsigned)a.reg, 0, &b, -1);
	else
		gsj_sse(j, 0xF3, 0, 0x7E, (unsigned)a.reg, 0, &b, -1);
}

/* movhps: a load merges into x (for AVX, x, x, m), or a store */
void gsj_movhps(gsj* j, gsj_op a, gsj_op b)
{
	if (a.kind == GSJ_MEM)
	{
		if (j->avx)
			gsj_vex(j, 0, 0, 0, 0, 0x17, (unsigned)b.reg, 0, &a, -1);
		else
			gsj_sse(j, 0, 0, 0x17, (unsigned)b.reg, 0, &a, -1);
	}
	else if (j->avx)
		gsj_vex(j, 0, 0, 0, 0, 0x16, (unsigned)a.reg, (unsigned)a.reg, &b, -1);
	else
		gsj_sse(j, 0, 0, 0x16, (unsigned)a.reg, 0, &b, -1);
}

static void gsj_cvt2si(gsj* j, unsigned op, gsj_op r, gsj_op o)
{
	const int w = r.kind == GSJ_R64;
	if (j->avx)
		gsj_vex(j, 0xF2, 0, w, 0, op, (unsigned)r.reg, 0, &o, -1);
	else
		gsj_sse(j, 0xF2, 0, op, (unsigned)r.reg, w, &o, -1);
}

void gsj_cvtsd2si(gsj* j, gsj_op r, gsj_op op) { gsj_cvt2si(j, 0x2D, r, op); }
void gsj_cvttsd2si(gsj* j, gsj_op r, gsj_op op) { gsj_cvt2si(j, 0x2C, r, op); }

/* extractps and pextrd: x in reg, the destination in rm */
static void gsj_extract(gsj* j, unsigned op, gsj_op dst, gsj_op x, unsigned imm)
{
	if (j->avx)
		gsj_vex(j, 0x66, 0x3A, 0, 0, op, (unsigned)x.reg, 0, &dst, (int)(imm & 0xFF));
	else
		gsj_sse(j, 0x66, 0x3A, op, (unsigned)x.reg, 0, &dst, (int)(imm & 0xFF));
}

void gsj_extractps(gsj* j, gsj_op op, gsj_op x, unsigned imm) { gsj_extract(j, 0x17, op, x, imm); }
void gsj_pextrd(gsj* j, gsj_op op, gsj_op x, unsigned imm) { gsj_extract(j, 0x16, op, x, imm); }

/* pextrw: into a register, 0F C5 with the register in reg; into memory,
 * 0F 3A 15 */
void gsj_pextrw(gsj* j, gsj_op op, gsj_op x, unsigned imm)
{
	if (op.kind == GSJ_MEM)
	{
		gsj_extract(j, 0x15, op, x, imm);
		return;
	}
	if (j->avx)
		gsj_vex(j, 0x66, 0, 0, 0, 0xC5, (unsigned)op.reg, 0, &x, (int)(imm & 0xFF));
	else
		gsj_sse(j, 0x66, 0, 0xC5, (unsigned)op.reg, op.kind == GSJ_R64, &x, (int)(imm & 0xFF));
}

void gsj_pmovmskb(gsj* j, gsj_op r, gsj_op x)
{
	if (j->avx)
		gsj_vex(j, 0x66, 0, 0, gsj_ymm(&x), 0xD7, (unsigned)r.reg, 0, &x, -1);
	else
		gsj_sse(j, 0x66, 0, 0xD7, (unsigned)r.reg, r.kind == GSJ_R64, &x, -1);
}

void gsj_pblendvb(gsj* j, gsj_op x, gsj_op op)
{
	if (j->avx)
		gsj_vpblendvb(j, x, x, op, gsj_x(0));
	else
		gsj_sse(j, 0x66, 0x38, 0x10, (unsigned)x.reg, 0, &op, -1);
}

void gsj_vpblendvb(gsj* j, gsj_op x, gsj_op x2, gsj_op op, gsj_op x4)
{
	gsj_vex(j, 0x66, 0x3A, 0, gsj_ymm(&x), 0x4C, (unsigned)x.reg, (unsigned)x2.reg, &op, ((unsigned)x4.reg << 4) & 0xFF);
}

/* --- AVX and AVX2 only ----------------------------------------------- */

void gsj_vbroadcastss(gsj* j, gsj_op x, gsj_op op)
{
	gsj_vex(j, 0x66, 0x38, 0, gsj_ymm(&x), 0x18, (unsigned)x.reg, 0, &op, -1);
}

void gsj_vbroadcastsd(gsj* j, gsj_op y, gsj_op op)
{
	gsj_vex(j, 0x66, 0x38, 0, gsj_ymm(&y), 0x19, (unsigned)y.reg, 0, &op, -1);
}

void gsj_vbroadcasti128(gsj* j, gsj_op y, gsj_op m)
{
	gsj_vex(j, 0x66, 0x38, 0, gsj_ymm(&y), 0x5A, (unsigned)y.reg, 0, &m, -1);
}

void gsj_vbroadcastf128(gsj* j, gsj_op y, gsj_op m)
{
	gsj_vex(j, 0x66, 0x38, 0, gsj_ymm(&y), 0x1A, (unsigned)y.reg, 0, &m, -1);
}

void gsj_vextracti128(gsj* j, gsj_op op, gsj_op y, unsigned imm)
{
	gsj_vex(j, 0x66, 0x3A, 0, 1, 0x39, (unsigned)y.reg, 0, &op, (int)(imm & 0xFF));
}

void gsj_vinsertf128(gsj* j, gsj_op y1, gsj_op y2, gsj_op op, unsigned imm)
{
	gsj_vex(j, 0x66, 0x3A, 0, 1, 0x18, (unsigned)y1.reg, (unsigned)y2.reg, &op, (int)(imm & 0xFF));
}

void gsj_vinserti128(gsj* j, gsj_op y1, gsj_op y2, gsj_op op, unsigned imm)
{
	gsj_vex(j, 0x66, 0x3A, 0, 1, 0x38, (unsigned)y1.reg, (unsigned)y2.reg, &op, (int)(imm & 0xFF));
}

void gsj_vpbroadcastd(gsj* j, gsj_op x, gsj_op op)
{
	gsj_vex(j, 0x66, 0x38, 0, gsj_ymm(&x), 0x58, (unsigned)x.reg, 0, &op, -1);
}

void gsj_vpbroadcastq(gsj* j, gsj_op x, gsj_op op)
{
	gsj_vex(j, 0x66, 0x38, 0, gsj_ymm(&x), 0x59, (unsigned)x.reg, 0, &op, -1);
}

void gsj_vpbroadcastw(gsj* j, gsj_op x, gsj_op op)
{
	gsj_vex(j, 0x66, 0x38, 0, gsj_ymm(&x), 0x79, (unsigned)x.reg, 0, &op, -1);
}

void gsj_vpermq(gsj* j, gsj_op y, gsj_op op, unsigned imm)
{
	gsj_vex(j, 0x66, 0x3A, 1, gsj_ymm(&y), 0x00, (unsigned)y.reg, 0, &op, (int)(imm & 0xFF));
}

void gsj_vpgatherdd(gsj* j, gsj_op x1, gsj_op m, gsj_op x2)
{
	gsj_vex(j, 0x66, 0x38, 0, gsj_ymm(&x1), 0x90, (unsigned)x1.reg, (unsigned)x2.reg, &m, -1);
}

void gsj_vpsravd(gsj* j, gsj_op x1, gsj_op x2, gsj_op op)
{
	gsj_vex(j, 0x66, 0x38, 0, gsj_ymm(&x1), 0x46, (unsigned)x1.reg, (unsigned)x2.reg, &op, -1);
}

void gsj_vpsrlvd(gsj* j, gsj_op x1, gsj_op x2, gsj_op op)
{
	gsj_vex(j, 0x66, 0x38, 0, gsj_ymm(&x1), 0x45, (unsigned)x1.reg, (unsigned)x2.reg, &op, -1);
}
