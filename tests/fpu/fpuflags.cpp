/*  EE FPU flag behaviour of DIV.S and RSQRT.S, through the real FPU.cpp.
 *
 *  The console-capture suites score results, not FCR31, so these flag
 *  rules had nothing guarding them. The reference is what both
 *  recompilers emit and what the interpreter did before the soft model:
 *
 *    - DIV.S and RSQRT.S clamp but never set or clear O/U or SO/SU.
 *    - RSQRT.S with a negative, non-zero Ft sets I and SI and computes
 *      from |Ft|.
 *
 *  Self-contained: needs no ps2autotests checkout.
 *
 *  Usage: tests/fpu/fpuflags
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fenv.h>

#include "Common.h"
#include "R5900.h"

alignas(16) cpuRegisters cpuRegs;
alignas(16) fpuRegisters fpuRegs;

u32 vtlb_memRead32(u32) { return 0; }
void vtlb_memWrite32(u32, u32) {}
void intDoBranch(u32) {}

namespace R5900 { namespace Interpreter { namespace OpcodeImpl { namespace COP1 {
	void DIV_S(); void RSQRT_S();
} } } }
using namespace R5900::Interpreter::OpcodeImpl::COP1;

#define FS 1
#define FT 2
#define FD 3

#define FLAG_I  0x00020000u
#define FLAG_O  0x00008000u
#define FLAG_U  0x00004000u
#define FLAG_SI 0x00000040u
#define FLAG_SO 0x00000010u
#define FLAG_SU 0x00000008u
#define FLAGS_OU (FLAG_O | FLAG_U | FLAG_SO | FLAG_SU)

static int failures;

static void run(const char* name, void (*fn)(), u32 fs, u32 ft, u32 fcr_in,
                u32 mask, u32 want_bits, int check_res, u32 want_res)
{
	u32 got;
	cpuRegs.code = (FT << 16) | (FS << 11) | (FD << 6);
	fpuRegs.fpr[FS].UL = fs;
	fpuRegs.fpr[FT].UL = ft;
	fpuRegs.fpr[FD].UL = 0;
	fpuRegs.fprc[31] = fcr_in;
	fn();
	got = fpuRegs.fprc[31] & mask;
	if (got != want_bits)
	{
		printf("  FAIL %-34s fcr31&%08x = %08x, want %08x\n", name,
			(unsigned)mask, (unsigned)got, (unsigned)want_bits);
		failures++;
	}
	if (check_res && fpuRegs.fpr[FD].UL != want_res)
	{
		printf("  FAIL %-34s fd = %08x, want %08x\n", name,
			(unsigned)fpuRegs.fpr[FD].UL, (unsigned)want_res);
		failures++;
	}
}

int main(void)
{
	const u32 huge = 0x7F000000u; /* 2^127 */
	const u32 tiny = 0x00800000u; /* 2^-126 */

	fesetround(FE_TOWARDZERO);

	/* DIV.S: an overflowing and an underflowing quotient touch no O/U bit,
	 * whether those bits start clear or set. */
	run("div overflow, flags clear",  DIV_S, huge, tiny, 0, FLAGS_OU, 0, 0, 0);
	run("div underflow, flags clear", DIV_S, tiny, huge, 0, FLAGS_OU, 0, 0, 0);
	run("div overflow, flags set",    DIV_S, huge, tiny, FLAGS_OU, FLAGS_OU, FLAGS_OU, 0, 0);
	run("div ordinary, flags set",    DIV_S, 0x40000000u, 0x3F800000u, FLAGS_OU,
		FLAGS_OU, FLAGS_OU, 1, 0x40000000u);

	/* RSQRT.S: negative Ft raises I/SI and uses |Ft|: 1 / sqrt(4) = 0.5. */
	run("rsqrt negative ft raises I",  RSQRT_S, 0x3F800000u, 0xC0800000u, 0,
		FLAG_I | FLAG_SI, FLAG_I | FLAG_SI, 1, 0x3F000000u);
	run("rsqrt positive ft, no I",     RSQRT_S, 0x3F800000u, 0x40800000u, 0,
		FLAG_I | FLAG_SI, 0, 1, 0x3F000000u);
	run("rsqrt overflow, flags clear", RSQRT_S, huge, tiny, 0, FLAGS_OU, 0, 0, 0);
	run("rsqrt overflow, flags set",   RSQRT_S, huge, tiny, FLAGS_OU, FLAGS_OU, FLAGS_OU, 0, 0);
	run("rsqrt underflow, flags clear", RSQRT_S, tiny, 0x7E800000u, 0, FLAGS_OU, 0, 0, 0);

	if (failures)
	{
		printf("fpuflags: %d failure(s)\n", failures);
		return 1;
	}
	printf("fpuflags: all cases pass\n");
	return 0;
}
