/*  MMI divide differential test.
 *
 *  The EE recompiler builds the parallel divides by hand: a guard for
 *  INT_MIN / -1, a guard for division by zero, then CDQ and IDIV for the
 *  signed forms or XOR EDX,EDX and DIV for the unsigned one. Getting the
 *  last instruction of that sequence wrong is not a build error and not
 *  usually a crash -- an unsigned divide of a sign-extended dividend
 *  returns a wrong quotient for most operands and only raises #DE when
 *  the quotient will not fit in 32 bits. PDIVW and PDIVBW shipped that
 *  way and it went unnoticed for a long time.
 *
 *  This emits each sequence through the same macros the recompiler uses,
 *  executes it, and compares against the semantics in pcsx2/MMI.c. A
 *  divide that faults takes the process with it, which is the loudest
 *  possible failure and exactly what is wanted here.
 *
 *  Build and run:  sh tests/mmi/build.sh && ./tests/mmi/mmi_divcheck
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef uint8_t u8;

/* The two condition codes this file needs, with the values from
 * common/emitter/x86types.h. Declared here rather than including that
 * header so the test stays standalone C. */
enum { Jcc_Unconditional = -1, Jcc_NotEqual = 0x5 };

static u8* x86Ptr;

#include "common/emitter/c89emit.h"
#include "common/emitter/c89ops.h"

/* The emitted routine takes the dividend in EDI and the divisor in ESI and
 * returns the quotient in EAX with the remainder in EDX, so the caller can
 * check both halves of what the recompiler would have written to LO and HI. */
typedef struct { int32_t quot, rem; } Result;
typedef Result (*DivFn)(int32_t dividend, int32_t divisor);

/* ---------------------------------------------------------------------- */
/* References, transcribed from pcsx2/MMI.c.                            */

static void ref_signed(int32_t dd, int32_t dv, int32_t* q, int32_t* r)
{
	if ((uint32_t)dd == 0x80000000u && (uint32_t)dv == 0xffffffffu)
		{ *q = (int32_t)0x80000000; *r = 0; }
	else if (dv != 0)
		{ *q = dd / dv; *r = dd % dv; }
	else
		{ *q = (dd < 0) ? 1 : -1; *r = dd; }
}

static void ref_unsigned(uint32_t dd, uint32_t dv, int32_t* q, int32_t* r)
{
	if (dv != 0)
		{ *q = (int32_t)(dd / dv); *r = (int32_t)(dd % dv); }
	else
		{ *q = -1; *r = (int32_t)dd; }
}

/* ---------------------------------------------------------------------- */
/* Emit the same shape the recompiler emits: guards, then the divide.      */

static void emit_divide(int is_signed)
{
	uint8_t *cont_zero, *cont_ovf1, *cont_ovf2, *end_ovf = NULL, *end_zero;

	/* dividend -> eax, divisor -> ecx */
	xe_mov32_rr(XE_AX, XE_DI);
	xe_mov32_rr(XE_CX, XE_SI);

	if (is_signed)
	{
		xe_cmp32_ri(XE_AX, 0x80000000);
		xe_fwd_jcc8(Jcc_NotEqual, cont_ovf1);
		xe_cmp32_ri(XE_CX, 0xffffffff);
		xe_fwd_jcc8(Jcc_NotEqual, cont_ovf2);
		xe_xor32_rr(XE_DX, XE_DX);          /* eax stays 0x80000000 */
		xe_fwd_jcc8(Jcc_Unconditional, end_ovf);
		xe_fwd_set8(cont_ovf1);
		xe_fwd_set8(cont_ovf2);
	}

	xe_cmp32_ri(XE_CX, 0);
	xe_fwd_jcc8(Jcc_NotEqual, cont_zero);
	xe_mov32_rr(XE_DX, XE_AX);              /* remainder = dividend */
	if (is_signed)
	{
		xe_sar32_ri(XE_AX, 31);
		xe_shl32_ri(XE_AX, 1);
		xe_not32_r(XE_AX);                  /* (dd < 0) ? 1 : -1 */
	}
	else
		xe_mov32_ri(XE_AX, 0xffffffff);
	xe_fwd_jcc8(Jcc_Unconditional, end_zero);

	xe_fwd_set8(cont_zero);
	if (is_signed)
	{
		xe_cdq();
		xe_idiv32_r(XE_CX);
	}
	else
	{
		xe_xor32_rr(XE_DX, XE_DX);
		xe_div32_r(XE_CX);
	}

	if (is_signed)
		xe_fwd_set8(end_ovf);
	xe_fwd_set8(end_zero);

	/* Result { quot, rem } is returned in rax per the SysV ABI for a
	 * two-int struct: quotient in eax, remainder in edx -> shift into rax. */
	xe_shl64_ri(XE_DX, 32);
	xe_mov32_rr(XE_AX, XE_AX);              /* zero the top half of rax */
	xe_or64_rr(XE_AX, XE_DX);
	xe_ret();
}

static DivFn build(u8* page, int is_signed)
{
	x86Ptr = page;
	emit_divide(is_signed);
	return (DivFn)(void*)page;
}

int main(void)
{
	static const uint32_t vals[] = {
		0u, 1u, 2u, 7u, 100u, 32767u, 65535u,
		0x7fffffffu, 0x80000000u, 0x80000001u, 0xffffffffu, 0xfffffff9u,
		0xbf800000u, 0x3c000000u, 3584u, 0xeeeeeu
	};
	const int n = (int)(sizeof(vals) / sizeof(vals[0]));
	long checked = 0, bad = 0;
	u8* page;
	DivFn sdiv, udiv;
	int i, j;

	page = (u8*)mmap(NULL, 8192, PROT_READ | PROT_WRITE | PROT_EXEC,
	                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED)
	{
		fprintf(stderr, "mmap failed\n");
		return 2;
	}

	sdiv = build(page, 1);
	udiv = build(page + 4096, 0);

	for (i = 0; i < n; i++)
	{
		for (j = 0; j < n; j++)
		{
			int32_t rq, rr;
			Result got;

			ref_signed((int32_t)vals[i], (int32_t)vals[j], &rq, &rr);
			got = sdiv((int32_t)vals[i], (int32_t)vals[j]);
			checked++;
			if (got.quot != rq || got.rem != rr)
			{
				if (bad++ < 8)
					printf("signed   %08x / %08x: want q=%d r=%d got q=%d r=%d\n",
					       vals[i], vals[j], rq, rr, got.quot, got.rem);
			}

			ref_unsigned(vals[i], vals[j], &rq, &rr);
			got = udiv((int32_t)vals[i], (int32_t)vals[j]);
			checked++;
			if (got.quot != rq || got.rem != rr)
			{
				if (bad++ < 16)
					printf("unsigned %08x / %08x: want q=%d r=%d got q=%d r=%d\n",
					       vals[i], vals[j], rq, rr, got.quot, got.rem);
			}
		}
	}

	printf("mmi_divcheck: %ld cases, %ld mismatches\n", checked, bad);
	return bad != 0;
}
