/*  DMAC CPCOND0 against console captures.
 *
 *  Scores CPCOND0 in pcsx2/COP0.c against ps2autotests
 *  tests/dma/dmac/cpcond.expected, which nothing read.
 *
 *  CPCOND0 is the signal the EE's BC0F and BC0T branch on, and it is a
 *  pure function of two DMAC registers: it is set when every channel the
 *  program is watching has raised its interrupt, which the hardware
 *  computes as
 *
 *      ((STAT.CIS | ~PCR.CPC) & 0x3FF) == 0x3FF
 *
 *  -- a channel contributes only if PCR.CPC selects it, and an unselected
 *  channel reads as already satisfied. That is why the signal starts set
 *  with PCR clear: nothing is being watched, so the condition is
 *  vacuously true.
 *
 *  The capture walks six states, and the interesting ones are the pair
 *  either side of the DMA: selecting channel 9 in PCR drops the signal
 *  because that channel has not finished, and the transfer raising
 *  STAT.CIS9 brings it back. Clearing PCR again leaves it set for the
 *  same vacuous reason as the start, and restoring PCR leaves it set
 *  because CIS9 is still latched -- writing CIS9 back is what finally
 *  clears it, since STAT bits are acknowledged by writing one.
 *
 *  Usage: tests/ee/hwcpcond
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "Common.h"

/* The DMAC registers the signal reads. Real ones, since COP0.c's
 * CPCOND0 indexes them through dmacRegs. */
alignas(16) u8 eeHw[0x10000];
alignas(16) cpuRegisters cpuRegs;

/* COP0.c's other reachable symbols: the TLB array and the branch and
 * memory paths CPCOND0 itself never enters. */
static EEVM_MemoryAllocMess s_eemem;
EEVM_MemoryAllocMess* eeMem = &s_eemem;
tlbs tlb[48];
void intDoBranch(u32) { }
void vtlb_VMap(u32, u32, u32) { }
void vtlb_VMapBuffer(u32, void*, u32) { }
void vtlb_VMapUnmap(u32, u32) { }
void intSetBranch() { }
static R5900cpu s_cpu = {};
R5900cpu* Cpu = &s_cpu;

#define CPC9 (1u << 9)
#define CIS9 (1u << 9)

#include "R5900OpcodeTables.h"

int main(void)
{
	static const struct { const char* what; u32 cpc, cis; int want; }
	kSteps[] = {
		/* Straight from the capture's six lines. */
		{ "initial value",                    0,    0,    1 },
		{ "after setting PCR.CPC",            CPC9, 0,    0 },
		{ "after sending DMA",                CPC9, CIS9, 1 },
		{ "after clearing PCR.CPC",           0,    CIS9, 1 },
		{ "after restoring PCR.CPC",          CPC9, CIS9, 1 },
		{ "after resetting STAT.CIS",         CPC9, 0,    0 },
	};
	const int n = (int)(sizeof(kSteps)/sizeof(kSteps[0]));
	int i, pass = 0;

	for (i = 0; i < n; i++)
	{
		int got;
		memset(eeHw, 0, sizeof(eeHw));
		dmacRegs.pcr.CPC  = kSteps[i].cpc >> 0;
		dmacRegs.stat.CIS = kSteps[i].cis >> 0;
		got = intCPCOND0();
		if (got == kSteps[i].want) pass++;
		else
			printf("  %-28s console %d, ours %d\n",
			       kSteps[i].what, kSteps[i].want, got);
	}

	printf("hwcpcond: %d/%d CPCOND0 states match the console\n", pass, n);
	return pass != n;
}
