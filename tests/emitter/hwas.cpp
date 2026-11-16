/*  The C emitter macros against GNU as.
 *
 *  tests/emitter/oracle.cpp compared the C++ emitter against the C macros
 *  in common/emitter/c89ops.h. The C++ side has since been removed --
 *  xRegisterBase, xIndirectVoid and the JMP8/JZ8 family survive only in
 *  the comments that say which macro mirrors which -- so that file cannot
 *  build and there is nothing left for it to compare against.
 *
 *  The macros themselves are what every recompiler now emits through, and
 *  nothing tested them. This does, against an oracle that cannot rot the
 *  same way: GNU as. Each case names an instruction in Intel syntax, this
 *  assembles it, and the bytes are compared with what the macro emits.
 *
 *  Being an external assembler rather than a second copy of the same
 *  logic, it disagrees for real reasons. A macro that mirrors the old C++
 *  emitter's quirks rather than the architecture would show up here and
 *  not in a comparison against that emitter.
 *
 *  Needs `as` and `objdump` on PATH; skips cleanly if either is missing.
 *
 *  Usage: tests/emitter/hwas
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "common/emitter/x86types.h"
#include "common/emitter/c89ops.h"

/* This harness links no recompiler object, so it owns the cursor. */
PCSX2_THREAD_LOCAL u8* x86Ptr PCSX2_TLS_INITIAL_EXEC = nullptr;
PCSX2_THREAD_LOCAL XMMSSEType g_xmmtypes[iREGCNT_XMM] PCSX2_TLS_INITIAL_EXEC = {XMMT_INT};

/* Somewhere to emit into. */
static uint8_t s_buf[256];

/* Cases are collected first and assembled in one batch. Running as and
 * objdump per instruction costs two processes a case, which is fine for a
 * handful and not for the exhaustive register sweeps below -- batching
 * takes the whole run to two processes total, which is what makes covering
 * every register pair affordable.
 *
 * One source line assembles to one instruction for everything here, so
 * objdump's Nth line is case N. That assumption is checked: if the counts
 * disagree the run stops rather than comparing misaligned pairs. */

/* Comfortably above the current count. The sweeps add 256 cases a line,
 * so this is checked rather than assumed: overflowing it silently would
 * drop the tail of the run and still print a clean score. */
#define MAXCASES 32768

static char*   s_text[MAXCASES];
static uint8_t s_got[MAXCASES][16];
static int     s_gotlen[MAXCASES];
static int     s_ncases;

static void add_case(const char* text, void (*emit)(void))
{
	if (s_ncases >= MAXCASES)
	{ printf("  case table full at %d; raise MAXCASES\n", MAXCASES); exit(2); }
	x86Ptr = s_buf;
	emit();
	s_gotlen[s_ncases] = (int)(x86Ptr - s_buf);
	memcpy(s_got[s_ncases], s_buf, (size_t)s_gotlen[s_ncases]);
	s_text[s_ncases] = strdup(text);
	s_ncases++;
}

static int failures, shown;

