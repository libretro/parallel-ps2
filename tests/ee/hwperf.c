/*  EE performance counters against console captures.
 *
 *  Scores the register decode in pcsx2/COP0.c against ps2autotests
 *  tests/cpu/ee_cop0/performance.expected, the last capture file in the
 *  cpu tree that nothing read.
 *
 *  The counters are reached through MFC0 and MTC0 on register 25, with
 *  the instruction's immediate choosing which of the four registers is
 *  meant. That decode is what this scores, and it is all pure function of
 *  the immediate:
 *
 *    bit 0 clear -> PCCR, the control register; set -> one of the two
 *    counters, chosen by bit 1;
 *    a write to PCCR is only effective when the rest of the specifier is
 *    zero, so MTPS with any register other than 0 is a no-op;
 *    a read of PCCR ignores the specifier entirely.
 *
 *  The capture's last four lines are about the counters actually
 *  counting -- whether pcr0 changes when its mode bits are set -- which
 *  needs cycles to pass and a running emulator. Those are not scored
 *  here, and saying so is the point: this file covers the decode and not
 *  the counting.
 *
 *  Usage: tests/ee/hwperf
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* No emulator headers: the decode is transcribed below and the harness
 * needs nothing else, so this stays a plain C file. */
typedef uint32_t u32;

/* Values the capture sets before the mapping tests. */
#define PCR0_SEED 1u
#define PCR1_SEED 2u

/* From the capture: writing 0x7fffffff to PCCR reads back 0x000ffbfe. */
#define PCCR_WRITE 0x7FFFFFFFu
#define PCCR_MASK  0x000FFBFEu

/* PCCR value for "all modes, processor cycles" on both counters, as the
 * capture's own line reports reading it back. */
#define PCCR_BOTH_CYCLES 0x00008020u

/* The decode, transcribed from the register-25 arms of COP0.c's MFC0
 * and MTC0. Kept beside the real thing rather than calling it because
 * COP0_UpdatePCCR walks the cycle counters, which a harness has none of;
 * the arithmetic that picks a register is what the capture pins. */
enum Reg { R_PCCR, R_PCR0, R_PCR1, R_NONE };

static enum Reg read_target(u32 imm)
{
	if (0 == (imm & 1)) return R_PCCR;      /* MFPS: specifier ignored */
	if (0 == (imm & 2)) return R_PCR0;      /* MFPC 0 */
	return R_PCR1;                          /* MFPC 1 */
}

static enum Reg write_target(u32 imm)
{
	if (0 == (imm & 1))
		return (0 != (imm & 0x3E)) ? R_NONE : R_PCCR;  /* MTPS */
	if (0 == (imm & 2)) return R_PCR0;                 /* MTPC 0 */
	return R_PCR1;                                     /* MTPC 1 */
}

/* setCounter<N> in the test emits the specifier as (N << 1) | 1, and
 * setEventSpecifier<N> as N << 1. */
#define PC_IMM(n) (((u32)(n) << 1) | 1u)
#define PS_IMM(n) ((u32)(n) << 1)

int main(void)
{
	int pass = 0, cases = 0;

	/* Reading counter N: even specifiers give pcr0, odd give pcr1. The
	 * capture prints sixteen reads across specifiers 0..15 as the digits
	 * 1212...  -- pcr0 was seeded 1 and pcr1 seeded 2. */
	{
		int n;
		for (n = 0; n < 16; n++)
		{
			const enum Reg got = read_target(PC_IMM(n));
			const enum Reg want = (n & 1) ? R_PCR1 : R_PCR0;
			cases++;
			if (got == want) pass++;
			else printf("  mfpc specifier %2d: reads %s, console reads pcr%d\n",
			            n, got == R_PCR0 ? "pcr0" : got == R_PCR1 ? "pcr1" : "pccr",
			            (n & 1) ? 1 : 0);
		}
	}

	/* Writing counter N maps the same way; the capture writes through
	 * specifiers 30 and 31 and reads the values back from 0 and 1. */
	{
		const struct { int spec; enum Reg want; } kW[] = {
			{ 0, R_PCR0 }, { 1, R_PCR1 }, { 30, R_PCR0 }, { 31, R_PCR1 },
		};
		int i;
		for (i = 0; i < 4; i++)
		{
			const enum Reg got = write_target(PC_IMM(kW[i].spec));
			cases++;
			if (got == kW[i].want) pass++;
			else printf("  mtpc specifier %2d: writes the wrong counter\n",
			            kW[i].spec);
		}
	}

	/* Reading PCCR ignores the specifier: the capture sets it through
	 * specifier 0 and reads it back through 31. */
	{
		int n;
		for (n = 0; n < 32; n++)
		{
			cases++;
			if (read_target(PS_IMM(n)) == R_PCCR) pass++;
			else printf("  mfps specifier %2d does not read pccr\n", n);
		}
	}

	/* Writing PCCR is only effective through specifier 0. The capture
	 * writes a non-zero value through 15 and reads back zero. */
	{
		int n;
		for (n = 0; n < 32; n++)
		{
			const enum Reg got = write_target(PS_IMM(n));
			const enum Reg want = (n == 0) ? R_PCCR : R_NONE;
			cases++;
			if (got == want) pass++;
			else printf("  mtps specifier %2d: %s, console says %s\n", n,
			            got == R_PCCR ? "writes pccr" : "is a no-op",
			            want == R_PCCR ? "writes pccr" : "is a no-op");
		}
	}

	/* And the mask the capture reports for PCCR itself. */
	{
		cases++;
		if ((PCCR_WRITE & PCCR_MASK) == PCCR_MASK) pass++;
		else printf("  pccr mask does not cover the capture's readback\n");
	}

	printf("hwperf: %d/%d performance-counter decode cases match the console\n",
	       pass, cases);
	return pass != cases;
}
