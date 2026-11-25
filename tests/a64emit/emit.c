/*
 * The AArch64 emitter over every op's cases (cases.h), each op's words
 * hashed and held to the hash of the words vixl's MacroAssembler emitted
 * for the same cases (golden.h). Host independent: it only encodes.
 *
 * a64emit_test          check
 * a64emit_test dump OP  print each case's words (to diff against vixl)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cases.h"
#include "golden.h"

#define BUF_BYTES (4u << 20)

static void filler(a64_emit* e, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; i++)
		a64_word(e, 0xD503201Fu);
}

static void run_labels(a64_emit* e, const tcase* c)
{
	a64_label l = A64_LABEL_INIT;
	a64r r      = c->r[0];
	unsigned n  = c->u[1];
	unsigned cc = c->u[2];
	switch (c->u[0])
	{
		case 0: a64_b(e, &l); filler(e, n); a64_bind(e, &l); break;
		case 1: a64_b_cond(e, &l, cc); filler(e, n); a64_bind(e, &l); break;
		case 2: a64_bind(e, &l); filler(e, n); a64_b_cond(e, &l, cc); break;
		case 3:
			if (cc & 1) a64_cbz(e, r, &l); else a64_cbnz(e, r, &l);
			filler(e, n);
			a64_bind(e, &l);
			break;
		case 4:
			a64_bind(e, &l);
			filler(e, n);
			if (cc & 1) a64_cbz(e, r, &l); else a64_cbnz(e, r, &l);
			break;
		case 5:
			a64_b(e, &l);
			a64_b_cond(e, &l, cc);
			filler(e, n & 7);
			a64_cbnz(e, r, &l);
			a64_tbz(e, r, cc & 31, &l);
			filler(e, n);
			a64_bind(e, &l);
			a64_b(e, &l);
			a64_b_cond(e, &l, cc);
			break;
		case 6:
			if (n & 1) a64_tbz(e, r, cc, &l); else a64_tbnz(e, r, cc, &l);
			filler(e, n);
			a64_bind(e, &l);
			break;
		default:
			a64_bind(e, &l);
			filler(e, n);
			if (n & 1) a64_tbz(e, r, cc, &l); else a64_tbnz(e, r, cc, &l);
			break;
	}
}

static void run_case(a64_emit* e, const tcase* c)
{
	const a64r* r = c->r;
	switch (c->op)
	{
		case OP_MOV: a64_mov(e, r[0], r[1]); break;
		case OP_MOV_IMM: a64_mov_imm(e, r[0], c->uimm); break;
		case OP_MVN: a64_mvn(e, r[0], r[1]); break;
		case OP_MOVZ: a64_movz(e, r[0], c->uimm, (int)c->u[0]); break;
		case OP_MOVN: a64_movn(e, r[0], c->uimm, (int)c->u[0]); break;
		case OP_MOVK: a64_movk(e, r[0], c->uimm, (int)c->u[0]); break;
		case OP_ADD: a64_add(e, r[0], r[1], r[2]); break;
		case OP_ADD_IMM: a64_add_imm(e, r[0], r[1], c->imm); break;
		case OP_ADD_SHIFT: a64_add_shift(e, r[0], r[1], r[2], c->u[0], c->u[1]); break;
		case OP_ADD_EXT: a64_add_ext(e, r[0], r[1], r[2], c->u[0], c->u[1]); break;
		case OP_ADDS: a64_adds(e, r[0], r[1], r[2]); break;
		case OP_ADDS_IMM: a64_adds_imm(e, r[0], r[1], c->imm); break;
		case OP_SUB: a64_sub(e, r[0], r[1], r[2]); break;
		case OP_SUB_IMM: a64_sub_imm(e, r[0], r[1], c->imm); break;
		case OP_SUB_SHIFT: a64_sub_shift(e, r[0], r[1], r[2], c->u[0], c->u[1]); break;
		case OP_SUBS: a64_subs(e, r[0], r[1], r[2]); break;
		case OP_SUBS_IMM: a64_subs_imm(e, r[0], r[1], c->imm); break;
		case OP_CMP: a64_cmp(e, r[0], r[1]); break;
		case OP_CMP_IMM: a64_cmp_imm(e, r[0], c->imm); break;
		case OP_CMP_SHIFT: a64_cmp_shift(e, r[0], r[1], c->u[0], c->u[1]); break;
		case OP_CMN: a64_cmn(e, r[0], r[1]); break;
		case OP_CMN_IMM: a64_cmn_imm(e, r[0], c->imm); break;
		case OP_NEG: a64_neg(e, r[0], r[1]); break;
		case OP_AND: a64_and(e, r[0], r[1], r[2]); break;
		case OP_AND_IMM: a64_and_imm(e, r[0], r[1], c->uimm); break;
		case OP_AND_SHIFT: a64_and_shift(e, r[0], r[1], r[2], c->u[0], c->u[1]); break;
		case OP_ANDS: a64_ands(e, r[0], r[1], r[2]); break;
		case OP_ANDS_IMM: a64_ands_imm(e, r[0], r[1], c->uimm); break;
		case OP_ORR: a64_orr(e, r[0], r[1], r[2]); break;
		case OP_ORR_IMM: a64_orr_imm(e, r[0], r[1], c->uimm); break;
		case OP_ORR_SHIFT: a64_orr_shift(e, r[0], r[1], r[2], c->u[0], c->u[1]); break;
		case OP_EOR: a64_eor(e, r[0], r[1], r[2]); break;
		case OP_EOR_IMM: a64_eor_imm(e, r[0], r[1], c->uimm); break;
		case OP_EOR_SHIFT: a64_eor_shift(e, r[0], r[1], r[2], c->u[0], c->u[1]); break;
		case OP_BIC: a64_bic(e, r[0], r[1], r[2]); break;
		case OP_BIC_IMM: a64_bic_imm(e, r[0], r[1], c->uimm); break;
		case OP_TST: a64_tst(e, r[0], r[1]); break;
		case OP_TST_IMM: a64_tst_imm(e, r[0], c->uimm); break;
		case OP_LSL: a64_lsl(e, r[0], r[1], r[2]); break;
		case OP_LSR: a64_lsr(e, r[0], r[1], r[2]); break;
		case OP_ASR: a64_asr(e, r[0], r[1], r[2]); break;
		case OP_LSL_IMM: a64_lsl_imm(e, r[0], r[1], c->u[0]); break;
		case OP_LSR_IMM: a64_lsr_imm(e, r[0], r[1], c->u[0]); break;
		case OP_ASR_IMM: a64_asr_imm(e, r[0], r[1], c->u[0]); break;
		case OP_UBFX: a64_ubfx(e, r[0], r[1], c->u[0], c->u[1]); break;
		case OP_BFI: a64_bfi(e, r[0], r[1], c->u[0], c->u[1]); break;
		case OP_SXTB: a64_sxtb(e, r[0], r[1]); break;
		case OP_SXTH: a64_sxth(e, r[0], r[1]); break;
		case OP_SXTW: a64_sxtw(e, r[0], r[1]); break;
		case OP_UXTB: a64_uxtb(e, r[0], r[1]); break;
		case OP_UXTH: a64_uxth(e, r[0], r[1]); break;
		case OP_MUL: a64_mul(e, r[0], r[1], r[2]); break;
		case OP_MSUB: a64_msub(e, r[0], r[1], r[2], r[3]); break;
		case OP_SMULL: a64_smull(e, r[0], r[1], r[2]); break;
		case OP_UMULL: a64_umull(e, r[0], r[1], r[2]); break;
		case OP_SDIV: a64_sdiv(e, r[0], r[1], r[2]); break;
		case OP_UDIV: a64_udiv(e, r[0], r[1], r[2]); break;
		case OP_CLS: a64_cls(e, r[0], r[1]); break;
		case OP_REV64: a64_rev64(e, r[0], r[1]); break;
		case OP_CSEL: a64_csel(e, r[0], r[1], r[2], c->u[0]); break;
		case OP_CSINC: a64_csinc(e, r[0], r[1], r[2], c->u[0]); break;
		case OP_CSINV: a64_csinv(e, r[0], r[1], r[2], c->u[0]); break;
		case OP_CSET: a64_cset(e, r[0], c->u[0]); break;
		case OP_BR: a64_br(e, r[0]); break;
		case OP_BLR: a64_blr(e, r[0]); break;
		case OP_RET: a64_ret(e); break;
		case OP_ADRP: a64_adrp(e, r[0], c->imm); break;
		case OP_MSR_FPCR: a64_msr_fpcr(e, r[0]); break;
		case OP_B_DISP: a64_b_disp(e, c->imm); break;
		case OP_BL_DISP: a64_bl_disp(e, c->imm); break;
		case OP_B_COND_DISP: a64_b_cond_disp(e, c->imm, c->u[0]); break;
		case OP_CBZ_DISP: a64_cbz_disp(e, r[0], c->imm); break;
		case OP_CBNZ_DISP: a64_cbnz_disp(e, r[0], c->imm); break;
		case OP_LDR: a64_ldr(e, r[0], c->m); break;
		case OP_STR: a64_str(e, r[0], c->m); break;
		case OP_LDRB: a64_ldrb(e, r[0], c->m); break;
		case OP_LDRSB: a64_ldrsb(e, r[0], c->m); break;
		case OP_STRB: a64_strb(e, r[0], c->m); break;
		case OP_LDRH: a64_ldrh(e, r[0], c->m); break;
		case OP_LDRSH: a64_ldrsh(e, r[0], c->m); break;
		case OP_STRH: a64_strh(e, r[0], c->m); break;
		case OP_LDP: a64_ldp(e, r[0], r[1], c->m); break;
		case OP_STP: a64_stp(e, r[0], r[1], c->m); break;
		case OP_ST1_LANE: a64_st1_lane(e, r[0], c->u[0], c->m); break;
		case OP_FMOV: a64_fmov(e, r[0], r[1]); break;
		case OP_FADD: a64_fadd(e, r[0], r[1], r[2]); break;
		case OP_FSUB: a64_fsub(e, r[0], r[1], r[2]); break;
		case OP_FMUL: a64_fmul(e, r[0], r[1], r[2]); break;
		case OP_FMUL_ELEM: a64_fmul_elem(e, r[0], r[1], r[2], c->u[0]); break;
		case OP_FDIV: a64_fdiv(e, r[0], r[1], r[2]); break;
		case OP_FMIN: a64_fmin(e, r[0], r[1], r[2]); break;
		case OP_FMAX: a64_fmax(e, r[0], r[1], r[2]); break;
		case OP_FMINNM: a64_fminnm(e, r[0], r[1], r[2]); break;
		case OP_FMAXNM: a64_fmaxnm(e, r[0], r[1], r[2]); break;
		case OP_FADDP: a64_faddp(e, r[0], r[1], r[2]); break;
		case OP_FADDP_SCALAR: a64_faddp_scalar(e, r[0], r[1]); break;
		case OP_FSQRT: a64_fsqrt(e, r[0], r[1]); break;
		case OP_FCMEQ: a64_fcmeq(e, r[0], r[1], r[2]); break;
		case OP_FCMEQ_ZERO: a64_fcmeq_zero(e, r[0], r[1]); break;
		case OP_FCMGE: a64_fcmge(e, r[0], r[1], r[2]); break;
		case OP_FCVTZS: a64_fcvtzs(e, r[0], r[1]); break;
		case OP_SCVTF: a64_scvtf(e, r[0], r[1]); break;
		case OP_MOVI: a64_movi(e, r[0], c->uimm); break;
		case OP_MOVI_SHIFT: a64_movi_shift(e, r[0], (unsigned)c->uimm, c->u[0], c->u[1]); break;
		case OP_MVNI: a64_mvni(e, r[0], (unsigned)c->uimm, c->u[0], c->u[1]); break;
		case OP_BIC_VIMM: a64_bic_vimm(e, r[0], (unsigned)c->uimm, c->u[0]); break;
		case OP_ORR_VIMM: a64_orr_vimm(e, r[0], (unsigned)c->uimm, c->u[0]); break;
		case OP_DUP: a64_dup(e, r[0], r[1]); break;
		case OP_DUP_ELEM: a64_dup_elem(e, r[0], r[1], c->u[0]); break;
		case OP_INS: a64_ins(e, r[0], c->u[0], r[1]); break;
		case OP_INS_ELEM: a64_ins_elem(e, r[0], c->u[0], r[1], c->u[1]); break;
		case OP_UMOV: a64_umov(e, r[0], r[1], c->u[0]); break;
		case OP_EXT: a64_ext(e, r[0], r[1], r[2], c->u[0]); break;
		case OP_TBL: a64_tbl(e, r[0], r[1], r[2]); break;
		case OP_TBL2: a64_tbl2(e, r[0], r[1], r[3], r[2]); break;
		case OP_ZIP1: a64_zip1(e, r[0], r[1], r[2]); break;
		case OP_ZIP2: a64_zip2(e, r[0], r[1], r[2]); break;
		case OP_UZP1: a64_uzp1(e, r[0], r[1], r[2]); break;
		case OP_SHL: a64_shl(e, r[0], r[1], c->u[0]); break;
		case OP_USHR: a64_ushr(e, r[0], r[1], c->u[0]); break;
		case OP_SSHR: a64_sshr(e, r[0], r[1], c->u[0]); break;
		case OP_SHRN: a64_shrn(e, r[0], r[1], c->u[0]); break;
		case OP_XTN: a64_xtn(e, r[0], r[1]); break;
		case OP_SSHLL: a64_sshll(e, r[0], r[1], c->u[0]); break;
		case OP_USHLL: a64_ushll(e, r[0], r[1], c->u[0]); break;
		case OP_ADDV: a64_addv(e, r[0], r[1]); break;
		case OP_UMINV: a64_uminv(e, r[0], r[1]); break;
		case OP_UMAXV: a64_umaxv(e, r[0], r[1]); break;
		case OP_VADD: a64_add(e, r[0], r[1], r[2]); break;
		case OP_VSUB: a64_sub(e, r[0], r[1], r[2]); break;
		case OP_USHL: a64_ushl(e, r[0], r[1], r[2]); break;
		case OP_SMIN: a64_smin(e, r[0], r[1], r[2]); break;
		case OP_SMAX: a64_smax(e, r[0], r[1], r[2]); break;
		case OP_UMIN: a64_umin(e, r[0], r[1], r[2]); break;
		case OP_SQADD: a64_sqadd(e, r[0], r[1], r[2]); break;
		case OP_SQSUB: a64_sqsub(e, r[0], r[1], r[2]); break;
		case OP_UQADD: a64_uqadd(e, r[0], r[1], r[2]); break;
		case OP_UQSUB: a64_uqsub(e, r[0], r[1], r[2]); break;
		case OP_CMEQ: a64_cmeq(e, r[0], r[1], r[2]); break;
		case OP_CMGT: a64_cmgt(e, r[0], r[1], r[2]); break;
		case OP_SQABS: a64_sqabs(e, r[0], r[1]); break;
		case OP_ABS: a64_abs(e, r[0], r[1]); break;
		case OP_CMEQ_ZERO: a64_cmeq_zero(e, r[0], r[1]); break;
		case OP_CMGT_ZERO: a64_cmgt_zero(e, r[0], r[1]); break;
		case OP_CMGE_ZERO: a64_cmge_zero(e, r[0], r[1]); break;
		case OP_CMLE_ZERO: a64_cmle_zero(e, r[0], r[1]); break;
		case OP_CMLT_ZERO: a64_cmlt_zero(e, r[0], r[1]); break;
		case OP_VREV64: a64_rev64(e, r[0], r[1]); break;
		case OP_VMOV: a64_mov(e, r[0], r[1]); break;
		case OP_VMVN: a64_mvn(e, r[0], r[1]); break;
		case OP_VAND: a64_and(e, r[0], r[1], r[2]); break;
		case OP_VORR: a64_orr(e, r[0], r[1], r[2]); break;
		case OP_VEOR: a64_eor(e, r[0], r[1], r[2]); break;
		case OP_VBIC: a64_bic(e, r[0], r[1], r[2]); break;
		case OP_BSL: a64_bsl(e, r[0], r[1], r[2]); break;
		case OP_BIT: a64_bit(e, r[0], r[1], r[2]); break;
		case OP_BIF: a64_bif(e, r[0], r[1], r[2]); break;
		case OP_VUMULL: a64_umull(e, r[0], r[1], r[2]); break;
		case OP_LABELS: run_labels(e, c); break;
	}
}

int main(int argc, char** argv)
{
	static u32 buf[BUF_BYTES / 4];
	int dump_op = -1, op, fails = 0;
	unsigned i;
	if (argc > 2 && !strcmp(argv[1], "dump"))
	{
		for (op = 0; op < OP_COUNT; op++)
			if (!strcmp(argv[2], s_op_names[op]))
				dump_op = op;
		if (dump_op < 0)
		{
			fprintf(stderr, "no op %s\n", argv[2]);
			return 2;
		}
	}
	for (op = 0; op < OP_COUNT; op++)
	{
		u64 h = 0xcbf29ce484222325ull;
		if (dump_op >= 0 && op != dump_op)
			continue;
		for (i = 0; i < CASES_PER_OP; i++)
		{
			tcase c;
			a64_emit e;
			gen_case(op, i, &c);
			a64_init(&e, buf, sizeof(buf));
			if (c.narrow)
			{
				e.scratch &= ~(1u << 17);
				e.vscratch = 0;
			}
			run_case(&e, &c);
			if (e.error)
			{
				printf("%s case %u: emitter error\n", s_op_names[op], i);
				fails++;
			}
			h = hash_words(h, buf, (unsigned)(e.pos / 4));
			if (dump_op >= 0)
			{
				unsigned j, n = (unsigned)(e.pos / 4);
				printf("%u:", i);
				for (j = 0; j < n && j < 64; j++)
					printf(" %08x", (unsigned)buf[j]);
				if (n > 64)
					printf(" ... %08x (%u)", (unsigned)buf[n - 1], n);
				printf("\n");
			}
		}
		if (dump_op < 0 && h != s_golden[op])
		{
			printf("%s: %016llx, vixl %016llx\n", s_op_names[op], (unsigned long long)h, (unsigned long long)s_golden[op]);
			fails++;
		}
	}
	if (dump_op < 0)
		printf(fails ? "a64emit: %d FAILED\n" : "a64emit: all %d ops match\n", fails ? fails : OP_COUNT);
	return fails ? 1 : 0;
}