static int run_batch(void)
{
	FILE* f;
	char line[512];
	int i;

	f = fopen("/tmp/xe_oracle.s", "w");
	if (!f) return -1;
	fprintf(f, ".intel_syntax noprefix\n");
	for (i = 0; i < s_ncases; i++)
		fprintf(f, "%s\n", s_text[i]);
	fclose(f);

	if (system("as -o /tmp/xe_oracle.o /tmp/xe_oracle.s 2>/tmp/xe_oracle.err") != 0)
	{
		printf("  as rejected the batch; first errors:\n");
		f = fopen("/tmp/xe_oracle.err", "r");
		if (f)
		{
			int n = 0;
			while (n++ < 5 && fgets(line, sizeof(line), f)) printf("    %s", line);
			fclose(f);
		}
		return -1;
	}

	f = popen("objdump -d --insn-width=16 /tmp/xe_oracle.o"
	          " | sed -n 's/^ *[0-9a-f]*:\\t\\([0-9a-f ]*\\)\\t.*/\\1/p'", "r");
	if (!f) return -1;

	i = 0;
	while (i < s_ncases && fgets(line, sizeof(line), f))
	{
		uint8_t want[16];
		int nwant = 0;
		char* p = line;
		unsigned v;
		int consumed;

		while (nwant < 16 && sscanf(p, " %2x%n", &v, &consumed) == 1)
		{ want[nwant++] = (uint8_t)v; p += consumed; }
		if (nwant == 0) continue;

		if (s_gotlen[i] != nwant || memcmp(s_got[i], want, (size_t)nwant))
		{
			failures++;
			if (shown++ < 12)
			{
				int k;
				printf("  %-40s as:", s_text[i]);
				for (k = 0; k < nwant; k++) printf(" %02x", want[k]);
				printf("   macro:");
				for (k = 0; k < s_gotlen[i]; k++) printf(" %02x", s_got[i][k]);
				printf("\n");
			}
		}
		i++;
	}
	pclose(f);

	if (i != s_ncases)
	{
		printf("  objdump returned %d instructions for %d cases;"
		       " something assembled to more than one\n", i, s_ncases);
		return -1;
	}
	return 0;
}

/* Each case is the instruction text and a thunk that emits it. Kept
 * side by side so a mismatch names the instruction rather than a line. */
#define CASE(text, body) \
	do { struct L { static void f(void) { body; } }; add_case(text, &L::f); } while (0)

int main(void)
{
	if (system("as --version >/dev/null 2>&1") != 0
	 || system("objdump --version >/dev/null 2>&1") != 0)
	{
		printf("hwas: as or objdump not found; skipped\n");
		return 0;
	}

	/* The Win64 frame's xmm6-xmm15 saves: rsp needs a SIB byte, the low
	 * registers no REX and the high ones REX.R, and the last slots pass
	 * a byte displacement. */
	CASE("movups [rsp+0x20], xmm6",   { struct e_mem m; XE_MEM_BD(m, 4, 0x20); xe_movups_memxg(m, 6); });
	CASE("movups [rsp+0x30], xmm7",   { struct e_mem m; XE_MEM_BD(m, 4, 0x30); xe_movups_memxg(m, 7); });
	CASE("movups [rsp+0x40], xmm8",   { struct e_mem m; XE_MEM_BD(m, 4, 0x40); xe_movups_memxg(m, 8); });
	CASE("movups [rsp+0xb0], xmm15",  { struct e_mem m; XE_MEM_BD(m, 4, 0xb0); xe_movups_memxg(m, 15); });
	CASE("movups xmm6, [rsp+0x20]",   { struct e_mem m; XE_MEM_BD(m, 4, 0x20); xe_movups_xmemg(6, m); });
	CASE("movups xmm15, [rsp+0xb0]",  { struct e_mem m; XE_MEM_BD(m, 4, 0xb0); xe_movups_xmemg(15, m); });

	/* Register-to-register ALU, low and extended registers, since the REX
	 * prefix is where an encoder most often goes wrong. */
	CASE("add eax, ecx",     xe_add32_rr(0, 1));
	CASE("add ecx, eax",     xe_add32_rr(1, 0));
	CASE("add r8d, r9d",     xe_add32_rr(8, 9));
	CASE("add eax, r15d",    xe_add32_rr(0, 15));
	CASE("add r15d, eax",    xe_add32_rr(15, 0));
	CASE("sub eax, ecx",     xe_sub32_rr(0, 1));
	CASE("sub r10d, r11d",   xe_sub32_rr(10, 11));
	CASE("xor eax, eax",     xe_xor32_rr(0, 0));
	CASE("xor r12d, r13d",   xe_xor32_rr(12, 13));
	CASE("and eax, ecx",     xe_and32_rr(0, 1));
	CASE("or  eax, ecx",     xe_or32_rr(0, 1));
	CASE("cmp eax, ecx",     xe_cmp32_rr(0, 1));

	/* Immediates: the short form for a sign-extendable byte and the long
	 * form otherwise, and the eax special case. */
	CASE("add eax, 1",           xe_add32_ri(0, 1));
	CASE("add eax, 0x7f",        xe_add32_ri(0, 0x7f));
	CASE("add eax, 0x80",        xe_add32_ri(0, 0x80));
	CASE("add eax, 0x12345678",  xe_add32_ri(0, 0x12345678));
	CASE("add ecx, 1",           xe_add32_ri(1, 1));
	CASE("add ecx, 0x12345678",  xe_add32_ri(1, 0x12345678));
	CASE("add r8d, 1",           xe_add32_ri(8, 1));
	CASE("sub eax, 0x7fffffff",  xe_sub32_ri(0, 0x7fffffff));
	CASE("cmp eax, 0",           xe_cmp32_ri(0, 0));

	/* Moves. */
	CASE("mov eax, ecx",         xe_mov32_rr(0, 1));
	CASE("mov r8d, r9d",         xe_mov32_rr(8, 9));
	/* E_MOV_RI_SZ turns a zero immediate into XOR reg,reg deliberately --
	 * it saves bytes and the comment there says the C++ emitter did the
	 * same. So the oracle instruction is the XOR: comparing against a
	 * literal mov would be testing that the optimisation is absent. */
	CASE("xor eax, eax",         xe_mov32_ri(0, 0));
	CASE("xor r13d, r13d",       xe_mov32_ri(13, 0));
	CASE("mov eax, 0x12345678",  xe_mov32_ri(0, 0x12345678));
	CASE("mov r15d, 0x1",        xe_mov32_ri(15, 1));

	/* 64-bit forms: the REX.W prefix, and mov64_ri's choice between the
	 * sign-extended 32-bit form and the full ten-byte immediate. */
	CASE("mov rax, rcx",         xe_mov64_rr(0, 1));
	CASE("mov r8, r9",           xe_mov64_rr(8, 9));
	CASE("add rax, rcx",         xe_add64_rr(0, 1));
	CASE("add r10, r11",         xe_add64_rr(10, 11));
	CASE("cmp rax, rcx",         xe_cmp64_rr(0, 1));
	CASE("movsxd rax, ecx",      xe_movsxd_rr(0, 1));
	CASE("movsxd r8, r9d",       xe_movsxd_rr(8, 9));
	/* movsx from a word in memory writes the whole 32-bit register; the
	 * PDIVBW divisor is loaded this way. */
	CASE("movsx ecx, WORD PTR ds:0x12345678", xe_movsx32_rm16(1, 0x12345678));
	CASE("movsx r9d, WORD PTR ds:0x100",      xe_movsx32_rm16(9, 0x100));
	/* bsr carries REX for extended registers. */
	CASE("bsr eax, ecx",         xe_bsr32_rr(0, 1));
	CASE("bsr r8d, r9d",         xe_bsr32_rr(8, 9));
	CASE("bsr eax, r12d",        xe_bsr32_rr(0, 12));
	/* Byte registers 4..7 are spl/bpl/sil/dil, never ah/ch/dh/bh. */
	CASE("movzx eax, dil",       xe_movzx32_r8(0, 7));
	CASE("movsx ecx, sil",       xe_movsx32_r8(1, 6));
	CASE("movzx r8d, r9b",       xe_movzx32_r8(8, 9));
	CASE("test sil, sil",        xe_test8_rr(6, 6));
	CASE("test al, al",          xe_test8_rr(0, 0));
	CASE("test dil, 0x2",        xe_test8_ri(7, 2));
	CASE("test al, 0x2",         xe_test8_ri(0, 2));
	CASE("cmp bpl, 0x5",         xe_cmp8_ri(5, 5));
	CASE("cmp al, 0x0",          xe_cmp8_ri(0, 0));
	/* An index with no base, extended or not: the SIB index field takes
	 * the low three bits and REX.X the fourth. */
	CASE("mov rax, QWORD PTR [rcx*4+0x1000]",
		struct e_mem m_; E_MEM(m_, E_NOREG, 1, 4, 0x1000); xe_mov64_rmemg(0, m_));
	CASE("mov rax, QWORD PTR [r8*4+0x1000]",
		struct e_mem m_; E_MEM(m_, E_NOREG, 8, 4, 0x1000); xe_mov64_rmemg(0, m_));
	CASE("mov r9, QWORD PTR [r15*8+0x40]",
		struct e_mem m_; E_MEM(m_, E_NOREG, 15, 8, 0x40); xe_mov64_rmemg(9, m_));

	/* Shifts by an immediate, including the one-bit short form. */
	CASE("shl eax, 1",           xe_shl32_ri(0, 1));
	CASE("shl eax, 5",           xe_shl32_ri(0, 5));
	CASE("shl r12d, 31",         xe_shl32_ri(12, 31));
	CASE("shr eax, 1",           xe_shr32_ri(0, 1));
	CASE("shr ecx, 7",           xe_shr32_ri(1, 7));
	CASE("sar eax, 1",           xe_sar32_ri(0, 1));
	CASE("sar r13d, 12",         xe_sar32_ri(13, 12));
	CASE("shl rax, 1",           xe_shl64_ri(0, 1));
	CASE("shl rax, 40",          xe_shl64_ri(0, 40));
	CASE("shr rcx, 33",          xe_shr64_ri(1, 33));
	CASE("sar rdx, 63",          xe_sar64_ri(2, 63));

	/* Unary forms and test, which the branch paths lean on. */
	CASE("not eax",              xe_not32_r(0));
	CASE("not r14d",             xe_not32_r(14));
	CASE("neg eax",              xe_neg32_r(0));
	CASE("neg r11d",             xe_neg32_r(11));
	CASE("not rax",              xe_not64_r(0));
	CASE("test eax, eax",        xe_test32_rr(0, 0));
	CASE("test ecx, edx",        xe_test32_rr(1, 2));
	CASE("test r8d, r9d",        xe_test32_rr(8, 9));
	CASE("test rax, rax",        xe_test64_rr(0, 0));

	/* Sign and zero extension, where the operand widths differ. */
	CASE("movzx eax, cl",        xe_movzx32_r8(0, 1));
	CASE("movzx eax, cx",        xe_movzx32_r16(0, 1));
	CASE("movzx r8d, r9b",       xe_movzx32_r8(8, 9));

	/* imul32_r is the one-operand form: edx:eax = eax * r. */
	CASE("imul ecx",             xe_imul32_r(1));
	CASE("imul r11d",            xe_imul32_r(11));

	/* SSE. The recompilers emit far more of this than of the integer ALU,
	 * and the register-extension bit lands in a different place from the
	 * general-purpose forms. */
	CASE("movaps xmm0, xmm1",        xe_movaps_xx(0, 1));
	CASE("movaps xmm8, xmm9",        xe_movaps_xx(8, 9));
	CASE("movdqa xmm0, xmm1",        xe_movdqa_xx(0, 1));
	CASE("movdqa xmm10, xmm11",      xe_movdqa_xx(10, 11));
	CASE("paddd xmm0, xmm1",         xe_paddd_xx(0, 1));
	CASE("paddd xmm12, xmm13",       xe_paddd_xx(12, 13));
	CASE("psubd xmm0, xmm1",         xe_psubd_xx(0, 1));
	CASE("pand xmm2, xmm3",          xe_pand_xx(2, 3));
	CASE("por xmm4, xmm5",           xe_por_xx(4, 5));
	CASE("pxor xmm6, xmm7",          xe_pxor_xx(6, 7));
	CASE("pxor xmm15, xmm15",        xe_pxor_xx(15, 15));
	CASE("punpckldq xmm0, xmm1",     xe_punpckldq_xx(0, 1));
	CASE("punpckhdq xmm0, xmm1",     xe_punpckhdq_xx(0, 1));
	CASE("punpcklqdq xmm8, xmm9",    xe_punpcklqdq_xx(8, 9));
	CASE("punpcklbw xmm0, xmm1",     xe_punpcklbw_xx(0, 1));
	CASE("punpckhwd xmm2, xmm3",     xe_punpckhwd_xx(2, 3));

	/* Memory operands. This is where an x86 encoder is easiest to get
	 * wrong -- ModRM, the SIB byte and the displacement all interact --
	 * and the register-only cases above would not catch any of it.
	 *
	 * The addresses here are absolute and fit in 32 bits, which the
	 * emitter encodes with the no-base SIB form (modrm 04, sib 25). The
	 * ds: prefix in the oracle text is how Intel syntax spells the same
	 * thing to as; without it, as assembles a RIP-relative operand and
	 * every case disagrees for a reason that has nothing to do with the
	 * emitter. */
	CASE("add eax, DWORD PTR ds:0x12345678", xe_add32_rm(0, 0x12345678));
	CASE("add ecx, DWORD PTR ds:0x100",      xe_add32_rm(1, 0x100));
	CASE("add r8d, DWORD PTR ds:0x12345678", xe_add32_rm(8, 0x12345678));
	CASE("sub eax, DWORD PTR ds:0x12345678", xe_sub32_rm(0, 0x12345678));
	CASE("cmp eax, DWORD PTR ds:0x12345678", xe_cmp32_rm(0, 0x12345678));
	CASE("mov eax, DWORD PTR ds:0x12345678", xe_mov32_rm(0, 0x12345678));
	CASE("mov r15d, DWORD PTR ds:0x40",      xe_mov32_rm(15, 0x40));
	CASE("mov DWORD PTR ds:0x12345678, ecx", xe_mov32_mr(0x12345678, 1));
	CASE("mov DWORD PTR ds:0x40, r14d",      xe_mov32_mr(0x40, 14));
	CASE("sub DWORD PTR ds:0x12345678, edx", xe_sub32_mr(0x12345678, 2));
	CASE("mov rax, QWORD PTR ds:0x12345678", xe_mov64_rm(0, 0x12345678));
	CASE("mov QWORD PTR ds:0x12345678, rcx", xe_mov64_mr(0x12345678, 1));
	CASE("add rax, QWORD PTR ds:0x12345678", xe_add64_rm(0, 0x12345678));

	/* Immediates to memory, where the operand size has to be spelled out
	 * because neither side can infer it. */
	CASE("mov DWORD PTR ds:0x12345678, 0x1",        xe_mov32_mi(0x12345678, 1));
	CASE("mov DWORD PTR ds:0x100, 0xdeadbeef",      xe_mov32_mi(0x100, 0xdeadbeef));
	CASE("cmp DWORD PTR ds:0x12345678, 0x7f",       xe_cmp32_mi(0x12345678, 0x7f));
	CASE("cmp DWORD PTR ds:0x12345678, 0x12345678", xe_cmp32_mi(0x12345678, 0x12345678));
	CASE("sub DWORD PTR ds:0x100, 0x10",            xe_sub32_mi(0x100, 0x10));

	/* Sweeping one op over all 256 register pairs is a line, so the set
	 * below is the instruction mix the recompilers actually emit rather
	 * than a sample of it. */
#define SWEEP_GUARD()                                                         \
	do { if (s_ncases >= MAXCASES)                                            \
		{ printf("  case table full at %d; raise MAXCASES\n", MAXCASES);      \
		  exit(2); } } while (0)

#define SWEEP_RR(text, macro, names)                                          \
	do { int d_, e_; char t_[96];                                             \
		for (d_ = 0; d_ < 16; d_++) for (e_ = 0; e_ < 16; e_++) {             \
			SWEEP_GUARD();                                                    \
			snprintf(t_, sizeof(t_), "%s %s, %s", text, names[d_], names[e_]);\
			x86Ptr = s_buf; macro(d_, e_);                                    \
			s_gotlen[s_ncases] = (int)(x86Ptr - s_buf);                       \
			memcpy(s_got[s_ncases], s_buf, (size_t)s_gotlen[s_ncases]);       \
			s_text[s_ncases++] = strdup(t_); } } while (0)

#define SWEEP_RI(text, macro, names, imm)                                     \
	do { int d_; char t_[96];                                                 \
		for (d_ = 0; d_ < 16; d_++) {                                         \
			SWEEP_GUARD();                                                    \
			snprintf(t_, sizeof(t_), "%s %s, %d", text, names[d_], (int)(imm));\
			x86Ptr = s_buf; macro(d_, imm);                                   \
			s_gotlen[s_ncases] = (int)(x86Ptr - s_buf);                       \
			memcpy(s_got[s_ncases], s_buf, (size_t)s_gotlen[s_ncases]);       \
			s_text[s_ncases++] = strdup(t_); } } while (0)

	/* Exhaustive register sweeps. The hand-written cases above pick a few
	 * registers each, which catches an opcode mistake but not one that
	 * only shows for a particular encoding -- rsp and rbp are the classic
	 * pair, since 4 and 5 in a ModRM field mean "SIB follows" and "disp32,
	 * no base", and r12 and r13 inherit the same treatment through REX.
	 * These loops cover all 256 pairs for each op instead. */
	{
		static const char* const r32[16] = {
			"eax","ecx","edx","ebx","esp","ebp","esi","edi",
			"r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d" };
		static const char* const r64[16] = {
			"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
			"r8","r9","r10","r11","r12","r13","r14","r15" };
		static const char* const rxmm[16] = {
			"xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
			"xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15" };
		char t[96];
		int d, e;

		for (d = 0; d < 16; d++)
			for (e = 0; e < 16; e++)
			{
				snprintf(t, sizeof(t), "add %s, %s", r32[d], r32[e]);
				{ const int dd = d, ee = e;
				  x86Ptr = s_buf; xe_add32_rr(dd, ee);
				  s_gotlen[s_ncases] = (int)(x86Ptr - s_buf);
				  memcpy(s_got[s_ncases], s_buf, (size_t)s_gotlen[s_ncases]);
				  s_text[s_ncases++] = strdup(t); }

				/* E_MOV_RR elides a move from a register to itself and
				 * emits nothing; E_MOV_RR_RAW exists for callers that need
				 * the bytes. as always emits the instruction, so the
				 * diagonal is not a case -- comparing it would be asserting
				 * the elision is absent. */
				if (d != e)
				{
				snprintf(t, sizeof(t), "mov %s, %s", r64[d], r64[e]);
				{ const int dd = d, ee = e;
				  x86Ptr = s_buf; xe_mov64_rr(dd, ee);
				  s_gotlen[s_ncases] = (int)(x86Ptr - s_buf);
				  memcpy(s_got[s_ncases], s_buf, (size_t)s_gotlen[s_ncases]);
				  s_text[s_ncases++] = strdup(t); }
				}

				snprintf(t, sizeof(t), "movaps %s, %s", rxmm[d], rxmm[e]);
				{ const int dd = d, ee = e;
				  x86Ptr = s_buf; xe_movaps_xx(dd, ee);
				  s_gotlen[s_ncases] = (int)(x86Ptr - s_buf);
				  memcpy(s_got[s_ncases], s_buf, (size_t)s_gotlen[s_ncases]);
				  s_text[s_ncases++] = strdup(t); }

				snprintf(t, sizeof(t), "paddd %s, %s", rxmm[d], rxmm[e]);
				{ const int dd = d, ee = e;
				  x86Ptr = s_buf; xe_paddd_xx(dd, ee);
				  s_gotlen[s_ncases] = (int)(x86Ptr - s_buf);
				  memcpy(s_got[s_ncases], s_buf, (size_t)s_gotlen[s_ncases]);
				  s_text[s_ncases++] = strdup(t); }
			}

		/* And every register against a memory operand, which is where the
		 * rsp/rbp encodings actually bite. */
		for (d = 0; d < 16; d++)
		{
			snprintf(t, sizeof(t), "mov %s, DWORD PTR ds:0x12345678", r32[d]);
			{ const int dd = d;
			  x86Ptr = s_buf; xe_mov32_rm(dd, 0x12345678);
			  s_gotlen[s_ncases] = (int)(x86Ptr - s_buf);
			  memcpy(s_got[s_ncases], s_buf, (size_t)s_gotlen[s_ncases]);
			  s_text[s_ncases++] = strdup(t); }

			snprintf(t, sizeof(t), "mov DWORD PTR ds:0x12345678, %s", r32[d]);
			{ const int dd = d;
			  x86Ptr = s_buf; xe_mov32_mr(0x12345678, dd);
			  s_gotlen[s_ncases] = (int)(x86Ptr - s_buf);
			  memcpy(s_got[s_ncases], s_buf, (size_t)s_gotlen[s_ncases]);
			  s_text[s_ncases++] = strdup(t); }
		}

		/* The rest of the integer ALU, both widths. */
		SWEEP_RR("sub", xe_sub32_rr, r32);
		SWEEP_RR("and", xe_and32_rr, r32);
		SWEEP_RR("or",  xe_or32_rr,  r32);
		SWEEP_RR("xor", xe_xor32_rr, r32);
		SWEEP_RR("cmp", xe_cmp32_rr, r32);
		SWEEP_RR("test", xe_test32_rr, r32);
		SWEEP_RR("add", xe_add64_rr, r64);
		SWEEP_RR("sub", xe_sub64_rr, r64);
		SWEEP_RR("cmp", xe_cmp64_rr, r64);
		SWEEP_RR("test", xe_test64_rr, r64);
		/* movsxd's destination is 64-bit and its source 32-bit, so it needs
		 * a name from each table rather than one twice -- sweeping it with
		 * r64 on both sides makes as reject the batch outright. */
		{
			int d2, e2;
			char t3[96];
			for (d2 = 0; d2 < 16; d2++)
				for (e2 = 0; e2 < 16; e2++)
				{
					snprintf(t3, sizeof(t3), "movsxd %s, %s", r64[d2], r32[e2]);
					x86Ptr = s_buf; xe_movsxd_rr(d2, e2);
					s_gotlen[s_ncases] = (int)(x86Ptr - s_buf);
					memcpy(s_got[s_ncases], s_buf, (size_t)s_gotlen[s_ncases]);
					s_text[s_ncases++] = strdup(t3);
				}
		}

		/* The rest of the SSE mix. */
		SWEEP_RR("movdqa", xe_movdqa_xx, rxmm);
		SWEEP_RR("psubd",  xe_psubd_xx,  rxmm);
		SWEEP_RR("pand",   xe_pand_xx,   rxmm);
		SWEEP_RR("por",    xe_por_xx,    rxmm);
		SWEEP_RR("pxor",   xe_pxor_xx,   rxmm);
		SWEEP_RR("punpckldq",  xe_punpckldq_xx,  rxmm);
		SWEEP_RR("punpckhdq",  xe_punpckhdq_xx,  rxmm);
		SWEEP_RR("punpcklqdq", xe_punpcklqdq_xx, rxmm);
		SWEEP_RR("punpcklbw",  xe_punpcklbw_xx,  rxmm);
		SWEEP_RR("punpckhwd",  xe_punpckhwd_xx,  rxmm);

		/* Shifts across every count, which is where the one-bit short form
		 * and the 0x1f masking both live. */
		{
			int sh;
			for (sh = 1; sh < 32; sh++)
			{
				SWEEP_RI("shl", xe_shl32_ri, r32, sh);
				SWEEP_RI("shr", xe_shr32_ri, r32, sh);
				SWEEP_RI("sar", xe_sar32_ri, r32, sh);
			}
		}

		/* Immediates at their encoding boundaries, over every register.
		 * The register sweeps above vary the ModRM byte and leave the
		 * immediate alone, so they say nothing about where the short form
		 * ends: 0x7f fits a sign-extended byte and 0x80 does not, -0x80
		 * fits and -0x81 does not, and the eax forms have their own
		 * opcode. Those four transitions are the whole of the choice. */
		{
			static const long kImm[] = {
				0, 1, -1, 0x7f, 0x80, -0x80, -0x81,
				0x7fff, 0x7fffffffL, -0x7fffffffL - 1
			};
			const int nimm = (int)(sizeof(kImm)/sizeof(kImm[0]));
			int ii;
			for (ii = 0; ii < nimm; ii++)
			{
				SWEEP_RI("add", xe_add32_ri, r32, kImm[ii]);
				SWEEP_RI("sub", xe_sub32_ri, r32, kImm[ii]);
				SWEEP_RI("cmp", xe_cmp32_ri, r32, kImm[ii]);
				SWEEP_RI("and", xe_and32_ri, r32, kImm[ii]);
				SWEEP_RI("or",  xe_or32_ri,  r32, kImm[ii]);
				SWEEP_RI("xor", xe_xor32_ri, r32, kImm[ii]);
			}
		}

		/* Memory operands at several displacements. One address exercises
		 * one displacement encoding; these cover a zero, a byte-sized one,
		 * a word-sized one and a full 32-bit one. */
		{
			static const unsigned long kAddr[] = {
				0x0, 0x40, 0x7f, 0x80, 0x1000, 0x12345678UL, 0x7ffffff0UL
			};
			const int naddr = (int)(sizeof(kAddr)/sizeof(kAddr[0]));
			int ai, rr;
			char t4[96];
			for (ai = 0; ai < naddr; ai++)
				for (rr = 0; rr < 16; rr++)
				{
					SWEEP_GUARD();
					snprintf(t4, sizeof(t4), "add %s, DWORD PTR ds:0x%lx",
					         r32[rr], kAddr[ai]);
					x86Ptr = s_buf; xe_add32_rm(rr, kAddr[ai]);
					s_gotlen[s_ncases] = (int)(x86Ptr - s_buf);
					memcpy(s_got[s_ncases], s_buf, (size_t)s_gotlen[s_ncases]);
					s_text[s_ncases++] = strdup(t4);

					SWEEP_GUARD();
					snprintf(t4, sizeof(t4), "mov DWORD PTR ds:0x%lx, %s",
					         kAddr[ai], r32[rr]);
					x86Ptr = s_buf; xe_mov32_mr(kAddr[ai], rr);
					s_gotlen[s_ncases] = (int)(x86Ptr - s_buf);
					memcpy(s_got[s_ncases], s_buf, (size_t)s_gotlen[s_ncases]);
					s_text[s_ncases++] = strdup(t4);
				}
		}

		/* Unary forms over every register. */
		{
			int u;
			char t2[96];
			for (u = 0; u < 16; u++)
			{
				snprintf(t2, sizeof(t2), "not %s", r32[u]);
				x86Ptr = s_buf; xe_not32_r(u);
				s_gotlen[s_ncases] = (int)(x86Ptr - s_buf);
				memcpy(s_got[s_ncases], s_buf, (size_t)s_gotlen[s_ncases]);
				s_text[s_ncases++] = strdup(t2);

				snprintf(t2, sizeof(t2), "neg %s", r32[u]);
				x86Ptr = s_buf; xe_neg32_r(u);
				s_gotlen[s_ncases] = (int)(x86Ptr - s_buf);
				memcpy(s_got[s_ncases], s_buf, (size_t)s_gotlen[s_ncases]);
				s_text[s_ncases++] = strdup(t2);
			}
		}
	}

	if (run_batch() != 0) return 2;

	printf("hwas: %d/%d C emitter macros match GNU as\n",
	       s_ncases - failures, s_ncases);
	return failures != 0;
}
